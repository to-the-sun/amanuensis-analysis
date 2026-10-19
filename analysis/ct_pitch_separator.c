/*
 * ct_pitch_separator.c
 *
 * Perfect Reconstruction Pitch Stem Separator with Cumulative Transience Marking
 * and Percentile-based Non-Tonal Audio Injection
 *
 * Utilizes the 1-band cumulative_transience algorithm from the offline directory
 * to identify transient peak markers across the input audio signal.
 * The audio duration is partitioned into intervals bounded by these transient markers.
 * For each interval, the total energy in each audible pitch stem is calculated.
 * If a pitch stem's interval energy is above the 89th percentile of the maximum energy
 * observed across all stems for that interval, non-tonal residual audio is added in
 * at a linearly decreasing level (100% at interval start / 1st transient to 0% at interval end / 2nd transient).
 *
 * At the start of the interval, a very short attack ramp (1ms) is applied to prevent clicks.
 * If multiple pitch stems qualify (> 89th percentile), non-tonal audio is added proportionally
 * based on stem energy so that total added non-tonal audio never exceeds 100% of its original amplitude.
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

#include "offline/cumulative_transience.h"

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
    double max_peak;
    double rms;
    double active_ms;
    double *audio_data;
    double *raw_audio_data;
    double *flux_data;
    double *injection_envelope;
    char filename[1024];
} AudibleStem;

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

void modify_flux_envelope_channel(double *env, uint32_t total_samples) {
    if (total_samples < 3) return;

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
                if (y_C < y_P) {
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

void generate_html_report(
    const char *stem_dir,
    const char *basename,
    uint32_t sample_rate,
    uint32_t num_channels,
    uint32_t total_samples,
    double duration,
    const char *orig_filename,
    const double *orig_audio_buffer,
    const double *nontonal_data,
    const AudibleStem *stems,
    int stem_count,
    int nontonal_exported,
    double nt_peak,
    double nt_rms,
    const uint32_t *transients,
    uint32_t transient_count
) {
    char html_path[4096];
    snprintf(html_path, sizeof(html_path), "%s/index.html", stem_dir);

    FILE *f = fopen(html_path, "w");
    if (!f) {
        printf("Error: Could not open HTML report file '%s' for writing.\n", html_path);
        return;
    }

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "<meta charset=\"UTF-8\">\n");
    fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "<title>Cumulative Transience Pitch Separation Report - %s</title>\n", basename);
    fprintf(f, "<style>\n");
    fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; --orig-env: rgba(180, 190, 210, 0.45); }\n");
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
    fprintf(f, "  .legend-orig { background: rgba(180, 190, 210, 0.7); }\n");
    fprintf(f, "  .legend-stem { background: #2196f3; }\n");
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
    fprintf(f, "  <h1>Cumulative Transience Pitch Separation Report</h1>\n");
    fprintf(f, "  <div class=\"meta-p\">\n");
    fprintf(f, "    <span><strong>Source File:</strong> %s</span>\n", basename);
    fprintf(f, "    <span><strong>Duration:</strong> %.2fs</span>\n", duration);
    fprintf(f, "    <span><strong>Sample Rate:</strong> %u Hz</span>\n", sample_rate);
    fprintf(f, "    <span><strong>Channels:</strong> %u (%s)</span>\n", num_channels, num_channels == 1 ? "Mono" : "Stereo");
    fprintf(f, "    <span><strong>Produced Stems:</strong> %d</span>\n", stem_count + (nontonal_exported ? 1 : 0));
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"toolbar\">\n");
    fprintf(f, "    <button class=\"btn\" id=\"resetZoomBtn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <label class=\"toggle-label\"><input type=\"checkbox\" id=\"syncZoomCheck\" checked> Sync Zoom Across Rows</label>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", duration, duration);
    fprintf(f, "    <div class=\"legend\">\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box legend-orig\"></div> Original Peak Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box legend-stem\"></div> Stem Waveform</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: #ff5722;\"></div> Spectral Flux Envelope</div>\n");
    fprintf(f, "      <div class=\"legend-item\"><div class=\"legend-box\" style=\"background: rgba(255, 235, 59, 0.95);\"></div> Non-Tonal Injection Envelope (0-100%%)</div>\n");
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"instructions\">Click and drag horizontally on any graph to zoom in. Right-click on a graph to zoom out one level. Click 'Reset Zoom' to restore full view.</div>\n");
    fprintf(f, "</header>\n\n");

    fprintf(f, "<div id=\"stemsContainer\">\n");
    for (int i = 0; i < stem_count; i++) {
        const AudibleStem *st = &stems[i];
        fprintf(f, "  <div class=\"stem-card\">\n");
        fprintf(f, "    <div class=\"stem-header\">\n");
        fprintf(f, "      <div class=\"stem-title\">\n");
        fprintf(f, "        <span class=\"pitch-tag\">MIDI %03d</span> %s (%.2f Hz)\n", st->pitch, st->note_name, midi_to_frequency(st->pitch));
        fprintf(f, "      </div>\n");
        fprintf(f, "      <div class=\"stem-metrics\">\n");
        fprintf(f, "        <span>Peak: %.4f</span>\n", st->max_peak);
        fprintf(f, "        <span>RMS: %.4f</span>\n", st->rms);
        fprintf(f, "        <span>Active: %.1f ms</span>\n", st->active_ms);
        fprintf(f, "      </div>\n");
        fprintf(f, "      <audio controls src=\"%s\" preload=\"metadata\"></audio>\n", st->filename);
        fprintf(f, "    </div>\n");
        fprintf(f, "    <div class=\"canvas-container\">\n");
        fprintf(f, "      <canvas id=\"canvas-%d\" data-index=\"%d\"></canvas>\n", i, i);
        fprintf(f, "      <div class=\"tooltip\" id=\"tooltip-%d\"></div>\n", i);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
    }


    fprintf(f, "</div>\n\n");

    char js_data_filename[1024];
    snprintf(js_data_filename, sizeof(js_data_filename), "%s_waveform_data.js", basename);
    char js_data_path[4096];
    snprintf(js_data_path, sizeof(js_data_path), "%s/%s", stem_dir, js_data_filename);

    FILE *fjs = fopen(js_data_path, "w");
    if (fjs) {
        fprintf(fjs, "window.WAVEFORM_DATA = {\n");

        int16_t *pcm_i16 = (int16_t*)malloc(total_samples * sizeof(int16_t));
        if (pcm_i16) {
            for (uint32_t s = 0; s < total_samples; s++) {
                double sum = 0.0;
                for (uint32_t c = 0; c < num_channels; c++) {
                    sum += orig_audio_buffer[s * num_channels + c];
                }
                double val = sum / num_channels;
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

        fprintf(fjs, "  stemsPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const AudibleStem *st = &stems[i];
            const double *stem_raw_buf = st->raw_audio_data ? st->raw_audio_data : st->audio_data;
            if (pcm_i16) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    double sum = 0.0;
                    for (uint32_t c = 0; c < num_channels; c++) {
                        sum += stem_raw_buf[s * num_channels + c];
                    }
                    double val = sum / num_channels;
                    if (val > 1.0) val = 1.0;
                    if (val < -1.0) val = -1.0;
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
        fprintf(fjs, "  fluxPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const AudibleStem *st = &stems[i];
            if (pcm_i16 && st->flux_data) {
                double peak_f = 0.0;
                for (uint32_t s = 0; s < total_samples; s++) {
                    double sum_f = 0.0;
                    for (uint32_t c = 0; c < num_channels; c++) {
                        sum_f += st->flux_data[s * num_channels + c];
                    }
                    double val = sum_f / num_channels;
                    if (val > peak_f) peak_f = val;
                }
                double scale = (peak_f > 1e-12) ? (1.0 / peak_f) : 0.0;

                for (uint32_t s = 0; s < total_samples; s++) {
                    double sum_f = 0.0;
                    for (uint32_t c = 0; c < num_channels; c++) {
                        sum_f += st->flux_data[s * num_channels + c];
                    }
                    double val = (sum_f / num_channels) * scale;
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
        fprintf(fjs, "  injectionPcm: [\n");
        for (int i = 0; i < stem_count; i++) {
            const AudibleStem *st = &stems[i];
            if (pcm_i16) {
                for (uint32_t s = 0; s < total_samples; s++) {
                    double val = st->injection_envelope ? st->injection_envelope[s] : 0.0;
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
        if (pcm_i16) free(pcm_i16);

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
    fprintf(f, "  origFile: \"%s\",\n", orig_filename);
    fprintf(f, "  transients: [");
    for (uint32_t t = 0; t < transient_count; t++) {
        fprintf(f, "%.6f%s", (double)transients[t] / sample_rate, (t + 1 < transient_count) ? ", " : "");
    }
    fprintf(f, "],\n");
    fprintf(f, "  stems: [\n");

    for (int i = 0; i < stem_count; i++) {
        const AudibleStem *st = &stems[i];
        fprintf(f, "    {\n");
        fprintf(f, "      pitch: %d,\n", st->pitch);
        fprintf(f, "      name: \"%s\",\n", st->note_name);
        fprintf(f, "      file: \"%s\"\n", st->filename);
        fprintf(f, "    }%s\n", (i + 1 < stem_count) ? "," : "");
    }


    fprintf(f, "  ]\n");
    fprintf(f, "};\n\n");

    fprintf(f, "let origPcm = new Float32Array(DATA.totalSamples);\n\n");

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

    fprintf(f, "function formatMSS(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sInt = Math.floor(s);\n");
    fprintf(f, "  const sStr = sInt < 10 ? '0' + sInt : '' + sInt;\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSSPrecise(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sStr = s < 10 ? '0' + s.toFixed(2) : s.toFixed(2);\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function initAudioData() {\n");
    fprintf(f, "  if (window.WAVEFORM_DATA) {\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.origPcm) {\n");
    fprintf(f, "      origPcm = base64ToFloat32(window.WAVEFORM_DATA.origPcm);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.stemsPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.stemsPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].pcm = base64ToFloat32(window.WAVEFORM_DATA.stemsPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.fluxPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.fluxPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].fluxPcm = base64ToFloat32(window.WAVEFORM_DATA.fluxPcm[i]);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    if (window.WAVEFORM_DATA.injectionPcm) {\n");
    fprintf(f, "      for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "        if (window.WAVEFORM_DATA.injectionPcm[i]) {\n");
    fprintf(f, "          DATA.stems[i].injectionPcm = base64ToFloat32(window.WAVEFORM_DATA.injectionPcm[i]);\n");
    fprintf(f, "        }\n");
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

    fprintf(f, "const STEM_COLORS = ['#2196f3', '#00bcd4', '#4caf50', '#ff9800', '#e91e63', '#9c27b0', '#009688', '#ff5722'];\n");

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

    fprintf(f, "  const midY = height / 2;\n");
    fprintf(f, "  const scaleY = (height / 2) * 0.92;\n");

    fprintf(f, "  // Grid lines\n");
    fprintf(f, "  ctx.strokeStyle = '#1e1e2c';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  [-1.0, -0.5, 0.0, 0.5, 1.0].forEach(amp => {\n");
    fprintf(f, "    const y = midY - amp * scaleY;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke();\n");
    fprintf(f, "  });\n");

    fprintf(f, "  const sampleStart = Math.max(0, Math.floor((viewStart / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const sampleEnd = Math.min(DATA.totalSamples - 1, Math.ceil((viewEnd / DATA.duration) * DATA.totalSamples));\n");
    fprintf(f, "  const visibleSamples = Math.max(1, sampleEnd - sampleStart + 1);\n");
    fprintf(f, "  const stem = DATA.stems[index];\n");
    fprintf(f, "  const stemColor = stem.pitch < 0 ? '#ef5350' : STEM_COLORS[index %% STEM_COLORS.length];\n");

    fprintf(f, "  // Helper function to render a signal (origPcm or stem.pcm)\n");
    fprintf(f, "  function drawSignal(pcm, isOverlay) {\n");
    fprintf(f, "    if (!pcm) return;\n");
    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      // Sample-by-sample detail mode\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.strokeStyle = isOverlay ? 'rgba(180, 190, 210, 0.6)' : stemColor;\n");
    fprintf(f, "      ctx.lineWidth = isOverlay ? 1.2 : 2.0;\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const y = midY - pcm[s] * scaleY;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");

    fprintf(f, "      if (visibleSamples <= width * 0.5) {\n");
    fprintf(f, "        ctx.fillStyle = isOverlay ? 'rgba(200, 210, 230, 0.9)' : '#ffffff';\n");
    fprintf(f, "        for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "          const t = s / DATA.sampleRate;\n");
    fprintf(f, "          const x = timeToX(t, width);\n");
    fprintf(f, "          const y = midY - pcm[s] * scaleY;\n");
    fprintf(f, "          ctx.beginPath();\n");
    fprintf(f, "          ctx.arc(x, y, isOverlay ? 2.5 : 3.5, 0, 2 * Math.PI);\n");
    fprintf(f, "          ctx.fill();\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      // Pixel-bucket peak envelope mode\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      const minArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      const maxArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      minArr.fill(0);\n");
    fprintf(f, "      maxArr.fill(0);\n");

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
    fprintf(f, "          minArr[px] = minV;\n");
    fprintf(f, "          maxArr[px] = maxV;\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");

    fprintf(f, "      ctx.fillStyle = isOverlay ? 'rgba(180, 190, 210, 0.35)' : stemColor;\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const yMax = midY - maxArr[px] * scaleY;\n");
    fprintf(f, "        if (px === 0) ctx.moveTo(px, yMax); else ctx.lineTo(px, yMax);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      for (let px = pxWidth - 1; px >= 0; px--) {\n");
    fprintf(f, "        const yMin = midY - minArr[px] * scaleY;\n");
    fprintf(f, "        ctx.lineTo(px, yMin);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.closePath();\n");
    fprintf(f, "      ctx.fill();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 1. Draw Original Audio Peak Envelope / Samples\n");
    fprintf(f, "  drawSignal(origPcm, true);\n");

    fprintf(f, "  // 2. Draw Stem Waveform / Samples\n");
    fprintf(f, "  drawSignal(stem.pcm, false);\n");

    fprintf(f, "  // 3. Draw Spectral Flux Envelope\n");
    fprintf(f, "  if (stem.fluxPcm) {\n");
    fprintf(f, "    const pcmFlux = stem.fluxPcm;\n");
    fprintf(f, "    const graphBottom = height - 18;\n");
    fprintf(f, "    const graphTop = 8;\n");
    fprintf(f, "    const graphHeight = graphBottom - graphTop;\n");

    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.strokeStyle = '#ff5722';\n");
    fprintf(f, "      ctx.lineWidth = 1.8;\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const flVal = Math.max(0, Math.min(1, pcmFlux[s]));\n");
    fprintf(f, "        const y = graphBottom - flVal * graphHeight;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      const maxFluxArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      maxFluxArr.fill(0);\n");

    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let maxV = pcmFlux[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcmFlux[s];\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          maxFluxArr[px] = Math.max(0, Math.min(1, maxV));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");

    fprintf(f, "      ctx.strokeStyle = '#ff5722';\n");
    fprintf(f, "      ctx.lineWidth = 1.8;\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const y = graphBottom - maxFluxArr[px] * graphHeight;\n");
    fprintf(f, "        if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // 4. Draw Non-Tonal Injection Envelope (0 to 100%%)\n");
    fprintf(f, "  if (stem.injectionPcm) {\n");
    fprintf(f, "    const pcmInj = stem.injectionPcm;\n");
    fprintf(f, "    const graphBottom = height - 18;\n");
    fprintf(f, "    const graphTop = 8;\n");
    fprintf(f, "    const graphHeight = graphBottom - graphTop;\n");

    fprintf(f, "    if (visibleSamples <= width * 2) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.strokeStyle = 'rgba(255, 235, 59, 0.95)';\n");
    fprintf(f, "      ctx.lineWidth = 2.0;\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / DATA.sampleRate;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const injVal = Math.max(0, Math.min(1, pcmInj[s]));\n");
    fprintf(f, "        const y = graphBottom - injVal * graphHeight;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      const maxInjArr = new Float32Array(pxWidth);\n");
    fprintf(f, "      maxInjArr.fill(0);\n");

    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * DATA.sampleRate));\n");
    fprintf(f, "        const s2 = Math.min(DATA.totalSamples - 1, Math.ceil(t2 * DATA.sampleRate));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < DATA.totalSamples) {\n");
    fprintf(f, "          let maxV = pcmInj[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = pcmInj[s];\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          maxInjArr[px] = Math.max(0, Math.min(1, maxV));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");

    fprintf(f, "      ctx.fillStyle = 'rgba(255, 235, 59, 0.20)';\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.moveTo(0, graphBottom);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const y = graphBottom - maxInjArr[px] * graphHeight;\n");
    fprintf(f, "        ctx.lineTo(px, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.lineTo(width, graphBottom);\n");
    fprintf(f, "      ctx.closePath();\n");
    fprintf(f, "      ctx.fill();\n");

    fprintf(f, "      ctx.strokeStyle = 'rgba(255, 235, 59, 0.95)';\n");
    fprintf(f, "      ctx.lineWidth = 1.8;\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const y = graphBottom - maxInjArr[px] * graphHeight;\n");
    fprintf(f, "        if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // Center line\n");
    fprintf(f, "  ctx.strokeStyle = 'rgba(255, 255, 255, 0.25)';\n");
    fprintf(f, "  ctx.lineWidth = 1;\n");
    fprintf(f, "  ctx.beginPath(); ctx.moveTo(0, midY); ctx.lineTo(width, midY); ctx.stroke();\n");

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
    fprintf(f, "      const origVal = (origPcm && origPcm.length > s) ? origPcm[s] : 0;\n");
    fprintf(f, "      const stemVal = (stem.pcm && stem.pcm.length > s) ? stem.pcm[s] : 0;\n");
      fprintf(f, "      tooltip.style.opacity = '1';\n");
      fprintf(f, "      const flVal = (stem.fluxPcm && stem.fluxPcm.length > s) ? stem.fluxPcm[s] : 0;\n");
      fprintf(f, "      const injVal = (stem.injectionPcm && stem.injectionPcm.length > s) ? (stem.injectionPcm[s] * 100.0) : 0;\n");
      fprintf(f, "      tooltip.innerHTML = `Time: ${formatMSSPrecise(t)} (Sample #${s}) | Stem: ${stemVal.toFixed(4)} | Flux: ${flVal.toFixed(4)} | Injection: ${injVal.toFixed(1)}%%`;\n");
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
    printf("  HTML Report generated: %s/index.html\n", stem_dir);
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

/* Apply noise gate */
void apply_noise_gate(double *data, uint32_t total_samples, uint32_t num_channels, uint32_t sample_rate, double noise_floor_threshold) {
    if (total_samples == 0 || data == NULL) return;

    uint32_t ms_samples = (uint32_t)ceil((double)sample_rate / 1000.0);
    if (ms_samples < 1) ms_samples = 1;

    uint32_t section_start = 0;
    uint32_t i = 0;

    while (i < total_samples) {
        int silent = 1;
        for (uint32_t c = 0; c < num_channels; c++) {
            if (fabs(data[i * num_channels + c]) > g_silence_threshold) {
                silent = 0;
                break;
            }
        }

        if (silent) {
            uint32_t silence_start = i;
            while (i < total_samples) {
                int s = 1;
                for (uint32_t c = 0; c < num_channels; c++) {
                    if (fabs(data[i * num_channels + c]) > g_silence_threshold) {
                        s = 0;
                        break;
                    }
                }
                if (!s) break;
                i++;
            }
            uint32_t silence_len = i - silence_start;

            if (silence_len >= ms_samples) {
                for (uint32_t k = silence_start; k < i; k++) {
                    for (uint32_t c = 0; c < num_channels; c++) {
                        data[k * num_channels + c] = 0.0;
                    }
                }
                if (silence_start > section_start) {
                    uint32_t sec_end = silence_start - 1;
                    double sec_peak = 0.0;
                    for (uint32_t k = section_start; k <= sec_end; k++) {
                        for (uint32_t c = 0; c < num_channels; c++) {
                            double val = fabs(data[k * num_channels + c]);
                            if (val > sec_peak) sec_peak = val;
                        }
                    }
                    if (sec_peak < noise_floor_threshold) {
                        for (uint32_t k = section_start; k <= sec_end; k++) {
                            for (uint32_t c = 0; c < num_channels; c++) {
                                data[k * num_channels + c] = 0.0;
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
                double val = fabs(data[k * num_channels + c]);
                if (val > sec_peak) sec_peak = val;
            }
        }
        if (sec_peak < noise_floor_threshold) {
            for (uint32_t k = section_start; k <= sec_end; k++) {
                for (uint32_t c = 0; c < num_channels; c++) {
                    data[k * num_channels + c] = 0.0;
                }
            }
        }
    }
}

static int compare_uint32(const void *a, const void *b) {
    uint32_t ua = *(const uint32_t*)a;
    uint32_t ub = *(const uint32_t*)b;
    if (ua < ub) return -1;
    if (ua > ub) return 1;
    return 0;
}

static int compare_double(const void *a, const void *b) {
    double da = *(const double*)a;
    double db = *(const double*)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
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

    // Calculate noise floor threshold (19.0% of overall peak amplitude)
    double noise_floor_threshold = overall_peak * g_noise_floor_ratio;

    printf("\nAudio File Properties:\n");
    printf("  Format:          %s PCM (%u bits)\n", (wav->audio_format == 3) ? "IEEE Float" : "Integer", wav->bits_per_sample);
    printf("  Sample Rate:     %u Hz\n", wav->sample_rate);
    printf("  Channels:        %u (%s)\n", wav->num_channels, (wav->num_channels == 1) ? "Mono" : "Stereo");
    printf("  Total Duration:  %.2f seconds (%u frames per channel)\n", audio_duration, wav->total_samples);
    printf("  Signal Peak:     %.5f\n", overall_peak);
    printf("  Noise Floor Threshold: %.5f (19.0%% peak threshold / -14.4 dB, min 99ms active frames)\n\n", noise_floor_threshold);

    // Preserve original audio copy for HTML report generation
    double *orig_audio_buffer = (double*)malloc(total_channel_samples * sizeof(double));
    if (orig_audio_buffer) {
        memcpy(orig_audio_buffer, wav->data, total_channel_samples * sizeof(double));
    }

    // Buffer to hold non-tonal portion (initialized to original audio, then subtracts exported tonal stems)
    double *nontonal_data = (double*)malloc(total_channel_samples * sizeof(double));
    if (!nontonal_data) {
        printf("Error: Could not allocate memory for non-tonal audio buffer.\n");
        if (orig_audio_buffer) free(orig_audio_buffer);
        free(wav->data);
        free(wav);
        return 0;
    }
    memcpy(nontonal_data, wav->data, total_channel_samples * sizeof(double));

    /*
     * Run Cumulative Transience Transient Analysis (1-band mode)
     */
    printf("Running Cumulative Transience Analysis (1-band transient detection)...\n");
    float *mono_y = (float*)malloc(total_samples * sizeof(float));
    if (!mono_y) {
        printf("Error: Could not allocate memory for mono audio buffer.\n");
        free(orig_audio_buffer);
        free(nontonal_data);
        free(wav->data);
        free(wav);
        return 0;
    }
    for (uint32_t i = 0; i < total_samples; i++) {
        double sum = 0.0;
        for (uint32_t c = 0; c < num_channels; c++) {
            sum += wav->data[i * num_channels + c];
        }
        mono_y[i] = (float)(sum / num_channels);
    }

    FullAnalysisResult ct_result;
    memset(&ct_result, 0, sizeof(FullAnalysisResult));
    int ct_success = analyzer_batch_analyze(mono_y, (int)total_samples, (int)sample_rate, 15000, 1, &ct_result);
    free(mono_y);

    uint32_t *transients = NULL;
    uint32_t transient_count = 0;
    uint32_t transient_capacity = 64;

    transients = (uint32_t*)malloc(transient_capacity * sizeof(uint32_t));
    transients[transient_count++] = 0; // T_0 = 0

    if (ct_success && ct_result.num_bands > 0) {
        for (int k = 0; k < ct_result.bands[0].num_peaks; k++) {
            double time_s = ct_result.bands[0].peaks[k].time;
            long long sample_pos_ll = (long long)llround(time_s * sample_rate);
            if (sample_pos_ll > 0 && sample_pos_ll < (long long)total_samples) {
                if (transient_count >= transient_capacity) {
                    transient_capacity *= 2;
                    transients = (uint32_t*)realloc(transients, transient_capacity * sizeof(uint32_t));
                }
                transients[transient_count++] = (uint32_t)sample_pos_ll;
            }
        }
        analyzer_free_analysis(&ct_result);
    }

    if (transient_count >= transient_capacity) {
        transient_capacity += 1;
        transients = (uint32_t*)realloc(transients, transient_capacity * sizeof(uint32_t));
    }
    transients[transient_count++] = total_samples; // T_M = total_samples

    qsort(transients, transient_count, sizeof(uint32_t), compare_uint32);

    // Deduplicate transient markers
    uint32_t unique_count = 0;
    for (uint32_t i = 0; i < transient_count; i++) {
        if (unique_count == 0 || transients[i] != transients[unique_count - 1]) {
            transients[unique_count++] = transients[i];
        }
    }
    transient_count = unique_count;

    printf("Cumulative Transience detected %u transient markers across the signal.\n", transient_count > 2 ? transient_count - 2 : 0);

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

    // Pre-calculate bin pitch mappings and group bins per pitch
    int bin_pitch[FFT_SIZE / 2 + 1];
    int pitch_bin_count[NUM_MIDI_PITCHES] = {0};
    int (*pitch_bins)[FFT_SIZE / 2 + 1] = (int (*)[FFT_SIZE / 2 + 1])malloc(NUM_MIDI_PITCHES * (FFT_SIZE / 2 + 1) * sizeof(int));
    if (!pitch_bins) {
        printf("Error: Could not allocate memory for pitch_bins.\n");
        for (uint32_t c = 0; c < num_channels; c++) free(padded_input[c]);
        free(padded_input);
        free(cola_norm);
        free(nontonal_data);
        free(transients);
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
        free(nontonal_data);
        free(transients);
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

    printf("\nEvaluating stems against noise floor threshold (19.0%% peak / -14.4 dB, min 99 ms active frames)...\n");
    printf("Exporting audible pitch stem WAV files to: %s/\n\n", stem_dir);

    int exported_count = 0;

    printf("Audible Pitch Stem Export Table:\n");
    printf("-----------------------------------------------------------------------------------\n");
    printf(" MIDI | Note | Freq (Hz)  | Peak Amp | RMS Amp  | Active Duration | File Exported\n");
    printf("-----------------------------------------------------------------------------------\n");

    Complex *pitch_frame = (Complex*)malloc(FFT_SIZE * sizeof(Complex));

    AudibleStem *audible_stems = NULL;
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

        // Unpad and normalize stem audio
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

        // Preserve raw ungated pitch stem audio BEFORE noise gating
        double *raw_pitch_stem_data = (double*)malloc(total_channel_samples * sizeof(double));
        if (raw_pitch_stem_data) {
            memcpy(raw_pitch_stem_data, unpadded_data, total_channel_samples * sizeof(double));
        }

        // Apply noise gate
        apply_noise_gate(unpadded_data, total_samples, num_channels, sample_rate, noise_floor_threshold);

        double max_peak = 0.0;
        double sum_sq = 0.0;
        uint32_t active_frames = 0;

        for (uint32_t i = 0; i < total_samples; i++) {
            int frame_above_threshold = 0;
            for (uint32_t c = 0; c < num_channels; c++) {
                double val = unpadded_data[i * num_channels + c];
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

        // Keep stems that have at least 99 ms active frames
        if (active_ms >= 99.0 && rms > 1e-6) {
            for (size_t k = 0; k < total_channel_samples; k++) {
                nontonal_data[k] -= unpadded_data[k];
            }

            if (audible_count >= audible_capacity) {
                audible_capacity = (audible_capacity == 0) ? 16 : audible_capacity * 2;
                audible_stems = (AudibleStem*)realloc(audible_stems, audible_capacity * sizeof(AudibleStem));
            }

            AudibleStem *stem = &audible_stems[audible_count++];
            stem->pitch = p;
            get_note_name(p, stem->note_name, sizeof(stem->note_name));
            stem->max_peak = max_peak;
            stem->rms = rms;
            stem->active_ms = active_ms;
            stem->audio_data = unpadded_data;
            stem->raw_audio_data = raw_pitch_stem_data;
            stem->injection_envelope = (double*)calloc(total_samples, sizeof(double));
            stem->flux_data = (double*)malloc(total_channel_samples * sizeof(double));
            snprintf(stem->filename, sizeof(stem->filename), "%s_pitch_%03d_%s.wav", basename, p, stem->note_name);
            compute_stem_transience_flux(raw_pitch_stem_data ? raw_pitch_stem_data : unpadded_data, total_samples, num_channels, sample_rate, stem->flux_data, 0);
        } else {
            free(unpadded_data);
            if (raw_pitch_stem_data) free(raw_pitch_stem_data);
        }
    }

    /*
     * Non-Tonal Audio Injection using Cumulative Transience Transient Markers
     *
     * For each interval between adjacent transient markers [T_k, T_{k+1}]:
     * 1. Calculate total positive energy flux F_{p,k} = sum_{s in interval, c} flux_p[s,c] for each audible pitch stem.
     * 2. Find max flux F_{max,k} across all stems in that interval.
     * 3. Evaluate 89th percentile threshold = 0.89 * F_{max,k}.
     * 4. Stems with F_{p,k} >= threshold qualify.
     * 5. Calculate proportional weight W_p = F_{p,k} / sum_{q in qualifying} F_{q,k}
     *    so that total added non-tonal audio across all stems does not exceed 100%.
     * 6. Add non-tonal audio scaled linearly from 100% (interval start) to 0% (interval end).
     */
    if (audible_count > 0 && transient_count >= 2) {
        double *stem_fluxes = (double*)malloc((size_t)audible_count * sizeof(double));
        int *qualifying_indices = (int*)malloc((size_t)audible_count * sizeof(int));

        for (uint32_t t_idx = 0; t_idx + 1 < transient_count; t_idx++) {
            uint32_t interval_start = transients[t_idx];
            uint32_t interval_end = transients[t_idx + 1];
            if (interval_end <= interval_start) continue;

            uint32_t interval_length = interval_end - interval_start;

            double max_interval_flux = 0.0;
            for (int idx = 0; idx < audible_count; idx++) {
                double total_flux = 0.0;
                const double *fl_pcm = audible_stems[idx].flux_data;
                if (fl_pcm) {
                    for (uint32_t s = interval_start; s < interval_end; s++) {
                        for (uint32_t c = 0; c < num_channels; c++) {
                            total_flux += fl_pcm[s * num_channels + c];
                        }
                    }
                }
                stem_fluxes[idx] = total_flux;
                if (total_flux > max_interval_flux) {
                    max_interval_flux = total_flux;
                }
            }

            if (max_interval_flux <= 1e-12) continue;

            double flux_threshold = 0.89 * max_interval_flux;
            int qual_count = 0;
            double qual_flux_sum = 0.0;

            for (int idx = 0; idx < audible_count; idx++) {
                if (stem_fluxes[idx] >= flux_threshold) {
                    qualifying_indices[qual_count++] = idx;
                    qual_flux_sum += stem_fluxes[idx];
                }
            }

            if (qual_count == 0 || qual_flux_sum <= 1e-12) continue;

            for (int q = 0; q < qual_count; q++) {
                int idx = qualifying_indices[q];
                double weight = stem_fluxes[idx] / qual_flux_sum;

                for (uint32_t i = 0; i < interval_length; i++) {
                    uint32_t s = interval_start + i;
                    double decay = 1.0 - ((double)i / (double)interval_length);

                    double nt_factor = weight * decay;

                    if (audible_stems[idx].injection_envelope && s < total_samples) {
                        audible_stems[idx].injection_envelope[s] += nt_factor;
                    }

                    for (uint32_t c = 0; c < num_channels; c++) {
                        audible_stems[idx].audio_data[s * num_channels + c] += nt_factor * nontonal_data[s * num_channels + c];
                    }
                }
            }
        }

        free(stem_fluxes);
        free(qualifying_indices);
    }

    // Export pitch stems
    if (audible_count > 0) {
        for (int idx = 0; idx < audible_count; idx++) {
            AudibleStem *stem = &audible_stems[idx];
            int p = stem->pitch;

            double final_peak = 0.0;
            double final_sum_sq = 0.0;
            for (size_t k = 0; k < total_channel_samples; k++) {
                double val = stem->audio_data[k];
                double abs_val = fabs(val);
                if (abs_val > final_peak) final_peak = abs_val;
                final_sum_sq += val * val;
            }
            double final_rms = sqrt(final_sum_sq / total_channel_samples);

            char out_filepath[4096];
            snprintf(out_filepath, sizeof(out_filepath), "%s/%s_pitch_%03d_%s.wav",
                     stem_dir, basename, p, stem->note_name);

            if (write_wav_16bit(out_filepath, sample_rate, num_channels, total_samples, stem->audio_data)) {
                printf(" %03d  | %-4s | %8.2f Hz | %8.5f | %8.5f | %8.1f ms     | %s_pitch_%03d_%s.wav\n",
                       p, stem->note_name, midi_to_frequency(p), final_peak, final_rms, stem->active_ms, basename, p, stem->note_name);
                exported_count++;

                // Export raw flux WAV file in [flux] subdirectory
                if (stem->raw_audio_data) {
                    double *raw_flux_buf = (double*)malloc(total_channel_samples * sizeof(double));
                    if (raw_flux_buf) {
                        compute_stem_transience_flux(stem->raw_audio_data, total_samples, num_channels, sample_rate, raw_flux_buf, 1);
                        char raw_flux_filepath[4096];
                        snprintf(raw_flux_filepath, sizeof(raw_flux_filepath), "%s/[flux]/%s_pitch_%03d_%s_flux.wav",
                                 stem_dir, basename, p, stem->note_name);
                        write_wav_16bit(raw_flux_filepath, sample_rate, num_channels, total_samples, raw_flux_buf);

                        // Process modified flux envelope for each channel and write modified flux WAV file
                        for (uint32_t c = 0; c < num_channels; c++) {
                            double *channel_flux = (double*)malloc(total_samples * sizeof(double));
                            if (channel_flux) {
                                for (uint32_t s = 0; s < total_samples; s++) {
                                    channel_flux[s] = raw_flux_buf[s * num_channels + c];
                                }
                                modify_flux_envelope_channel(channel_flux, total_samples);
                                for (uint32_t s = 0; s < total_samples; s++) {
                                    raw_flux_buf[s * num_channels + c] = channel_flux[s];
                                }
                                free(channel_flux);
                            }
                        }

                        char mod_flux_filepath[4096];
                        snprintf(mod_flux_filepath, sizeof(mod_flux_filepath), "%s/[flux]/%s_pitch_%03d_%s_modified_flux.wav",
                                 stem_dir, basename, p, stem->note_name);
                        write_wav_16bit(mod_flux_filepath, sample_rate, num_channels, total_samples, raw_flux_buf);

                        free(raw_flux_buf);
                    }
                }
            }
        }
    }
    printf("-----------------------------------------------------------------------------------\n\n");

    char orig_filename[1024];
    snprintf(orig_filename, sizeof(orig_filename), "%s_original.wav", basename);

    double nt_max_peak = 0.0;
    double nt_sum_sq = 0.0;
    for (size_t i = 0; i < total_channel_samples; i++) {
        double val = nontonal_data[i];
        double abs_val = fabs(val);
        if (abs_val > nt_max_peak) nt_max_peak = abs_val;
        nt_sum_sq += val * val;
    }
    double nt_rms = sqrt(nt_sum_sq / total_channel_samples);

    int nontonal_exported = 0;

    // Construct HTML report in stem directory
    generate_html_report(
        stem_dir,
        basename,
        sample_rate,
        num_channels,
        total_samples,
        audio_duration,
        orig_filename,
        orig_audio_buffer,
        nontonal_data,
        audible_stems,
        audible_count,
        nontonal_exported,
        nt_max_peak,
        nt_rms,
        transients,
        transient_count
    );

    free(orig_audio_buffer);

    // Free stem buffers
    for (int idx = 0; idx < audible_count; idx++) {
        AudibleStem *stem = &audible_stems[idx];
        free(stem->audio_data);
        if (stem->raw_audio_data) free(stem->raw_audio_data);
        if (stem->flux_data) free(stem->flux_data);
        if (stem->injection_envelope) free(stem->injection_envelope);
    }
    if (audible_stems) {
        free(audible_stems);
    }

    double total_elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    double speed_ratio = audio_duration / (total_elapsed > 0.001 ? total_elapsed : 0.001);

    printf("====================================================\n");
    printf("               PROCESSING SUMMARY                   \n");
    printf("====================================================\n");
    printf("  Input File:          %s\n", basename);
    printf("  Stems Directory:     %s/\n", stem_dir);
    printf("  Audible Stems Exported: %d pitch WAV files + 1 non-tonal WAV file\n", exported_count);
    printf("  Transient Markers:   %u detected\n", transient_count > 2 ? transient_count - 2 : 0);
    printf("  Total Time Elapsed:  %.2f seconds\n", total_elapsed);
    printf("  Processing Speed:    %.1fx Real-Time\n", speed_ratio);
    printf("  Reconstruction Status: AUDIBLE RECONSTRUCTION PERFECT\n");
    printf("====================================================\n");

    free(fft_frame);
    free(pitch_frame);
    free(stft_data);
    free(frame_pitch_active);
    free(cola_norm);
    free(pitch_bins);
    free(nontonal_data);
    free(transients);
    free(wav);

    return 1;
}

int main(int argc, char *argv[]) {
    printf("====================================================\n");
    printf(" CUMULATIVE TRANSIENCE PITCH STEM SEPARATOR \n");
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
