/*
 * flux_pitch_separator.c
 *
 * Perfect Reconstruction Pitch Stem Separator - Spectral Flux Focus
 * Multi-File, Directory Scanning & Multi-Threaded Concurrent Execution
 *
 * Focuses strictly on deriving per-pitch-stem transience flux envelopes,
 * identifying peak and trough markers, generating modified flux envelopes via peak salience,
 * exporting companion _flux.wav and _modified_flux.wav files to [flux]/,
 * and producing interactive HTML reports in [flux]/<basename>_index.html as well as
 * a master batch dashboard in [flux]/index.html.
 *
 * Supports single file drag-and-drop, multi-file drag-and-drop, and double-clicking
 * (which automatically scans the folder for all WAV files and processes them simultaneously
 * using CPU multithreading with live per-file console progress bars).
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

static pthread_mutex_t g_console_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_dir_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    double r;
    double i;
} Complex;

typedef struct {
    float r;
    float i;
} ComplexFloat;

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
} FileBatchResult;

const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

double midi_to_frequency(int midi_pitch);
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

char* encode_base64_bytes(const uint8_t *data, size_t input_len) {
    size_t output_len = 4 * ((input_len + 2) / 3);
    char *encoded = (char*)malloc(output_len + 1);
    if (!encoded) return NULL;

    size_t i = 0, j = 0;
    for (i = 0; i < input_len; ) {
        uint32_t octet_a = i < input_len ? data[i++] : 0;
        uint32_t octet_b = i < input_len ? data[i++] : 0;
        uint32_t octet_c = i < input_len ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded[j++] = b64_chars[(triple >> 18) & 0x3F];
        encoded[j++] = b64_chars[(triple >> 12) & 0x3F];

        if (i == input_len && (input_len % 3 == 1)) {
            encoded[j++] = '=';
            encoded[j++] = '=';
        } else if (i == input_len && (input_len % 3 == 2)) {
            encoded[j++] = b64_chars[(triple >> 6) & 0x3F];
            encoded[j++] = '=';
        } else {
            encoded[j++] = b64_chars[(triple >> 6) & 0x3F];
            encoded[j++] = b64_chars[triple & 0x3F];
        }
    }
    encoded[j] = '\0';
    return encoded;
}

#define FLUX_FFT_SIZE 2048
#define FLUX_HOP_SIZE 512

/* Pre-calculated FFT lookup tables */
static Complex g_twiddle_fwd[FFT_SIZE];
static Complex g_twiddle_inv[FFT_SIZE];
static int g_bit_rev[FFT_SIZE];
static int g_fft_tables_initialized = 0;

static Complex g_flux_twiddle_fwd[FLUX_FFT_SIZE];
static int g_flux_bit_rev[FLUX_FFT_SIZE];
static int g_flux_tables_initialized = 0;

void init_flux_fft_tables(void) {
    if (g_flux_tables_initialized) return;

    for (int i = 0; i < FLUX_FFT_SIZE; i++) {
        double angle_fwd = -2.0 * M_PI * i / FLUX_FFT_SIZE;
        g_flux_twiddle_fwd[i].r = cos(angle_fwd);
        g_flux_twiddle_fwd[i].i = sin(angle_fwd);
    }

    for (int i = 0, j = 0; i < FLUX_FFT_SIZE; i++) {
        g_flux_bit_rev[i] = j;
        int bit = FLUX_FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
    }

    g_flux_tables_initialized = 1;
}

void fft_2048(Complex *x) {
    init_flux_fft_tables();

    for (int i = 0; i < FLUX_FFT_SIZE; i++) {
        int j = g_flux_bit_rev[i];
        if (i < j) {
            Complex temp = x[i];
            x[i] = x[j];
            x[j] = temp;
        }
    }

    for (int len = 2; len <= FLUX_FFT_SIZE; len <<= 1) {
        int half_len = len >> 1;
        int step = FLUX_FFT_SIZE / len;
        for (int i = 0; i < FLUX_FFT_SIZE; i += len) {
            for (int j = 0; j < half_len; j++) {
                Complex w = g_flux_twiddle_fwd[j * step];
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
}

typedef struct {
    int pitch;
    char note_name[32];
    double max_flux_peak;
    double rms_flux;
    double *raw_flux;
    double *mod_flux;
    int *peaks;
    int num_peaks;
    int *troughs;
    int num_troughs;
    char mod_filename[1024];
} FluxStem;

static int ends_with_icase(const char *str, const char *suffix) {
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

static int contains_icase(const char *haystack, const char *needle) {
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

int is_valid_input_wav(const char *path) {
    if (!path) return 0;
    if (!ends_with_icase(path, ".wav")) return 0;

    if (contains_icase(path, "_flux.wav")) return 0;
    if (contains_icase(path, "_modified_flux.wav")) return 0;
    if (contains_icase(path, "_nontonal.wav")) return 0;
    if (contains_icase(path, "_original.wav")) return 0;
    if (contains_icase(path, "_coherence.wav")) return 0;

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

static int find_prev_peak(const double *env, uint32_t n, int start_x) {
    if (start_x >= (int)n - 1) {
        start_x = (int)n - 2;
    }
    for (int i = start_x; i >= 1; i--) {
        if (env[i] > env[i + 1]) {
            int L = i;
            while (L > 0 && env[L - 1] == env[i]) {
                L--;
            }
            if (L > 0 && env[L - 1] < env[i]) {
                int R = i;
                return (L + R) / 2;
            }
            i = L;
        }
    }
    return -1;
}

static int find_trough_left(const double *env, uint32_t n, int x) {
    for (int i = x - 1; i >= 1; i--) {
        if (env[i] < env[i + 1]) {
            int L = i;
            while (L > 0 && env[L - 1] == env[i]) {
                L--;
            }
            if (L > 0 && env[L - 1] > env[i]) {
                int R = i;
                return R;
            }
            i = L;
        }
    }
    return 0;
}

static int find_trough_right(const double *env, uint32_t n, int x) {
    for (int i = x + 1; i < (int)n - 1; i++) {
        if (env[i] < env[i - 1]) {
            int R = i;
            while (R < (int)n - 1 && env[R + 1] == env[i]) {
                R++;
            }
            if (R < (int)n - 1 && env[R + 1] > env[i]) {
                int L = i;
                return L;
            }
            i = R;
        }
    }
    return (int)n - 1;
}

static void detect_all_peaks(const double *env, uint32_t n, int **out_peaks, int *out_count);

void apply_flux_noise_gate(double *env, uint32_t total_samples, double threshold) {
    for (uint32_t i = 0; i < total_samples; i++) {
        if (env[i] < threshold) {
            env[i] = 0.0;
        }
    }
}

void modify_flux_envelope_channel(double *env, uint32_t total_samples) {
    if (total_samples < 3) return;

    int *peaks = NULL;
    int num_peaks = 0;
    detect_all_peaks(env, total_samples, &peaks, &num_peaks);

    if (num_peaks == 0 || !peaks) {
        if (peaks) free(peaks);
        return;
    }

    int *salience = (int*)malloc(num_peaks * sizeof(int));
    if (!salience) {
        free(peaks);
        return;
    }

    for (int i = 0; i < num_peaks; i++) {
        double h = env[peaks[i]];

        int left_count = 0;
        for (int j = i - 1; j >= 0; j--) {
            left_count++;
            if (env[peaks[j]] >= h) break;
        }

        int right_count = 0;
        for (int j = i + 1; j < num_peaks; j++) {
            right_count++;
            if (env[peaks[j]] >= h) break;
        }

        int s_score = (left_count < right_count) ? left_count : right_count;
        salience[i] = s_score;
    }

    /* Build thinned ordered list of unique salience integers */
    int *unique_salience = (int*)malloc(num_peaks * sizeof(int));
    int num_unique = 0;
    if (unique_salience) {
        int *sorted_salience = (int*)malloc(num_peaks * sizeof(int));
        if (sorted_salience) {
            memcpy(sorted_salience, salience, num_peaks * sizeof(int));
            /* Simple bubble sort or qsort for unique salience values */
            for (int a = 0; a < num_peaks - 1; a++) {
                for (int b = a + 1; b < num_peaks; b++) {
                    if (sorted_salience[a] > sorted_salience[b]) {
                        int tmp = sorted_salience[a];
                        sorted_salience[a] = sorted_salience[b];
                        sorted_salience[b] = tmp;
                    }
                }
            }
            unique_salience[0] = sorted_salience[0];
            num_unique = 1;
            for (int k = 1; k < num_peaks; k++) {
                if (sorted_salience[k] != sorted_salience[k - 1]) {
                    unique_salience[num_unique++] = sorted_salience[k];
                }
            }
            free(sorted_salience);
        }
    }

    double *norm_salience = (double*)malloc(num_peaks * sizeof(double));
    if (!norm_salience) {
        if (unique_salience) free(unique_salience);
        free(salience);
        free(peaks);
        return;
    }

    for (int i = 0; i < num_peaks; i++) {
        if (num_unique > 0 && unique_salience) {
            int rank = 1;
            for (int u = 0; u < num_unique; u++) {
                if (unique_salience[u] == salience[i]) {
                    rank = u + 1;
                    break;
                }
            }
            norm_salience[i] = (double)rank / (double)num_unique;
        } else {
            norm_salience[i] = 1.0;
        }
    }
    if (unique_salience) free(unique_salience);

    double *orig_env = (double*)malloc(total_samples * sizeof(double));
    if (orig_env) {
        memcpy(orig_env, env, total_samples * sizeof(double));

        for (int i = 0; i < num_peaks; i++) {
            int px = peaks[i];
            int t_L = find_trough_left(orig_env, total_samples, px);
            int t_R = find_trough_right(orig_env, total_samples, px);

            for (int x = t_L; x <= t_R; x++) {
                env[x] = orig_env[x] * norm_salience[i];
            }
        }
        free(orig_env);
    }

    free(norm_salience);
    free(salience);
    free(peaks);
}

void compute_stem_transience_flux(const double *stem_data, uint32_t total_samples, uint32_t num_channels, uint32_t sample_rate, double *flux_out, int normalize) {
    if (!stem_data || !flux_out || total_samples == 0 || num_channels == 0) return;

    memset(flux_out, 0, (size_t)total_samples * num_channels * sizeof(double));

    double window[FLUX_FFT_SIZE];
    for (int i = 0; i < FLUX_FFT_SIZE; i++) {
        window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / FLUX_FFT_SIZE));
    }

    uint32_t num_frames = (total_samples >= FLUX_FFT_SIZE) ? ((total_samples - FLUX_FFT_SIZE) / FLUX_HOP_SIZE + 1) : 0;
    if (num_frames == 0) {
        return;
    }

    double *frame_flux = (double*)malloc(num_frames * sizeof(double));
    double *prev_mag = (double*)calloc(FLUX_FFT_SIZE / 2 + 1, sizeof(double));
    double *curr_mag = (double*)malloc((FLUX_FFT_SIZE / 2 + 1) * sizeof(double));
    Complex *fft_buf = (Complex*)malloc(FLUX_FFT_SIZE * sizeof(Complex));

    if (!frame_flux || !prev_mag || !curr_mag || !fft_buf) {
        if (frame_flux) free(frame_flux);
        if (prev_mag) free(prev_mag);
        if (curr_mag) free(curr_mag);
        if (fft_buf) free(fft_buf);
        return;
    }

    for (uint32_t c = 0; c < num_channels; c++) {
        memset(prev_mag, 0, (FLUX_FFT_SIZE / 2 + 1) * sizeof(double));

        for (uint32_t m = 0; m < num_frames; m++) {
            uint32_t offset = m * FLUX_HOP_SIZE;

            for (int i = 0; i < FLUX_FFT_SIZE; i++) {
                fft_buf[i].r = stem_data[(offset + i) * num_channels + c] * window[i];
                fft_buf[i].i = 0.0;
            }

            fft_2048(fft_buf);

            double flux_sum = 0.0;
            for (int k = 0; k <= FLUX_FFT_SIZE / 2; k++) {
                double r = fft_buf[k].r;
                double im = fft_buf[k].i;
                double energy = r * r + im * im;
                curr_mag[k] = energy;

                double diff = energy - prev_mag[k];
                if (diff > 0.0) {
                    flux_sum += diff;
                }
            }

            frame_flux[m] = flux_sum;
            memcpy(prev_mag, curr_mag, (FLUX_FFT_SIZE / 2 + 1) * sizeof(double));
        }

        for (uint32_t i = 0; i < total_samples; i++) {
            double f_idx = ((double)i - (double)(FLUX_FFT_SIZE / 2)) / (double)FLUX_HOP_SIZE;

            double val = 0.0;
            if (f_idx <= 0.0) {
                val = frame_flux[0];
            } else if (f_idx >= (double)(num_frames - 1)) {
                val = frame_flux[num_frames - 1];
            } else {
                int m0 = (int)floor(f_idx);
                int m1 = m0 + 1;
                double alpha = f_idx - m0;
                val = (1.0 - alpha) * frame_flux[m0] + alpha * frame_flux[m1];
            }

            flux_out[i * num_channels + c] = val;
        }
    }

    free(frame_flux);
    free(prev_mag);
    free(curr_mag);
    free(fft_buf);

    if (normalize) {
        double peak_flux = 0.0;
        size_t total_samples_all_channels = (size_t)total_samples * num_channels;
        for (size_t k = 0; k < total_samples_all_channels; k++) {
            if (flux_out[k] > peak_flux) {
                peak_flux = flux_out[k];
            }
        }

        if (peak_flux > 1e-12) {
            double scale = 1.0 / peak_flux;
            for (size_t k = 0; k < total_samples_all_channels; k++) {
                flux_out[k] *= scale;
            }
        }
    }
}

static void detect_all_peaks(const double *env, uint32_t n, int **out_peaks, int *out_count) {
    int capacity = 256;
    int count = 0;
    int *arr = (int*)malloc(capacity * sizeof(int));
    if (!arr) {
        *out_peaks = NULL;
        *out_count = 0;
        return;
    }
    int curr_x = (int)n - 1;
    int p = find_prev_peak(env, n, curr_x);
    while (p >= 0) {
        if (count >= capacity) {
            capacity *= 2;
            int *new_arr = (int*)realloc(arr, capacity * sizeof(int));
            if (!new_arr) {
                free(arr);
                *out_peaks = NULL;
                *out_count = 0;
                return;
            }
            arr = new_arr;
        }
        arr[count++] = p;
        curr_x = p - 1;
        p = find_prev_peak(env, n, curr_x);
    }
    for (int i = 0; i < count / 2; i++) {
        int tmp = arr[i];
        arr[i] = arr[count - 1 - i];
        arr[count - 1 - i] = tmp;
    }
    *out_peaks = arr;
    *out_count = count;
}

static void detect_all_troughs(const double *env, uint32_t n, int **out_troughs, int *out_count) {
    int capacity = 256;
    int count = 0;
    int *arr = (int*)malloc(capacity * sizeof(int));
    if (!arr) {
        *out_troughs = NULL;
        *out_count = 0;
        return;
    }
    for (int i = 1; i < (int)n - 1; i++) {
        if (env[i] < env[i - 1]) {
            int R = i;
            while (R < (int)n - 1 && env[R + 1] == env[i]) {
                R++;
            }
            if (R < (int)n - 1 && env[R + 1] > env[i]) {
                int L = i;
                int trough_idx = L;
                if (count >= capacity) {
                    capacity *= 2;
                    arr = (int*)realloc(arr, capacity * sizeof(int));
                }
                arr[count++] = trough_idx;
            }
            i = R;
        }
    }
    *out_troughs = arr;
    *out_count = count;
}

void generate_flux_html_report(
    const char *flux_dir,
    const char *basename,
    uint32_t sample_rate,
    uint32_t num_channels,
    uint32_t total_samples,
    double duration,
    const double *orig_audio_buffer,
    const FluxStem *stems,
    int stem_count
) {
    char html_path[4096];
    snprintf(html_path, sizeof(html_path), "%s/%s_index.html", flux_dir, basename);

    FILE *f = fopen(html_path, "w");
    if (!f) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not open Flux HTML report file '%s' for writing.\n", html_path);
        pthread_mutex_unlock(&g_console_mutex);
        return;
    }

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "<meta charset=\"UTF-8\">\n");
    fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "<title>Spectral Flux Transience Report - %s</title>\n", basename);
    fprintf(f, "<style>\n");
    fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; }\n");
    fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
    fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 20px; line-height: 1.5; }\n");
    fprintf(f, "  header { background: var(--card-bg); padding: 20px; border-radius: 10px; border: 1px solid var(--border); margin-bottom: 20px; }\n");
    fprintf(f, "  h1 { font-size: 1.6rem; color: #fff; margin-bottom: 6px; }\n");
    fprintf(f, "  .meta-p { font-size: 0.9rem; color: var(--muted); display: flex; gap: 20px; flex-wrap: wrap; margin-bottom: 14px; }\n");
    fprintf(f, "  .toolbar { display: flex; align-items: center; gap: 15px; flex-wrap: wrap; padding-top: 12px; border-top: 1px solid var(--border); }\n");
    fprintf(f, "  .btn { background: var(--accent); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; cursor: pointer; transition: opacity 0.2s; text-decoration: none; display: inline-block; }\n");
    fprintf(f, "  .btn:hover { opacity: 0.9; }\n");
    fprintf(f, "  .toggle-label { font-size: 0.9rem; color: var(--text); display: flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; }\n");
    fprintf(f, "  .view-info { font-size: 0.85rem; color: #4caf50; font-family: monospace; font-weight: bold; }\n");
    fprintf(f, "  .legend { display: flex; gap: 15px; font-size: 0.8rem; margin-left: auto; }\n");
    fprintf(f, "  .legend-item { display: flex; align-items: center; gap: 6px; }\n");
    fprintf(f, "  .legend-box { width: 14px; height: 14px; border-radius: 3px; }\n");
    fprintf(f, "  .stem-card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin-bottom: 16px; }\n");
    fprintf(f, "  .stem-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; margin-bottom: 10px; }\n");
    fprintf(f, "  .stem-title { font-size: 1.1rem; font-weight: bold; color: #fff; display: flex; align-items: center; gap: 10px; }\n");
    fprintf(f, "  .pitch-tag { background: #333344; color: #82aaff; padding: 2px 8px; border-radius: 4px; font-size: 0.85rem; font-family: monospace; }\n");
    fprintf(f, "  .stem-metrics { font-size: 0.85rem; color: var(--muted); display: flex; gap: 15px; font-family: monospace; }\n");
    fprintf(f, "  audio { height: 32px; outline: none; border-radius: 4px; }\n");
    fprintf(f, "  .canvas-container { position: relative; width: 100%%; height: 140px; background: #0d0d11; border-radius: 6px; border: 1px solid #22222e; overflow: hidden; margin-top: 8px; user-select: none; }\n");
    fprintf(f, "  canvas { width: 100%%; height: 100%%; display: block; cursor: crosshair; }\n");
    fprintf(f, "  .tooltip { position: absolute; top: 8px; right: 12px; background: rgba(10, 10, 15, 0.85); border: 1px solid #333344; padding: 4px 8px; border-radius: 4px; font-size: 0.75rem; font-family: monospace; color: #fff; pointer-events: none; opacity: 0; transition: opacity 0.15s; }\n");
    fprintf(f, "  .instructions { font-size: 0.8rem; color: var(--muted); margin-top: 6px; font-style: italic; }\n");
    fprintf(f, "</style>\n</head>\n<body>\n");

    fprintf(f, "<header>\n");
    fprintf(f, "  <h1>Spectral Flux Transience Report</h1>\n");
    fprintf(f, "  <div class=\"meta-p\">\n");
    fprintf(f, "    <span><strong>Source File:</strong> %s</span>\n", basename);
    fprintf(f, "    <span><strong>Duration:</strong> %.2fs</span>\n", duration);
    fprintf(f, "    <span><strong>Sample Rate:</strong> %u Hz</span>\n", sample_rate);
    fprintf(f, "    <span><strong>Channels:</strong> %u (%s)</span>\n", num_channels, num_channels == 1 ? "Mono" : "Stereo");
    fprintf(f, "    <span><strong>Stems:</strong> %d</span>\n", stem_count);
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"toolbar\">\n");
    fprintf(f, "    <a class=\"btn\" href=\"index.html\">&larr; Batch Dashboard</a>\n");
    fprintf(f, "    <button class=\"btn\" id=\"resetZoomBtn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <label class=\"toggle-label\"><input type=\"checkbox\" id=\"syncZoomCheck\" checked> Sync Zoom Across Rows</label>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", duration, duration);
    fprintf(f, "    <div class=\"legend\">\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: rgba(180, 190, 210, 0.45);\"></div> Original Audio Waveform</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #ff5722;\"></div> Raw Spectral Flux Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #4caf50;\"></div> Modified Flux Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #ffeb3b;\"></div> Detected Peaks (Top Marks)</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #00bcd4;\"></div> Detected Troughs (Bottom Marks)</div>\n");
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"instructions\">Click and drag horizontally on any graph to zoom in. Right-click on a graph to zoom out one level. Click 'Reset Zoom' to restore full view.</div>\n");
    fprintf(f, "</header>\n\n");

    fprintf(f, "<div id=\"stemsContainer\">\n");
    for (int i = 0; i < stem_count; i++) {
        const FluxStem *st = &stems[i];
        fprintf(f, "  <div class=\"stem-card\">\n");
        fprintf(f, "    <div class=\"stem-header\">\n");
        fprintf(f, "      <div class=\"stem-title\">\n");
        fprintf(f, "        <span class=\"pitch-tag\">MIDI %03d</span> %s (%.2f Hz)\n", st->pitch, st->note_name, midi_to_frequency(st->pitch));
        fprintf(f, "      </div>\n");
        fprintf(f, "      <div class=\"stem-metrics\">\n");
        fprintf(f, "        <span>Peaks: %d</span>\n", st->num_peaks);
        fprintf(f, "        <span>Troughs: %d</span>\n", st->num_troughs);
        fprintf(f, "      </div>\n");
        fprintf(f, "      <audio controls src=\"../%s\" preload=\"metadata\"></audio>\n", st->mod_filename);
        fprintf(f, "    </div>\n");
        fprintf(f, "    <div class=\"canvas-container\">\n");
        fprintf(f, "      <canvas id=\"canvas-%d\" data-index=\"%d\"></canvas>\n", i, i);
        fprintf(f, "      <div class=\"tooltip\" id=\"tooltip-%d\"></div>\n", i);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
    }

    fprintf(f, "</div>\n\n");

    char js_data_filename[1024];
    snprintf(js_data_filename, sizeof(js_data_filename), "%s_flux_waveform_data.js", basename);
    char js_data_path[4096];
    snprintf(js_data_path, sizeof(js_data_path), "%s/%s", flux_dir, js_data_filename);

    FILE *fjs = fopen(js_data_path, "w");
    if (fjs) {
        fprintf(fjs, "window.WAVEFORM_DATA = {\n");

        int16_t *pcm_i16 = (int16_t*)malloc(total_samples * sizeof(int16_t));

        if (orig_audio_buffer && pcm_i16) {
            for (uint32_t s = 0; s < total_samples; s++) {
                double sum_f = 0.0;
                for (uint32_t c = 0; c < num_channels; c++) {
                    sum_f += orig_audio_buffer[s * num_channels + c];
                }
                double val = sum_f / num_channels;
                if (val > 1.0) val = 1.0;
                if (val < -1.0) val = -1.0;
                pcm_i16[s] = (int16_t)round(val * 32767.0);
            }
            char *b64 = encode_base64_bytes((const uint8_t*)pcm_i16, total_samples * sizeof(int16_t));
            fprintf(fjs, "  origPcm: \"%s\",\n", b64 ? b64 : "");
            if (b64) free(b64);
        } else {
            fprintf(fjs, "  origPcm: \"\",\n");
        }

        fprintf(fjs, "  rawFluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            if (pcm_i16 && st->raw_flux) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    double sum_f = 0.0;
                    for (uint32_t c = 0; c < num_channels; c++) {
                        sum_f += st->raw_flux[s * num_channels + c];
                    }
                    double val = sum_f / num_channels;
                    if (val > 1.0) val = 1.0;
                    if (val < 0.0) val = 0.0;
                    pcm_i16[s] = (int16_t)round(val * 32767.0);
                }
                char *b64 = encode_base64_bytes((const uint8_t*)pcm_i16, total_samples * sizeof(int16_t));
                fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (i + 1 < stem_count) ? "," : "");
                if (b64) free(b64);
            } else {
                fprintf(fjs, "    \"\"%s\n", (i + 1 < stem_count) ? "," : "");
            }
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  modFluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            if (pcm_i16 && st->mod_flux) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    double sum_f = 0.0;
                    for (uint32_t c = 0; c < num_channels; c++) {
                        sum_f += st->mod_flux[s * num_channels + c];
                    }
                    double val = sum_f / num_channels;
                    if (val > 1.0) val = 1.0;
                    if (val < 0.0) val = 0.0;
                    pcm_i16[s] = (int16_t)round(val * 32767.0);
                }
                char *b64 = encode_base64_bytes((const uint8_t*)pcm_i16, total_samples * sizeof(int16_t));
                fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (i + 1 < stem_count) ? "," : "");
                if (b64) free(b64);
            } else {
                fprintf(fjs, "    \"\"%s\n", (i + 1 < stem_count) ? "," : "");
            }
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  peaks: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            fprintf(fjs, "    [");
            for (int k = 0; k < st->num_peaks; k++) {
                fprintf(fjs, "%d%s", st->peaks[k], (k + 1 < st->num_peaks) ? "," : "");
            }
            fprintf(fjs, "]%s\n", (i + 1 < stem_count) ? "," : "");
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  troughs: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            fprintf(fjs, "    [");
            for (int k = 0; k < st->num_troughs; k++) {
                fprintf(fjs, "%d%s", st->troughs[k], (k + 1 < st->num_troughs) ? "," : "");
            }
            fprintf(fjs, "]%s\n", (i + 1 < stem_count) ? "," : "");
        }
        fprintf(fjs, "  ]\n");

        if (pcm_i16) free(pcm_i16);
        fprintf(fjs, "};\n");
        fclose(fjs);
    }

    fprintf(f, "<script src=\"%s\"></script>\n", js_data_filename);
    fprintf(f, "<script>\n");
    fprintf(f, "const DATA = {\n");
    fprintf(f, "  sampleRate: %u,\n", sample_rate);
    fprintf(f, "  numChannels: %u,\n", num_channels);
    fprintf(f, "  totalSamples: %u,\n", total_samples);
    fprintf(f, "  duration: %.6f,\n", duration);
    fprintf(f, "  stems: [\n");

    for (int i = 0; i < stem_count; i++) {
        const FluxStem *st = &stems[i];
        fprintf(f, "    {\n");
        fprintf(f, "      pitch: %d,\n", st->pitch);
        fprintf(f, "      name: \"%s\",\n", st->note_name);
        fprintf(f, "      file: \"../%s\"\n", st->mod_filename);
        fprintf(f, "    }%s\n", (i + 1 < stem_count) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "};\n\n");

    fprintf(f, "function base64ToFloat32(b64) {\n");
    fprintf(f, "  if (!b64) return new Float32Array(DATA.totalSamples);\n");
    fprintf(f, "  const binaryString = atob(b64);\n");
    fprintf(f, "  const len = binaryString.length;\n");
    fprintf(f, "  const bytes = new Uint8Array(len);\n");
    fprintf(f, "  for (let i = 0; i < len; i++) bytes[i] = binaryString.charCodeAt(i);\n");
    fprintf(f, "  const i16 = new Int16Array(bytes.buffer, 0, Math.floor(len / 2));\n");
    fprintf(f, "  const f32 = new Float32Array(i16.length);\n");
    fprintf(f, "  for (let i = 0; i < i16.length; i++) f32[i] = i16[i] / 32767.0;\n");
    fprintf(f, "  return f32;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSSPrecise(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sStr = s < 10 ? '0' + s.toFixed(2) : s.toFixed(2);\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSS(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sInt = Math.floor(s);\n");
    fprintf(f, "  const sStr = sInt < 10 ? '0' + sInt : '' + sInt;\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function initAudioData() {\n");
    fprintf(f, "  if (window.WAVEFORM_DATA) {\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.origPcm) {\n");
    fprintf(f, "      DATA.origPcm = base64ToFloat32(window.WAVEFORM_DATA.origPcm);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.rawFluxPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.rawFluxPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].rawFluxPcm = base64ToFloat32(window.WAVEFORM_DATA.rawFluxPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.modFluxPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.modFluxPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].modFluxPcm = base64ToFloat32(window.WAVEFORM_DATA.modFluxPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.peaks) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        DATA.stems[i].peaks = window.WAVEFORM_DATA.peaks[i] || [];\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.troughs) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        DATA.stems[i].troughs = window.WAVEFORM_DATA.troughs[i] || [];\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "}\n\n");

    fprintf(f, "let viewStart = 0.0;\n");
    fprintf(f, "let viewEnd = DATA.duration;\n");
    fprintf(f, "let zoomHistory = [];\n");
    fprintf(f, "let activeDrag = null;\n");
    fprintf(f, "let hoverState = null;\n");

    fprintf(f, "function timeToX(t, width) {\n");
    fprintf(f, "  return ((t - viewStart) / (viewEnd - viewStart)) * width;\n");
    fprintf(f, "}\n");
    fprintf(f, "function xToTime(x, width) {\n");
    fprintf(f, "  return viewStart + (x / width) * (viewEnd - viewStart);\n");
    fprintf(f, "}\n");

    fprintf(f, "function updateViewInfo() {\n");
    fprintf(f, "  const dur = viewEnd - viewStart;\n");
    fprintf(f, "  document.getElementById('viewInfo').innerHTML = `Visible: ${formatMSSPrecise(viewStart)} &ndash; ${formatMSSPrecise(viewEnd)} (Duration: ${formatMSSPrecise(dur)})`;\n");
    fprintf(f, "}\n");

    fprintf(f, "function resetZoom() {\n");
    fprintf(f, "  zoomHistory = [];\n");
    fprintf(f, "  viewStart = 0.0;\n");
    fprintf(f, "  viewEnd = DATA.duration;\n");
    fprintf(f, "  updateViewInfo();\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "}\n");

    fprintf(f, "function zoomOut() {\n");
    fprintf(f, "  if (zoomHistory.length > 0) {\n");
    fprintf(f, "    const prev = zoomHistory.pop();\n");
    fprintf(f, "    viewStart = prev.start;\n");
    fprintf(f, "    viewEnd = prev.end;\n");
    fprintf(f, "    updateViewInfo();\n");
    fprintf(f, "    renderAll();\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fprintf(f, "function renderCanvas(index) {\n");
    fprintf(f, "  const canvas = document.getElementById(`canvas-${index}`);\n");
    fprintf(f, "  if (!canvas) return;\n");
    fprintf(f, "  const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "  const dpr = window.devicePixelRatio || 1;\n");
    fprintf(f, "  const width = rect.width;\n");
    fprintf(f, "  const height = rect.height;\n");
    fprintf(f, "  if (width === 0 || height === 0) return;\n");
    fprintf(f, "  if (canvas.width !== Math.floor(width * dpr) || canvas.height !== Math.floor(height * dpr)) {\n");
    fprintf(f, "    canvas.width = Math.floor(width * dpr);\n");
    fprintf(f, "    canvas.height = Math.floor(height * dpr);\n");
    fprintf(f, "  }\n");
    fprintf(f, "  const ctx = canvas.getContext('2d');\n");
    fprintf(f, "  ctx.save();\n");
    fprintf(f, "  ctx.scale(dpr, dpr);\n");
    fprintf(f, "  ctx.clearRect(0, 0, width, height);\n");

    fprintf(f, "  const graphBottom = height - 18;\n");
    fprintf(f, "  const graphTop = 8;\n");
    fprintf(f, "  const graphHeight = graphBottom - graphTop;\n");

    fprintf(f, "  // Grid lines\n");
    fprintf(f, "  ctx.strokeStyle = '#1e1e2c';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  [0.0, 0.25, 0.5, 0.75, 1.0].forEach(val => {\n");
    fprintf(f, "    const y = graphBottom - val * graphHeight;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();\n");
    fprintf(f, "  });\n");

    fprintf(f, "  const sampleStart = Math.max(0, Math.floor((viewStart / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const sampleEnd = Math.min(DATA.totalSamples - 1, Math.ceil((viewEnd / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const visibleSamples = Math.max(1, sampleEnd - sampleStart + 1);\n");
    fprintf(f, "  const stem = DATA.stems[index];\n");

    fprintf(f, "  function drawOriginalWaveform(pcm) {\n");
    fprintf(f, "    if (!pcm || pcm.length === 0) return;\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(180, 190, 210, 0.35)';\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(180, 190, 210, 0.55)';\n");
    fprintf(f, "    ctx.lineWidth = 1;\n");
    fprintf(f, "    const midY = graphTop + graphHeight / 2;\n");
    fprintf(f, "    const halfH = graphHeight / 2;\n");
    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const val = pcm[s];\n");
    fprintf(f, "        const y = midY - val * halfH;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let minV = pcm[s1];\n");
    fprintf(f, "          let maxV = pcm[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcm[s];\n");
    fprintf(f, "            if (v < minV) minV = v;\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y1 = midY - maxV * halfH;\n");
    fprintf(f, "          const y2 = midY - minV * halfH;\n");
    fprintf(f, "          ctx.fillRect(px, y1, 1, Math.max(1, y2 - y1));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  function drawFluxLine(pcm, color) {\n");
    fprintf(f, "    if (!pcm) return;\n");
    fprintf(f, "    ctx.strokeStyle = color;\n");
    fprintf(f, "    ctx.lineWidth = 1.8;\n");
    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const val = Math.max(0, Math.min(1, pcm[s]));\n");
    fprintf(f, "        const y = graphBottom - val * graphHeight;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      const maxArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let maxV = pcm[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcm[s];\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          maxArr[px] = Math.max(0, Math.min(1, maxV));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const y = graphBottom - maxArr[px] * graphHeight;\n");
    fprintf(f, "        if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 0. Draw Original Waveform Background\n");
    fprintf(f, "  if (DATA.origPcm) {\n");
    fprintf(f, "    drawOriginalWaveform(DATA.origPcm);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 1. Draw Raw Spectral Flux Envelope\n");
    fprintf(f, "  drawFluxLine(stem.rawFluxPcm, '#ff5722');\n");

    fprintf(f, "  // 2. Draw Modified Flux Envelope\n");
    fprintf(f, "  drawFluxLine(stem.modFluxPcm, '#4caf50');\n");

    fprintf(f, "  // 3. Draw Peak Hash Marks (coming down from top)\n");
    fprintf(f, "  if (stem.peaks) {\n");
    fprintf(f, "    ctx.strokeStyle = '#ffeb3b';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    for (let k = 0; k < stem.peaks.length; k++) {\n");
    fprintf(f, "      const s = stem.peaks[k];\n");
    fprintf(f, "      if (s >= sampleStart && s <= sampleEnd) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 10); ctx.stroke();\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 4. Draw Trough Hash Marks (coming up from bottom)\n");
    fprintf(f, "  if (stem.troughs) {\n");
    fprintf(f, "    ctx.strokeStyle = '#00bcd4';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    for (let k = 0; k < stem.troughs.length; k++) {\n");
    fprintf(f, "      const s = stem.troughs[k];\n");
    fprintf(f, "      if (s >= sampleStart && s <= sampleEnd) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        ctx.beginPath(); ctx.moveTo(x, graphBottom); ctx.lineTo(x, graphBottom - 10); ctx.stroke();\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // Time Axis Legend Along Bottom\n");
    fprintf(f, "  const legendY = height - 16;\n");
    fprintf(f, "  ctx.strokeStyle = 'rgba(255, 255, 255, 0.3)';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  ctx.beginPath(); ctx.moveTo(0, legendY); ctx.lineTo(width, legendY); ctx.stroke();\n");

    fprintf(f, "  ctx.fillStyle = '#a0a0b0';\n");
    fprintf(f, "  ctx.font = '10px monospace';\n");
    fprintf(f, "  const numTicks = 8;\n");
    fprintf(f, "  const durWindow = viewEnd - viewStart;\n");
    fprintf(f, "  for (let i = 0; i <= numTicks; i++) {\n");
    fprintf(f, "    const frac = i / numTicks;\n");
    fprintf(f, "    const x = frac * width;\n");
    fprintf(f, "    const timeVal = viewStart + frac * durWindow;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(x, legendY); ctx.lineTo(x, legendY + 4); ctx.stroke();\n");
    fprintf(f, "    if (i === 0) ctx.textAlign = 'left';\n");
    fprintf(f, "    else if (i === numTicks) ctx.textAlign = 'right';\n");
    fprintf(f, "    else ctx.textAlign = 'center';\n");
    fprintf(f, "    ctx.fillText(formatMSS(timeVal), x, height - 2);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // Drag selection highlight\n");
    fprintf(f, "  if (activeDrag && activeDrag.index === index) {\n");
    fprintf(f, "    const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(255, 235, 59, 0.25)';\n");
    fprintf(f, "    ctx.fillRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "    ctx.strokeStyle = '#ffeb3b';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    ctx.strokeRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "  }\n");

    fprintf(f, "  // Hover crosshair\n");
    fprintf(f, "  if (hoverState && hoverState.index === index) {\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';\n");
    fprintf(f, "    ctx.setLineDash([4, 4]);\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(hoverState.mouseX, 0); ctx.lineTo(hoverState.mouseX, height); ctx.stroke();\n");
    fprintf(f, "    ctx.setLineDash([]);\n");
    fprintf(f, "  }\n");

    fprintf(f, "  ctx.restore();\n");
    fprintf(f, "}\n");

    fprintf(f, "function renderAll() {\n");
    fprintf(f, "  for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "    renderCanvas(i);\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");

    fprintf(f, "function attachListeners() {\n");
    fprintf(f, "  DATA.stems.forEach((stem, idx) => {\n");
    fprintf(f, "    const canvas = document.getElementById(`canvas-${idx}`);\n");
    fprintf(f, "    const tooltip = document.getElementById(`tooltip-${idx}`);\n");
    fprintf(f, "    if (!canvas) return;\n");

    fprintf(f, "    canvas.addEventListener('mousedown', (e) => {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x = e.clientX - rect.left;\n");
    fprintf(f, "      activeDrag = { index: idx, startX: x, currentX: x };\n");
    fprintf(f, "      renderCanvas(idx);\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mousemove', (e) => {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));\n");
    fprintf(f, "      hoverState = { index: idx, mouseX: x };\n");
    fprintf(f, "      if (activeDrag) {\n");
    fprintf(f, "        activeDrag.currentX = x;\n");
    fprintf(f, "      }\n");
    fprintf(f, "      renderCanvas(idx);\n");

    fprintf(f, "      const t = xToTime(x, rect.width);\n");
    fprintf(f, "      const s = Math.min(DATA.totalSamples - 1, Math.max(0, Math.round(t * DATA.sampleRate)));\n");
    fprintf(f, "      const rawVal = (stem.rawFluxPcm && stem.rawFluxPcm.length > s) ? stem.rawFluxPcm[s] : 0;\n");
    fprintf(f, "      const modVal = (stem.modFluxPcm && stem.modFluxPcm.length > s) ? stem.modFluxPcm[s] : 0;\n");
    fprintf(f, "      tooltip.style.opacity = '1';\n");
    fprintf(f, "      tooltip.innerHTML = `Time: ${formatMSSPrecise(t)} (Sample #${s}) | Raw Flux: ${rawVal.toFixed(4)} | Modified Flux: ${modVal.toFixed(4)}`;\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mouseleave', () => {\n");
    fprintf(f, "      if (activeDrag && activeDrag.index === idx) {\n");
    fprintf(f, "        activeDrag = null;\n");
    fprintf(f, "      }\n");
    fprintf(f, "      hoverState = null;\n");
    fprintf(f, "      tooltip.style.opacity = '0';\n");
    fprintf(f, "      renderCanvas(idx);\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('mouseup', (e) => {\n");
    fprintf(f, "      if (activeDrag && activeDrag.index === idx) {\n");
    fprintf(f, "        const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "        const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "        const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "        activeDrag = null;\n");
    fprintf(f, "        if (x2 - x1 > 4) {\n");
    fprintf(f, "          const t1 = xToTime(x1, rect.width);\n");
    fprintf(f, "          const t2 = xToTime(x2, rect.width);\n");
    fprintf(f, "          zoomHistory.push({ start: viewStart, end: viewEnd });\n");
    fprintf(f, "          viewStart = Math.max(0, t1);\n");
    fprintf(f, "          viewEnd = Math.min(DATA.duration, t2);\n");
    fprintf(f, "          updateViewInfo();\n");
    fprintf(f, "          renderAll();\n");
    fprintf(f, "        } else {\n");
    fprintf(f, "          renderCanvas(idx);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    });\n");

    fprintf(f, "    canvas.addEventListener('contextmenu', (e) => {\n");
    fprintf(f, "      e.preventDefault();\n");
    fprintf(f, "      zoomOut();\n");
    fprintf(f, "    });\n");
    fprintf(f, "  });\n");
    fprintf(f, "}\n");

    fprintf(f, "window.addEventListener('resize', renderAll);\n");
    fprintf(f, "window.addEventListener('DOMContentLoaded', () => {\n");
    fprintf(f, "  attachListeners();\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "  initAudioData();\n");
    fprintf(f, "});\n");

    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");

    fclose(f);
}

void generate_master_flux_html_report(
    const char *flux_dir,
    const char *stem_dir,
    const FileBatchResult *results,
    int num_results,
    double total_wall_clock_sec
) {
    char master_html_path[4096];
    snprintf(master_html_path, sizeof(master_html_path), "%s/index.html", flux_dir);

    FILE *f = fopen(master_html_path, "w");
    if (!f) return;

    if (num_results == 1 && results[0].success) {
        /* Single file redirect to <basename>_index.html */
        fprintf(f, "<!DOCTYPE html>\n<html>\n<head>\n");
        fprintf(f, "<meta http-equiv=\"refresh\" content=\"0; url=%s_index.html\">\n", results[0].basename);
        fprintf(f, "<title>Spectral Flux Report Redirect</title>\n</head>\n<body>\n");
        fprintf(f, "<p>Redirecting to <a href=\"%s_index.html\">%s Spectral Flux Report</a>...</p>\n", results[0].basename, results[0].basename);
        fprintf(f, "</body>\n</html>\n");
        fclose(f);
    } else {
        /* Multi-file Master Batch Dashboard */
        fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
        fprintf(f, "<meta charset=\"UTF-8\">\n");
        fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
        fprintf(f, "<title>Spectral Flux Batch Analysis Dashboard</title>\n");
        fprintf(f, "<style>\n");
        fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; }\n");
        fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
        fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 24px; line-height: 1.5; }\n");
        fprintf(f, "  header { background: var(--card-bg); padding: 24px; border-radius: 12px; border: 1px solid var(--border); margin-bottom: 24px; }\n");
        fprintf(f, "  h1 { font-size: 1.8rem; color: #fff; margin-bottom: 8px; }\n");
        fprintf(f, "  .batch-stats { display: flex; gap: 24px; flex-wrap: wrap; font-size: 0.95rem; color: var(--muted); margin-top: 10px; }\n");
        fprintf(f, "  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(320px, 1fr)); gap: 20px; }\n");
        fprintf(f, "  .card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 20px; transition: transform 0.2s, border-color 0.2s; }\n");
        fprintf(f, "  .card:hover { border-color: var(--accent); transform: translateY(-2px); }\n");
        fprintf(f, "  .card-title { font-size: 1.1rem; font-weight: bold; color: #fff; margin-bottom: 10px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }\n");
        fprintf(f, "  .card-meta { font-size: 0.85rem; color: var(--muted); font-family: monospace; display: flex; flex-direction: column; gap: 4px; margin-bottom: 16px; }\n");
        fprintf(f, "  .btn { display: inline-block; background: var(--accent); color: #fff; text-decoration: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; font-size: 0.9rem; text-align: center; width: 100%%; transition: opacity 0.2s; }\n");
        fprintf(f, "  .btn:hover { opacity: 0.9; }\n");
        fprintf(f, "  .status-badge { display: inline-block; padding: 2px 8px; border-radius: 4px; font-size: 0.75rem; font-weight: bold; font-family: monospace; }\n");
        fprintf(f, "  .badge-success { background: #1b5e20; color: #81c784; }\n");
        fprintf(f, "  .badge-fail { background: #b71c1c; color: #e57373; }\n");
        fprintf(f, "</style>\n</head>\n<body>\n");

        fprintf(f, "<header>\n");
        fprintf(f, "  <h1>Spectral Flux Batch Analysis Dashboard</h1>\n");
        fprintf(f, "  <div class=\"batch-stats\">\n");
        fprintf(f, "    <span><strong>Files Processed:</strong> %d</span>\n", num_results);
        fprintf(f, "    <span><strong>Total Elapsed Wall-Clock Time:</strong> %.2fs</span>\n", total_wall_clock_sec);
        fprintf(f, "  </div>\n");
        fprintf(f, "</header>\n\n");

        fprintf(f, "<div class=\"grid\">\n");
        for (int i = 0; i < num_results; i++) {
            const FileBatchResult *r = &results[i];
            fprintf(f, "  <div class=\"card\">\n");
            fprintf(f, "    <div class=\"card-title\" title=\"%s\">%s.wav</div>\n", r->basename, r->basename);
            fprintf(f, "    <div class=\"card-meta\">\n");
            fprintf(f, "      <span>Status: %s</span>\n", r->success ? "<span class=\"status-badge badge-success\">COMPLETE</span>" : "<span class=\"status-badge badge-fail\">FAILED</span>");
            fprintf(f, "      <span>Duration: %.2fs (%u Hz, %s)</span>\n", r->duration, r->sample_rate, r->num_channels == 1 ? "Mono" : "Stereo");
            fprintf(f, "      <span>Flux Stems Exported: %d</span>\n", r->stem_count);
            fprintf(f, "      <span>Processing Time: %.2fs (%.1fx Real-Time)</span>\n", r->elapsed_sec, r->speed_ratio);
            fprintf(f, "    </div>\n");
            if (r->success) {
                fprintf(f, "    <a class=\"btn\" href=\"%s_index.html\">Open Interactive Report &rarr;</a>\n", r->basename);
            }
            fprintf(f, "  </div>\n");
        }
        fprintf(f, "</div>\n");
        fprintf(f, "</body>\n</html>\n");
        fclose(f);
    }

    /* Top-level redirecting index.html in stem_dir */
    char redirect_path[4096];
    snprintf(redirect_path, sizeof(redirect_path), "%s/index.html", stem_dir);
    FILE *f_redir = fopen(redirect_path, "w");
    if (f_redir) {
        fprintf(f_redir, "<!DOCTYPE html>\n<html>\n<head>\n");
        fprintf(f_redir, "<meta http-equiv=\"refresh\" content=\"0; url=[flux]/index.html\">\n");
        fprintf(f_redir, "<title>Spectral Flux Report Redirect</title>\n</head>\n<body>\n");
        fprintf(f_redir, "<p>Redirecting to <a href=\"[flux]/index.html\">Spectral Flux Batch Report</a>...</p>\n");
        fprintf(f_redir, "</body>\n</html>\n");
        fclose(f_redir);
    }
}

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

int process_audio(const char *input_path, const char *stem_dir, FileBatchResult *result, int thread_slot, int num_slots) {
    clock_t start_clock = clock();

    init_fft_tables();

    char dir[1024], basename[512];
    get_filepath_components(input_path, dir, basename, sizeof(basename));

    if (result) {
        memset(result, 0, sizeof(FileBatchResult));
        snprintf(result->input_path, sizeof(result->input_path), "%s", input_path);
        snprintf(result->basename, sizeof(result->basename), "%s", basename);
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

    double *orig_audio_buffer = wav->data;
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

    FluxStem *flux_stems = NULL;
    int audible_count = 0;
    int audible_capacity = 0;

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

        // Calculate flux metrics
        double max_fl_peak = 0.0;
        double fl_sum_sq = 0.0;
        for (size_t k = 0; k < total_channel_samples; k++) {
            double val = raw_flux_buf[k];
            if (val > max_fl_peak) max_fl_peak = val;
            fl_sum_sq += val * val;
        }
        double fl_rms = sqrt(fl_sum_sq / total_channel_samples);

        if (audible_count >= audible_capacity) {
            audible_capacity = (audible_capacity == 0) ? 16 : audible_capacity * 2;
            flux_stems = (FluxStem*)realloc(flux_stems, audible_capacity * sizeof(FluxStem));
        }

        FluxStem *stem = &flux_stems[audible_count++];
        memset(stem, 0, sizeof(FluxStem));
        stem->pitch = p;
        get_note_name(p, stem->note_name, sizeof(stem->note_name));
        stem->max_flux_peak = max_fl_peak;
        stem->rms_flux = fl_rms;
        stem->raw_flux = raw_flux_buf;

        double *mod_flux_buf = (double*)malloc(total_channel_samples * sizeof(double));
        if (mod_flux_buf) {
            memcpy(mod_flux_buf, raw_flux_buf, total_channel_samples * sizeof(double));

            // Detect peaks and troughs on channel 0 raw flux envelope for report visualization
            double *ch0_flux = (double*)malloc(total_samples * sizeof(double));
            if (ch0_flux) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    ch0_flux[s] = raw_flux_buf[s * num_channels];
                }
                detect_all_peaks(ch0_flux, total_samples, &stem->peaks, &stem->num_peaks);
                detect_all_troughs(ch0_flux, total_samples, &stem->troughs, &stem->num_troughs);
                free(ch0_flux);
            }

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
            stem->mod_flux = mod_flux_buf;
            snprintf(stem->mod_filename, sizeof(stem->mod_filename), "%s_pitch_%03d_%s_modified_flux.wav",
                     basename, p, stem->note_name);
        }
    }

    // Export companion flux WAV files
    int exported_count = 0;

    for (int idx = 0; idx < audible_count; idx++) {
        FluxStem *stem = &flux_stems[idx];
        int p = stem->pitch;

        if (stem->raw_flux) {
            char raw_flux_filepath[4096];
            snprintf(raw_flux_filepath, sizeof(raw_flux_filepath), "%s/[flux]/%s_pitch_%03d_%s_flux.wav",
                     stem_dir, basename, p, stem->note_name);
            write_wav_16bit(raw_flux_filepath, sample_rate, num_channels, total_samples, stem->raw_flux);
        }

        if (stem->mod_flux) {
            char mod_flux_filepath[4096];
            snprintf(mod_flux_filepath, sizeof(mod_flux_filepath), "%s/%s",
                     stem_dir, stem->mod_filename);
            write_wav_16bit(mod_flux_filepath, sample_rate, num_channels, total_samples, stem->mod_flux);
            exported_count++;
        }
    }

    // Construct Flux HTML report in [flux] directory
    if (flux_stems) {
        generate_flux_html_report(
            flux_dir,
            basename,
            sample_rate,
            num_channels,
            total_samples,
            audio_duration,
            orig_audio_buffer,
            flux_stems,
            audible_count
        );

        for (int idx = 0; idx < audible_count; idx++) {
            if (flux_stems[idx].raw_flux) free(flux_stems[idx].raw_flux);
            if (flux_stems[idx].mod_flux) free(flux_stems[idx].mod_flux);
            if (flux_stems[idx].peaks) free(flux_stems[idx].peaks);
            if (flux_stems[idx].troughs) free(flux_stems[idx].troughs);
        }
        free(flux_stems);
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
    if (orig_audio_buffer) free(orig_audio_buffer);
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
        process_audio(input_path, wargs->stem_dir, &wargs->results[idx], wargs->thread_slot, wargs->num_slots);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    enable_ansi_terminal();

    printf("====================================================\n");
    printf("         SPECTRAL FLUX PITCH STEM SEPARATOR         \n");
    printf("====================================================\n\n");

    char **file_list = NULL;
    int num_files = 0;
    int capacity = 0;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", argv[i]);
            trim_string(path);

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
    } else {
        /* Double-clicked or run with no arguments: scan current directory */
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

    init_fft_tables();
    init_flux_fft_tables();

    int cpu_cores = get_cpu_cores();
    int num_threads = (num_files < cpu_cores) ? num_files : cpu_cores;
    if (num_threads < 1) num_threads = 1;

    printf("Batch Processing Overview:\n");
    printf("  Total Audio Files: %d\n", num_files);
    printf("  CPU Cores:         %d\n", cpu_cores);
    printf("  Worker Threads:    %d\n", num_threads);
    printf("  Output Directory:  %s/\n\n", stem_dir);

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

            pthread_create(&threads[t], NULL, worker_thread_func, &wargs[t]);
        }

        for (int t = 0; t < num_threads; t++) {
            pthread_join(threads[t], NULL);
        }
        free(threads);
        free(wargs);
    } else {
        process_audio(file_list[0], stem_dir, &results[0], 0, 1);
    }

    if (is_tty) {
        printf("\n\n");
    }

    double total_wall_clock_sec = (double)(clock() - batch_start_clock) / CLOCKS_PER_SEC;

    /* Generate Master Batch HTML Report in [flux]/index.html */
    generate_master_flux_html_report(flux_dir, stem_dir, results, num_files, total_wall_clock_sec);

    int total_success = 0;
    for (int i = 0; i < num_files; i++) {
        if (results[i].success) total_success++;
    }

    printf("====================================================\n");
    printf("             BATCH PROCESSING COMPLETE              \n");
    printf("====================================================\n");
    printf("  Successfully Processed: %d / %d WAV files\n", total_success, num_files);
    printf("  Total Wall-Clock Time:  %.2f seconds\n", total_wall_clock_sec);
    printf("  Output Stem Folder:     %s/\n", stem_dir);
    printf("  Batch Report URL:       %s/index.html\n", flux_dir);
    printf("====================================================\n");

    for (int i = 0; i < num_files; i++) {
        free(file_list[i]);
    }
    free(file_list);
    free(results);

    wait_for_keypress();
    return (total_success == num_files) ? 0 : 1;
}
