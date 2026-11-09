/*
 * analyze_palettes.c
 *
 * High-performance audio pattern report generator.
 * Single efficient C executable.
 *
 * Features:
 *  - Drag & drop single audio file or multiple audio files.
 *  - Double-click execution (when run with no path arguments): automatically scans
 *    the directory where the executable resides for all WAV files and processes them.
 *  - Console remains open on double-click / completion / error via wait_for_keypress().
 *  - Decodes 16-bit, 24-bit, 32-bit PCM and 32-bit float WAV audio using dr_wav.
 *  - Downmixes multi-channel audio to mono efficiently.
 *  - Fast silence-delimited active section detection (RMS, -50 dB, 0.5 s min silence).
 *  - Spectral flux onset envelope + log-mel band autocorrelation to find optimal
 *    loop periodicities and bar counts.
 *  - Generates interactive pattern reports matching pattern_report_builder:
 *      index.html
 *      <audio-basename>.html
 *      <audio-basename>.wav
 *      data/<audio-basename>/summary.js
 *      data/<audio-basename>/l8_*.js
 *      data/<audio-basename>/l64_*.js
 *      data/<audio-basename>/l512_*.js
 *  - Multi-threaded processing across files using POSIX threads.
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
#include <strings.h>

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

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "templates.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK_BINS 65536
#define COARSE_STEP 4096

static const int LEVELS_STEPS[] = { 8, 64, 512 };
#define NUM_LEVELS 3

static const char* PALETTE_COLORS[] = {
    "#4dabf7", "#ffa94d", "#51cf66", "#cc5de8"
};
#define NUM_PALETTE_COLORS 4

typedef struct {
    double start;
    double end;
    int partial;
} Segment;

typedef struct {
    int id;
    char name[128];
    char color[32];
    double start;
    double end;
    double length;
    int bars;
    int num_segments;
    Segment *segments;
} Pattern;

typedef struct {
    char filepath[2048];
    char filename[512];
    char basename[512];
    char out_dir[2048];
    double duration;
    int sample_rate;
    int channels;
    int num_patterns;
    int success;
    double elapsed_sec;
} FileResult;

static pthread_mutex_t g_console_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Forward declarations */
static void wait_for_keypress(void);
static void trim_string(char *str);
static void get_executable_directory(char *out_dir, size_t out_size);
static void ensure_dir_exists(const char *path);
static int is_valid_wav_file(const char *filename);
static void scan_directory_for_wavs(const char *dir_path, char ***file_list, int *num_files, int *capacity);
static int get_cpu_cores(void);

/* Base64 encoding helper */
static void base64_encode(const unsigned char *in, size_t in_len, char *out) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < in_len) {
        uint32_t oct_a = i < in_len ? in[i++] : 0;
        int has_b = (i < in_len);
        uint32_t oct_b = has_b ? in[i++] : 0;
        int has_c = (i < in_len);
        uint32_t oct_c = has_c ? in[i++] : 0;

        uint32_t triple = (oct_a << 16) | (oct_b << 8) | oct_c;

        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = has_b ? b64[(triple >> 6) & 0x3F] : '=';
        out[j++] = has_c ? b64[triple & 0x3F] : '=';
    }
    out[j] = '\0';
}

/* Fast in-place radix-2 Cooley-Tukey FFT */
typedef struct {
    double r;
    double i;
} Complex;

static void fft_radix2(Complex *a, int n, int invert) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            Complex tmp = a[i];
            a[i] = a[j];
            a[j] = tmp;
        }
    }

    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len * (invert ? -1.0 : 1.0);
        Complex wlen = { cos(ang), sin(ang) };
        for (int i = 0; i < n; i += len) {
            Complex w = { 1.0, 0.0 };
            for (int j = 0; j < len / 2; j++) {
                Complex u = a[i + j];
                Complex v = {
                    a[i + j + len / 2].r * w.r - a[i + j + len / 2].i * w.i,
                    a[i + j + len / 2].r * w.i + a[i + j + len / 2].i * w.r
                };
                a[i + j].r = u.r + v.r;
                a[i + j].i = u.i + v.i;
                a[i + j + len / 2].r = u.r - v.r;
                a[i + j + len / 2].i = u.i - v.i;
                double next_r = w.r * wlen.r - w.i * wlen.i;
                double next_i = w.r * wlen.i + w.i * wlen.r;
                w.r = next_r;
                w.i = next_i;
            }
        }
    }

    if (invert) {
        double inv_n = 1.0 / n;
        for (int i = 0; i < n; i++) {
            a[i].r *= inv_n;
            a[i].i *= inv_n;
        }
    }
}

static int next_pow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* String replacement helper */
static char* str_replace(const char *orig, const char *rep, const char *with) {
    if (!orig || !rep) return NULL;
    size_t rep_len = strlen(rep);
    if (rep_len == 0) return strdup(orig);
    if (!with) with = "";
    size_t with_len = strlen(with);

    size_t count = 0;
    const char *tmp = orig;
    while ((tmp = strstr(tmp, rep)) != NULL) {
        count++;
        tmp += rep_len;
    }

    size_t new_len = strlen(orig) + count * (with_len - rep_len);
    char *result = (char*)malloc(new_len + 1);
    if (!result) return NULL;

    char *out_ptr = result;
    const char *in_ptr = orig;
    while (*in_ptr) {
        const char *match = strstr(in_ptr, rep);
        if (match == in_ptr) {
            memcpy(out_ptr, with, with_len);
            out_ptr += with_len;
            in_ptr += rep_len;
        } else if (match) {
            size_t n = match - in_ptr;
            memcpy(out_ptr, in_ptr, n);
            out_ptr += n;
            in_ptr += n;
        } else {
            strcpy(out_ptr, in_ptr);
            break;
        }
    }
    result[new_len] = '\0';
    return result;
}

/* Format time as M:SS.ss */
static void fmt_precise(double t, char *out, size_t out_len) {
    if (t < 0.0) t = 0.0;
    int m = (int)(t / 60.0);
    double s = t - m * 60.0;
    snprintf(out, out_len, "%d:%05.2f", m, s);
}

/* Build segments for a pattern */
static Segment* build_segments(double start, double end, double length, int *num_segs) {
    int cap = 16;
    int count = 0;
    Segment *segs = (Segment*)malloc(cap * sizeof(Segment));
    double t = start;
    while (t < end - 0.01) {
        double e = t + length;
        if (e > end) e = end;
        if (count >= cap) {
            cap *= 2;
            segs = (Segment*)realloc(segs, cap * sizeof(Segment));
        }
        segs[count].start = floor(t * 1000.0 + 0.5) / 1000.0;
        segs[count].end = floor(e * 1000.0 + 0.5) / 1000.0;
        segs[count].partial = ((e - t) < (length - 0.001)) ? 1 : 0;
        count++;
        t = e;
    }
    *num_segs = count;
    return segs;
}

/* Compute min/max bins from int16 PCM */
static int16_t* compute_minmax_bins(const int16_t *pcm, size_t n, int step, size_t *out_nb) {
    size_t nb = (n + step - 1) / step;
    *out_nb = nb;
    int16_t *out = (int16_t*)malloc(nb * 2 * sizeof(int16_t));
    if (!out) return NULL;

    for (size_t b = 0; b < nb; b++) {
        size_t start = b * step;
        size_t end = start + step;
        if (end > n) end = n;

        int16_t mn = 32767;
        int16_t mx = -32768;
        if (start < end) {
            mn = pcm[start];
            mx = pcm[start];
            for (size_t i = start + 1; i < end; i++) {
                int16_t v = pcm[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
        } else {
            mn = 0;
            mx = 0;
        }
        out[b * 2] = mn;
        out[b * 2 + 1] = mx;
    }
    return out;
}

/* Write chunked tier JS files */
static void write_tier_level(const int16_t *inter, size_t nb, int step, int level_idx,
                             const char *data_dir, size_t *out_nchunks) {
    size_t nchunks = (nb + CHUNK_BINS - 1) / CHUNK_BINS;
    if (nchunks == 0) nchunks = 1;
    *out_nchunks = nchunks;

    for (size_t c = 0; c < nchunks; c++) {
        size_t start_bin = c * CHUNK_BINS;
        size_t end_bin = start_bin + CHUNK_BINS;
        if (end_bin > nb) end_bin = nb;
        size_t chunk_items = (end_bin > start_bin) ? (end_bin - start_bin) * 2 : 0;

        const int16_t *chunk_ptr = &inter[start_bin * 2];
        size_t in_bytes = chunk_items * sizeof(int16_t);
        size_t b64_len = 4 * ((in_bytes + 2) / 3);
        char *b64_str = (char*)malloc(b64_len + 1);
        if (b64_str) {
            base64_encode((const unsigned char*)chunk_ptr, in_bytes, b64_str);
            char chunk_file[8192];
            snprintf(chunk_file, sizeof(chunk_file), "%s/l%d_%zu.js", data_dir, step, c);
            FILE *f = fopen(chunk_file, "w");
            if (f) {
                fprintf(f, "window.__patternChunkArrive(\"%d_%zu\",[\"%s\"]);\n", level_idx, c, b64_str);
                fclose(f);
            }
            free(b64_str);
        }
    }
}

/* Estimate bar count and validate base pulse */
static void estimate_bars_and_length(double lag, int *out_bars, double *out_len) {
    *out_len = lag;
    for (int b = 1; b <= 8; b++) {
        if (b == 5 || b == 7) continue;
        double bar_len = lag / b;
        if (bar_len >= 1.7 && bar_len <= 2.3) {
            *out_bars = b;
            return;
        }
    }
    int est = (int)floor(lag / 2.07 + 0.5);
    if (est < 1) est = 1;
    *out_bars = est;
}

/* Peak finder for autocorrelation array */
static int find_acf_peaks(const double *ac, int n_ac, double fps, double min_lag, double max_lag,
                          double *out_lags, double *out_vals, int max_peaks) {
    int min_dist = (int)(0.5 * fps);
    if (min_dist < 1) min_dist = 1;

    int count = 0;
    for (int i = 1; i < n_ac - 1; i++) {
        double v = ac[i];
        if (v <= ac[i - 1] || v <= ac[i + 1]) continue;

        double lag = i / fps;
        if (lag < min_lag || lag > max_lag) continue;

        /* Prominence check: check local troughs within min_dist */
        int left = i - min_dist; if (left < 0) left = 0;
        int right = i + min_dist; if (right >= n_ac) right = n_ac - 1;
        double min_left = ac[left];
        for (int k = left; k < i; k++) { if (ac[k] < min_left) min_left = ac[k]; }
        double min_right = ac[i + 1];
        for (int k = i + 1; k <= right; k++) { if (ac[k] < min_right) min_right = ac[k]; }
        double prom = (min_left > min_right) ? (v - min_left) : (v - min_right);
        if (prom < 0.005) continue;

        if (count < max_peaks) {
            out_lags[count] = lag;
            out_vals[count] = v;
            count++;
        } else {
            /* Replace lowest if higher */
            int min_idx = 0;
            for (int k = 1; k < count; k++) {
                if (out_vals[k] < out_vals[min_idx]) min_idx = k;
            }
            if (v > out_vals[min_idx]) {
                out_lags[min_idx] = lag;
                out_vals[min_idx] = v;
            }
        }
    }

    /* Sort descending by peak value */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (out_vals[j] > out_vals[i]) {
                double tv = out_vals[i]; out_vals[i] = out_vals[j]; out_vals[j] = tv;
                double tl = out_lags[i]; out_lags[i] = out_lags[j]; out_lags[j] = tl;
            }
        }
    }
    return count;
}

/* Process a single audio file */
static int process_audio_file(const char *input_path, const char *workspace_dir, FileResult *result) {
    clock_t start_clock = clock();

    char filename[512] = {0};
    const char *last_slash = strrchr(input_path, '/');
    const char *last_backslash = strrchr(input_path, '\\');
    const char *base_ptr = (last_backslash && (!last_slash || last_backslash > last_slash)) ? last_backslash + 1 : (last_slash ? last_slash + 1 : input_path);
    snprintf(filename, sizeof(filename), "%s", base_ptr);

    char basename[512] = {0};
    snprintf(basename, sizeof(basename), "%s", filename);
    char *dot = strrchr(basename, '.');
    if (dot) *dot = '\0';

    snprintf(result->filepath, sizeof(result->filepath), "%s", input_path);
    snprintf(result->filename, sizeof(result->filename), "%s", filename);
    snprintf(result->basename, sizeof(result->basename), "%s", basename);
    snprintf(result->out_dir, sizeof(result->out_dir), "%s", workspace_dir);
    result->success = 0;

    pthread_mutex_lock(&g_console_mutex);
    printf("\n------------------------------------------------------------\n");
    printf("Processing: %s\n", filename);
    pthread_mutex_unlock(&g_console_mutex);

    /* Open audio via dr_wav */
    unsigned int channels = 0;
    unsigned int sample_rate = 0;
    drwav_uint64 total_pcm_frames = 0;
    float *p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        input_path, &channels, &sample_rate, &total_pcm_frames, NULL
    );

    if (!p_sample_data || total_pcm_frames == 0 || sample_rate == 0) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Could not open or decode audio file '%s'\n", input_path);
        pthread_mutex_unlock(&g_console_mutex);
        if (p_sample_data) drwav_free(p_sample_data, NULL);
        return 0;
    }

    size_t total_samples = (size_t)total_pcm_frames;
    double duration = (double)total_samples / (double)sample_rate;
    result->duration = duration;
    result->sample_rate = sample_rate;
    result->channels = channels;

    /* Downmix to float and signed 16-bit PCM */
    float *mono_float = (float*)malloc(total_samples * sizeof(float));
    int16_t *mono_pcm16 = (int16_t*)malloc(total_samples * sizeof(int16_t));
    if (!mono_float || !mono_pcm16) {
        pthread_mutex_lock(&g_console_mutex);
        printf("Error: Out of memory allocating PCM buffers for '%s'\n", filename);
        pthread_mutex_unlock(&g_console_mutex);
        drwav_free(p_sample_data, NULL);
        if (mono_float) free(mono_float);
        if (mono_pcm16) free(mono_pcm16);
        return 0;
    }

    if (channels == 1) {
        for (size_t i = 0; i < total_samples; i++) {
            float s = p_sample_data[i];
            mono_float[i] = s;
            double v = s * 32767.0;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            mono_pcm16[i] = (int16_t)(floor(v + 0.5));
        }
    } else {
        for (size_t i = 0; i < total_samples; i++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < channels; c++) {
                sum += p_sample_data[i * channels + c];
            }
            float s = (float)(sum / channels);
            mono_float[i] = s;
            double v = s * 32767.0;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            mono_pcm16[i] = (int16_t)(floor(v + 0.5));
        }
    }
    drwav_free(p_sample_data, NULL);

    pthread_mutex_lock(&g_console_mutex);
    printf("Loaded audio: %.2f seconds (%zu samples, %u Hz, %u ch)\n",
           duration, total_samples, sample_rate, channels);
    pthread_mutex_unlock(&g_console_mutex);

    /* Copy or export audio WAV directly into workspace_dir */
    char out_wav_path[4096];
    snprintf(out_wav_path, sizeof(out_wav_path), "%s/%s", workspace_dir, filename);
    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = sample_rate;
    format.bitsPerSample = 16;
    drwav wav_out;
    if (drwav_init_file_write(&wav_out, out_wav_path, &format, NULL)) {
        drwav_write_pcm_frames(&wav_out, total_samples, mono_pcm16);
        drwav_uninit(&wav_out);
    }

    /* Create data/<basename> directory */
    char data_rel_dir[1024];
    snprintf(data_rel_dir, sizeof(data_rel_dir), "data/%s", basename);
    char data_full_dir[4096];
    snprintf(data_full_dir, sizeof(data_full_dir), "%s/%s", workspace_dir, data_rel_dir);
    ensure_dir_exists(data_full_dir);

    /* Detect silence-delimited active sections using 0.1s RMS, -50dB */
    int win = (int)floor(0.1 * sample_rate + 0.5);
    if (win < 1) win = 1;
    size_t num_wins = total_samples / win;

    typedef struct {
        double start;
        double end;
    } AudioSection;

    AudioSection *sections = NULL;
    int num_sections = 0;
    int sec_cap = 16;
    sections = (AudioSection*)malloc(sec_cap * sizeof(AudioSection));

    if (num_wins < 2) {
        sections[num_sections].start = 0.0;
        sections[num_sections].end = duration;
        num_sections++;
    } else {
        typedef struct { double s; double e; } Run;
        Run *runs = (Run*)malloc(num_wins * sizeof(Run));
        int num_runs = 0;
        int in_run = 0;
        int run_start = 0;

        for (size_t w = 0; w < num_wins; w++) {
            double sum_sq = 0.0;
            size_t off = w * win;
            for (int k = 0; k < win; k++) {
                double val = mono_float[off + k];
                sum_sq += val * val;
            }
            double rms = sqrt(sum_sq / win);
            double db = 20.0 * log10(rms + 1e-12);
            int active = (db > -50.0);

            if (active && !in_run) {
                in_run = 1;
                run_start = (int)w;
            } else if (!active && in_run) {
                in_run = 0;
                runs[num_runs].s = run_start * 0.1;
                runs[num_runs].e = w * 0.1;
                num_runs++;
            }
        }
        if (in_run) {
            runs[num_runs].s = run_start * 0.1;
            runs[num_runs].e = num_wins * 0.1;
            num_runs++;
        }

        if (num_runs == 0) {
            sections[num_sections].start = 0.0;
            sections[num_sections].end = duration;
            num_sections++;
        } else {
            /* Merge runs separated by < 0.5s min silence */
            Run merged[1024];
            int num_merged = 0;
            merged[0] = runs[0];
            num_merged = 1;

            for (int r = 1; r < num_runs && num_merged < 1024; r++) {
                if (runs[r].s - merged[num_merged - 1].e < 0.5) {
                    merged[num_merged - 1].e = runs[r].e;
                } else {
                    merged[num_merged++] = runs[r];
                }
            }

            for (int m = 0; m < num_merged; m++) {
                double s = merged[m].s; if (s < 0.0) s = 0.0;
                double e = merged[m].e; if (e > duration) e = duration;
                if (e - s >= 1.0) {
                    if (num_sections >= sec_cap) {
                        sec_cap *= 2;
                        sections = (AudioSection*)realloc(sections, sec_cap * sizeof(AudioSection));
                    }
                    sections[num_sections].start = floor(s * 1000.0 + 0.5) / 1000.0;
                    sections[num_sections].end = floor(e * 1000.0 + 0.5) / 1000.0;
                    num_sections++;
                }
            }
            if (num_sections == 0) {
                sections[num_sections].start = 0.0;
                sections[num_sections].end = duration;
                num_sections++;
            }
        }
        free(runs);
    }

    pthread_mutex_lock(&g_console_mutex);
    printf("Detected %d section(s):\n", num_sections);
    for (int s = 0; s < num_sections; s++) {
        printf("  Section %d: %.2f - %.2f s (dur: %.2f s)\n",
               s + 1, sections[s].start, sections[s].end, sections[s].end - sections[s].start);
    }
    pthread_mutex_unlock(&g_console_mutex);

    /* STFT & Spectral Flux / Mel Analysis */
    int nfft = 2048;
    if (total_samples < (size_t)nfft) {
        nfft = next_pow2((int)total_samples);
        if (nfft < 16) nfft = 16;
    }
    int hop = nfft / 2;
    double fps = (double)sample_rate / hop;
    int num_frames = (int)((total_samples - nfft) / hop + 1);
    if (num_frames < 4) num_frames = 4;

    /* Construct 64-band Mel filterbank */
    int num_bands = 64;
    double fmin = 30.0;
    double fmax = (double)sample_rate / 2.0;
    double hz_to_mel_min = 2595.0 * log10(1.0 + fmin / 700.0);
    double hz_to_mel_max = 2595.0 * log10(1.0 + fmax / 700.0);
    double mel_pts[66];
    for (int b = 0; b < 66; b++) {
        double m = hz_to_mel_min + (hz_to_mel_max - hz_to_mel_min) * b / 65.0;
        mel_pts[b] = 700.0 * (pow(10.0, m / 2595.0) - 1.0);
    }

    int num_freqs = nfft / 2 + 1;
    double *fft_freqs = (double*)malloc(num_freqs * sizeof(double));
    for (int k = 0; k < num_freqs; k++) {
        fft_freqs[k] = (double)k * sample_rate / nfft;
    }

    /* Mel filterbank weights B[num_bands][num_freqs] */
    float *mel_weights = (float*)calloc(num_bands * num_freqs, sizeof(float));
    for (int b = 0; b < num_bands; b++) {
        double lo = mel_pts[b];
        double ce = mel_pts[b + 1];
        double hi = mel_pts[b + 2];
        for (int k = 0; k < num_freqs; k++) {
            double f = fft_freqs[k];
            double up = (f - lo) / (ce - lo + 1e-9);
            double down = (hi - f) / (hi - ce + 1e-9);
            double w = (up < down) ? up : down;
            if (w < 0.0) w = 0.0;
            mel_weights[b * num_freqs + k] = (float)w;
        }
    }
    free(fft_freqs);

    /* Precompute Hann window */
    Complex *stft_in = (Complex*)malloc(nfft * sizeof(Complex));
    double *hann_win = (double*)malloc(nfft * sizeof(double));
    for (int i = 0; i < nfft; i++) {
        hann_win[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / nfft));
    }

    /* Log-Mel spectrogram M[num_bands][num_frames] */
    float *log_mel = (float*)malloc(num_bands * num_frames * sizeof(float));
    float *onset_flux = (float*)calloc(num_frames, sizeof(float));

    /* Power spectrum buffer */
    float *power_spec = (float*)malloc(num_freqs * sizeof(float));

    for (int f = 0; f < num_frames; f++) {
        size_t off = f * hop;
        for (int i = 0; i < nfft; i++) {
            size_t idx = off + i;
            stft_in[i].r = (idx < total_samples) ? (mono_float[idx] * hann_win[i]) : 0.0;
            stft_in[i].i = 0.0;
        }
        fft_radix2(stft_in, nfft, 0);

        for (int k = 0; k < num_freqs; k++) {
            double r = stft_in[k].r;
            double im = stft_in[k].i;
            power_spec[k] = (float)(r * r + im * im);
        }

        /* Matrix multiplication B @ P */
        for (int b = 0; b < num_bands; b++) {
            double dot = 0.0;
            const float *w_row = &mel_weights[b * num_freqs];
            for (int k = 0; k < num_freqs; k++) {
                dot += w_row[k] * power_spec[k];
            }
            float log_val = (float)log1p(dot);
            log_mel[b * num_frames + f] = log_val;

            if (f > 0) {
                float prev = log_mel[b * num_frames + (f - 1)];
                float diff = log_val - prev;
                if (diff > 0.0f) {
                    onset_flux[f] += diff;
                }
            }
        }
    }

    free(power_spec);
    free(hann_win);
    free(stft_in);
    free(mel_weights);

    /* Normalize onset flux envelope */
    double onset_sum = 0.0;
    for (int f = 0; f < num_frames; f++) onset_sum += onset_flux[f];
    double onset_mean = onset_sum / num_frames;
    for (int f = 0; f < num_frames; f++) onset_flux[f] -= (float)onset_mean;

    /* Detect pattern per active section */
    Pattern *patterns = (Pattern*)malloc(num_sections * sizeof(Pattern));
    int valid_patterns = 0;

    for (int s = 0; s < num_sections; s++) {
        double sec_start = sections[s].start;
        double sec_end = sections[s].end;
        double sec_dur = sec_end - sec_start;

        int i0 = (int)floor(sec_start * fps + 0.5);
        int i1 = (int)floor(sec_end * fps + 0.5);
        if (i0 < 0) i0 = 0;
        if (i1 > num_frames) i1 = num_frames;
        int n_sec = i1 - i0;
        if (n_sec < 4) continue;

        double max_lag = 30.0;
        if (sec_dur / 2.0 < max_lag) max_lag = sec_dur / 2.0;
        if (max_lag < 0.5) max_lag = 0.5;

        int maxlag = (int)floor(max_lag * fps);
        if (maxlag > n_sec - 2) maxlag = n_sec - 2;
        if (maxlag < 2) continue;

        /* Extract onset segment and normalize */
        double *od = (double*)malloc(n_sec * sizeof(double));
        double od_sum = 0.0;
        for (int i = 0; i < n_sec; i++) {
            od[i] = onset_flux[i0 + i];
            od_sum += od[i];
        }
        double od_mean = od_sum / n_sec;
        double od_norm_sq = 0.0;
        for (int i = 0; i < n_sec; i++) {
            od[i] -= od_mean;
            od_norm_sq += od[i] * od[i];
        }
        double od_norm = sqrt(od_norm_sq) + 1e-12;
        for (int i = 0; i < n_sec; i++) od[i] /= od_norm;

        /* Autocorrelation via FFT */
        int nff = next_pow2(2 * n_sec - 1);
        Complex *acf_buf = (Complex*)calloc(nff, sizeof(Complex));
        for (int i = 0; i < n_sec; i++) acf_buf[i].r = od[i];
        fft_radix2(acf_buf, nff, 0);

        for (int i = 0; i < nff; i++) {
            double r = acf_buf[i].r;
            double im = acf_buf[i].i;
            acf_buf[i].r = r * r + im * im;
            acf_buf[i].i = 0.0;
        }
        fft_radix2(acf_buf, nff, 1);

        double *ac_onset = (double*)malloc((maxlag + 1) * sizeof(double));
        double ac0 = acf_buf[0].r / n_sec;
        if (ac0 < 1e-12) ac0 = 1.0;

        for (int lag = 0; lag <= maxlag; lag++) {
            int count = n_sec - lag;
            if (count < 1) count = 1;
            double unbiased = (acf_buf[lag].r / count);
            ac_onset[lag] = unbiased / ac0;
        }

        free(acf_buf);
        free(od);

        /* Find onset ACF peaks */
        double p_lags[16] = {0};
        double p_vals[16] = {0};
        int num_peaks = find_acf_peaks(ac_onset, maxlag + 1, fps, 0.5, max_lag, p_lags, p_vals, 10);
        free(ac_onset);

        double best_loop = 2.134; /* default standard loop fallback */
        if (num_peaks > 0) {
            /* Select candidate loop with highest prominence and musical bar fit */
            best_loop = p_lags[0];
            /* Prefer shortest prominent candidate if an integer harmonic is present */
            for (int k = 1; k < num_peaks; k++) {
                if (p_vals[k] > 0.15) {
                    double r = best_loop / p_lags[k];
                    int r_int = (int)floor(r + 0.5);
                    if (r_int >= 2 && r_int <= 4 && fabs(r - r_int) < 0.05) {
                        if (p_vals[k] >= 0.7 * p_vals[0]) {
                            best_loop = p_lags[k];
                        }
                    }
                }
            }
        }

        int bars = 1;
        estimate_bars_and_length(best_loop, &bars, &best_loop);

        Pattern *p = &patterns[valid_patterns];
        p->id = valid_patterns;
        snprintf(p->name, sizeof(p->name), "Pattern %d", valid_patterns + 1);
        snprintf(p->color, sizeof(p->color), "%s", PALETTE_COLORS[valid_patterns % NUM_PALETTE_COLORS]);
        p->start = sec_start;
        p->end = sec_end;
        p->length = floor(best_loop * 1000.0 + 0.5) / 1000.0;
        p->bars = bars;
        p->segments = build_segments(p->start, p->end, p->length, &p->num_segments);

        pthread_mutex_lock(&g_console_mutex);
        printf("  -> %s: %.2f - %.2f s | loop: %.3f s (%d bar%s) | %d segments\n",
               p->name, p->start, p->end, p->length, p->bars, (p->bars == 1 ? "" : "s"), p->num_segments);
        pthread_mutex_unlock(&g_console_mutex);

        valid_patterns++;
    }

    free(sections);
    free(onset_flux);
    free(log_mel);

    result->num_patterns = valid_patterns;

    /* Compute Waveform Tiers */
    /* Coarse: step 4096 */
    size_t coarse_bins = 0;
    int16_t *inter_coarse = compute_minmax_bins(mono_pcm16, total_samples, COARSE_STEP, &coarse_bins);

    /* Write summary.js */
    size_t coarse_bytes = coarse_bins * 2 * sizeof(int16_t);
    size_t b64_coarse_len = 4 * ((coarse_bytes + 2) / 3);
    char *b64_coarse = (char*)malloc(b64_coarse_len + 1);
    if (b64_coarse && inter_coarse) {
        base64_encode((const unsigned char*)inter_coarse, coarse_bytes, b64_coarse);
    }

    /* Fine tiers: steps 8, 64, 512 */
    size_t level_bins[NUM_LEVELS] = {0};
    size_t level_chunks[NUM_LEVELS] = {0};

    for (int li = 0; li < NUM_LEVELS; li++) {
        int step = LEVELS_STEPS[li];
        size_t nb = 0;
        int16_t *tier_inter = compute_minmax_bins(mono_pcm16, total_samples, step, &nb);
        level_bins[li] = nb;
        if (tier_inter) {
            write_tier_level(tier_inter, nb, step, li, data_full_dir, &level_chunks[li]);
            free(tier_inter);
        }
    }

    /* Serialize PATTERN_META JSON */
    char summary_js_path[8192];
    snprintf(summary_js_path, sizeof(summary_js_path), "%s/summary.js", data_full_dir);
    FILE *f_sum = fopen(summary_js_path, "w");
    if (f_sum) {
        fprintf(f_sum, "window.PATTERN_META = {\"file\":\"%s\",\"sampleRate\":%u,\"channels\":%u,\"totalSamples\":%zu,\"duration\":%.6f,\"dataDir\":\"%s\",\"chunkBins\":%d,\"coarseStep\":%d,\"coarseBins\":%zu,\"levels\":[",
                filename, sample_rate, channels, total_samples, duration, data_rel_dir, CHUNK_BINS, COARSE_STEP, coarse_bins);
        for (int li = 0; li < NUM_LEVELS; li++) {
            fprintf(f_sum, "%s{\"step\":%d,\"minmax\":true,\"bins\":%zu}",
                    (li > 0 ? "," : ""), LEVELS_STEPS[li], level_bins[li]);
        }
        fprintf(f_sum, "],\"patterns\":[");
        for (int i = 0; i < valid_patterns; i++) {
            Pattern *p = &patterns[i];
            fprintf(f_sum, "%s{\"id\":%d,\"name\":\"%s\",\"color\":\"%s\",\"start\":%.3f,\"end\":%.3f,\"length\":%.3f,\"bars\":%d,\"segments\":[",
                    (i > 0 ? "," : ""), p->id, p->name, p->color, p->start, p->end, p->length, p->bars);
            for (int j = 0; j < p->num_segments; j++) {
                Segment *sg = &p->segments[j];
                fprintf(f_sum, "%s{\"start\":%.3f,\"end\":%.3f,\"partial\":%s}",
                        (j > 0 ? "," : ""), sg->start, sg->end, (sg->partial ? "true" : "false"));
            }
            fprintf(f_sum, "]}");
        }
        fprintf(f_sum, "]};\n");
        fprintf(f_sum, "window.PATTERN_COARSE = \"%s\";\n", b64_coarse ? b64_coarse : "");
        fclose(f_sum);
    }
    if (b64_coarse) free(b64_coarse);
    if (inter_coarse) free(inter_coarse);

    /* Generate Pattern Cards HTML */
    size_t cards_buf_cap = 65536;
    char *cards_html = (char*)malloc(cards_buf_cap);
    cards_html[0] = '\0';
    size_t cards_len = 0;

    for (int i = 0; i < valid_patterns; i++) {
        Pattern *p = &patterns[i];
        char meta_text[256];
        char pstart_str[32], pend_str[32];
        fmt_precise(p->start, pstart_str, sizeof(pstart_str));
        fmt_precise(p->end, pend_str, sizeof(pend_str));
        snprintf(meta_text, sizeof(meta_text), "%s &ndash; %s &middot; %.3f s &middot; %d bar%s &middot; %d segments",
                 pstart_str, pend_str, p->length, p->bars, (p->bars == 1 ? "" : "s"), p->num_segments);

        char card_header[2048];
        snprintf(card_header, sizeof(card_header),
                 "<div class=\"pcard\" style=\"--c:%s\">"
                 "<div class=\"phead\"><span class=\"pdot\"></span><span class=\"pname\">%s</span>"
                 "<span class=\"pmeta\">%s</span>"
                 "<span class=\"pactions\"><button class=\"mini\" onclick=\"zoomPattern(%d)\">Zoom</button>"
                 "<button class=\"mini\" onclick=\"playPattern(%d)\">Play</button></span></div>"
                 "<div class=\"segrow\">",
                 p->color, p->name, meta_text, i, i);

        size_t needed = cards_len + strlen(card_header) + p->num_segments * 256 + 64;
        if (needed >= cards_buf_cap) {
            cards_buf_cap = needed * 2;
            cards_html = (char*)realloc(cards_html, cards_buf_cap);
        }
        strcat(cards_html, card_header);
        cards_len = strlen(cards_html);

        for (int j = 0; j < p->num_segments; j++) {
            Segment *sg = &p->segments[j];
            double dur = sg->end - sg->start;
            char s_start[32], s_end[32];
            fmt_precise(sg->start, s_start, sizeof(s_start));
            fmt_precise(sg->end, s_end, sizeof(s_end));

            char chip[512];
            snprintf(chip, sizeof(chip),
                     "<button class=\"seg%s\" onclick=\"playSegment(%d,%d)\" title=\"Segment %d &middot; %s &ndash; %s &middot; %.3f s%s\">%d%s</button>",
                     (sg->partial ? " partial" : ""), i, j, j + 1, s_start, s_end, dur,
                     (sg->partial ? " &middot; partial" : ""), j + 1, (sg->partial ? "*" : ""));
            strcat(cards_html, chip);
        }
        strcat(cards_html, "</div></div>\n");
        cards_len = strlen(cards_html);
    }

    /* Build and write <basename>.html */
    char report_title[512];
    snprintf(report_title, sizeof(report_title), "Pattern Analysis - %s", filename);

    char meta_line[1024];
    snprintf(meta_line, sizeof(meta_line),
             "<span><strong>Source:</strong> %s</span>"
             "<span><strong>Duration:</strong> %.2fs</span>"
             "<span><strong>Sample Rate:</strong> %u Hz</span>"
             "<span><strong>Channels:</strong> %u (%s)</span>"
             "<span><strong>Patterns:</strong> %d</span>",
             filename, duration, sample_rate, channels,
             (channels == 1 ? "Mono" : "Stereo"), valid_patterns);

    char summary_script_ref[2048];
    snprintf(summary_script_ref, sizeof(summary_script_ref), "%s/summary.js", data_rel_dir);

    char *h1 = str_replace(g_html_template, "__TITLE__", report_title);
    char *h2 = str_replace(h1, "__META_LINE__", meta_line); free(h1);
    char *h3 = str_replace(h2, "__AUDIO_SRC__", filename); free(h2);
    char *h4 = str_replace(h3, "__PATTERN_CARDS__", cards_html); free(h3);
    char *h5 = str_replace(h4, "__DATA_SCRIPT__", summary_script_ref); free(h4);
    char *final_html = str_replace(h5, "__REPORT_JS__", g_report_js); free(h5);
    free(cards_html);

    char report_path[4096];
    snprintf(report_path, sizeof(report_path), "%s/%s.html", workspace_dir, basename);
    FILE *f_rep = fopen(report_path, "w");
    if (f_rep) {
        fputs(final_html, f_rep);
        fclose(f_rep);
    }
    free(final_html);

    /* Clean up pattern memory */
    for (int i = 0; i < valid_patterns; i++) {
        if (patterns[i].segments) free(patterns[i].segments);
    }
    free(patterns);

    free(mono_float);
    free(mono_pcm16);

    double elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    result->elapsed_sec = elapsed;
    result->success = 1;

    pthread_mutex_lock(&g_console_mutex);
    printf("Completed %s in %.2f s -> %s.html\n", filename, elapsed, basename);
    pthread_mutex_unlock(&g_console_mutex);

    return 1;
}

/* Worker thread context */
typedef struct {
    char **file_list;
    int num_files;
    char workspace_dir[2048];
    FileResult *results;
    int *next_file_idx;
    pthread_mutex_t *queue_mutex;
} WorkerArgs;

static void* worker_thread_func(void *arg) {
    WorkerArgs *w = (WorkerArgs*)arg;
    while (1) {
        int idx = -1;
        pthread_mutex_lock(w->queue_mutex);
        if (*w->next_file_idx < w->num_files) {
            idx = (*w->next_file_idx)++;
        }
        pthread_mutex_unlock(w->queue_mutex);

        if (idx < 0) break;
        process_audio_file(w->file_list[idx], w->workspace_dir, &w->results[idx]);
    }
    return NULL;
}

/* Helper functions */
static void wait_for_keypress(void) {
    printf("\nPress ENTER to exit...\n");
    fflush(stdout);
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    system("pause");
#else
    if (isatty(fileno(stdin))) {
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
        if (c == '\n') getchar();
    } else {
        FILE *tty = fopen("/dev/tty", "r");
        if (tty) {
            fgetc(tty);
            fclose(tty);
        }
    }
#endif
}

static void trim_string(char *str) {
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

static void get_executable_directory(char *out_dir, size_t out_size) {
    if (!out_dir || out_size == 0) return;
    char exe_path[4096] = {0};
#if defined(_WIN32) || defined(_WIN64)
    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)) > 0) {
        char *last_slash = strrchr(exe_path, '/');
        char *last_backslash = strrchr(exe_path, '\\');
        char *last_sep = (last_backslash && (!last_slash || last_backslash > last_slash)) ? last_backslash : last_slash;
        if (last_sep) {
            *last_sep = '\0';
            snprintf(out_dir, out_size, "%s", exe_path);
            return;
        }
    }
#else
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *last_slash = strrchr(exe_path, '/');
        if (last_slash) {
            *last_slash = '\0';
            snprintf(out_dir, out_size, "%s", exe_path);
            return;
        }
    }
#endif
    snprintf(out_dir, out_size, ".");
}

static void ensure_dir_exists(const char *path) {
    if (!path || !*path) return;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char orig = tmp[i];
            tmp[i] = '\0';
            MKDIR(tmp);
            tmp[i] = orig;
        }
    }
    MKDIR(tmp);
}

static int is_valid_wav_file(const char *filename) {
    if (!filename) return 0;
    size_t len = strlen(filename);
    if (len < 4) return 0;
    const char *ext = filename + len - 4;
    return (strcasecmp(ext, ".wav") == 0);
}

static void scan_directory_for_wavs(const char *dir_path, char ***file_list, int *num_files, int *capacity) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_name[0] == '.') continue;
        if (is_valid_wav_file(dir->d_name)) {
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, dir->d_name);
            if (*num_files >= *capacity) {
                *capacity = (*capacity == 0) ? 16 : (*capacity * 2);
                *file_list = (char**)realloc(*file_list, (*capacity) * sizeof(char*));
            }
            (*file_list)[(*num_files)++] = strdup(full_path);
        }
    }
    closedir(d);
}

static int get_cpu_cores(void) {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors > 0 ? (int)sysinfo.dwNumberOfProcessors : 4;
#else
    long procs = sysconf(_SC_NPROCESSORS_ONLN);
    return procs > 0 ? (int)procs : 4;
#endif
}

int main(int argc, char *argv[]) {
    printf("====================================================\n");
    printf("         ANALYZE PALETTES - AUDIO PATTERNS          \n");
    printf("====================================================\n\n");

    char **file_list = NULL;
    int num_files = 0;
    int capacity = 0;
    int have_path_args = 0;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (strncmp(argv[i], "--", 2) == 0) {
                continue;
            }
            char path[2048];
            snprintf(path, sizeof(path), "%s", argv[i]);
            trim_string(path);
            have_path_args = 1;

            DIR *d = opendir(path);
            if (d) {
                closedir(d);
                scan_directory_for_wavs(path, &file_list, &num_files, &capacity);
            } else if (is_valid_wav_file(path)) {
                if (num_files >= capacity) {
                    capacity = (capacity == 0) ? 16 : capacity * 2;
                    file_list = (char**)realloc(file_list, capacity * sizeof(char*));
                }
                file_list[num_files++] = strdup(path);
            }
        }
    }

    if (!have_path_args) {
        /* Double-clicked execution: scan executable's directory */
        char exe_dir[2048] = {0};
        get_executable_directory(exe_dir, sizeof(exe_dir));
        printf("Double-click mode: scanning directory '%s' for WAV files...\n", exe_dir);
        scan_directory_for_wavs(exe_dir, &file_list, &num_files, &capacity);

        if (num_files == 0 && strcmp(exe_dir, ".") != 0) {
            scan_directory_for_wavs(".", &file_list, &num_files, &capacity);
        }

        if (num_files == 0) {
            char input_path[2048] = {0};
            printf("No WAV files found in executable folder.\n");
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
                } else if (is_valid_wav_file(input_path)) {
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

    /* Target output workspace directory:
     * By default, create pattern_reports directory next to first audio file or executable. */
    char workspace_dir[2048] = {0};
    const char *first_file = file_list[0];
    const char *last_slash = strrchr(first_file, '/');
    const char *last_backslash = strrchr(first_file, '\\');
    const char *last_sep = (last_backslash && (!last_slash || last_backslash > last_slash)) ? last_backslash : last_slash;
    if (last_sep) {
        size_t dlen = last_sep - first_file;
        char parent_dir[2048] = {0};
        strncpy(parent_dir, first_file, dlen);
        parent_dir[dlen] = '\0';
        snprintf(workspace_dir, sizeof(workspace_dir), "%s/pattern_reports", parent_dir);
    } else {
        snprintf(workspace_dir, sizeof(workspace_dir), "pattern_reports");
    }
    ensure_dir_exists(workspace_dir);

    int cpu_cores = get_cpu_cores();
    int num_threads = (num_files < cpu_cores) ? num_files : cpu_cores;
    if (num_threads < 1) num_threads = 1;

    printf("Analysis Batch Overview:\n");
    printf("  Total Audio Files:   %d\n", num_files);
    printf("  Workspace Output:    %s\n", workspace_dir);
    printf("  Worker Threads:      %d (CPU cores: %d)\n\n", num_threads, cpu_cores);

    FileResult *results = (FileResult*)calloc(num_files, sizeof(FileResult));
    int next_file_idx = 0;
    pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;

    pthread_t *threads = (pthread_t*)malloc(num_threads * sizeof(pthread_t));
    WorkerArgs *wargs = (WorkerArgs*)malloc(num_threads * sizeof(WorkerArgs));

    for (int t = 0; t < num_threads; t++) {
        wargs[t].file_list = file_list;
        wargs[t].num_files = num_files;
        snprintf(wargs[t].workspace_dir, sizeof(wargs[t].workspace_dir), "%s", workspace_dir);
        wargs[t].results = results;
        wargs[t].next_file_idx = &next_file_idx;
        wargs[t].queue_mutex = &queue_mutex;
        pthread_create(&threads[t], NULL, worker_thread_func, &wargs[t]);
    }

    for (int t = 0; t < num_threads; t++) {
        pthread_join(threads[t], NULL);
    }

    free(threads);
    free(wargs);

    /* Generate index.html */
    size_t cards_cap = 65536;
    char *index_cards = (char*)malloc(cards_cap);
    index_cards[0] = '\0';
    size_t ic_len = 0;

    int total_success = 0;
    for (int i = 0; i < num_files; i++) {
        if (!results[i].success) continue;
        total_success++;
        char dur_str[32];
        fmt_precise(results[i].duration, dur_str, sizeof(dur_str));

        char card[1024];
        snprintf(card, sizeof(card),
                 "<a class=\"card\" href=\"%s.html\"><strong>%s</strong>"
                 "<span>%s &middot; %d pattern%s</span>"
                 "<span class=\"btn\">Open report</span></a>\n",
                 results[i].basename, results[i].filename,
                 dur_str, results[i].num_patterns,
                 (results[i].num_patterns == 1 ? "" : "s"));

        if (ic_len + strlen(card) + 32 >= cards_cap) {
            cards_cap *= 2;
            index_cards = (char*)realloc(index_cards, cards_cap);
        }
        strcat(index_cards, card);
        ic_len = strlen(index_cards);
    }

    char *idx1 = str_replace(g_index_template, "__INDEX_TITLE__", "Audio Pattern Reports");
    char *idx2 = str_replace(idx1, "__INDEX_DESC__", "Full waveforms with detected pattern regions, loop segments, zooming, playback and playhead jumping.");
    free(idx1);
    char *final_index = str_replace(idx2, "__CARDS__", index_cards);
    free(idx2);
    free(index_cards);

    char index_path[4096];
    snprintf(index_path, sizeof(index_path), "%s/index.html", workspace_dir);
    FILE *f_idx = fopen(index_path, "w");
    if (f_idx) {
        fputs(final_index, f_idx);
        fclose(f_idx);
    }
    free(final_index);

    printf("\n====================================================\n");
    printf("Batch Complete: %d / %d file(s) analyzed successfully.\n", total_success, num_files);
    printf("Interactive reports generated in: %s\n", workspace_dir);
    printf("Index report: %s/index.html\n", workspace_dir);
    printf("====================================================\n");

    for (int i = 0; i < num_files; i++) {
        free(file_list[i]);
    }
    free(file_list);
    free(results);

    wait_for_keypress();
    return (total_success == num_files) ? 0 : 1;
}
