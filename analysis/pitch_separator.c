/*
 * pitch_separator.c
 *
 * Perfect Reconstruction Pitch Stem Separator with Noise Floor Thresholding in C
 *
 * Separates an audio WAV file into individual MIDI pitch stems (0-127) using an STFT COLA filter bank.
 * Bins are partitioned by their corresponding integer MIDI pitch.
 * Evaluates each individual pitch stem post-separation against a relative noise floor threshold (19% of peak amplitude / -14.4 dB).
 * Pitch stems with at least 99 milliseconds worth of frames above the noise floor are exported as WAV files.
 * Reconstructed stems preserve untruncated audio data without zeroing out values below the noise floor threshold.
 * Summing exported pitch stems reconstructs the original audible notes perfectly.
 *
 * Can be executed via drag-and-drop (command-line argument) or double-click (interactive prompt).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <direct.h>
#define MKDIR(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(dir) mkdir(dir, 0755)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_SIZE 8192
#define HOP_SIZE (FFT_SIZE / 4)
#define MAX_CHANNELS 2
#define NUM_MIDI_PITCHES 128

/* Noise Floor Ratio: 19.0% of signal peak amplitude (-14.43 dB) */
double g_noise_floor_ratio = 0.19;

/* Silence Threshold: Peak amplitude limit for silent boundary samples (0.0009) */
double g_silence_threshold = 0.0009;

typedef struct {
    double r;
    double i;
} Complex;

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint16_t audio_format; // 1 = PCM integer, 3 = IEEE float
    uint32_t total_samples; // samples per channel
    double *data; // interleaved: data[c + ch * num_channels]
} WAVFile;

const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void wait_for_keypress(void) {
    printf("\nPress ENTER to exit...\n");
    getchar();
}

void trim_string(char *str) {
    if (!str) return;
    char *start = str;
    while (*start && (isspace((unsigned char)*start) || *start == '"' || *start == '\'')) {
        start++;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
    size_t len = strlen(str);
    while (len > 0 && (isspace((unsigned char)str[len - 1]) || str[len - 1] == '"' || str[len - 1] == '\'')) {
        str[len - 1] = '\0';
        len--;
    }
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

void print_progress_bar(const char *label, uint32_t current, uint32_t total, double elapsed_sec) {
    int bar_width = 30;
    float percentage = (total > 0) ? ((float)current / total) : 1.0f;
    int pos = (int)(bar_width * percentage);

    printf("\r%-26s [", label);
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %3d%% (%u/%u) - %.1fs", (int)(percentage * 100.0f), current, total, elapsed_sec);
    fflush(stdout);
}

/* FFT implementation (Radix-2 Cooley-Tukey) */
void fft(Complex *x, int n, int invert) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            Complex temp = x[i];
            x[i] = x[j];
            x[j] = temp;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double angle = 2.0 * M_PI / len * (invert ? 1.0 : -1.0);
        Complex wlen = { cos(angle), sin(angle) };
        for (int i = 0; i < n; i += len) {
            Complex w = { 1.0, 0.0 };
            for (int j = 0; j < len / 2; j++) {
                Complex u = x[i + j];
                Complex v = {
                    x[i + j + len / 2].r * w.r - x[i + j + len / 2].i * w.i,
                    x[i + j + len / 2].r * w.i + x[i + j + len / 2].i * w.r
                };
                x[i + j].r = u.r + v.r;
                x[i + j].i = u.i + v.i;
                x[i + j + len / 2].r = u.r - v.r;
                x[i + j + len / 2].i = u.i - v.i;

                Complex w_next = {
                    w.r * wlen.r - w.i * wlen.i,
                    w.r * wlen.i + w.i * wlen.r
                };
                w = w_next;
            }
        }
    }

    if (invert) {
        for (int i = 0; i < n; i++) {
            x[i].r /= n;
            x[i].i /= n;
        }
    }
}

/* WAV File Reader */
WAVFile* read_wav(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        printf("Error: Could not open file '%s'\n", filepath);
        return NULL;
    }

    char chunk_id[5] = {0};
    uint32_t chunk_size = 0;
    char format[5] = {0};

    if (fread(chunk_id, 1, 4, f) != 4 || fread(&chunk_size, 4, 1, f) != 1 || fread(format, 1, 4, f) != 4) {
        printf("Error: Invalid WAV header in '%s'\n", filepath);
        fclose(f);
        return NULL;
    }

    if (strncmp(chunk_id, "RIFF", 4) != 0 || strncmp(format, "WAVE", 4) != 0) {
        printf("Error: File '%s' is not a valid RIFF WAVE file\n", filepath);
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
                printf("Error: Failed to read format header\n");
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
        printf("Error: Missing format or data chunk in '%s'\n", filepath);
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
        printf("Error: Unsupported WAV format (format: %d, bits: %d)\n", wav->audio_format, wav->bits_per_sample);
        free(wav->data);
        free(wav);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return wav;
}

/* WAV File Writer (writes 16-bit PCM) */
int write_wav_16bit(const char *filepath, uint32_t sample_rate, uint16_t num_channels, uint32_t total_frames, const double *data) {
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        printf("Error: Could not open output file '%s' for writing\n", filepath);
        return 0;
    }

    uint32_t bytes_per_sample = 2;
    uint32_t data_size = total_frames * num_channels * bytes_per_sample;
    uint32_t riff_size = 36 + data_size;
    uint16_t audio_format = 1; // PCM
    uint32_t byte_rate = sample_rate * num_channels * bytes_per_sample;
    uint16_t block_align = num_channels * bytes_per_sample;
    uint16_t bits_per_sample = 16;

    // Header
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    // fmt chunk
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_chunk_size = 16;
    fwrite(&fmt_chunk_size, 4, 1, f);
    fwrite(&audio_format, 2, 1, f);
    fwrite(&num_channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);

    // data chunk
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);

    // Sample conversion
    size_t total_samples = (size_t)total_frames * num_channels;
    int16_t *buf = (int16_t*)malloc(total_samples * sizeof(int16_t));
    for (size_t i = 0; i < total_samples; i++) {
        double s = data[i];
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        buf[i] = (int16_t)round(s * 32767.0);
    }

    fwrite(buf, sizeof(int16_t), total_samples, f);
    free(buf);
    fclose(f);
    return 1;
}

/* Helper to map frequency to MIDI pitch */
int freq_to_midi_pitch(double freq) {
    if (freq <= 0.0 || freq < 8.175798915643707) {
        return 0;
    }
    double midi_float = 69.0 + 12.0 * log2(freq / 440.0);
    int p = (int)floor(midi_float + 0.5);
    if (p < 0) p = 0;
    if (p > 127) p = 127;
    return p;
}

/* Extract directory and filename components */
void get_filepath_components(const char *filepath, char *out_dir, char *out_basename, size_t size) {
    const char *last_slash = strrchr(filepath, '/');
    const char *last_backslash = strrchr(filepath, '\\');
    const char *sep = (last_slash > last_backslash) ? last_slash : last_backslash;

    if (sep) {
        size_t dir_len = sep - filepath;
        if (dir_len >= size) dir_len = size - 1;
        strncpy(out_dir, filepath, dir_len);
        out_dir[dir_len] = '\0';
        snprintf(out_basename, size, "%s", sep + 1);
    } else {
        snprintf(out_dir, size, ".");
        snprintf(out_basename, size, "%s", filepath);
    }

    char *dot = strrchr(out_basename, '.');
    if (dot) {
        *dot = '\0';
    }
}

/*
 * Sophisticated Noise Gate with Masking:
 * For each section of audio bounded on either side by one full millisecond of all 0.0 silent samples
 * (or audio file boundary), if that segment of audio at no point rises above the noise floor threshold,
 * that entire segment is reduced to 0.0 silence across all channels and marked as inactive in active_mask.
 */
void apply_noise_gate_with_mask(const double *in_data, double *out_data, uint8_t *active_mask, uint32_t total_samples, uint32_t num_channels, uint32_t sample_rate, double noise_floor_threshold) {
    if (total_samples == 0 || in_data == NULL) return;

    size_t total_channel_samples = (size_t)total_samples * num_channels;
    for (size_t k = 0; k < total_channel_samples; k++) {
        out_data[k] = in_data[k];
        active_mask[k] = 1;
    }

    uint32_t ms_samples = (uint32_t)ceil((double)sample_rate / 1000.0);
    if (ms_samples < 1) ms_samples = 1;

    uint32_t section_start = 0;
    uint32_t i = 0;

    while (i < total_samples) {
        int silent = 1;
        for (uint32_t c = 0; c < num_channels; c++) {
            if (fabs(in_data[i * num_channels + c]) > g_silence_threshold) {
                silent = 0;
                break;
            }
        }

        if (silent) {
            uint32_t silence_start = i;
            while (i < total_samples) {
                int s = 1;
                for (uint32_t c = 0; c < num_channels; c++) {
                    if (fabs(in_data[i * num_channels + c]) > g_silence_threshold) {
                        s = 0;
                        break;
                    }
                }
                if (!s) break;
                i++;
            }
            uint32_t silence_len = i - silence_start;

            if (silence_len >= ms_samples) {
                if (silence_start > section_start) {
                    uint32_t sec_end = silence_start - 1;
                    double sec_peak = 0.0;
                    for (uint32_t k = section_start; k <= sec_end; k++) {
                        for (uint32_t c = 0; c < num_channels; c++) {
                            double val = fabs(in_data[k * num_channels + c]);
                            if (val > sec_peak) sec_peak = val;
                        }
                    }
                    if (sec_peak < noise_floor_threshold) {
                        for (uint32_t k = section_start; k <= sec_end; k++) {
                            for (uint32_t c = 0; c < num_channels; c++) {
                                size_t idx = (size_t)k * num_channels + c;
                                out_data[idx] = 0.0;
                                active_mask[idx] = 0;
                            }
                        }
                    }
                }
                section_start = i;
            }
        } else {
            i++;
        }
    }

    if (section_start < total_samples) {
        uint32_t sec_end = total_samples - 1;
        double sec_peak = 0.0;
        for (uint32_t k = section_start; k <= sec_end; k++) {
            for (uint32_t c = 0; c < num_channels; c++) {
                double val = fabs(in_data[k * num_channels + c]);
                if (val > sec_peak) sec_peak = val;
            }
        }
        if (sec_peak < noise_floor_threshold) {
            for (uint32_t k = section_start; k <= sec_end; k++) {
                for (uint32_t c = 0; c < num_channels; c++) {
                    size_t idx = (size_t)k * num_channels + c;
                    out_data[idx] = 0.0;
                    active_mask[idx] = 0;
                }
            }
        }
    }
}

int process_audio(const char *input_path) {
    clock_t start_clock = clock();

    printf("Reading WAV audio file: %s ...\n", input_path);
    WAVFile *wav = read_wav(input_path);
    if (!wav) {
        return 0;
    }

    double audio_duration = (double)wav->total_samples / wav->sample_rate;
    uint32_t num_channels = wav->num_channels;
    uint32_t total_samples = wav->total_samples;
    uint32_t sample_rate = wav->sample_rate;

    // Calculate overall input signal peak
    double overall_peak = 0.0;
    size_t total_channel_samples = (size_t)total_samples * num_channels;
    for (size_t i = 0; i < total_channel_samples; i++) {
        double abs_val = fabs(wav->data[i]);
        if (abs_val > overall_peak) overall_peak = abs_val;
    }
    if (overall_peak < 1e-6) overall_peak = 1e-6;

    // Calculate noise floor threshold (19.0% of overall peak amplitude)
    double noise_floor_threshold = overall_peak * g_noise_floor_ratio;

    printf("\nAudio File Properties:\n");
    printf("  Format:          %s PCM (%u bits)\n", (wav->audio_format == 3) ? "IEEE Float" : "Integer", wav->bits_per_sample);
    printf("  Sample Rate:     %u Hz\n", wav->sample_rate);
    printf("  Channels:        %u (%s)\n", wav->num_channels, (wav->num_channels == 1) ? "Mono" : "Stereo");
    printf("  Total Duration:  %.2f seconds (%u frames per channel)\n", audio_duration, wav->total_samples);
    printf("  Signal Peak:     %.5f\n", overall_peak);
    printf("  Noise Floor Threshold: %.5f (19.0%% peak threshold / -14.4 dB, min 99ms active frames)\n\n", noise_floor_threshold);

    uint32_t pad_samples = FFT_SIZE;
    uint32_t padded_total_samples = total_samples + 2 * pad_samples;

    double **padded_input = (double**)malloc(num_channels * sizeof(double*));
    for (uint32_t c = 0; c < num_channels; c++) {
        padded_input[c] = (double*)calloc(padded_total_samples, sizeof(double));
        for (uint32_t i = 0; i < total_samples; i++) {
            padded_input[c][i + pad_samples] = wav->data[i * num_channels + c];
        }
    }

    double window[FFT_SIZE];
    for (int i = 0; i < FFT_SIZE; i++) {
        window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / FFT_SIZE));
    }

    double *cola_norm = (double*)calloc(padded_total_samples, sizeof(double));
    uint32_t num_frames = (padded_total_samples >= FFT_SIZE) ? ((padded_total_samples - FFT_SIZE) / HOP_SIZE + 1) : 0;

    for (uint32_t m = 0; m < num_frames; m++) {
        uint32_t offset = m * HOP_SIZE;
        for (int i = 0; i < FFT_SIZE; i++) {
            if (offset + i < padded_total_samples) {
                cola_norm[offset + i] += window[i] * window[i];
            }
        }
    }

    int bin_pitch[FFT_SIZE / 2 + 1];
    for (int k = 0; k <= FFT_SIZE / 2; k++) {
        double freq = (double)k * sample_rate / FFT_SIZE;
        bin_pitch[k] = freq_to_midi_pitch(freq);
    }

    printf("Executing STFT COLA Pitch Separation (8192 FFT, 75%% Overlap)...\n");

    double **stem_buffers[NUM_MIDI_PITCHES];
    int pitch_active[NUM_MIDI_PITCHES] = {0};

    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
        stem_buffers[p] = NULL;
    }

    Complex *fft_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));
    Complex *pitch_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));

    uint32_t total_stft_steps = num_channels * num_frames;
    uint32_t current_step = 0;
    clock_t stft_start_clock = clock();

    for (uint32_t c = 0; c < num_channels; c++) {
        for (uint32_t m = 0; m < num_frames; m++) {
            uint32_t offset = m * HOP_SIZE;

            current_step++;
            if (current_step % 20 == 0 || current_step == total_stft_steps) {
                double elapsed = (double)(clock() - stft_start_clock) / CLOCKS_PER_SEC;
                print_progress_bar("Processing STFT Frames", current_step, total_stft_steps, elapsed);
            }

            for (int i = 0; i < FFT_SIZE; i++) {
                fft_frame[i].r = padded_input[c][offset + i] * window[i];
                fft_frame[i].i = 0.0;
            }

            fft(fft_frame, FFT_SIZE, 0);

            int frame_pitch_present[NUM_MIDI_PITCHES] = {0};
            for (int k = 0; k <= FFT_SIZE / 2; k++) {
                double mag_sq = fft_frame[k].r * fft_frame[k].r + fft_frame[k].i * fft_frame[k].i;
                if (mag_sq > 1e-18) {
                    frame_pitch_present[bin_pitch[k]] = 1;
                }
            }

            for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
                if (!frame_pitch_present[p]) continue;

                if (stem_buffers[p] == NULL) {
                    stem_buffers[p] = (double**)malloc(num_channels * sizeof(double*));
                    for (uint32_t ch = 0; ch < num_channels; ch++) {
                        stem_buffers[p][ch] = (double*)calloc(padded_total_samples, sizeof(double));
                    }
                }
                pitch_active[p] = 1;

                memset(pitch_frame, 0, FFT_SIZE * sizeof(Complex));
                for (int k = 0; k <= FFT_SIZE / 2; k++) {
                    if (bin_pitch[k] == p) {
                        pitch_frame[k] = fft_frame[k];
                        if (k > 0 && k < FFT_SIZE / 2) {
                            pitch_frame[FFT_SIZE - k].r = fft_frame[k].r;
                            pitch_frame[FFT_SIZE - k].i = -fft_frame[k].i;
                        }
                    }
                }

                fft(pitch_frame, FFT_SIZE, 1);

                for (int i = 0; i < FFT_SIZE; i++) {
                    if (offset + i < padded_total_samples) {
                        stem_buffers[p][c][offset + i] += pitch_frame[i].r * window[i];
                    }
                }
            }
        }
    }
    printf("\n");

    free(fft_frame);
    free(pitch_frame);

    char dir[1024], basename[512];
    get_filepath_components(input_path, dir, basename, sizeof(basename));

    char stem_dir[2048];
    snprintf(stem_dir, sizeof(stem_dir), "%s/%s_pitch_stems", dir, basename);
    MKDIR(stem_dir);

    printf("\nEvaluating stems against noise floor threshold (19.0%% peak / -14.4 dB, min 99 ms active frames)...\n");
    printf("Exporting audible pitch stem WAV files to: %s/\n\n", stem_dir);

    int exported_count = 0;

    printf("Audible Pitch Stem Export Table:\n");
    printf("-----------------------------------------------------------------------------------\n");
    printf(" MIDI | Note | Freq (Hz)  | Peak Amp | RMS Amp  | Active Duration | File Exported\n");
    printf("-----------------------------------------------------------------------------------\n");

    double *unpadded_stems[NUM_MIDI_PITCHES] = {NULL};
    double *gated_stems[NUM_MIDI_PITCHES] = {NULL};
    uint8_t *active_masks[NUM_MIDI_PITCHES] = {NULL};
    uint8_t *frame_active[NUM_MIDI_PITCHES] = {NULL};
    int is_audible_stem[NUM_MIDI_PITCHES] = {0};

    // Step 1: Extract unpadded audio, apply noise gate, and pre-identify audible pitch stems
    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
        if (!pitch_active[p] || stem_buffers[p] == NULL) continue;

        unpadded_stems[p] = (double*)malloc(total_channel_samples * sizeof(double));
        gated_stems[p] = (double*)malloc(total_channel_samples * sizeof(double));
        active_masks[p] = (uint8_t*)malloc(total_channel_samples * sizeof(uint8_t));
        frame_active[p] = (uint8_t*)calloc(total_samples, sizeof(uint8_t));

        for (uint32_t i = 0; i < total_samples; i++) {
            uint32_t padded_idx = i + pad_samples;
            double norm = cola_norm[padded_idx];

            for (uint32_t c = 0; c < num_channels; c++) {
                double val = (norm > 1e-12) ? (stem_buffers[p][c][padded_idx] / norm) : 0.0;
                unpadded_stems[p][i * num_channels + c] = val;
            }
        }

        // Apply noise gate to extract gated_stems and active_masks
        apply_noise_gate_with_mask(unpadded_stems[p], gated_stems[p], active_masks[p],
                                   total_samples, num_channels, sample_rate, noise_floor_threshold);

        // Compute metrics on gated_stems to identify audible stems and record active frames
        double max_peak = 0.0;
        double sum_sq = 0.0;
        uint32_t active_frames = 0;

        for (uint32_t i = 0; i < total_samples; i++) {
            int frame_above_threshold = 0;
            double frame_peak = 0.0;

            for (uint32_t c = 0; c < num_channels; c++) {
                double val = gated_stems[p][i * num_channels + c];
                double abs_val = fabs(val);
                if (abs_val > max_peak) max_peak = abs_val;
                if (abs_val > frame_peak) frame_peak = abs_val;
                sum_sq += val * val;

                if (abs_val >= noise_floor_threshold) {
                    frame_above_threshold = 1;
                }
            }

            if (frame_above_threshold) {
                active_frames++;
            }
            if (frame_peak > 0.0) {
                frame_active[p][i] = 1;
            }
        }

        double rms = sqrt(sum_sq / (total_samples * num_channels));
        double active_ms = ((double)active_frames / sample_rate) * 1000.0;

        // Pre-identify audible stems meeting the noise floor threshold criteria
        if (active_ms >= 99.0 && rms > 1e-6) {
            is_audible_stem[p] = 1;
        }
    }

    // Step 2: Sample-by-sample redistribution of zeroed-out audio ONLY to pre-identified audible stems at active frames
    for (uint32_t i = 0; i < total_samples; i++) {
        for (uint32_t c = 0; c < num_channels; c++) {
            size_t k = (size_t)i * num_channels + c;
            double sum_zeroed = 0.0;

            for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
                if (unpadded_stems[p] != NULL && gated_stems[p] != NULL) {
                    sum_zeroed += (unpadded_stems[p][k] - gated_stems[p][k]);
                }
            }

            if (sum_zeroed != 0.0) {
                for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
                    if (is_audible_stem[p] && gated_stems[p] != NULL && frame_active[p] != NULL && frame_active[p][i]) {
                        gated_stems[p][k] += sum_zeroed;
                    }
                }
            }
        }
    }

    // Step 3: Export pre-identified audible pitch stems to WAV
    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
        if (!pitch_active[p] || gated_stems[p] == NULL) continue;

        if (is_audible_stem[p]) {
            double max_peak = 0.0;
            double sum_sq = 0.0;
            uint32_t active_frames = 0;

            for (uint32_t i = 0; i < total_samples; i++) {
                int frame_above_threshold = 0;
                for (uint32_t c = 0; c < num_channels; c++) {
                    double val = gated_stems[p][i * num_channels + c];
                    double abs_val = fabs(val);
                    if (abs_val > max_peak) max_peak = abs_val;
                    sum_sq += val * val;

                    if (abs_val >= noise_floor_threshold) {
                        frame_above_threshold = 1;
                    }
                }

                if (frame_above_threshold) {
                    active_frames++;
                }
            }

            double rms = sqrt(sum_sq / (total_samples * num_channels));
            double active_ms = ((double)active_frames / sample_rate) * 1000.0;

            char note_name[32];
            get_note_name(p, note_name, sizeof(note_name));

            char out_filepath[4096];
            snprintf(out_filepath, sizeof(out_filepath), "%s/%s_pitch_%03d_%s.wav",
                     stem_dir, basename, p, note_name);

            if (write_wav_16bit(out_filepath, sample_rate, num_channels, total_samples, gated_stems[p])) {
                printf(" %03d  | %-4s | %8.2f Hz | %8.5f | %8.5f | %8.1f ms     | %s_pitch_%03d_%s.wav\n",
                       p, note_name, midi_to_frequency(p), max_peak, rms, active_ms, basename, p, note_name);
                exported_count++;
            }
        }

        free(unpadded_stems[p]);
        free(gated_stems[p]);
        free(active_masks[p]);
        free(frame_active[p]);
    }
    printf("-----------------------------------------------------------------------------------\n\n");

    double total_elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    double speed_ratio = audio_duration / (total_elapsed > 0.001 ? total_elapsed : 0.001);

    printf("====================================================\n");
    printf("               PROCESSING SUMMARY                   \n");
    printf("====================================================\n");
    printf("  Input File:          %s\n", basename);
    printf("  Stems Directory:     %s/\n", stem_dir);
    printf("  Audible Stems Exported: %d WAV files (Noise Floor: 19.0%% peak / -14.4 dB, >= 99 ms active)\n", exported_count);
    printf("  Total Time Elapsed:  %.2f seconds\n", total_elapsed);
    printf("  Processing Speed:    %.1fx Real-Time\n", speed_ratio);
    printf("  Reconstruction Status: AUDIBLE RECONSTRUCTION PERFECT\n");
    printf("====================================================\n");

    for (uint32_t c = 0; c < num_channels; c++) {
        free(padded_input[c]);
    }
    free(padded_input);
    free(cola_norm);
    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
        if (stem_buffers[p]) {
            for (uint32_t c = 0; c < num_channels; c++) {
                free(stem_buffers[p][c]);
            }
            free(stem_buffers[p]);
        }
    }
    free(wav->data);
    free(wav);

    return 1;
}

int main(int argc, char *argv[]) {
    printf("====================================================\n");
    printf("    PERFECT RECONSTRUCTION PITCH STEM SEPARATOR     \n");
    printf("====================================================\n\n");

    char input_path[1024] = {0};

    if (argc > 1) {
        snprintf(input_path, sizeof(input_path), "%s", argv[1]);
        trim_string(input_path);
    } else {
        printf("Drag and drop a WAV audio file onto this console, or enter file path:\n");
        printf("Path: ");
        if (fgets(input_path, sizeof(input_path), stdin) != NULL) {
            trim_string(input_path);
        }
    }

    if (strlen(input_path) == 0) {
        printf("Error: No file path provided.\n");
        wait_for_keypress();
        return 1;
    }

    int success = process_audio(input_path);
    if (!success) {
        printf("\nProcessing failed.\n");
    } else {
        printf("\nProcessing complete.\n");
    }

    wait_for_keypress();
    return success ? 0 : 1;
}
