/*
 * flux_shared.h
 *
 * Types and helpers used by both flux_separator and flux_report: the decoded
 * WAV container, the per-file run summary the batch dashboard renders, pitch
 * naming, case-insensitive filename matching, the double-click console prompt,
 * and the console lock the separator's worker threads write through.
 */

#ifndef FLUX_SHARED_H
#define FLUX_SHARED_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint16_t audio_format; // 1 = PCM integer, 3 = IEEE float
    uint32_t total_samples; // samples per channel
    double *data; // interleaved: data[c + ch * num_channels]
} WAVFile;

typedef struct {
    char input_path[1024];
    char basename[512];
    uint32_t sample_rate;
    uint16_t num_channels;
    uint32_t total_samples;
    double duration;
    int stem_count;
    double elapsed_sec;
    double speed_ratio;
    int success;
    int skipped;
} FileBatchResult;

/* Serialises console writes from concurrent separator worker threads. */
extern pthread_mutex_t g_console_mutex;

WAVFile* read_wav(const char *filepath);

void get_note_name(int midi_pitch, char *buffer, size_t buffer_size);
double midi_to_frequency(int midi_pitch);

void wait_for_keypress(void);

int contains_icase(const char *haystack, const char *needle);
int starts_with_icase(const char *str, const char *prefix);
int ends_with_icase(const char *str, const char *suffix);

#endif /* FLUX_SHARED_H */
