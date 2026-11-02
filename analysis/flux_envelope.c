/*
 * flux_envelope.c
 *
 * Spectral flux envelope maths shared by the separator and the report tool: the
 * 2048-point FFT, per-stem transience flux, peak-salience envelope modification,
 * and peak/trough marker detection.
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


#define FLUX_FFT_SIZE 2048
#define FLUX_HOP_SIZE 512

/* Pre-calculated flux FFT lookup tables */
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

void detect_all_peaks(const double *env, uint32_t n, int **out_peaks, int *out_count);

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

void detect_all_peaks(const double *env, uint32_t n, int **out_peaks, int *out_count) {
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

void detect_all_troughs(const double *env, uint32_t n, int **out_troughs, int *out_count) {
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
