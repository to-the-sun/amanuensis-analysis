/*
 * flux_shared.c
 *
 * Helpers shared by flux_separator and flux_report: WAV decoding, pitch naming,
 * case-insensitive filename matching, the double-click console prompt, and the
 * console lock the separator's worker threads write through.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "flux_shared.h"
#include "flux_envelope.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* Serialises console writes from concurrent separator worker threads. */
pthread_mutex_t g_console_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

int ends_with_icase(const char *str, const char *suffix) {
    if (!str || !suffix) return 0;
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (str_len < suf_len) return 0;
    const char *p = str + str_len - suf_len;
    for (size_t i = 0; i < suf_len; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

int contains_icase(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        int match = 1;
        for (size_t j = 0; j < nlen; j++) {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

int starts_with_icase(const char *str, const char *prefix) {
    if (!str || !prefix) return 0;
    size_t plen = strlen(prefix);
    if (strlen(str) < plen) return 0;
    for (size_t i = 0; i < plen; i++) {
        if (tolower((unsigned char)str[i]) != tolower((unsigned char)prefix[i])) {
            return 0;
        }
    }
    return 1;
}

void get_note_name(int midi_pitch, char *buffer, size_t buffer_size) {
    if (midi_pitch < 0 || midi_pitch > 127) {
        snprintf(buffer, buffer_size, "Unknown");
        return;
    }
    int note_idx = midi_pitch % 12;
    int octave = (midi_pitch / 12) - 1;
    snprintf(buffer, buffer_size, "%s%d", NOTE_NAMES[note_idx], octave);
}

double midi_to_frequency(int midi_pitch) {
    return 440.0 * pow(2.0, (midi_pitch - 69.0) / 12.0);
}

void wait_for_keypress(void) {
    printf("\nPress ENTER to exit...\n");
    fflush(stdout);
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    system("pause");
#else
    if (isatty(fileno(stdin))) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
        if (c == '\n') {
            getchar();
        }
    } else {
        FILE *tty = fopen("/dev/tty", "r");
        if (tty) {
            fgetc(tty);
            fclose(tty);
        }
    }
#endif
}

/* WAV File Reader */
WAVFile* read_wav(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not open file '%s'\n", filepath);
        pthread_mutex_unlock(&g_console_mutex);
        return NULL;
    }

    char chunk_id[5] = {0};
    uint32_t chunk_size = 0;
    char format[5] = {0};

    if (fread(chunk_id, 1, 4, f) != 4 || fread(&chunk_size, 4, 1, f) != 1 || fread(format, 1, 4, f) != 4) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Invalid WAV header in '%s'\n", filepath);
        pthread_mutex_unlock(&g_console_mutex);
        fclose(f);
        return NULL;
    }

    if (strncmp(chunk_id, "RIFF", 4) != 0 || strncmp(format, "WAVE", 4) != 0) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: File '%s' is not a valid RIFF WAVE file\n", filepath);
        pthread_mutex_unlock(&g_console_mutex);
        fclose(f);
        return NULL;
    }

    WAVFile *wav = (WAVFile*)calloc(1, sizeof(WAVFile));
    uint32_t data_size = 0;
    long data_pos = 0;

    while (fread(chunk_id, 1, 4, f) == 4 && fread(&chunk_size, 4, 1, f) == 1) {
        chunk_id[4] = '\0';
        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t audio_fmt = 0, channels = 0, bits = 0;
            uint32_t srate = 0, bytes_per_sec = 0;
            uint16_t block_align = 0;

            if (fread(&audio_fmt, 2, 1, f) != 1 ||
                fread(&channels, 2, 1, f) != 1 ||
                fread(&srate, 4, 1, f) != 1 ||
                fread(&bytes_per_sec, 4, 1, f) != 1 ||
                fread(&block_align, 2, 1, f) != 1 ||
                fread(&bits, 2, 1, f) != 1) {
                pthread_mutex_lock(&g_console_mutex);
                printf("Error: Failed to read format header in '%s'\n", filepath);
                pthread_mutex_unlock(&g_console_mutex);
                free(wav);
                fclose(f);
                return NULL;
            }

            wav->audio_format = audio_fmt;
            wav->num_channels = channels;
            wav->sample_rate = srate;
            wav->bits_per_sample = bits;

            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
        } else if (strncmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            data_pos = ftell(f);
            fseek(f, chunk_size, SEEK_CUR);
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (data_size == 0 || wav->num_channels == 0 || wav->sample_rate == 0) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Missing format or data chunk in '%s'\n", filepath);
        pthread_mutex_unlock(&g_console_mutex);
        free(wav);
        fclose(f);
        return NULL;
    }

    uint32_t bytes_per_sample = wav->bits_per_sample / 8;
    uint32_t total_frames = data_size / (wav->num_channels * bytes_per_sample);
    wav->total_samples = total_frames;
    wav->data = (double*)malloc((size_t)total_frames * wav->num_channels * sizeof(double));

    fseek(f, data_pos, SEEK_SET);

    size_t total_data_samples = (size_t)total_frames * wav->num_channels;
    if (wav->audio_format == 1 && wav->bits_per_sample == 16) {
        int16_t *buf = (int16_t*)malloc(total_data_samples * sizeof(int16_t));
        size_t read_cnt = fread(buf, sizeof(int16_t), total_data_samples, f);
        (void)read_cnt;
        for (size_t i = 0; i < total_data_samples; i++) {
            wav->data[i] = buf[i] / 32768.0;
        }
        free(buf);
    } else if (wav->audio_format == 1 && wav->bits_per_sample == 24) {
        uint8_t *buf = (uint8_t*)malloc(total_data_samples * 3);
        size_t read_cnt = fread(buf, 3, total_data_samples, f);
        (void)read_cnt;
        for (size_t i = 0; i < total_data_samples; i++) {
            int32_t val = (buf[i * 3] | (buf[i * 3 + 1] << 8) | (buf[i * 3 + 2] << 16));
            if (val & 0x800000) val |= 0xFF000000;
            wav->data[i] = val / 8388608.0;
        }
        free(buf);
    } else if (wav->audio_format == 3 && wav->bits_per_sample == 32) {
        float *buf = (float*)malloc(total_data_samples * sizeof(float));
        size_t read_cnt = fread(buf, sizeof(float), total_data_samples, f);
        (void)read_cnt;
        for (size_t i = 0; i < total_data_samples; i++) {
            wav->data[i] = buf[i];
        }
        free(buf);
    } else {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Unsupported WAV format (format: %d, bits: %d) in '%s'\n", wav->audio_format, wav->bits_per_sample, filepath);
        pthread_mutex_unlock(&g_console_mutex);
        free(wav->data);
        free(wav);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return wav;
}
