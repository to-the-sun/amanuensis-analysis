/*
 * flux_separator.c
 *
 * Perfect Reconstruction Pitch Stem Separator - Spectral Flux Focus
 * Multi-File, Directory Scanning & Multi-Threaded Concurrent Execution
 *
 * Focuses strictly on deriving per-pitch-stem transience flux envelopes,
 * generating modified flux envelopes via peak salience, and exporting the
 * recombined modified flux WAV that the later tools consume.
 *
 * Only [flux]/<basename>_recombined_modified_flux.wav is written by a normal
 * run. The per-stem _modified_flux.wav companions and the [flux] report data
 * are debug artifacts: they are produced only when --debug is passed (see
 * flux_separator_debug.bat), so a normal run spends no time detecting peak and
 * trough markers or writing files nothing downstream reads. The reports stay on
 * demand in either case: flux_report.exe builds them from the stored report
 * data when it exists.
 *
 * Supports single file drag-and-drop, multi-file drag-and-drop, and double-clicking
 * (which automatically scans the folder for all WAV files and processes them simultaneously
 * using CPU multithreading with live per-file console progress bars).
 *
 * Highly optimized memory footprint: each pitch stem's modified flux WAV is written
 * and released as soon as it is finished, so memory footprint stays very low
 * (~300MB per file).
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#define MKDIR(dir) _mkdir(dir)
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(dir) mkdir(dir, 0755)
#endif

#include "flux_shared.h"
#include "flux_envelope.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FFT_SIZE 8192
#define HOP_SIZE (FFT_SIZE / 4)
#define MAX_CHANNELS 2
#define NUM_MIDI_PITCHES 128

/* Noise Floor Ratio: 19.0% of signal peak amplitude (-14.43 dB) */
double g_noise_floor_ratio = 0.19;

/* Silence Threshold: Peak amplitude limit for silent boundary samples (0.009) */
double g_silence_threshold = 0.009;

static pthread_mutex_t g_dir_mutex = PTHREAD_MUTEX_INITIALIZER;

void enable_ansi_terminal(void) {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#endif
}

/*
 * Returns 1 when the directory holds any pitch stem WAV exported for the given
 * source basename. Pitch stems and their modified flux companions are named
 * "<basename>_pitch_<nnn>_<note>.wav" and
 * "<basename>_pitch_<nnn>_<note>_modified_flux.wav", so the "<basename>_pitch_"
 * prefix identifies stems belonging to exactly this source file.
 */
static int dir_contains_pitch_stems_for(const char *dir_path, const char *basename) {
    DIR *d = opendir(dir_path);
    if (!d) return 0;

    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s_pitch_", basename);

    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (starts_with_icase(entry->d_name, prefix) && ends_with_icase(entry->d_name, ".wav")) {
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int pitch_stems_exist_for(const char *stem_dir, const char *basename) {
    return dir_contains_pitch_stems_for(stem_dir, basename);
}

/*
 * The deferred report needs the raw flux envelopes and their markers. When they
 * are missing (a folder from before the reports became on-demand), the file is
 * not considered finished and is separated again so flux_report can rebuild it.
 */
static int flux_report_data_exists(const char *stem_dir, const char *basename) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/[flux]/%s_flux_envelopes.bin", stem_dir, basename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/*
 * A normal (non-debug) run leaves only the recombined modified flux WAV in
 * [flux], so that file is what marks the work as already done.
 */
static int flux_recombined_exists(const char *stem_dir, const char *basename) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/[flux]/%s_recombined_modified_flux.wav", stem_dir, basename);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int is_valid_input_wav(const char *path) {
    if (!path) return 0;
    if (!ends_with_icase(path, ".wav")) return 0;

    if (contains_icase(path, "_flux.wav")) return 0;
    if (contains_icase(path, "_modified_flux.wav")) return 0;
    if (contains_icase(path, "_recombined_modified_flux.wav")) return 0;
    if (contains_icase(path, "_nontonal.wav")) return 0;
    if (contains_icase(path, "_original.wav")) return 0;
    if (contains_icase(path, "_coherence.wav")) return 0;

    if (contains_icase(path, "preview.wav")) return 0;
    if (contains_icase(path, "glued.wav")) return 0;

    if (contains_icase(path, "pitch_stems") || contains_icase(path, "[flux]")) return 0;

    const char *filename = strrchr(path, '/');
    const char *bs = strrchr(path, '\\');
    if (bs > filename) filename = bs;
    if (filename) filename++; else filename = path;
    if (filename[0] == '.') return 0;

    return 1;
}

void scan_directory_for_wavs(const char *dir_path, char ***file_list, int *num_files, int *capacity) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        char full_path[2048];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir->d_name);

        if (is_valid_input_wav(full_path)) {
            if (*num_files >= *capacity) {
                *capacity = (*capacity == 0) ? 16 : (*capacity * 2);
                *file_list = (char**)realloc(*file_list, (*capacity) * sizeof(char*));
            }
            (*file_list)[*num_files] = strdup(full_path);
            (*num_files)++;
        }
    }
    closedir(d);
}

int get_cpu_cores(void) {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    int num = (int)sysinfo.dwNumberOfProcessors;
    return num > 0 ? num : 1;
#else
    long num = sysconf(_SC_NPROCESSORS_ONLN);
    return num > 0 ? (int)num : 1;
#endif
}

void ensure_dir_exists(const char *path) {
    pthread_mutex_lock(&g_dir_mutex);
    MKDIR(path);
    pthread_mutex_unlock(&g_dir_mutex);
}

void print_multithreaded_progress(
    int thread_slot,
    int num_slots,
    const char *basename,
    const char *stage,
    uint32_t current,
    uint32_t total,
    double elapsed_sec
) {
    int is_tty = 1;
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    is_tty = _isatty(_fileno(stdout));
#else
    is_tty = isatty(fileno(stdout));
#endif

    if (!is_tty) {
        if (current == total || (total > 0 && current % (total > 5 ? (total / 5) : 1) == 0)) {
            pthread_mutex_lock(&g_console_mutex);
            printf("[%s] %s %u/%u (%.0f%%) - %.1fs\n",
                   basename, stage, current, total,
                   total > 0 ? ((double)current / total * 100.0) : 100.0,
                   elapsed_sec);
            fflush(stdout);
            pthread_mutex_unlock(&g_console_mutex);
        }
        return;
    }

    pthread_mutex_lock(&g_console_mutex);

    int move_up = num_slots - thread_slot;
    int bar_width = 20;
    float percentage = (total > 0) ? ((float)current / total) : 1.0f;
    int pos = (int)(bar_width * percentage);

    printf("\033[%dA", move_up);
    printf("\033[2K\r[Thread %d | %-16.16s] %-8.8s [", thread_slot + 1, basename, stage);
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) printf("=");
        else if (i == pos) printf(">");
        else printf(" ");
    }
    printf("] %3d%% (%u/%u) - %.1fs", (int)(percentage * 100.0f), current, total, elapsed_sec);
    printf("\033[%dB\r", move_up);
    fflush(stdout);

    pthread_mutex_unlock(&g_console_mutex);
}

/* Pre-calculated FFT lookup tables for the separator's own 8192-point STFT */
static Complex g_twiddle_fwd[FFT_SIZE];
static Complex g_twiddle_inv[FFT_SIZE];
static int g_bit_rev[FFT_SIZE];
static int g_fft_tables_initialized = 0;

void init_fft_tables(void) {
    if (g_fft_tables_initialized) return;

    for (int i = 0; i < FFT_SIZE; i++) {
        double angle_fwd = -2.0 * M_PI * i / FFT_SIZE;
        g_twiddle_fwd[i].r = cos(angle_fwd);
        g_twiddle_fwd[i].i = sin(angle_fwd);

        double angle_inv = 2.0 * M_PI * i / FFT_SIZE;
        g_twiddle_inv[i].r = cos(angle_inv);
        g_twiddle_inv[i].i = sin(angle_inv);
    }

    for (int i = 0, j = 0; i < FFT_SIZE; i++) {
        g_bit_rev[i] = j;
        int bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
    }

    g_fft_tables_initialized = 1;
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

/* Optimized FFT implementation using pre-calculated lookup tables */
void fft(Complex *x, int n, int invert) {
    for (int i = 0; i < n; i++) {
        int j = g_bit_rev[i];
        if (i < j) {
            Complex temp = x[i];
            x[i] = x[j];
            x[j] = temp;
        }
    }

    const Complex *twiddle = invert ? g_twiddle_inv : g_twiddle_fwd;

    for (int len = 2; len <= n; len <<= 1) {
        int half_len = len >> 1;
        int step = FFT_SIZE / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half_len; j++) {
                Complex w = twiddle[j * step];
                Complex u = x[i + j];
                Complex v = {
                    x[i + j + half_len].r * w.r - x[i + j + half_len].i * w.i,
                    x[i + j + half_len].r * w.i + x[i + j + half_len].i * w.r
                };
                x[i + j].r = u.r + v.r;
                x[i + j].i = u.i + v.i;
                x[i + j + half_len].r = u.r - v.r;
                x[i + j + half_len].i = u.i - v.i;
            }
        }
    }

    if (invert) {
        double inv_n = 1.0 / n;
        for (int i = 0; i < n; i++) {
            x[i].r *= inv_n;
            x[i].i *= inv_n;
        }
    }
}

/* WAV File Writer (writes 16-bit PCM) */
int write_wav_16bit(const char *filepath, uint32_t sample_rate, uint16_t num_channels, uint32_t total_frames, const double *data) {
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not open output file '%s' for writing\n", filepath);
        pthread_mutex_unlock(&g_console_mutex);
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

static void copy_helper_files_to_pitch_stems(const char *dir, const char *stem_dir) {
    const char *files_to_copy[] = {
        "analyze_group.bat",
        "analyze.exe",
        "song_builder.exe",
        "flux_report.exe"
    };
    int num_files = sizeof(files_to_copy) / sizeof(files_to_copy[0]);

    for (int i = 0; i < num_files; i++) {
        const char *filename = files_to_copy[i];
        char src_path[4096];
        char dst_path[4096];
        int found = 0;

        if (strlen(dir) == 0 || strcmp(dir, ".") == 0) {
            snprintf(src_path, sizeof(src_path), "%s", filename);
        } else {
            snprintf(src_path, sizeof(src_path), "%s/%s", dir, filename);
        }

        FILE *f = fopen(src_path, "rb");
        if (f) {
            fclose(f);
            found = 1;
        } else if (strlen(dir) != 0 && strcmp(dir, ".") != 0) {
            snprintf(src_path, sizeof(src_path), "%s", filename);
            f = fopen(src_path, "rb");
            if (f) {
                fclose(f);
                found = 1;
            }
        }

        if (found) {
            snprintf(dst_path, sizeof(dst_path), "%s/%s", stem_dir, filename);
            FILE *in = fopen(src_path, "rb");
            FILE *out = fopen(dst_path, "wb");
            if (in && out) {
                char buf[8192];
                size_t bytes;
                while ((bytes = fread(buf, 1, sizeof(buf), in)) > 0) {
                    fwrite(buf, 1, bytes, out);
                }
                fclose(in);
                fclose(out);
                printf("  Copied helper file: %s -> %s\n", src_path, dst_path);
            } else {
                if (in) fclose(in);
                if (out) fclose(out);
            }
        }
    }
}

int process_audio(const char *input_path, const char *stem_dir, FileBatchResult *result, int thread_slot, int num_slots, int debug_mode) {
    clock_t start_clock = clock();

    init_fft_tables();

    char dir[1024], basename[512];
    get_filepath_components(input_path, dir, basename, sizeof(basename));

    if (result) {
        memset(result, 0, sizeof(FileBatchResult));
        snprintf(result->input_path, sizeof(result->input_path), "%s", input_path);
        snprintf(result->basename, sizeof(result->basename), "%s", basename);
    }

    /*
     * A debug run leaves pitch stems and report data behind, while a normal run
     * leaves only the recombined flux WAV, so each mode checks for its own
     * output rather than for files the other mode never writes.
     */
    int already_processed = debug_mode
        ? (pitch_stems_exist_for(stem_dir, basename) && flux_report_data_exists(stem_dir, basename))
        : flux_recombined_exists(stem_dir, basename);

    if (already_processed) {
        if (result) result->skipped = 1;
        pthread_mutex_lock(&g_console_mutex);
        if (debug_mode) {
            printf("  Skipping %s: pitch stems and report data already exist for it.\n", input_path);
        } else {
            printf("  Skipping %s: its recombined flux WAV already exists.\n", input_path);
        }
        fflush(stdout);
        pthread_mutex_unlock(&g_console_mutex);
        print_multithreaded_progress(thread_slot, num_slots, basename, "Skipped", 100, 100, 0.0);
        return 1;
    }

    print_multithreaded_progress(thread_slot, num_slots, basename, "Reading", 0, 100, 0.0);

    WAVFile *wav = read_wav(input_path);
    if (!wav) {
        if (result) result->success = 0;
        print_multithreaded_progress(thread_slot, num_slots, basename, "Failed", 0, 100, 0.0);
        return 0;
    }

    double audio_duration = (double)wav->total_samples / wav->sample_rate;
    uint32_t num_channels = wav->num_channels;
    uint32_t total_samples = wav->total_samples;
    uint32_t sample_rate = wav->sample_rate;

    if (result) {
        result->sample_rate = sample_rate;
        result->num_channels = num_channels;
        result->total_samples = total_samples;
        result->duration = audio_duration;
    }

    // Calculate overall input signal peak
    double overall_peak = 0.0;
    size_t total_channel_samples = (size_t)total_samples * num_channels;
    for (size_t i = 0; i < total_channel_samples; i++) {
        double abs_val = fabs(wav->data[i]);
        if (abs_val > overall_peak) overall_peak = abs_val;
    }
    if (overall_peak < 1e-6) overall_peak = 1e-6;

    double noise_floor_threshold = overall_peak * g_noise_floor_ratio;

    uint32_t pad_samples = FFT_SIZE;
    uint32_t padded_total_samples = total_samples + 2 * pad_samples;

    double **padded_input = (double**)malloc(num_channels * sizeof(double*));
    for (uint32_t c = 0; c < num_channels; c++) {
        padded_input[c] = (double*)calloc(padded_total_samples, sizeof(double));
        for (uint32_t i = 0; i < total_samples; i++) {
            padded_input[c][i + pad_samples] = wav->data[i * num_channels + c];
        }
    }

    free(wav->data);
    wav->data = NULL;

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
    int pitch_bin_count[NUM_MIDI_PITCHES] = {0};
    int (*pitch_bins)[FFT_SIZE / 2 + 1] = (int (*)[FFT_SIZE / 2 + 1])malloc(NUM_MIDI_PITCHES * (FFT_SIZE / 2 + 1) * sizeof(int));
    if (!pitch_bins) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not allocate memory for pitch_bins (%s).\n", basename);
        pthread_mutex_unlock(&g_console_mutex);
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input);
        free(cola_norm);
        free(wav);
        if (result) result->success = 0;
        return 0;
    }

    for (int k = 0; k <= FFT_SIZE / 2; k++) {
        double freq = (double)k * sample_rate / FFT_SIZE;
        int p = freq_to_midi_pitch(freq);
        bin_pitch[k] = p;
        pitch_bins[p][pitch_bin_count[p]++] = k;
    }

    size_t num_bins_per_frame = FFT_SIZE / 2 + 1;
    size_t total_stft_entries = (size_t)num_channels * num_frames * num_bins_per_frame;

    ComplexFloat *stft_data = (ComplexFloat*)malloc(total_stft_entries * sizeof(ComplexFloat));
    if (!stft_data) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not allocate memory for STFT matrix (%s).\n", basename);
        pthread_mutex_unlock(&g_console_mutex);
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input);
        free(cola_norm);
        free(pitch_bins);
        free(wav);
        if (result) result->success = 0;
        return 0;
    }

    size_t frame_pitch_active_size = (size_t)num_channels * num_frames * NUM_MIDI_PITCHES;
    uint8_t *frame_pitch_active = (uint8_t*)calloc(frame_pitch_active_size, sizeof(uint8_t));
    int pitch_active[NUM_MIDI_PITCHES] = {0};

    Complex *fft_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));

    uint32_t total_stft_steps = num_channels * num_frames;
    uint32_t current_step = 0;
    clock_t stft_start_clock = clock();

    for (uint32_t c = 0; c < num_channels; c++) {
        for (uint32_t m = 0; m < num_frames; m++) {
            uint32_t offset = m * HOP_SIZE;

            current_step++;
            if (current_step % 50 == 0 || current_step == total_stft_steps) {
                double elapsed = (double)(clock() - stft_start_clock) / CLOCKS_PER_SEC;
                print_multithreaded_progress(thread_slot, num_slots, basename, "STFT", current_step, total_stft_steps, elapsed);
            }

            for (int i = 0; i < FFT_SIZE; i++) {
                fft_frame[i].r = padded_input[c][offset + i] * window[i];
                fft_frame[i].i = 0.0;
            }

            fft(fft_frame, FFT_SIZE, 0);

            size_t frame_base = ((size_t)c * num_frames + m) * num_bins_per_frame;
            size_t frame_p_base = ((size_t)c * num_frames + m) * NUM_MIDI_PITCHES;

            for (int k = 0; k <= FFT_SIZE / 2; k++) {
                stft_data[frame_base + k].r = (float)fft_frame[k].r;
                stft_data[frame_base + k].i = (float)fft_frame[k].i;

                double mag_sq = fft_frame[k].r * fft_frame[k].r + fft_frame[k].i * fft_frame[k].i;
                if (mag_sq > 1e-18) {
                    int p = bin_pitch[k];
                    pitch_active[p] = 1;
                    frame_pitch_active[frame_p_base + p] = 1;
                }
            }
        }
    }

    for (uint32_t c = 0; c < num_channels; c++) {
        free(padded_input[c]);
    }
    free(padded_input);
    padded_input = NULL;

    ensure_dir_exists(stem_dir);

    char flux_dir[8192];
    snprintf(flux_dir, sizeof(flux_dir), "%s/[flux]", stem_dir);
    ensure_dir_exists(flux_dir);

    Complex *pitch_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));

    /*
     * Report data for flux_report: the raw flux envelope of every stem plus the
     * peak and trough markers detected on it. The report itself (its JS data
     * file and HTML) is built on demand from this, so the multi-gigabyte JS
     * data file is never written during separation. Written to a .part file and
     * renamed into place only once the whole file is done.
     */
    char envelope_path[8192];
    char envelope_part_path[8192];
    snprintf(envelope_path, sizeof(envelope_path), "%s/%s_flux_envelopes.bin", flux_dir, basename);
    snprintf(envelope_part_path, sizeof(envelope_part_path), "%s.part", envelope_path);

    FILE *envelope_file = NULL;
    if (debug_mode) {
        envelope_file = fopen(envelope_part_path, "wb");
    }
    if (envelope_file) {
        uint16_t header_channels = num_channels;
        uint16_t header_reserved = 0;
        uint32_t header_stem_count = 0;

        fwrite("FLUXENV1", 1, 8, envelope_file);
        fwrite(&sample_rate, sizeof(uint32_t), 1, envelope_file);
        fwrite(&total_samples, sizeof(uint32_t), 1, envelope_file);
        fwrite(&header_channels, sizeof(uint16_t), 1, envelope_file);
        fwrite(&header_reserved, sizeof(uint16_t), 1, envelope_file);
        fwrite(&header_stem_count, sizeof(uint32_t), 1, envelope_file);
    }

    int audible_count = 0;

    double *recombined_mod_flux = (double*)calloc(total_channel_samples, sizeof(double));

    clock_t flux_start_clock = clock();

    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
        if (p % 16 == 0 || p == NUM_MIDI_PITCHES - 1) {
            double elapsed = (double)(clock() - flux_start_clock) / CLOCKS_PER_SEC;
            print_multithreaded_progress(thread_slot, num_slots, basename, "Flux", p + 1, NUM_MIDI_PITCHES, elapsed);
        }

        if (!pitch_active[p] || pitch_bin_count[p] == 0) continue;

        double *stem_buf = (double*)calloc((size_t)padded_total_samples * num_channels, sizeof(double));
        if (!stem_buf) continue;

        for (uint32_t c = 0; c < num_channels; c++) {
            for (uint32_t m = 0; m < num_frames; m++) {
                size_t frame_p_idx = ((size_t)c * num_frames + m) * NUM_MIDI_PITCHES + p;
                if (!frame_pitch_active[frame_p_idx]) continue;

                size_t frame_base = ((size_t)c * num_frames + m) * num_bins_per_frame;

                memset(pitch_frame, 0, FFT_SIZE * sizeof(Complex));
                for (int b = 0; b < pitch_bin_count[p]; b++) {
                    int k = pitch_bins[p][b];
                    ComplexFloat val = stft_data[frame_base + k];
                    pitch_frame[k].r = (double)val.r;
                    pitch_frame[k].i = (double)val.i;
                    if (k > 0 && k < FFT_SIZE / 2) {
                        pitch_frame[FFT_SIZE - k].r = (double)val.r;
                        pitch_frame[FFT_SIZE - k].i = -(double)val.i;
                    }
                }

                fft(pitch_frame, FFT_SIZE, 1);

                uint32_t offset = m * HOP_SIZE;
                for (int i = 0; i < FFT_SIZE; i++) {
                    if (offset + i < padded_total_samples) {
                        stem_buf[(offset + i) * num_channels + c] += pitch_frame[i].r * window[i];
                    }
                }
            }
        }

        // Unpad stem audio
        double *unpadded_data = (double*)malloc((size_t)total_samples * num_channels * sizeof(double));
        for (uint32_t i = 0; i < total_samples; i++) {
            uint32_t padded_idx = i + pad_samples;
            double norm = cola_norm[padded_idx];

            for (uint32_t c = 0; c < num_channels; c++) {
                double val = (norm > 1e-12) ? (stem_buf[padded_idx * num_channels + c] / norm) : 0.0;
                unpadded_data[i * num_channels + c] = val;
            }
        }
        free(stem_buf);

        // Evaluate stem energy and active duration against noise floor threshold
        double stem_max_peak = 0.0;
        double stem_sum_sq = 0.0;
        uint32_t active_frames = 0;

        for (uint32_t i = 0; i < total_samples; i++) {
            int frame_above_threshold = 0;
            for (uint32_t c = 0; c < num_channels; c++) {
                double val = unpadded_data[i * num_channels + c];
                double abs_val = fabs(val);
                if (abs_val > stem_max_peak) stem_max_peak = abs_val;
                stem_sum_sq += val * val;

                if (abs_val >= noise_floor_threshold) {
                    frame_above_threshold = 1;
                }
            }

            if (frame_above_threshold) {
                active_frames++;
            }
        }

        double stem_rms = sqrt(stem_sum_sq / (total_samples * num_channels));
        double active_ms = ((double)active_frames / sample_rate) * 1000.0;

        double *raw_flux_buf = NULL;
        if (active_ms >= 99.0 && stem_rms > 1e-6) {
            raw_flux_buf = (double*)malloc(total_channel_samples * sizeof(double));
            if (raw_flux_buf) {
                compute_stem_transience_flux(unpadded_data, total_samples, num_channels, sample_rate, raw_flux_buf, 1);
            }
        }

        // Free reconstructed main pitch stem audio immediately
        free(unpadded_data);
        unpadded_data = NULL;

        if (!raw_flux_buf) continue;

        audible_count++;

        char note_name[32];
        get_note_name(p, note_name, sizeof(note_name));

        /*
         * Report data is a debug artifact. Marker detection walks the whole
         * envelope of every stem and nothing downstream reads the result, so it
         * is skipped outright unless --debug asked for it.
         */
        if (debug_mode) {
            // Detect peak and trough markers on channel 0 of the raw flux envelope
            int *peaks = NULL;
            int *troughs = NULL;
            int num_peaks = 0;
            int num_troughs = 0;

            double *ch0_flux = (double*)malloc((size_t)total_samples * sizeof(double));
            if (ch0_flux) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    ch0_flux[s] = raw_flux_buf[(size_t)s * num_channels];
                }
                detect_all_peaks(ch0_flux, total_samples, &peaks, &num_peaks);
                detect_all_troughs(ch0_flux, total_samples, &troughs, &num_troughs);
                free(ch0_flux);
            }

            if (envelope_file) {
                int32_t pitch_written = (int32_t)p;
                int32_t peaks_written = (int32_t)num_peaks;
                int32_t troughs_written = (int32_t)num_troughs;

                fwrite(&pitch_written, sizeof(int32_t), 1, envelope_file);
                fwrite(&peaks_written, sizeof(int32_t), 1, envelope_file);
                if (num_peaks > 0) fwrite(peaks, sizeof(int32_t), (size_t)num_peaks, envelope_file);
                fwrite(&troughs_written, sizeof(int32_t), 1, envelope_file);
                if (num_troughs > 0) fwrite(troughs, sizeof(int32_t), (size_t)num_troughs, envelope_file);

                int16_t *mono_flux = (int16_t*)malloc((size_t)total_samples * sizeof(int16_t));
                if (mono_flux) {
                    for (uint32_t s = 0; s < total_samples; s++) {
                        double sum_f = 0.0;
                        for (uint32_t c = 0; c < num_channels; c++) {
                            sum_f += raw_flux_buf[(size_t)s * num_channels + c];
                        }
                        double val = sum_f / (double)num_channels;
                        if (val > 1.0) val = 1.0;
                        if (val < 0.0) val = 0.0;
                        mono_flux[s] = (int16_t)round(val * 32767.0);
                    }
                    fwrite(mono_flux, sizeof(int16_t), (size_t)total_samples, envelope_file);
                    free(mono_flux);
                }
            }

            if (peaks) free(peaks);
            if (troughs) free(troughs);
        }

        double *mod_flux_buf = (double*)malloc(total_channel_samples * sizeof(double));
        if (mod_flux_buf) {
            memcpy(mod_flux_buf, raw_flux_buf, total_channel_samples * sizeof(double));

            // Process modified flux envelope for each channel
            for (uint32_t c = 0; c < num_channels; c++) {
                double *channel_flux = (double*)malloc(total_samples * sizeof(double));
                if (channel_flux) {
                    for (uint32_t s = 0; s < total_samples; s++) {
                        channel_flux[s] = mod_flux_buf[s * num_channels + c];
                    }
                    modify_flux_envelope_channel(channel_flux, total_samples);
                    for (uint32_t s = 0; s < total_samples; s++) {
                        mod_flux_buf[s * num_channels + c] = channel_flux[s];
                    }
                    free(channel_flux);
                }
            }

            if (recombined_mod_flux) {
                for (size_t k = 0; k < total_channel_samples; k++) {
                    recombined_mod_flux[k] += mod_flux_buf[k];
                }
            }
        }

        // Export the companion modified flux WAV file immediately (debug only)
        if (debug_mode && mod_flux_buf) {
            char mod_flux_filepath[4096];
            snprintf(mod_flux_filepath, sizeof(mod_flux_filepath),
                     "%s/%s_pitch_%03d_%s_modified_flux.wav",
                     stem_dir, basename, p, note_name);
            write_wav_16bit(mod_flux_filepath, sample_rate, num_channels, total_samples, mod_flux_buf);
        }

        if (mod_flux_buf) {
            free(mod_flux_buf);
            mod_flux_buf = NULL;
        }

        // Free full-precision double arrays immediately to optimize RAM footprint
        free(raw_flux_buf);
        raw_flux_buf = NULL;
    }

    if (recombined_mod_flux) {
        if (audible_count > 0) {
            double max_rec_peak = 0.0;
            for (size_t k = 0; k < total_channel_samples; k++) {
                double abs_val = fabs(recombined_mod_flux[k]);
                if (abs_val > max_rec_peak) {
                    max_rec_peak = abs_val;
                }
            }

            if (max_rec_peak > 1e-12) {
                double scale = 1.0 / max_rec_peak;
                for (size_t k = 0; k < total_channel_samples; k++) {
                    recombined_mod_flux[k] *= scale;
                }
            }

            char recombined_filepath[12288];
            snprintf(recombined_filepath, sizeof(recombined_filepath), "%s/%s_recombined_modified_flux.wav",
                     flux_dir, basename);
            write_wav_16bit(recombined_filepath, sample_rate, num_channels, total_samples, recombined_mod_flux);
        }
        free(recombined_mod_flux);
        recombined_mod_flux = NULL;
    }

    int exported_count = audible_count;

    if (envelope_file) {
        uint32_t stems_written = (uint32_t)audible_count;

        if (audible_count > 0) {
            fseek(envelope_file, 20L, SEEK_SET);
            fwrite(&stems_written, sizeof(uint32_t), 1, envelope_file);
        }
        fclose(envelope_file);

        if (audible_count > 0) {
            remove(envelope_path);
            if (rename(envelope_part_path, envelope_path) != 0) {
                pthread_mutex_lock(&g_console_mutex);
                printf("  Warning: could not finalize the report data for %s.\n", basename);
                pthread_mutex_unlock(&g_console_mutex);
            }
        } else {
            remove(envelope_part_path);
        }
    }

    double total_elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    double speed_ratio = audio_duration / (total_elapsed > 0.001 ? total_elapsed : 0.001);

    if (result) {
        result->stem_count = exported_count;
        result->elapsed_sec = total_elapsed;
        result->speed_ratio = speed_ratio;
        result->success = 1;
    }

    print_multithreaded_progress(thread_slot, num_slots, basename, "Complete", 100, 100, total_elapsed);

    free(fft_frame);
    free(pitch_frame);
    free(stft_data);
    free(frame_pitch_active);
    free(cola_norm);
    free(pitch_bins);
    free(wav);

    return 1;
}

typedef struct {
    char **file_list;
    int num_files;
    char stem_dir[4096];
    FileBatchResult *results;
    int *next_file_idx;
    pthread_mutex_t *queue_mutex;
    int thread_slot;
    int num_slots;
    int debug_mode;
} WorkerArgs;

void* worker_thread_func(void *arg) {
    WorkerArgs *wargs = (WorkerArgs*)arg;
    while (1) {
        int idx = -1;
        pthread_mutex_lock(wargs->queue_mutex);
        if (*wargs->next_file_idx < wargs->num_files) {
            idx = (*wargs->next_file_idx)++;
        }
        pthread_mutex_unlock(wargs->queue_mutex);

        if (idx < 0) break;

        const char *input_path = wargs->file_list[idx];
        process_audio(input_path, wargs->stem_dir, &wargs->results[idx], wargs->thread_slot,
                      wargs->num_slots, wargs->debug_mode);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    enable_ansi_terminal();

    printf("====================================================\n");
    printf("               SPECTRAL FLUX SEPARATOR              \n");
    printf("====================================================\n\n");

    char **file_list = NULL;
    int num_files = 0;
    int capacity = 0;
    int debug_mode = 0;
    int have_path_args = 0;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--debug") == 0) {
                debug_mode = 1;
                continue;
            }
            if (starts_with_icase(argv[i], "--")) {
                printf("Ignoring unknown option: %s\n", argv[i]);
                continue;
            }

            char path[1024];
            snprintf(path, sizeof(path), "%s", argv[i]);
            trim_string(path);
            have_path_args = 1;

            DIR *d = opendir(path);
            if (d) {
                closedir(d);
                scan_directory_for_wavs(path, &file_list, &num_files, &capacity);
            } else if (is_valid_input_wav(path)) {
                if (num_files >= capacity) {
                    capacity = (capacity == 0) ? 16 : capacity * 2;
                    file_list = (char**)realloc(file_list, capacity * sizeof(char*));
                }
                file_list[num_files++] = strdup(path);
            }
        }
    }

    if (!have_path_args) {
        /* Double-clicked, or run with only flags: scan current directory */
        scan_directory_for_wavs(".", &file_list, &num_files, &capacity);

        /* Fallback: If no WAV files found in current directory, prompt interactively */
        if (num_files == 0) {
            char input_path[1024] = {0};
            printf("Drag and drop a WAV audio file onto this console, or enter file path:\n");
            printf("Path: ");
            if (fgets(input_path, sizeof(input_path), stdin) != NULL) {
                trim_string(input_path);
            }

            if (strlen(input_path) > 0) {
                DIR *d = opendir(input_path);
                if (d) {
                    closedir(d);
                    scan_directory_for_wavs(input_path, &file_list, &num_files, &capacity);
                } else if (is_valid_input_wav(input_path)) {
                    if (num_files >= capacity) {
                        capacity = 16;
                        file_list = (char**)realloc(file_list, capacity * sizeof(char*));
                    }
                    file_list[num_files++] = strdup(input_path);
                }
            }
        }
    }

    if (num_files == 0) {
        printf("Error: No valid WAV audio files found or provided.\n");
        wait_for_keypress();
        return 1;
    }

    /* Extract common output directory */
    char dir[1024], basename[512];
    get_filepath_components(file_list[0], dir, basename, sizeof(basename));

    char stem_dir[4096];
    if (strlen(dir) == 0 || strcmp(dir, ".") == 0) {
        snprintf(stem_dir, sizeof(stem_dir), "pitch_stems");
    } else {
        snprintf(stem_dir, sizeof(stem_dir), "%s/pitch_stems", dir);
    }
    ensure_dir_exists(stem_dir);

    char flux_dir[8192];
    snprintf(flux_dir, sizeof(flux_dir), "%s/[flux]", stem_dir);
    ensure_dir_exists(flux_dir);

    copy_helper_files_to_pitch_stems(dir, stem_dir);

    init_fft_tables();
    init_flux_fft_tables();

    int cpu_cores = get_cpu_cores();
    int num_threads = (num_files < cpu_cores) ? num_files : cpu_cores;
    if (num_threads < 1) num_threads = 1;

    printf("Batch Processing Overview:\n");
    printf("  Total Audio Files: %d\n", num_files);
    printf("  CPU Cores:         %d\n", cpu_cores);
    printf("  Worker Threads:    %d\n", num_threads);
    printf("  Output Directory:  %s/\n", stem_dir);
    printf("  Debug Output:      %s\n\n", debug_mode
           ? "enabled (per-stem flux WAVs + report data)"
           : "disabled (recombined flux WAV only)");

    int is_tty = 1;
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    is_tty = _isatty(_fileno(stdout));
#else
    is_tty = isatty(fileno(stdout));
#endif

    if (is_tty) {
        for (int t = 0; t < num_threads; t++) {
            printf("[Thread %d] Initializing worker thread...\n", t + 1);
        }
        fflush(stdout);
    }

    FileBatchResult *results = (FileBatchResult*)calloc(num_files, sizeof(FileBatchResult));
    int next_file_idx = 0;
    pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

    clock_t batch_start_clock = clock();

    if (num_threads > 1) {
        pthread_t *threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
        WorkerArgs *wargs = (WorkerArgs*)malloc(num_threads * sizeof(WorkerArgs));

        for (int t = 0; t < num_threads; t++) {
            wargs[t].file_list = file_list;
            wargs[t].num_files = num_files;
            snprintf(wargs[t].stem_dir, sizeof(wargs[t].stem_dir), "%s", stem_dir);
            wargs[t].results = results;
            wargs[t].next_file_idx = &next_file_idx;
            wargs[t].queue_mutex = &queue_mutex;
            wargs[t].thread_slot = t;
            wargs[t].num_slots = num_threads;
            wargs[t].debug_mode = debug_mode;

            pthread_create(&threads[t], NULL, worker_thread_func, &wargs[t]);
        }

        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
        free(threads);
        free(wargs);
    } else {
        process_audio(file_list[0], stem_dir, &results[0], 0, 1, debug_mode);
    }

    if (is_tty) {
        printf("\n\n");
    }

    double total_wall_clock_sec = (double)(clock() - batch_start_clock) / CLOCKS_PER_SEC;

    /* Reports are built on demand: flux_report.exe reads the WAVs written above. */
    int total_success = 0;
    int total_skipped = 0;
    for (int i = 0; i < num_files; i++) {
        if (results[i].success) total_success++;
        if (results[i].skipped) total_skipped++;
    }

    printf("====================================================\n");
    printf("             BATCH PROCESSING COMPLETE              \n");
    printf("====================================================\n");
    printf("  Successfully Processed: %d / %d WAV files\n", total_success, num_files);
    if (total_skipped > 0) {
        printf("  Skipped (Stems Exist):  %d / %d WAV files\n", total_skipped, num_files);
    }
    printf("  Total Wall-Clock Time:  %.2f seconds\n", total_wall_clock_sec);
    printf("  Output Stem Folder:     %s/\n", stem_dir);

    if (debug_mode) {
        char report_tool_path[4096];
        snprintf(report_tool_path, sizeof(report_tool_path), "%s/flux_report.exe", stem_dir);
        FILE *report_tool = fopen(report_tool_path, "rb");
        if (report_tool) {
            fclose(report_tool);
            printf("  Reports On Demand:      double-click %s\n", report_tool_path);
        } else {
            printf("  Reports On Demand:      copy flux_report.exe into %s, then double-click it\n", stem_dir);
        }
    } else {
        printf("  Debug Output:           disabled (double-click flux_separator_debug.bat for it)\n");
    }
    printf("====================================================\n");

    for (int i = 0; i < num_files; i++) {
        free(file_list[i]);
    }
    free(file_list);
    free(results);

    wait_for_keypress();
    return (total_success + total_skipped == num_files) ? 0 : 1;
}
