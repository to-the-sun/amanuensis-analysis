/*
 * flux_separator_atonal.c
 *
 * Experimental copy: harmonic combs claim tonal bins, what is left is tracked as
 * mode regions and ridges, grouped into sources and exported as atonal stems.
 * flux_separator.c itself is untouched; see atonal-separation-plan.md.
 *
 * Perfect Reconstruction Pitch Stem Separator - Spectral Flux Focus
 * Multi-File, Directory Scanning & Multi-Threaded Concurrent Execution
 *
 * Focuses strictly on deriving per-pitch-stem transience flux envelopes,
 * exporting each stem's modified flux WAV next to the pitch stems, and
 * recombining them into the [flux] modified flux WAV the later tools consume.
 *
 * A normal run writes pitch_stems/<basename>_pitch_<nnn>_<note>_modified_flux.wav
 * for every audible stem and [flux]/<basename>_recombined_modified_flux.wav. The
 * Under --debug (see flux_separator_atonal_debug.bat) every stem's unmodified
 * flux envelope is also written into [flux] as <stem>_raw_flux.wav, beside the
 * modified envelope in pitch_stems, with the raw recombined file
 * (basename_recombined_raw_flux.wav) next to the modified one.
 *
 * Supports single file drag-and-drop, multi-file drag-and-drop, and double-clicking
 * (which automatically scans the folder for all WAV files and processes them simultaneously
 * using CPU multithreading with live per-file console progress bars).
 *
 * Highly optimized memory footprint: WAV outputs and compact JS report data are generated
 * stream-style per pitch stem so memory footprint remains extremely low (~300MB per file).
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
    int skipped;
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
    int16_t *raw_flux_i16; // mono averaged int16_t PCM array for JS report data
    int16_t *mod_flux_i16; // mono averaged int16_t PCM array for JS report data
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

static int starts_with_icase(const char *str, const char *prefix) {
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
    snprintf(prefix, sizeof(prefix), "%s_", basename);

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

/* Does <stem_dir>/<relative> exist? Used to tell whether a run is already done. */
static int flux_output_exists(const char *stem_dir, const char *relative) {
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", stem_dir, relative);

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
        if (pcm_i16) free(pcm_i16);

        fprintf(fjs, "  rawFluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const FluxStem *st = &stems[i];
            if (st->raw_flux_i16) {
                char *b64 = encode_base64_bytes((const uint8_t*)st->raw_flux_i16, total_samples * sizeof(int16_t));
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
            if (st->mod_flux_i16) {
                char *b64 = encode_base64_bytes((const uint8_t*)st->mod_flux_i16, total_samples * sizeof(int16_t));
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

    char single_report[4096];
    snprintf(single_report, sizeof(single_report), "%s/%s_index.html", flux_dir,
             num_results > 0 ? results[0].basename : "");
    FILE *single_f = (num_results == 1 && (results[0].success || results[0].skipped))
        ? fopen(single_report, "rb") : NULL;
    if (single_f) fclose(single_f);

    if (single_f) {
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
    fprintf(f, "  .badge-skip { background: #4a3b12; color: #ffd54f; }\n");
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
            fprintf(f, "      <span>Status: %s</span>\n", r->skipped
                ? "<span class=\"status-badge badge-skip\">SKIPPED (EXISTING STEMS)</span>"
                : (r->success
                    ? "<span class=\"status-badge badge-success\">COMPLETE</span>"
                    : "<span class=\"status-badge badge-fail\">FAILED</span>"));
            if (r->skipped) {
                fprintf(f, "      <span>Pitch stems already existed; analysis skipped.</span>\n");
            }
            fprintf(f, "      <span>Duration: %.2fs (%u Hz, %s)</span>\n", r->duration, r->sample_rate, r->num_channels == 1 ? "Mono" : "Stereo");
            fprintf(f, "      <span>Flux Stems Exported: %d</span>\n", r->stem_count);
            fprintf(f, "      <span>Processing Time: %.2fs (%.1fx Real-Time)</span>\n", r->elapsed_sec, r->speed_ratio);
            fprintf(f, "    </div>\n");
            char report_path[4096];
            snprintf(report_path, sizeof(report_path), "%s/%s_index.html", flux_dir, r->basename);
            FILE *rf = fopen(report_path, "rb");
            if (rf) {
                fclose(rf);
                fprintf(f, "    <a class=\"btn\" href=\"%s_index.html\">Open Interactive Report &rarr;</a>\n", r->basename);
            }
            fprintf(f, "  </div>\n");
        }
        fprintf(f, "</div>\n");
        fprintf(f, "</body>\n</html>\n");
        fclose(f);
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

static void copy_helper_files_to_pitch_stems(const char *dir, const char *stem_dir) {
    const char *files_to_copy[] = {
        "analyze_group.bat",
        "analyze.exe",
        "song_builder.exe"
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

/* ---------- atonal routing: combs, mode regions, ridges, groups ---------- */

#define LABEL_PITCH_BASE 1   /* 1..128 = pitch slot + 1 */
#define LABEL_RIDGE_BASE 512 /* 512 + ridge id for residual bins */
#define ROUTE_EPS 0.01       /* a comb must explain 1% of the frame's residual */
#define TROUGH_SPLIT 0.5     /* split only where the trough is half the lower peak */
#define ATTACK_FRAMES 2
#define DECAY_RATIO 3.0
#define MAX_COMB_CANDIDATES 48
#define MAX_COMB_HARMONICS 24
#define MIN_F0_HZ 20.0
#define MAX_F0_HZ 5000.0

typedef struct {
    int lo, hi;
    double mass;
    double centroid;
    int ridge;
} Region;

typedef struct {
    int start_frame, end_frame;
    int lo, hi;
    double sx, sy, sxy, sxx;
    int n;
    double decay;
    double last_mass;
    double peak_flux;
    int peak_frame;
    double peak_centroid;
    int group;
} Ridge;

typedef struct {
    int start_frame;
    int end_frame;
    double decay;
    int peak_frame;
    double anchor_peak;
} Group;

typedef struct {
    char name[1024];
    float *flux;
    double peak;
    int atonal;
    int index;
    double anchor_hz;
    float *raw;
} DestOut;

static int region_overlap(const Region *a, const Region *b) {
    int lo = a->lo > b->lo ? a->lo : b->lo;
    int hi = a->hi < b->hi ? a->hi : b->hi;
    return (hi >= lo) ? (hi - lo + 1) : 0;
}

/* Claim harmonic combs, then carve what is left into mode regions. */
static int route_frame(const double *power, double *resid, int nbins, double binw,
                       uint16_t *labels, Region *regions) {
    memcpy(resid, power, (size_t)nbins * sizeof(double));
    memset(labels, 0, (size_t)nbins * sizeof(uint16_t));

    double total = 0.0;
    for (int k = 0; k < nbins; k++) total += resid[k];
    if (!(total > 0.0)) return 0;

    int cand[MAX_COMB_CANDIDATES];
    for (;;) {
        double frame_peak = 0.0;
        for (int k = 0; k < nbins; k++) if (resid[k] > frame_peak) frame_peak = resid[k];
        if (!(frame_peak > 0.0)) break;

        int ncand = 0;
        double low = frame_peak * 1e-4;
        for (int k = 2; k < nbins - 1; k++) {
            if (!(resid[k] > resid[k - 1] && resid[k] >= resid[k + 1])) continue;
            if (resid[k] < low) continue;
            double f0 = k * binw;
            if (f0 < MIN_F0_HZ || f0 > MAX_F0_HZ) continue;
            if (ncand < MAX_COMB_CANDIDATES) {
                cand[ncand++] = k;
            } else {
                int weakest = 0;
                for (int c = 1; c < ncand; c++) if (resid[cand[c]] < resid[cand[weakest]]) weakest = c;
                if (resid[k] > resid[cand[weakest]]) cand[weakest] = k;
            }
        }
        if (ncand == 0) break;

        double best = 0.0;
        int best_k = -1;
        for (int c = 0; c < ncand; c++) {
            double f0 = cand[c] * binw;
            int idx0 = (int)lround(f0 / binw);
            int lo0 = idx0 - 2, hi0 = idx0 + 2;
            if (lo0 < 0) lo0 = 0;
            if (hi0 > nbins - 1) hi0 = nbins - 1;
            double fund = 0.0;
            for (int j = lo0; j <= hi0; j++) if (resid[j] > fund) fund = resid[j];
            if (!(fund > 0.0)) continue;

            double score = 0.0;
            int hits = 0;
            for (int h = 1; h <= MAX_COMB_HARMONICS; h++) {
                double f = f0 * h;
                if (f >= binw * (double)(nbins - 1)) break;
                int idx = (int)lround(f / binw);
                int tol = 2 + h / 4;
                int lo = idx - tol, hi = idx + tol;
                if (lo < 0) lo = 0;
                if (hi > nbins - 1) hi = nbins - 1;
                double m = 0.0;
                for (int j = lo; j <= hi; j++) if (resid[j] > m) m = resid[j];
                if (m >= 0.05 * fund) { score += m; hits++; }
            }
            /* a single partial is a mode, not a harmonic series */
            if (hits < 3) continue;
            if (score > best) { best = score; best_k = cand[c]; }
        }
        if (best_k < 0 || best <= ROUTE_EPS * total) break;

        double f0 = best_k * binw;
        uint16_t lab = (uint16_t)(LABEL_PITCH_BASE + freq_to_midi_pitch(f0));
        for (int h = 1; h <= MAX_COMB_HARMONICS; h++) {
            double f = f0 * h;
            if (f >= binw * (double)(nbins - 1)) break;
            int idx = (int)lround(f / binw);
            int tol = 2 + h / 4;
            int lo = idx - tol, hi = idx + tol;
            if (lo < 0) lo = 0;
            if (hi > nbins - 1) hi = nbins - 1;
            for (int j = lo; j <= hi; j++) {
                if (resid[j] > 0.0) { labels[j] = lab; resid[j] = 0.0; }
            }
        }
    }

    int *max_idx = (int*)malloc((size_t)nbins * sizeof(int));
    int *kept = (int*)malloc((size_t)nbins * sizeof(int));
    if (!max_idx || !kept) { free(max_idx); free(kept); return 0; }

    int nmax = 0;
    for (int k = 1; k < nbins - 1; k++) {
        if (resid[k] <= 0.0) continue;
        if (!(resid[k] > resid[k - 1] && resid[k] >= resid[k + 1])) continue;
        max_idx[nmax++] = k;
    }
    int nreg = 0;
    for (int i = 0; i < nmax; i++) {
        if (nreg == 0) { kept[nreg++] = max_idx[i]; continue; }
        int a = kept[nreg - 1], b = max_idx[i];
        double tv = resid[a];
        for (int j = a; j <= b; j++) if (resid[j] < tv) tv = resid[j];
        double pk = resid[a] < resid[b] ? resid[a] : resid[b];
        if (tv > TROUGH_SPLIT * pk) {
            if (resid[b] > resid[a]) kept[nreg - 1] = b;
            continue;
        }
        kept[nreg++] = b;
    }

    if (nreg == 0) { free(max_idx); free(kept); return 0; }

    for (int i = 0; i < nreg; i++) {
        int lo = (i == 0) ? 0 : kept[i - 1];
        int hi = (i == nreg - 1) ? nbins - 1 : kept[i];
        if (i > 0) {
            double tv = resid[kept[i - 1]];
            int ti = kept[i - 1];
            for (int j = kept[i - 1]; j <= kept[i]; j++) {
                if (resid[j] < tv) { tv = resid[j]; ti = j; }
            }
            lo = ti + 1;
        }
        if (i < nreg - 1) {
            double tv = resid[kept[i]];
            int ti = kept[i];
            for (int j = kept[i]; j <= kept[i + 1]; j++) {
                if (resid[j] < tv) { tv = resid[j]; ti = j; }
            }
            hi = ti;
        }
        Region *r = &regions[i];
        r->lo = lo;
        r->hi = hi;
        r->mass = 0.0;
        r->centroid = 0.0;
        r->ridge = -1;
        for (int j = lo; j <= hi; j++) {
            if (labels[j] != 0) continue;
            r->mass += resid[j];
            r->centroid += resid[j] * (double)j;
        }
        if (r->mass > 0.0) r->centroid /= r->mass;
        for (int j = lo; j <= hi; j++) {
            if (labels[j] == 0) labels[j] = (uint16_t)(LABEL_RIDGE_BASE + i);
        }
    }

    free(max_idx);
    free(kept);
    return nreg;
}

static void ridge_start(Ridge *r, int frame, const Region *reg) {
    memset(r, 0, sizeof(*r));
    r->start_frame = frame;
    r->end_frame = frame;
    r->lo = reg->lo;
    r->hi = reg->hi;
    r->last_mass = reg->mass;
    r->peak_flux = reg->mass;
    r->peak_frame = frame;
    r->peak_centroid = reg->centroid;
    r->group = -1;
}

static void ridge_update(Ridge *r, int frame, const Region *reg) {
    double d = reg->mass - r->last_mass;
    if (d > r->peak_flux) {
        r->peak_flux = d;
        r->peak_frame = frame;
        r->peak_centroid = reg->centroid;
    }
    r->last_mass = reg->mass;
    r->end_frame = frame;
    r->lo = reg->lo;
    r->hi = reg->hi;
    double y = log(reg->mass + 1e-30);
    r->sx += (double)frame;
    r->sy += y;
    r->sxy += (double)frame * y;
    r->sxx += (double)frame * (double)frame;
    r->n++;
}

static double ridge_decay(const Ridge *r) {
    if (r->n < 3) return 0.0;
    double den = (double)r->n * r->sxx - r->sx * r->sx;
    if (fabs(den) < 1e-12) return 0.0;
    return ((double)r->n * r->sxy - r->sx * r->sy) / den;
}

static int group_ridges(Ridge *ridges, int nridges, Group *groups) {
    int ngroups = 0;
    for (int i = 0; i < nridges; i++) {
        ridges[i].decay = ridge_decay(&ridges[i]);
        int target = -1;
        for (int g = 0; g < ngroups; g++) {
            if (ridges[i].start_frame > groups[g].end_frame + ATTACK_FRAMES) continue;
            double a = fabs(ridges[i].decay), b = fabs(groups[g].decay);
            double lo = a < b ? a : b, hi = a < b ? b : a;
            if (lo < 1e-9) { target = g; break; }   /* no decay evidence yet: merge */
            if (hi / lo > DECAY_RATIO) continue;
            target = g;
            break;
        }
        if (target < 0) {
            target = ngroups++;
            memset(&groups[target], 0, sizeof(Group));
            groups[target].start_frame = ridges[i].start_frame;
            groups[target].end_frame = ridges[i].end_frame;
            groups[target].decay = ridges[i].decay;
            groups[target].peak_frame = ridges[i].peak_frame;
            groups[target].anchor_peak = 0.0;
        }
        ridges[i].group = target;
        if (ridges[i].end_frame > groups[target].end_frame) groups[target].end_frame = ridges[i].end_frame;
        if (ridges[i].peak_flux > groups[target].anchor_peak) {
            groups[target].anchor_peak = ridges[i].peak_flux;
            groups[target].peak_frame = ridges[i].peak_frame;
        }
    }
    return ngroups;
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

    char raw_done[1024];
    snprintf(raw_done, sizeof(raw_done), "[flux]/%s_recombined_raw_flux.wav", basename);
    if (pitch_stems_exist_for(stem_dir, basename) &&
        (!debug_mode || flux_output_exists(stem_dir, raw_done))) {
        if (result) result->skipped = 1;
        pthread_mutex_lock(&g_console_mutex);
        printf("  Skipping %s: stems already exist for it.\n", input_path);
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
    for (int i = 0; i < FFT_SIZE; i++) window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / FFT_SIZE));

    double *cola_norm = (double*)calloc(padded_total_samples, sizeof(double));
    uint32_t num_frames = (padded_total_samples >= FFT_SIZE) ? ((padded_total_samples - FFT_SIZE) / HOP_SIZE + 1) : 0;

    for (uint32_t m = 0; m < num_frames; m++) {
        uint32_t offset = m * HOP_SIZE;
        for (int i = 0; i < FFT_SIZE; i++) {
            if (offset + i < padded_total_samples) cola_norm[offset + i] += window[i] * window[i];
        }
    }

    size_t num_bins_per_frame = FFT_SIZE / 2 + 1;
    int nbins = (int)num_bins_per_frame;
    double binw = (double)sample_rate / (double)FFT_SIZE;

    size_t total_stft_entries = (size_t)num_channels * num_frames * num_bins_per_frame;
    ComplexFloat *stft_data = (ComplexFloat*)malloc(total_stft_entries * sizeof(ComplexFloat));
    if (!stft_data) {
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input); free(cola_norm); free(wav->data); free(wav);
        if (result) result->success = 0;
        return 0;
    }

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
            for (int k = 0; k < nbins; k++) {
                stft_data[frame_base + k].r = (float)fft_frame[k].r;
                stft_data[frame_base + k].i = (float)fft_frame[k].i;
            }
        }
    }

    for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
    free(padded_input);

    /* Routing pass: one label map shared by all channels. */
    uint16_t *labels = (uint16_t*)calloc((size_t)num_frames * (size_t)nbins, sizeof(uint16_t));
    double *power = (double*)malloc((size_t)nbins * sizeof(double));
    double *resid = (double*)malloc((size_t)nbins * sizeof(double));
    Region *regions = (Region*)malloc((size_t)nbins * sizeof(Region));
    Region *prev_regions = (Region*)malloc((size_t)nbins * sizeof(Region));
    int *prev_ridge = (int*)malloc((size_t)nbins * sizeof(int));

    Ridge *ridges = NULL;
    int nridges = 0, ridge_cap = 0;
    int nprev = 0;

    clock_t route_start_clock = clock();

    for (uint32_t m = 0; m < num_frames; m++) {
        size_t base = (size_t)m * num_bins_per_frame;
        for (int k = 0; k < nbins; k++) {
            double acc = 0.0;
            for (uint32_t c = 0; c < num_channels; c++) {
                ComplexFloat v = stft_data[((size_t)c * num_frames + m) * num_bins_per_frame + k];
                acc += (double)v.r * v.r + (double)v.i * v.i;
            }
            power[k] = acc;
        }

        uint16_t *row = labels + base;
        int nreg = route_frame(power, resid, nbins, binw, row, regions);

        for (int i = 0; i < nreg; i++) {
            int best = -1, best_ov = 0;
            for (int p = 0; p < nprev; p++) {
                int ov = region_overlap(&regions[i], &prev_regions[p]);
                if (ov > best_ov) { best_ov = ov; best = p; }
            }
            if (best >= 0) {
                int rid = prev_ridge[best];
                regions[i].ridge = rid;
                ridge_update(&ridges[rid], (int)m, &regions[i]);
            } else {
                if (nridges >= ridge_cap) {
                    ridge_cap = (ridge_cap == 0) ? 64 : ridge_cap * 2;
                    ridges = (Ridge*)realloc(ridges, (size_t)ridge_cap * sizeof(Ridge));
                }
                regions[i].ridge = nridges;
                ridge_start(&ridges[nridges++], (int)m, &regions[i]);
            }
            for (int j = regions[i].lo; j <= regions[i].hi; j++) {
                if (row[j] == (uint16_t)(LABEL_RIDGE_BASE + i)) {
                    row[j] = (uint16_t)(LABEL_RIDGE_BASE + regions[i].ridge);
                }
            }
        }

        memcpy(prev_regions, regions, (size_t)nreg * sizeof(Region));
        for (int i = 0; i < nreg; i++) prev_ridge[i] = regions[i].ridge;
        nprev = nreg;

        if (m % 32 == 0 || m + 1 == num_frames) {
            double elapsed = (double)(clock() - route_start_clock) / CLOCKS_PER_SEC;
            print_multithreaded_progress(thread_slot, num_slots, basename, "Route", m + 1, num_frames, elapsed);
        }
    }

    Group *groups = (Group*)calloc((size_t)(nridges > 0 ? nridges : 1), sizeof(Group));
    int ngroups = group_ridges(ridges, nridges, groups);

    size_t dest_count = (size_t)NUM_MIDI_PITCHES + (size_t)ngroups;
    size_t mask_bytes = ((size_t)nbins + 7) / 8;
    uint8_t *dest_bins = (uint8_t*)calloc(dest_count * mask_bytes, 1);
    uint8_t *dest_active = (uint8_t*)calloc(dest_count * (size_t)num_frames, 1);

    for (uint32_t m = 0; m < num_frames; m++) {
        uint16_t *row = labels + (size_t)m * (size_t)nbins;
        for (int k = 0; k < nbins; k++) {
            uint16_t lab = row[k];
            if (lab == 0) continue;
            size_t d;
            if (lab < LABEL_RIDGE_BASE) {
                d = (size_t)(lab - LABEL_PITCH_BASE);
            } else {
                int rid = (int)lab - LABEL_RIDGE_BASE;
                if (rid < 0 || rid >= nridges) continue;
                int g = ridges[rid].group;
                if (g < 0) continue;
                d = (size_t)NUM_MIDI_PITCHES + (size_t)g;
            }
            if (d >= dest_count) continue;
            dest_bins[d * mask_bytes + (size_t)(k >> 3)] |= (uint8_t)(1u << (k & 7));
            dest_active[d * (size_t)num_frames + m] = 1;
        }
    }

    ensure_dir_exists(stem_dir);
    char flux_dir[8192];
    snprintf(flux_dir, sizeof(flux_dir), "%s/[flux]", stem_dir);
    ensure_dir_exists(flux_dir);

    Complex *pitch_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));
    size_t total_channel_samples = (size_t)total_samples * num_channels;

    DestOut *outs = NULL;
    int nout = 0, out_cap = 0;
    int nq_dropped = 0;

    clock_t synth_start_clock = clock();
    int dest_done = 0;

    for (size_t d = 0; d < dest_count; d++) {
        int any_bin = 0;
        for (size_t b = 0; b < mask_bytes; b++) if (dest_bins[d * mask_bytes + b]) { any_bin = 1; break; }
        if (!any_bin) continue;

        int any_frame = 0;
        for (uint32_t m = 0; m < num_frames; m++) if (dest_active[d * (size_t)num_frames + m]) { any_frame = 1; break; }
        if (!any_frame) continue;

        dest_done++;
        if (dest_done % 16 == 0) {
            double elapsed = (double)(clock() - synth_start_clock) / CLOCKS_PER_SEC;
            print_multithreaded_progress(thread_slot, num_slots, basename, "Synthesize", dest_done, (int)dest_count, elapsed);
        }

        double *stem_buf = (double*)calloc((size_t)padded_total_samples * num_channels, sizeof(double));
        if (!stem_buf) continue;

        for (uint32_t c = 0; c < num_channels; c++) {
            for (uint32_t m = 0; m < num_frames; m++) {
                if (!dest_active[d * (size_t)num_frames + m]) continue;
                size_t frame_base = ((size_t)c * num_frames + m) * num_bins_per_frame;
                memset(pitch_frame, 0, FFT_SIZE * sizeof(Complex));
                for (int k = 0; k < nbins; k++) {
                    if (!(dest_bins[d * mask_bytes + (size_t)(k >> 3)] & (uint8_t)(1u << (k & 7)))) continue;
                    ComplexFloat v = stft_data[frame_base + k];
                    pitch_frame[k].r = (double)v.r;
                    pitch_frame[k].i = (double)v.i;
                    if (k > 0 && k < FFT_SIZE / 2) {
                        pitch_frame[FFT_SIZE - k].r = (double)v.r;
                        pitch_frame[FFT_SIZE - k].i = -(double)v.i;
                    }
                }
                fft(pitch_frame, FFT_SIZE, 1);
                uint32_t offset = m * HOP_SIZE;
                for (int i = 0; i < FFT_SIZE; i++) {
                    if (offset + i < padded_total_samples) {
                        stem_buf[(size_t)(offset + i) * num_channels + c] += pitch_frame[i].r * window[i];
                    }
                }
            }
        }

        double *unpadded = (double*)malloc(total_channel_samples * sizeof(double));
        if (!unpadded) { free(stem_buf); continue; }
        for (uint32_t i = 0; i < total_samples; i++) {
            uint32_t padded_idx = i + pad_samples;
            double norm = cola_norm[padded_idx];
            for (uint32_t c = 0; c < num_channels; c++) {
                unpadded[(size_t)i * num_channels + c] = (norm > 1e-12)
                    ? stem_buf[(size_t)padded_idx * num_channels + c] / norm : 0.0;
            }
        }
        free(stem_buf);

        uint32_t active_count = 0;
        for (uint32_t i = 0; i < total_samples; i++) {
            int hit = 0;
            for (uint32_t c = 0; c < num_channels; c++) {
                if (fabs(unpadded[(size_t)i * num_channels + c]) >= g_silence_threshold) hit = 1;
            }
            if (hit) active_count++;
        }

        if ((double)active_count < 0.099 * (double)sample_rate) {
            nq_dropped++;
            free(unpadded);
            continue;
        }

        double *raw_flux = (double*)malloc(total_channel_samples * sizeof(double));
        if (!raw_flux) { free(unpadded); continue; }
        compute_stem_transience_flux(unpadded, total_samples, num_channels, sample_rate, raw_flux, 0);
        free(unpadded);

        float *raw_snapshot = NULL;
        if (debug_mode) {
            raw_snapshot = (float*)malloc(total_channel_samples * sizeof(float));
            if (raw_snapshot) {
                for (size_t k = 0; k < total_channel_samples; k++) raw_snapshot[k] = (float)raw_flux[k];
            }
        }

        for (uint32_t c = 0; c < num_channels; c++) {
            double *env = (double*)malloc((size_t)total_samples * sizeof(double));
            if (!env) continue;
            for (uint32_t s = 0; s < total_samples; s++) env[s] = raw_flux[(size_t)s * num_channels + c];
            modify_flux_envelope_channel(env, total_samples);
            for (uint32_t s = 0; s < total_samples; s++) raw_flux[(size_t)s * num_channels + c] = env[s];
            free(env);
        }

        double peak = 0.0;
        for (size_t k = 0; k < total_channel_samples; k++) if (raw_flux[k] > peak) peak = raw_flux[k];

        if (nout >= out_cap) {
            out_cap = (out_cap == 0) ? 16 : out_cap * 2;
            outs = (DestOut*)realloc(outs, (size_t)out_cap * sizeof(DestOut));
        }
        DestOut *o = &outs[nout];
        memset(o, 0, sizeof(*o));
        o->peak = peak;
        o->raw = raw_snapshot;
        o->flux = (float*)malloc(total_channel_samples * sizeof(float));
        if (!o->flux) { free(raw_flux); free(raw_snapshot); continue; }
        for (size_t k = 0; k < total_channel_samples; k++) o->flux[k] = (float)raw_flux[k];
        free(raw_flux);

        if (d < (size_t)NUM_MIDI_PITCHES) {
            char note[32];
            o->atonal = 0;
            o->index = (int)d;
            get_note_name((int)d, note, sizeof(note));
            snprintf(o->name, sizeof(o->name), "%s_pitch_%03d_%s_modified_flux.wav", basename, (int)d, note);
        } else {
            int g = (int)d - NUM_MIDI_PITCHES;
            o->atonal = 1;
            o->index = g;
            o->anchor_hz = 0.0;
            if (g < ngroups) {
                int best_r = -1;
                double best_pf = -1.0;
                for (int r = 0; r < nridges; r++) {
                    if (ridges[r].group != g) continue;
                    if (ridges[r].peak_flux > best_pf) { best_pf = ridges[r].peak_flux; best_r = r; }
                }
                if (best_r >= 0) o->anchor_hz = ridges[best_r].peak_centroid * binw;
            }
            snprintf(o->name, sizeof(o->name), "%s_atonal_%03d_modified_flux.wav", basename, g);
        }
        nout++;
    }

    double shared_peak = 0.0;
    for (int i = 0; i < nout; i++) if (outs[i].peak > shared_peak) shared_peak = outs[i].peak;
    if (!(shared_peak > 1e-12)) shared_peak = 1.0;

    double *recombined = (double*)calloc(total_channel_samples, sizeof(double));
    double *recombined_raw = debug_mode ? (double*)calloc(total_channel_samples, sizeof(double)) : NULL;
    double *wb = (double*)malloc(total_channel_samples * sizeof(double));
    int written = 0;

    for (int i = 0; i < nout; i++) {
        if (!outs[i].flux || !wb) continue;
        for (size_t k = 0; k < total_channel_samples; k++) {
            double v = (double)outs[i].flux[k] / shared_peak;
            wb[k] = v;
            if (recombined) recombined[k] += v;
        }
        char path[8192];
        snprintf(path, sizeof(path), "%s/%s", stem_dir, outs[i].name);
        if (write_wav_16bit(path, sample_rate, num_channels, total_samples, wb)) written++;

        if (outs[i].raw) {
            char raw_name[512];
            snprintf(raw_name, sizeof(raw_name), "%s", outs[i].name);
            char *tag = strstr(raw_name, "_modified_flux.wav");
            if (tag) snprintf(tag, sizeof(raw_name) - (size_t)(tag - raw_name), "_raw_flux.wav");
            for (size_t k = 0; k < total_channel_samples; k++) {
                double v = (double)outs[i].raw[k] / shared_peak;
                wb[k] = v;
                if (recombined_raw) recombined_raw[k] += v;
            }
            snprintf(path, sizeof(path), "%s/%s", flux_dir, raw_name);
            write_wav_16bit(path, sample_rate, num_channels, total_samples, wb);
            free(outs[i].raw);
            outs[i].raw = NULL;
        }

        free(outs[i].flux);
        outs[i].flux = NULL;
    }

    if (recombined) {
        double rec_peak = 0.0;
        for (size_t k = 0; k < total_channel_samples; k++) if (fabs(recombined[k]) > rec_peak) rec_peak = fabs(recombined[k]);
        if (rec_peak > 1e-12) for (size_t k = 0; k < total_channel_samples; k++) recombined[k] /= rec_peak;
        char rec_path[12288];
        snprintf(rec_path, sizeof(rec_path), "%s/%s_recombined_modified_flux.wav", flux_dir, basename);
        write_wav_16bit(rec_path, sample_rate, num_channels, total_samples, recombined);
        free(recombined);
    }

    if (recombined_raw) {
        double raw_peak = 0.0;
        for (size_t k = 0; k < total_channel_samples; k++) if (fabs(recombined_raw[k]) > raw_peak) raw_peak = fabs(recombined_raw[k]);
        if (raw_peak > 1e-12) for (size_t k = 0; k < total_channel_samples; k++) recombined_raw[k] /= raw_peak;
        char raw_rec_path[12288];
        snprintf(raw_rec_path, sizeof(raw_rec_path), "%s/%s_recombined_raw_flux.wav", flux_dir, basename);
        write_wav_16bit(raw_rec_path, sample_rate, num_channels, total_samples, recombined_raw);
        free(recombined_raw);
    }
    free(wb);

    int atonal_written = 0;
    for (int i = 0; i < nout; i++) if (outs[i].atonal) atonal_written++;

    pthread_mutex_lock(&g_console_mutex);
    printf("\n  %s: %d files (%d pitch, %d atonal), %d ridges, %d groups, %d dropped by qualification, shared peak %.6f\n",
           basename, written, written - atonal_written, atonal_written, nridges, ngroups, nq_dropped, shared_peak);
    for (int i = 0; i < nout; i++) {
        if (!outs[i].atonal) continue;
        printf("    atonal %03d anchor %.0f Hz\n", outs[i].index, outs[i].anchor_hz);
    }
    fflush(stdout);
    pthread_mutex_unlock(&g_console_mutex);

    double total_elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    if (result) {
        result->stem_count = written;
        result->elapsed_sec = total_elapsed;
        result->speed_ratio = audio_duration / (total_elapsed > 0.001 ? total_elapsed : 0.001);
        result->success = 1;
    }

    print_multithreaded_progress(thread_slot, num_slots, basename, "Complete", 100, 100, total_elapsed);

    free(outs);
    free(groups);
    free(ridges);
    free(labels);
    free(power);
    free(resid);
    free(regions);
    free(prev_regions);
    free(prev_ridge);
    free(dest_bins);
    free(dest_active);
    free(pitch_frame);
    free(fft_frame);
    free(stft_data);
    free(cola_norm);
    free(wav->data);
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
           ? "enabled (raw and modified flux side by side in [flux])"
           : "disabled (no raw flux comparison)");

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

    /* Generate Master Batch HTML Report in [flux]/index.html (debug only) */
    if (debug_mode) {
        generate_master_flux_html_report(flux_dir, stem_dir, results, num_files, total_wall_clock_sec);
    }

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
        printf("  Batch Report URL:       %s/index.html\n", flux_dir);
    } else {
        printf("  Debug Output:           disabled (double-click flux_separator_atonal_debug.bat for raw flux)\n");
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
