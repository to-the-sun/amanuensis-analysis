/*
 * flux_pitch_separator.c
 *
 * Perfect Reconstruction Pitch Stem Separator - Spectral Flux Focus
 *
 * Focuses strictly on deriving per-pitch-stem transience flux envelopes,
 * applying noise gating to the flux envelope (0.009 threshold), identifying
 * peak and trough markers, generating modified flux envelopes via backward comparison,
 * exporting companion _flux.wav and _modified_flux.wav files to [flux]/,
 * and producing an interactive HTML report in [flux]/index.html.
 *
 * Does NOT perform cumulative transience analysis, non-tonal injection,
 * or noise gating on main pitch stems, and does NOT produce main pitch stem WAV files.
 * Reconstructed STFT stem audio is freed immediately after deriving raw spectral flux.
 */

#define _POSIX_C_SOURCE 200809L

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

const char *NOTE_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

double midi_to_frequency(int midi_pitch);
static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

void apply_flux_noise_gate(double *env, uint32_t total_samples, double threshold) {
    for (uint32_t i = 0; i < total_samples; i++) {
        if (env[i] < threshold) {
            env[i] = 0.0;
        }
    }
}

void modify_flux_envelope_channel(double *env, uint32_t total_samples) {
    if (total_samples < 3) return;

    apply_flux_noise_gate(env, total_samples, 0.009);

    int curr_x = (int)total_samples - 1;
    int P_x = find_prev_peak(env, total_samples, curr_x);

    while (P_x >= 0) {
        double y_P = env[P_x];

        int C_x = find_prev_peak(env, total_samples, P_x - 1);
        int comp_idx = 0;

        while (C_x >= 0) {
            comp_idx++;
            double y_C = env[C_x];

            if (comp_idx == 1) {
                if (y_C > y_P) {
                    double dx = (double)(P_x - C_x);
                    if (dx > 0) {
                        for (int x = C_x; x <= P_x; x++) {
                            double alpha = (double)(x - C_x) / dx;
                            env[x] = y_C + alpha * (y_P - y_C);
                        }
                    }
                    break;
                }
            } else {
                if (y_C >= y_P) {
                    break;
                } else {
                    double dx = (double)(P_x - C_x);
                    if (dx <= 0) {
                        C_x = find_prev_peak(env, total_samples, C_x - 1);
                        continue;
                    }

                    int intermediary_peak_count = 0;
                    int intermediary_hill_reduced = 0;

                    int K_x = find_prev_peak(env, total_samples, P_x - 1);
                    while (K_x > C_x) {
                        intermediary_peak_count++;
                        double alpha_K = (double)(K_x - C_x) / dx;
                        double line_y_K = y_C + alpha_K * (y_P - y_C);

                        if (env[K_x] > line_y_K) {
                            int t_L = find_trough_left(env, total_samples, K_x);
                            int t_R = find_trough_right(env, total_samples, K_x);

                            double y_TL = env[t_L];
                            double y_TR = env[t_R];
                            double hill_dx = (double)(t_R - t_L);

                            if (hill_dx > 0) {
                                for (int x = t_L; x <= t_R; x++) {
                                    double hill_alpha = (double)(x - t_L) / hill_dx;
                                    env[x] = y_TL + hill_alpha * (y_TR - y_TL);
                                }
                            } else {
                                env[t_L] = y_TL;
                            }
                            intermediary_hill_reduced = 1;
                        }
                        K_x = find_prev_peak(env, total_samples, K_x - 1);
                    }

                    if (intermediary_hill_reduced) {
                        C_x = find_prev_peak(env, total_samples, C_x - 1);
                        continue;
                    } else if (intermediary_peak_count > 0) {
                        break;
                    }
                }
            }

            C_x = find_prev_peak(env, total_samples, C_x - 1);
        }

        curr_x = P_x - 1;
        P_x = find_prev_peak(env, total_samples, curr_x);
    }
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
            arr = (int*)realloc(arr, capacity * sizeof(int));
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
    const FluxStem *stems,
    int stem_count
) {
    char html_path[4096];
    snprintf(html_path, sizeof(html_path), "%s/index.html", flux_dir);

    FILE *f = fopen(html_path, "w");
    if (!f) {
        printf("Error: Could not open Flux HTML report file '%s' for writing.\n", html_path);
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
    fprintf(f, "  .btn { background: var(--accent); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; cursor: pointer; transition: opacity 0.2s; }\n");
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
    fprintf(f, "    <button class=\"btn\" id=\"resetZoomBtn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <label class=\"toggle-label\"><input type=\"checkbox\" id=\"syncZoomCheck\" checked> Sync Zoom Across Rows</label>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", duration, duration);
    fprintf(f, "    <div class=\"legend\">\n");
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
        fprintf(f, "      <audio controls src=\"%s\" preload=\"metadata\"></audio>\n", st->mod_filename);
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
        fprintf(f, "      file: \"%s\"\n", st->mod_filename);
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
    printf("  Flux HTML Report generated: %s/index.html\n", flux_dir);
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

int process_audio(const char *input_path) {
    clock_t start_clock = clock();

    init_fft_tables();

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

    // Free raw input data buffer early
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
        printf("Error: Could not allocate memory for pitch_bins.\n");
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input);
        free(cola_norm);
        free(wav);
        return 0;
    }

    for (int k = 0; k <= FFT_SIZE / 2; k++) {
        double freq = (double)k * sample_rate / FFT_SIZE;
        int p = freq_to_midi_pitch(freq);
        bin_pitch[k] = p;
        pitch_bins[p][pitch_bin_count[p]++] = k;
    }

    printf("Executing STFT COLA Pitch Separation (8192 FFT, 75%% Overlap)...\n");

    size_t num_bins_per_frame = FFT_SIZE / 2 + 1;
    size_t total_stft_entries = (size_t)num_channels * num_frames * num_bins_per_frame;

    ComplexFloat *stft_data = (ComplexFloat*)malloc(total_stft_entries * sizeof(ComplexFloat));
    if (!stft_data) {
        printf("Error: Could not allocate memory for STFT matrix.\n");
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input);
        free(cola_norm);
        free(pitch_bins);
        free(wav);
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
            if (current_step % 100 == 0 || current_step == total_stft_steps) {
                double elapsed = (double)(clock() - stft_start_clock) / CLOCKS_PER_SEC;
                print_progress_bar("Analyzing STFT Frames", current_step, total_stft_steps, elapsed);
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
    printf("\n");

    for (uint32_t c = 0; c < num_channels; c++) {
        free(padded_input[c]);
    }
    free(padded_input);
    padded_input = NULL;

    char dir[1024], basename[512];
    get_filepath_components(input_path, dir, basename, sizeof(basename));

    char stem_dir[4096];
    snprintf(stem_dir, sizeof(stem_dir), "%s/%s_pitch_stems", dir, basename);
    MKDIR(stem_dir);

    char flux_dir[4096];
    snprintf(flux_dir, sizeof(flux_dir), "%s/[flux]", stem_dir);
    MKDIR(flux_dir);

    printf("\nDeriving spectral flux envelopes per pitch stem...\n");

    Complex *pitch_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));

    FluxStem *flux_stems = NULL;
    int audible_count = 0;
    int audible_capacity = 0;

    for (int p = 0; p < NUM_MIDI_PITCHES; p++) {
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

            // Apply noise gate (0.009) to a temporary buffer for peak/trough detection
            double *gate_temp = (double*)malloc(total_channel_samples * sizeof(double));
            if (gate_temp) {
                memcpy(gate_temp, raw_flux_buf, total_channel_samples * sizeof(double));
                for (size_t k = 0; k < total_channel_samples; k++) {
                    if (gate_temp[k] < 0.009) {
                        gate_temp[k] = 0.0;
                    }
                }

                double *ch0_flux = (double*)malloc(total_samples * sizeof(double));
                if (ch0_flux) {
                    for (uint32_t s = 0; s < total_samples; s++) {
                        ch0_flux[s] = gate_temp[s * num_channels];
                    }
                    detect_all_peaks(ch0_flux, total_samples, &stem->peaks, &stem->num_peaks);
                    detect_all_troughs(ch0_flux, total_samples, &stem->troughs, &stem->num_troughs);
                    free(ch0_flux);
                }
                free(gate_temp);
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

    printf("Spectral Flux Stem Export Table:\n");
    printf("-----------------------------------------------------------------------------------\n");
    printf(" MIDI | Note | Freq (Hz)  | Peak Flux | RMS Flux | Peaks | Troughs | Flux Files Exported\n");
    printf("-----------------------------------------------------------------------------------\n");

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
            snprintf(mod_flux_filepath, sizeof(mod_flux_filepath), "%s/[flux]/%s",
                     stem_dir, stem->mod_filename);
            write_wav_16bit(mod_flux_filepath, sample_rate, num_channels, total_samples, stem->mod_flux);

            printf(" %03d  | %-4s | %8.2f Hz | %9.5f | %8.5f | %5d | %7d | %s_pitch_%03d_%s_[flux|modified_flux].wav\n",
                   p, stem->note_name, midi_to_frequency(p), stem->max_flux_peak, stem->rms_flux,
                   stem->num_peaks, stem->num_troughs, basename, p, stem->note_name);
            exported_count++;
        }
    }
    printf("-----------------------------------------------------------------------------------\n\n");

    // Construct Flux HTML report in [flux] directory
    if (flux_stems) {
        generate_flux_html_report(
            flux_dir,
            basename,
            sample_rate,
            num_channels,
            total_samples,
            audio_duration,
            flux_stems,
            audible_count
        );

        // Write top-level redirecting index.html in stem_dir
        char redirect_path[4096];
        snprintf(redirect_path, sizeof(redirect_path), "%s/index.html", stem_dir);
        FILE *f_redir = fopen(redirect_path, "w");
        if (f_redir) {
            fprintf(f_redir, "<!DOCTYPE html>\n<html>\n<head>\n");
            fprintf(f_redir, "<meta http-equiv=\"refresh\" content=\"0; url=[flux]/index.html\">\n");
            fprintf(f_redir, "<title>Spectral Flux Report Redirect</title>\n</head>\n<body>\n");
            fprintf(f_redir, "<p>Redirecting to <a href=\"[flux]/index.html\">Spectral Flux Report</a>...</p>\n");
            fprintf(f_redir, "</body>\n</html>\n");
            fclose(f_redir);
        }

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

    printf("====================================================\n");
    printf("               PROCESSING SUMMARY                   \n");
    printf("====================================================\n");
    printf("  Input File:          %s\n", basename);
    printf("  Stems Directory:     %s/\n", stem_dir);
    printf("  Flux Stems Exported: %d raw/modified flux WAV pairs\n", exported_count);
    printf("  Total Time Elapsed:  %.2f seconds\n", total_elapsed);
    printf("  Processing Speed:    %.1fx Real-Time\n", speed_ratio);
    printf("  Status:              SPECTRAL FLUX DERIVATION COMPLETE\n");
    printf("====================================================\n");

    free(fft_frame);
    free(pitch_frame);
    free(stft_data);
    free(frame_pitch_active);
    free(cola_norm);
    free(pitch_bins);
    free(wav);

    return 1;
}

int main(int argc, char *argv[]) {
    printf("====================================================\n");
    printf("         SPECTRAL FLUX PITCH STEM SEPARATOR         \n");
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
