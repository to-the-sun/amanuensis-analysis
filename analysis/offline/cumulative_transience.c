#define _USE_MATH_DEFINES
#include "cumulative_transience.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N_FFT 2048
#define N_MELS 128
#define CACHE_SIZE 15201

static void fft(double* real, double* imag, int n) {
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            double tr = real[i]; real[i] = real[j]; real[j] = tr;
            double ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
        int k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        double wlen_r = cos(ang), wlen_i = -sin(ang);
        for (int i = 0; i < n; i += len) {
            double w_r = 1.0, w_i = 0.0;
            for (int k = 0; k < len / 2; k++) {
                double u_r = real[i + k], u_i = imag[i + k];
                double v_r = real[i + k + len / 2] * w_r - imag[i + k + len / 2] * w_i;
                double v_i = real[i + k + len / 2] * w_i + imag[i + k + len / 2] * w_r;
                real[i + k] = u_r + v_r; imag[i + k] = u_i + v_i;
                real[i + k + len / 2] = u_r - v_r; imag[i + k + len / 2] = u_i - v_i;
                double tmp_r = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r; w_r = tmp_r;
            }
        }
    }
}

typedef struct { int p_idx; int band_idx; } PeakRef;
static int compare_peaks(const void* a, const void* b) { return ((PeakRef*)a)->p_idx - ((PeakRef*)b)->p_idx; }

static float calculate_half_max(float* values, int n) {
    if (n <= 0) return 0;
    float max_v = values[0];
    for (int i = 1; i < n; i++) {
        if (values[i] > max_v) max_v = values[i];
    }
    return max_v / 2.0f;
}

static float calculate_prominence_global(TransientAnalyzer* self, int band_idx, int cache_idx, float* lmin_out, float* rmin_out) {
    int nf = self->cache_count;
    if (nf <= 0) {
        if (lmin_out) *lmin_out = 0;
        if (rmin_out) *rmin_out = 0;
        return 0;
    }

    int rptr = (self->cache_write_ptr - nf + CACHE_SIZE) % CACHE_SIZE;
    // Find how many steps we can go back from cache_idx to reach rptr
    int steps_back = (cache_idx - rptr + CACHE_SIZE) % CACHE_SIZE;
    // Find how many steps we can go forward from cache_idx to reach the latest frame (write_ptr - 1)
    int latest_idx = (self->cache_write_ptr - 1 + CACHE_SIZE) % CACHE_SIZE;
    int steps_forward = (latest_idx - cache_idx + CACHE_SIZE) % CACHE_SIZE;

    float val = self->dynamic_smoothings[band_idx * CACHE_SIZE + cache_idx];
    float lmin = val;
    // Search backwards in the global circular buffer until rptr (oldest valid frame)
    for (int k = 1; k <= steps_back; k++) {
        int idx = (cache_idx - k + CACHE_SIZE) % CACHE_SIZE;
        float v = self->dynamic_smoothings[band_idx * CACHE_SIZE + idx];
        if (v > val) break;
        if (v < lmin) lmin = v;
    }

    float rmin = val;
    // Search forwards in the global circular buffer until latest_idx (newest valid frame)
    for (int k = 1; k <= steps_forward; k++) {
        int idx = (cache_idx + k) % CACHE_SIZE;
        float v = self->dynamic_smoothings[band_idx * CACHE_SIZE + idx];
        if (v > val) break;
        if (v < rmin) rmin = v;
    }

    if (lmin_out) *lmin_out = lmin;
    if (rmin_out) *rmin_out = rmin;
    return val - (lmin > rmin ? lmin : rmin);
}

static double* create_mel_filterbank(int sr, int n_fft, int n_mels);

TransientAnalyzer* analyzer_create(double max_peak_value, SharedTransientBuffer* shared_buffer, void* lock_obj, ct_lock_func lock_func, ct_lock_func unlock_func, int window_ms, int num_bands) {
    TransientAnalyzer* self = (TransientAnalyzer*)calloc(1, sizeof(TransientAnalyzer));
    if (!self) return NULL;
    self->shared_buffer = shared_buffer;
    self->lock_obj = lock_obj;
    self->lock_func = lock_func;
    self->unlock_func = unlock_func;

    self->window_ms = (window_ms > 0 && window_ms <= 15000) ? window_ms : 15000;
    self->num_bands = (num_bands > 0 && num_bands <= MAX_BANDS) ? num_bands : MAX_BANDS;

    if (!self->shared_buffer) {
        self->private_max_peak = max_peak_value;
        self->private_min_score_seen = DBL_MAX;
        self->private_max_score_seen = -DBL_MAX;
        self->private_total_score_sum = 0;
        self->private_score_count = 0;
    }

    self->highest_peak_ms = -999.0;
    self->tolerance = 9.0;
    memset(self->bar_length_counts, 0, sizeof(self->bar_length_counts));
    for (int b = 0; b < self->num_bands; b++) {
        self->midpoint_lookback[b] = (double)self->window_ms;
        self->lookback_avg_delta[b] = 0.0;
        self->lookback_total_delta[b] = 0.0;
        self->lookback_p_count[b] = 0;
    }
    int win_len = self->window_ms + 1;
    for (int i = 0; i < win_len; i++) self->buffer_times[i] = (double)(i - self->window_ms);
    self->frame_duration_ms = 1.0;
    self->mel_spectrogram = (double*)calloc(N_MELS * CACHE_SIZE, sizeof(double));
    self->flux_envelopes = (float*)calloc(MAX_BANDS * CACHE_SIZE, sizeof(float));
    self->dynamic_smoothings = (float*)calloc(MAX_BANDS * CACHE_SIZE, sizeof(float));
    self->prominence_envelopes = (float*)calloc(MAX_BANDS * CACHE_SIZE, sizeof(float));
    self->fft_window = (double*)malloc(sizeof(double) * N_FFT);
    if (self->fft_window) for (int i = 0; i < N_FFT; i++) self->fft_window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (double)N_FFT));
    self->sample_rate = 44100;
    self->mel_filters = create_mel_filterbank(self->sample_rate, N_FFT, N_MELS);
    self->overlap_buffer = (float*)calloc(N_FFT * 4, sizeof(float));
    self->fft_real = (double*)malloc(sizeof(double) * N_FFT);
    self->fft_imag = (double*)malloc(sizeof(double) * N_FFT);
    if (!self->mel_spectrogram || !self->flux_envelopes || !self->dynamic_smoothings || !self->prominence_envelopes || !self->fft_window || !self->mel_filters || !self->overlap_buffer || !self->fft_real || !self->fft_imag) {
        analyzer_destroy(self); return NULL;
    }
    return self;
}

void analyzer_destroy(TransientAnalyzer* self) {
    if (!self) return;

    // Fix Ghost Peak Bug: Subtract all remaining active snapshots from shared/private buffer before destruction
    if (self->lock_func) self->lock_func(self->lock_obj);
    double* acc_buf = self->shared_buffer ? self->shared_buffer->accumulated_buffer : self->private_accumulated_buffer;
    int win_len = self->window_ms + 1;
    for (int b = 0; b < self->num_bands; b++) {
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) {
            for (int j = 0; j < win_len; j++) {
                acc_buf[j] -= curr->snapshot[j];
            }
            curr = curr->next;
        }
    }

    for (int b = 0; b < self->num_bands; b++) {
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) { SnapshotEntry* next = curr->next; free(curr); curr = next; }
        self->snapshot_heads[b] = NULL;
        self->snapshot_tails[b] = NULL;
    }
    if (self->unlock_func) self->unlock_func(self->lock_obj);

    free(self->overlap_buffer); free(self->combined_scratch); free(self->fft_real); free(self->fft_imag);
    free(self->mel_spectrogram); free(self->flux_envelopes); free(self->dynamic_smoothings); free(self->prominence_envelopes); free(self->fft_window); free(self->mel_filters); free(self);
}

void analyzer_clear(TransientAnalyzer* self) {
    if (!self) return;

    if (self->lock_func) self->lock_func(self->lock_obj);

    int win_len = self->window_ms + 1;
    if (self->shared_buffer) {
        memset(self->shared_buffer->accumulated_buffer, 0, sizeof(double) * win_len);
        self->shared_buffer->max_peak = 1.0;
        self->shared_buffer->min_score_seen = DBL_MAX;
        self->shared_buffer->max_score_seen = -DBL_MAX;
        self->shared_buffer->total_score_sum = 0;
        self->shared_buffer->score_count = 0;
    }

    memset(self->private_accumulated_buffer, 0, sizeof(double) * win_len);
    self->private_max_peak = 1.0;
    self->private_min_score_seen = DBL_MAX;
    self->private_max_score_seen = -DBL_MAX;
    self->private_total_score_sum = 0;
    self->private_score_count = 0;

    for (int b = 0; b < self->num_bands; b++) {
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) {
            SnapshotEntry* next = curr->next;
            free(curr);
            curr = next;
        }
        self->snapshot_heads[b] = NULL;
        self->snapshot_tails[b] = NULL;

        self->midpoint_lookback[b] = (double)self->window_ms;
        self->lookback_avg_delta[b] = 0.0;
        self->lookback_total_delta[b] = 0.0;
        self->lookback_p_count[b] = 0;
        self->smoothing_states[b] = 0.0f;
        self->smoothing_avgs[b] = 0.0;
    }

    self->highest_peak_ms = -999.0;
    memset(self->bar_length_counts, 0, sizeof(self->bar_length_counts));

    if (self->mel_spectrogram) memset(self->mel_spectrogram, 0, sizeof(double) * N_MELS * CACHE_SIZE);
    if (self->flux_envelopes) memset(self->flux_envelopes, 0, sizeof(float) * MAX_BANDS * CACHE_SIZE);
    if (self->dynamic_smoothings) memset(self->dynamic_smoothings, 0, sizeof(float) * MAX_BANDS * CACHE_SIZE);
    if (self->prominence_envelopes) memset(self->prominence_envelopes, 0, sizeof(float) * MAX_BANDS * CACHE_SIZE);

    self->cache_write_ptr = 0;
    self->cache_count = 0;
    self->max_mel_db = 0.0;

    if (self->overlap_buffer) memset(self->overlap_buffer, 0, sizeof(float) * N_FFT * 4);
    self->overlap_len = 0;

    self->total_frames_pushed = 0;
    self->total_samples_received = 0;

    if (self->unlock_func) self->unlock_func(self->lock_obj);
}

void analyzer_set_sample_rate(TransientAnalyzer* self, int sr) {
    if (self->sample_rate != sr) {
        self->sample_rate = sr; free(self->mel_filters);
        self->mel_filters = create_mel_filterbank(sr, N_FFT, N_MELS);
    }
    int hop = (int)(sr * 0.001); self->frame_duration_ms = 1000.0 * (double)hop / (double)sr;
    int win_len = self->window_ms + 1;
    for (int i = 0; i < win_len; i++) self->buffer_times[i] = (double)(i - self->window_ms) * self->frame_duration_ms;
}

double analyzer_get_max_peak(TransientAnalyzer* self) {
    if (self->shared_buffer) return self->shared_buffer->max_peak;
    return self->private_max_peak;
}

int analyzer_process_peak(TransientAnalyzer* self, int p_idx, int global_p_idx, int band_idx, double time, const float* env_ptr, int env_len, const int* all_valid_peak_indices, int all_valid_count, double detected_peak_val, double thresh_val, double left_min, double right_min, double prominence, PeakResult* result_out) {
    result_out->p_idx = p_idx; result_out->band_idx = band_idx; result_out->time = time;
    result_out->peak_val = (double)env_ptr[p_idx]; result_out->total_score = 0;
    result_out->detected_peak_val = detected_peak_val; result_out->thresh_val = thresh_val;
    result_out->left_min = left_min; result_out->right_min = right_min; result_out->prominence = prominence;
    result_out->num_qualifiers = 0;

    int win_len = self->window_ms + 1;
    int start = p_idx - self->window_ms;
    double peak_flux[BUFFER_LEN];
    for (int i = 0; i < win_len; i++) {
        int idx = start + i;
        peak_flux[i] = (idx < 0 || idx >= env_len) ? 0.0 : (double)env_ptr[idx];
    }

    if (self->lock_func) self->lock_func(self->lock_obj);

    double* acc_buf = self->shared_buffer ? self->shared_buffer->accumulated_buffer : self->private_accumulated_buffer;
    double q_sum = 0.0; bool found = false;
    // Exclude the last 99ms to avoid self-referential bias from the peak at zero.
    int m_len = win_len - 99; double sum = 0.0, max_v = -DBL_MAX, min_v = DBL_MAX;
    if (m_len > 0) {
        max_v = acc_buf[0];
        min_v = acc_buf[0];
        for (int i = 0; i < m_len; i++) {
            double v = acc_buf[i];
            sum += v;
            if (v > max_v) max_v = v;
            if (v < min_v) min_v = v;
        }
    }
    double midpoint = (m_len > 0) ? (sum / (double)m_len) : 0.0;
    int tol_idx = (int)round(self->tolerance / self->frame_duration_ms);
    if (tol_idx < 0) tol_idx = 0;

    for (int i = 0; i < all_valid_count; i++) {
        int s_idx = all_valid_peak_indices[i];
        // Qualifiers must be at least 99ms in the past to avoid self-reference.
        if (s_idx >= p_idx - self->window_ms && s_idx <= p_idx - 99) {
            int sp_idx = self->window_ms - (p_idx - s_idx);

            // Apply snapping within 2*tolerance (one tolerance on each side)
            int start_k = sp_idx - tol_idx;
            if (start_k < 0) start_k = 0;
            int end_k = sp_idx + tol_idx;
            if (end_k >= win_len) end_k = win_len - 1;

            int snap_idx = sp_idx;
            double max_snap_val = acc_buf[sp_idx];
            for (int k = start_k; k <= end_k; k++) {
                if (acc_buf[k] > max_snap_val) {
                    max_snap_val = acc_buf[k];
                    snap_idx = k;
                }
            }

            double val = acc_buf[snap_idx];
            double q = 0.0;
            if (val > midpoint) {
                if (max_v > midpoint) {
                    q = (val - midpoint) / (max_v - midpoint);
                }
            } else if (val < midpoint) {
                if (midpoint > min_v) {
                    q = (val - midpoint) / (midpoint - min_v);
                }
            }
            if (result_out->num_qualifiers < MAX_QUALIFIERS) {
                result_out->qualifiers[result_out->num_qualifiers].ms = self->buffer_times[snap_idx];
                result_out->qualifiers[result_out->num_qualifiers].orig_ms = self->buffer_times[sp_idx];
                result_out->qualifiers[result_out->num_qualifiers].val = q;
                result_out->num_qualifiers++;
            }
            q_sum += q;
            found = true;
        }
    }
    if (found) result_out->total_score = result_out->peak_val * q_sum;

    if (self->shared_buffer) {
        if (result_out->total_score < self->shared_buffer->min_score_seen) self->shared_buffer->min_score_seen = result_out->total_score;
        if (result_out->total_score > self->shared_buffer->max_score_seen) self->shared_buffer->max_score_seen = result_out->total_score;
        self->shared_buffer->total_score_sum += result_out->total_score; self->shared_buffer->score_count++;
    } else {
        if (result_out->total_score < self->private_min_score_seen) self->private_min_score_seen = result_out->total_score;
        if (result_out->total_score > self->private_max_score_seen) self->private_max_score_seen = result_out->total_score;
        self->private_total_score_sum += result_out->total_score; self->private_score_count++;
    }

    memcpy(result_out->snapshot, acc_buf, sizeof(double) * win_len);

    for (int i = 0; i < win_len; i++) acc_buf[i] += peak_flux[i];

    SnapshotEntry* entry = (SnapshotEntry*)malloc(sizeof(SnapshotEntry));
    if (entry) {
        entry->p_idx = global_p_idx;
        memcpy(entry->snapshot, peak_flux, sizeof(double) * win_len);
        entry->next = NULL;
        if (self->snapshot_tails[band_idx]) { self->snapshot_tails[band_idx]->next = entry; self->snapshot_tails[band_idx] = entry; }
        else { self->snapshot_heads[band_idx] = entry; self->snapshot_tails[band_idx] = entry; }
    }

    if (self->unlock_func) self->unlock_func(self->lock_obj);

    return 1;
}

bool analyzer_cleanup_snapshots(TransientAnalyzer* self, int frame) {
    int cleanup = frame - self->window_ms; bool updated = false;

    if (self->lock_func) self->lock_func(self->lock_obj);
    double* acc_buf = self->shared_buffer ? self->shared_buffer->accumulated_buffer : self->private_accumulated_buffer;

    int win_len = self->window_ms + 1;
    for (int b = 0; b < self->num_bands; b++) {
        while (self->snapshot_heads[b] && self->snapshot_heads[b]->p_idx <= cleanup) {
            SnapshotEntry* e = self->snapshot_heads[b];
            for (int j = 0; j < win_len; j++) acc_buf[j] -= e->snapshot[j];
            self->snapshot_heads[b] = e->next; if (!self->snapshot_heads[b]) self->snapshot_tails[b] = NULL;
            free(e); updated = true;
        }
    }

    if (self->unlock_func) self->unlock_func(self->lock_obj);

    return updated;
}

void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out) {
    bool updated = analyzer_cleanup_snapshots(self, frame);

    if (self->lock_func) self->lock_func(self->lock_obj);
    double* acc_buf = self->shared_buffer ? self->shared_buffer->accumulated_buffer : self->private_accumulated_buffer;

    // Exclude the last 99ms to avoid self-referential bias from the peak at zero.
    int win_len = self->window_ms + 1;
    int m_len = win_len - 99; double sum = 0.0, sum_sq = 0.0, max_v = -DBL_MAX;
    if (m_len > 0) {
        max_v = acc_buf[0];
        for (int i = 0; i < m_len; i++) {
            double v = acc_buf[i]; sum += v; sum_sq += v * v; if (v > max_v) max_v = v;
        }
    }
    double mean = (m_len > 0) ? (sum / (double)m_len) : 0.0;
    double var = (m_len > 0) ? (sum_sq / (double)m_len - mean * mean) : 0.0; if (var < 0) var = 0;
    metrics_out->std_dev = sqrt(var); metrics_out->mean = mean; metrics_out->contrast = (mean > 0) ? (max_v / mean) : 0;
    metrics_out->demarcation_line = mean;

    if (self->shared_buffer) {
        metrics_out->rating = (self->shared_buffer->score_count > 0) ? (self->shared_buffer->total_score_sum / self->shared_buffer->score_count) : 0;
        metrics_out->min_score_seen = self->shared_buffer->min_score_seen;
        metrics_out->max_score_seen = self->shared_buffer->max_score_seen;
    } else {
        metrics_out->rating = (self->private_score_count > 0) ? (self->private_total_score_sum / self->private_score_count) : 0;
        metrics_out->min_score_seen = self->private_min_score_seen;
        metrics_out->max_score_seen = self->private_max_score_seen;
    }

    metrics_out->buffer_updated = updated; metrics_out->highest_peak_valid = false;
    if (max_v > 0.1) {
        int hi_idx = -1; double hi_val = -DBL_MAX;
        for (int i = 0; i < m_len; i++) {
            double v = acc_buf[i]; if (v > mean && v > hi_val) { hi_val = v; hi_idx = i; }
        }
        if (hi_idx != -1) {
            metrics_out->highest_peak_ms = self->buffer_times[hi_idx]; metrics_out->highest_peak_valid = true;
            self->highest_peak_ms = metrics_out->highest_peak_ms;
        }
    }

    if (self->unlock_func) self->unlock_func(self->lock_obj);

    if (metrics_out->highest_peak_valid) {
        int bar_length = (int)round(fabs(metrics_out->highest_peak_ms));
        if (bar_length >= 0 && bar_length <= self->window_ms) {
            self->bar_length_counts[bar_length]++;
        }
    }

    double stability_sum = 0;
    int stability_count = 0;
    for (int i = 0; i <= self->window_ms; i++) {
        if (self->bar_length_counts[i] > 0) {
            stability_sum += (double)self->bar_length_counts[i];
            stability_count++;
        }
    }
    metrics_out->stability_score = (stability_count > 0) ? (stability_sum / (double)stability_count) : 0.0;

    // Calculate prominence averages over window duration
    int nf = self->cache_count;
    int win = (int)((double)self->window_ms / self->frame_duration_ms);
    if (win > nf) win = nf;
    if (win <= 0) win = 1;

    int wptr = self->cache_write_ptr;
    int num_b = (self->num_bands > 0 && self->num_bands <= MAX_BANDS) ? self->num_bands : MAX_BANDS;
    double g_fsum = 0;
    for (int b = 0; b < num_b; b++) {
        double psum = 0;
        double ssum = 0;
        double fsum = 0;
        float pmax = 0;
        for (int j = 0; j < win; j++) {
            int idx = (wptr - 1 - j + CACHE_SIZE) % CACHE_SIZE;
            float pv = self->prominence_envelopes[b * CACHE_SIZE + idx];
            psum += (double)pv;
            if (pv > pmax) pmax = pv;
            ssum += (double)self->dynamic_smoothings[b * CACHE_SIZE + idx];
            fsum += (double)self->flux_envelopes[b * CACHE_SIZE + idx];
        }
        metrics_out->band_prominence_avgs[b] = psum / (double)win;
        metrics_out->band_prominence_half_maxes[b] = (double)pmax / 2.0;
        metrics_out->band_smoothing_avgs[b] = ssum / (double)win;
        self->smoothing_avgs[b] = metrics_out->band_smoothing_avgs[b];
        metrics_out->band_flux_avgs[b] = fsum / (double)win;
        g_fsum += metrics_out->band_flux_avgs[b];
    }
    metrics_out->global_flux_avg = g_fsum / (double)num_b;

    double g_ssum = 0;
    for (int b = 0; b < num_b; b++) g_ssum += metrics_out->band_smoothing_avgs[b];
    metrics_out->global_smoothing_avg = g_ssum / (double)num_b;
}

double* analyzer_get_buffer(TransientAnalyzer* self) {
    if (self->shared_buffer) return self->shared_buffer->accumulated_buffer;
    return self->private_accumulated_buffer;
}

void analyzer_push_audio(TransientAnalyzer* self, const float* y, int len, int sr) {
    if (self->sample_rate != sr) {
        analyzer_set_sample_rate(self, sr);
        self->overlap_len = 0;
        self->total_samples_received = 0;
    }
    int hop = (int)(sr * 0.001);
    int combined_len = self->overlap_len + len;
    if (combined_len > self->combined_scratch_cap) {
        int ncap = combined_len + N_FFT * 2;
        float* ns = (float*)realloc(self->combined_scratch, sizeof(float) * ncap);
        if (ns) { self->combined_scratch = ns; self->combined_scratch_cap = ncap; } else return;
    }
    if (self->overlap_len > 0) memcpy(self->combined_scratch, self->overlap_buffer, sizeof(float) * self->overlap_len);
    if (len > 0) memcpy(self->combined_scratch + self->overlap_len, y, sizeof(float) * len);

    long long current_scratch_start = self->total_samples_received - self->overlap_len;
    long long current_total_samples = self->total_samples_received + len;

    while (1) {
        long long next_f = self->total_frames_pushed;
        long long center_sample_global = next_f * hop;
        long long end_sample_needed_global = center_sample_global + N_FFT / 2;

        if (end_sample_needed_global > current_total_samples) break;

        int f_idx = self->cache_write_ptr;
        double frame_max = -DBL_MAX;

        for (int i = 0; i < N_FFT; i++) {
            long long g_idx = center_sample_global - N_FFT / 2 + i;
            if (g_idx < 0) {
                self->fft_real[i] = 0.0;
            } else if (g_idx >= current_total_samples) {
                self->fft_real[i] = 0.0;
            } else {
                int l_idx = (int)(g_idx - current_scratch_start);
                self->fft_real[i] = (double)self->combined_scratch[l_idx] * self->fft_window[i];
            }
            self->fft_imag[i] = 0.0;
        }
        fft(self->fft_real, self->fft_imag, N_FFT);

        for (int m = 0; m < N_MELS; m++) {
            double mel = 0;
            for (int i = 0; i < (N_FFT / 2 + 1); i++) {
                double p = (self->fft_real[i] * self->fft_real[i] + self->fft_imag[i] * self->fft_imag[i]) / ((double)N_FFT * (double)N_FFT);
                mel += p * self->mel_filters[m * (N_FFT / 2 + 1) + i];
            }
            if (mel < 1e-10) mel = 1e-10;
            double db = 10.0 * log10(mel);
            self->mel_spectrogram[m * CACHE_SIZE + f_idx] = db;
            if (db > frame_max) frame_max = db;
        }

        if (frame_max > self->max_mel_db) self->max_mel_db = frame_max;
        double floor = self->max_mel_db - 80.0;
        for (int m = 0; m < N_MELS; m++) if (self->mel_spectrogram[m * CACHE_SIZE + f_idx] < floor) self->mel_spectrogram[m * CACHE_SIZE + f_idx] = floor;
        int prev = (f_idx - 1 + CACHE_SIZE) % CACHE_SIZE;
        int mels_per_band = N_MELS / self->num_bands;
        for (int b = 0; b < self->num_bands; b++) {
            double fsum = 0;
            int m_start = b * mels_per_band;
            int m_end = (b == self->num_bands - 1) ? N_MELS : (b + 1) * mels_per_band;
            for (int m = m_start; m < m_end; m++) {
                double d = self->mel_spectrogram[m * CACHE_SIZE + f_idx] - self->mel_spectrogram[m * CACHE_SIZE + prev];
                if (d > 0) fsum += d;
            }
            float flux = (float)(fsum / (double)(m_end - m_start));
            self->flux_envelopes[b * CACHE_SIZE + f_idx] = flux;

            float prev_smooth = self->smoothing_states[b];
            if (flux > prev_smooth) {
                self->smoothing_states[b] = flux;
            } else {
                // Decay by 1/200th of the distance to the flux per 1ms frame
                self->smoothing_states[b] = prev_smooth - (prev_smooth - flux) / 200.0f;
            }
            self->dynamic_smoothings[b * CACHE_SIZE + f_idx] = self->smoothing_states[b];
        }
        self->cache_write_ptr = (self->cache_write_ptr + 1) % CACHE_SIZE;
        if (self->cache_count < CACHE_SIZE) self->cache_count++;
        self->total_frames_pushed++;
    }

    self->total_samples_received = current_total_samples;
    long long next_window_start_global = (long long)self->total_frames_pushed * hop - N_FFT / 2;
    if (next_window_start_global < 0) next_window_start_global = 0;

    if (next_window_start_global < self->total_samples_received) {
        int rem = (int)(self->total_samples_received - next_window_start_global);
        if (rem > N_FFT * 4) rem = N_FFT * 4;
        int offset = (int)(next_window_start_global - current_scratch_start);
        memcpy(self->overlap_buffer, self->combined_scratch + offset, sizeof(float) * rem);
        self->overlap_len = rem;
    } else {
        self->overlap_len = 0;
    }
}

int analyzer_analyze_chunk(TransientAnalyzer* self, const float* y, int len, int sr, int buffer_start_frame, int active_start_frame, ChunkAnalysisResult* result_out) {
    (void)buffer_start_frame;
    analyzer_cleanup_snapshots(self, active_start_frame);
    analyzer_push_audio(self, y, len, sr);
    int nf = self->cache_count, rptr = (self->cache_write_ptr - nf + CACHE_SIZE) % CACHE_SIZE;
    float *envs[MAX_BANDS] = {0}, *sm_envs[MAX_BANDS] = {0}, *thrs[MAX_BANDS] = {0};
    float half_maxes[MAX_BANDS];

    int num_b = self->num_bands;
    for (int b = 0; b < num_b; b++) {
        envs[b] = (float*)malloc(sizeof(float) * nf);
        sm_envs[b] = (float*)malloc(sizeof(float) * nf);
        thrs[b] = (float*)malloc(sizeof(float) * nf);
        if (!envs[b] || !sm_envs[b] || !thrs[b]) { for(int k=0; k<=b; k++) { free(envs[k]); free(sm_envs[k]); free(thrs[k]); } return 0; }
        for (int j = 0; j < nf; j++) {
            envs[b][j] = self->flux_envelopes[b * CACHE_SIZE + (rptr + j) % CACHE_SIZE];
            sm_envs[b][j] = self->dynamic_smoothings[b * CACHE_SIZE + (rptr + j) % CACHE_SIZE];
        }

        // Use current lookback parameters for this chunk
        int n_dyn_b = (int)(self->midpoint_lookback[b] / self->frame_duration_ms);
        if (n_dyn_b > nf) {
            n_dyn_b = nf;
        }
        if (n_dyn_b <= 0) {
            n_dyn_b = 1;
        }

        half_maxes[b] = calculate_half_max(envs[b] + nf - n_dyn_b, n_dyn_b);
        for (int j = 0; j < nf; j++) {
            thrs[b][j] = (float)self->smoothing_avgs[b];
        }

        // Report the parameters that derived the lookback used for this chunk
        result_out->metrics.band_lookbacks[b] = self->midpoint_lookback[b];
        result_out->metrics.band_avg_deltas[b] = self->lookback_avg_delta[b];
        result_out->metrics.band_total_deltas[b] = self->lookback_total_delta[b];
        result_out->metrics.band_p_counts[b] = self->lookback_p_count[b];

        // Calculate and store parameters for the NEXT analysis chunk based on current peak density
        double avg_delta_ms = 0;
        double total_delta_ms = 0;
        int p_count = 0;
        int lookback_frames = (int)(self->midpoint_lookback[b] / self->frame_duration_ms);
        int cutoff = active_start_frame - lookback_frames;

        if (self->lock_func) self->lock_func(self->lock_obj);
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) {
            if (curr->p_idx >= cutoff) {
                p_count++;
            }
            curr = curr->next;
        }
        if (self->unlock_func) self->unlock_func(self->lock_obj);

        if (p_count > 1) {
            total_delta_ms = (double)self->window_ms;
            avg_delta_ms = total_delta_ms / (double)p_count;
        } else {
            // Expansion Case: If 0 or 1 peak, set delta to 0 to expand window to self->window_ms
            total_delta_ms = 0.0;
            avg_delta_ms = 0.0;
        }

        self->lookback_avg_delta[b] = avg_delta_ms;
        self->lookback_total_delta[b] = total_delta_ms;
        self->lookback_p_count[b] = p_count;
        self->midpoint_lookback[b] = (double)self->window_ms - avg_delta_ms;
        if (self->midpoint_lookback[b] < 100.0) {
            self->midpoint_lookback[b] = 100.0;
        }
        if (self->midpoint_lookback[b] > (double)self->window_ms) {
            self->midpoint_lookback[b] = (double)self->window_ms;
        }
    }
    int *bpeaks[MAX_BANDS] = {0}, bpeak_counts[MAX_BANDS] = {0}; float *bth[MAX_BANDS] = {0}, *bl[MAX_BANDS] = {0}, *br[MAX_BANDS] = {0}, *bp[MAX_BANDS] = {0};
    for (int b = 0; b < num_b; b++) {
        float *env = envs[b], *thr = thrs[b]; int *tp = (int*)malloc(sizeof(int) * nf);
        float *tt = (float*)malloc(sizeof(float) * nf), *tl = (float*)malloc(sizeof(float) * nf), *tr = (float*)malloc(sizeof(float) * nf), *tm = (float*)malloc(sizeof(float) * nf);
        int pc = 0;
        if (tp && tt && tl && tr && tm) {
            for (int f = 1; f < nf - 1; f++) {
                if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thr[f] && env[f] >= 0.0f) {
                    bool replaced = false, too_close = false;
                    if (pc > 0 && f - tp[pc-1] < 200) { too_close = true; if (env[f] > env[tp[pc-1]]) replaced = true; }
                    if (!too_close || replaced) {
                        float lmin_s, rmin_s;
                        int cache_idx_f = (rptr + f) % CACHE_SIZE;
                        float prom_s = calculate_prominence_global(self, b, cache_idx_f, &lmin_s, &rmin_s);

                        if (prom_s > self->smoothing_avgs[b]) {
                            if (replaced) { tp[pc-1] = f; tt[pc-1] = thr[f]; tl[pc-1] = lmin_s; tr[pc-1] = rmin_s; tm[pc-1] = prom_s; }
                            else { tp[pc] = f; tt[pc] = thr[f]; tl[pc] = lmin_s; tr[pc] = rmin_s; tm[pc] = prom_s; pc++; }
                        }
                    }
                }
            }
            bpeaks[b] = (int*)malloc(sizeof(int) * pc);
            bth[b] = (float*)malloc(sizeof(float) * pc);
            bl[b] = (float*)malloc(sizeof(float) * pc);
            br[b] = (float*)malloc(sizeof(float) * pc);
            bp[b] = (float*)malloc(sizeof(float) * pc);
            bpeak_counts[b] = pc;
            if (bpeaks[b]) memcpy(bpeaks[b], tp, sizeof(int) * pc);
            if (bth[b]) memcpy(bth[b], tt, sizeof(float) * pc);
            if (bl[b]) memcpy(bl[b], tl, sizeof(float) * pc);
            if (br[b]) memcpy(br[b], tr, sizeof(float) * pc);
            if (bp[b]) memcpy(bp[b], tm, sizeof(float) * pc);
        }
        free(tp); free(tt); free(tl); free(tr); free(tm);
    }
    float gmax = 0; bool any = false;
    for (int b = 0; b < num_b; b++) for (int i = 0; i < bpeak_counts[b]; i++) { float v = envs[b][bpeaks[b][i]]; if (!any || v > gmax) { gmax = v; any = true; } }
    if (any) {
        if (self->lock_func) self->lock_func(self->lock_obj);
        if (self->shared_buffer) {
            if (gmax > (float)self->shared_buffer->max_peak) self->shared_buffer->max_peak = (double)gmax;
        } else {
            if (gmax > (float)self->private_max_peak) self->private_max_peak = (double)gmax;
        }
        if (self->unlock_func) self->unlock_func(self->lock_obj);
    }
    int tot = 0; for (int b = 0; b < num_b; b++) tot += bpeak_counts[b];
    PeakRef* pref = (PeakRef*)malloc(sizeof(PeakRef) * (tot + 1)); int* aind = (int*)malloc(sizeof(int) * (tot + 1));
    if (pref && aind) {
        int curr = 0; for (int b = 0; b < num_b; b++) for (int i = 0; i < bpeak_counts[b]; i++) { pref[curr].p_idx = bpeaks[b][i]; pref[curr].band_idx = b; aind[curr] = bpeaks[b][i]; curr++; }
        qsort(pref, tot, sizeof(PeakRef), compare_peaks);
        result_out->peak_list.num_peaks = 0;
        long long gstart = self->total_frames_pushed - self->cache_count;
        for (int b = 0; b < num_b; b++) for (int i = 0; i < 100; i++) {
            long long gf = (long long)active_start_frame + i, lf = gf - gstart;
            result_out->last_flux[b][i] = (lf >= 0 && lf < nf) ? envs[b][lf] : 0;

            float smooth = 0;
            float prom = 0;
            if (lf >= 0 && lf < nf) {
                int cache_idx = (rptr + (int)lf) % CACHE_SIZE;
                smooth = self->dynamic_smoothings[b * CACHE_SIZE + cache_idx];
                prom = calculate_prominence_global(self, b, cache_idx, NULL, NULL);

                // Store calculated prominence in the persistent cache
                self->prominence_envelopes[b * CACHE_SIZE + cache_idx] = prom;
            }
            result_out->last_dynamic_smoothing[b][i] = smooth;
            result_out->last_prominence[b][i] = prom;
        }
        for (int i = 0; i < tot; i++) {
            int p_idx = pref[i].p_idx, b = pref[i].band_idx; long long gp = gstart + p_idx;
            if (gp >= (long long)active_start_frame && gp < (long long)active_start_frame + 100) {
                double pv = 0, tv = 0, lv = 0, rv = 0, prv = 0;
                for (int k = 0; k < bpeak_counts[b]; k++) if (bpeaks[b][k] == p_idx) { pv = envs[b][p_idx]; tv = bth[b][k]; lv = bl[b][k]; rv = br[b][k]; prv = bp[b][k]; break; }
                PeakResult pr; double time = (double)gp * self->frame_duration_ms / 1000.0;
                if (analyzer_process_peak(self, p_idx, (int)gp, b, time, envs[b], nf, aind, tot, pv, tv, lv, rv, prv, &pr)) {
                    if (result_out->peak_list.num_peaks < MAX_PEAKS_PER_CHUNK) {
                        pr.p_idx = (int)gp;
                        result_out->peak_list.peaks[result_out->peak_list.num_peaks++] = pr;
                    }
                }
            }
        }
        analyzer_update_metrics(self, active_start_frame + 100, &result_out->metrics);
        for (int b = 0; b < num_b; b++) result_out->metrics.band_midpoints[b] = (double)half_maxes[b];
    }
    for (int b = 0; b < num_b; b++) {
        free(envs[b]); free(sm_envs[b]); free(thrs[b]); if (bpeak_counts[b] > 0) { free(bpeaks[b]); free(bth[b]); free(bl[b]); free(br[b]); free(bp[b]); }
    }
    free(pref); free(aind); return 1;
}

static double* create_mel_filterbank(int sr, int n_fft, int n_mels) {
    double* f = (double*)malloc(sizeof(double) * n_mels * (n_fft / 2 + 1)); if(!f) return NULL;
    memset(f, 0, sizeof(double) * n_mels * (n_fft / 2 + 1));
    double fmin = 0, fmax = sr / 2.0; static const double fsp = 200.0 / 3.0, mlhz = 1000.0;
    const double mlml = (mlhz - 0.0) / fsp, lstp = log(6.4) / 27.0;
    #define HZ_TO_MEL(hz) ((hz) < mlhz ? ((hz) - 0.0) / fsp : mlml + log((hz) / mlhz) / lstp)
    #define MEL_TO_HZ(mel) ((mel) < mlml ? 0.0 + (mel) * fsp : mlhz * exp(lstp * ((mel) - mlml)))
    double mmin = HZ_TO_MEL(fmin), mmax = HZ_TO_MEL(fmax);
    double* mp = (double*)malloc(sizeof(double) * (n_mels + 2)); if(!mp) { free(f); return NULL; }
    for (int i = 0; i < n_mels + 2; i++) mp[i] = MEL_TO_HZ(mmin + i * (mmax - mmin) / (n_mels + 1));
    for (int j = 0; j < n_mels; j++) {
        double flow = mp[j], fmid = mp[j+1], fhigh = mp[j+2];
        for (int i = 0; i < (n_fft / 2 + 1); i++) {
            double frq = i * (double)sr / n_fft, w = 0;
            if (frq >= flow && frq <= fmid) w = (frq - flow) / (fmid - flow);
            else if (frq >= fmid && frq <= fhigh) w = (fhigh - frq) / (fhigh - fmid);
            f[j * (n_fft / 2 + 1) + i] = w;
        }
        double en = 2.0 / (fhigh - flow); for (int i = 0; i < (n_fft / 2 + 1); i++) f[j * (n_fft / 2 + 1) + i] *= en;
    }
    free(mp); return f;
}

int analyzer_batch_analyze(const float* y, int len, int sr, int window_ms, int num_bands, FullAnalysisResult* result_out) {
    if (window_ms > 15000) window_ms = 15000;
    if (window_ms <= 0) window_ms = 15000;
    if (num_bands <= 0 || num_bands > MAX_BANDS) num_bands = MAX_BANDS;
    result_out->num_bands = num_bands;
    int hop = (int)(sr * 0.001), num_f = (len + hop - 1) / hop;
    result_out->num_frames = num_f; result_out->times = (float*)malloc(sizeof(float) * num_f); if(!result_out->times) return 0;
    for (int i = 0; i < num_f; i++) result_out->times[i] = (float)i * (float)hop / (float)sr;
    result_out->ratings = (double*)malloc(sizeof(double) * num_f);
    result_out->std_devs = (double*)malloc(sizeof(double) * num_f);
    result_out->means = (double*)malloc(sizeof(double) * num_f);
    result_out->contrasts = (double*)malloc(sizeof(double) * num_f);
    result_out->stability_scores = (double*)malloc(sizeof(double) * num_f);
    result_out->highest_peaks_ms = (double*)malloc(sizeof(double) * num_f);
    result_out->demarcation_lines = (double*)malloc(sizeof(double) * num_f);
    result_out->rolling_global_flux_avg = (float*)calloc(num_f, sizeof(float));
    result_out->rolling_global_smoothing_avg = (float*)calloc(num_f, sizeof(float));
    for (int b = 0; b < num_bands; b++) {
        result_out->bands[b].envelope = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_dynamic_smoothing = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_prominence = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_prominence_avg = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_prominence_half_max = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_smoothing_avg = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_flux_avg = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_threshold = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_lookback = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_avg_delta = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_total_delta = (float*)calloc(num_f, sizeof(float));
        result_out->bands[b].rolling_p_count = (int*)calloc(num_f, sizeof(int));
        result_out->bands[b].peaks = NULL;
        result_out->bands[b].num_peaks = 0;
    }
    float* gpu_flux = (float*)malloc(sizeof(float) * num_bands * num_f);
    int gpu_ok = 0;
    if (gpu_flux) {
        gpu_ok = gpu_stft_process(y, len, sr, gpu_flux, num_f, num_bands);
    }

    TransientAnalyzer* a = analyzer_create(1.0, NULL, NULL, NULL, NULL, window_ms, num_bands); if(!a) { free(gpu_flux); return 0; }
    analyzer_set_sample_rate(a, sr); int step = hop * 100;
    PeakResult* pband[MAX_BANDS]; int pcap[MAX_BANDS];
    for(int b=0; b<num_bands; b++) { pcap[b] = 1024; pband[b] = (PeakResult*)malloc(sizeof(PeakResult) * pcap[b]); }
    int flush_samples = (int)(sr * 0.3);
    for (int last_t = 0; last_t < len + flush_samples; last_t += step) {
        int act_s = last_t - (int)(sr * 0.2), win_s = act_s - (int)(sr * (window_ms / 1000.0)); if (win_s < 0) win_s = 0;
        ChunkAnalysisResult* res = (ChunkAnalysisResult*)malloc(sizeof(ChunkAnalysisResult));
        if (!res) { analyzer_destroy(a); free(gpu_flux); return 0; }
        float* push_ptr = (float*)calloc(step, sizeof(float));
        if (last_t < len) {
            int rem = len - last_t;
            memcpy(push_ptr, y + last_t, sizeof(float) * (rem < step ? rem : step));
        }
        analyzer_analyze_chunk(a, push_ptr, step, sr, win_s / hop, act_s / hop, res);
        free(push_ptr);
        for (int b = 0; b < num_bands; b++) {
            for (int i = 0; i < 100; i++) {
                int f = act_s / hop + i;
                if (f >= 0 && f < num_f) {
                    if (gpu_ok) {
                        result_out->bands[b].envelope[f] = gpu_flux[b * num_f + f];
                    } else {
                        result_out->bands[b].envelope[f] = res->last_flux[b][i];
                    }
                    result_out->bands[b].rolling_dynamic_smoothing[f] = res->last_dynamic_smoothing[b][i];
                    result_out->bands[b].rolling_prominence[f] = res->last_prominence[b][i];
                    result_out->bands[b].rolling_prominence_avg[f] = (float)res->metrics.band_prominence_avgs[b];
                    result_out->bands[b].rolling_prominence_half_max[f] = (float)res->metrics.band_prominence_half_maxes[b];
                    result_out->bands[b].rolling_smoothing_avg[f] = (float)res->metrics.band_smoothing_avgs[b];
                    result_out->bands[b].rolling_flux_avg[f] = (float)res->metrics.band_flux_avgs[b];
                    result_out->bands[b].rolling_threshold[f] = (float)res->metrics.band_midpoints[b];
                    result_out->bands[b].rolling_lookback[f] = (float)res->metrics.band_lookbacks[b];
                    result_out->bands[b].rolling_avg_delta[f] = (float)res->metrics.band_avg_deltas[b];
                    result_out->bands[b].rolling_total_delta[f] = (float)res->metrics.band_total_deltas[b];
                    result_out->bands[b].rolling_p_count[f] = res->metrics.band_p_counts[b];
                }
            }
        }
        for (int i = 0; i < 100; i++) { int f = act_s / hop + i; if (f >= 0 && f < num_f) { result_out->ratings[f] = res->metrics.rating; result_out->std_devs[f] = res->metrics.std_dev; result_out->means[f] = res->metrics.mean; result_out->contrasts[f] = res->metrics.contrast; result_out->stability_scores[f] = res->metrics.stability_score; result_out->highest_peaks_ms[f] = res->metrics.highest_peak_valid ? res->metrics.highest_peak_ms : -999.0; result_out->demarcation_lines[f] = res->metrics.demarcation_line; result_out->rolling_global_flux_avg[f] = (float)res->metrics.global_flux_avg; result_out->rolling_global_smoothing_avg[f] = (float)res->metrics.global_smoothing_avg; } }
        for (int i = 0; i < res->peak_list.num_peaks; i++) {
            PeakResult* pr = &res->peak_list.peaks[i]; int b = pr->band_idx;
            if (result_out->bands[b].num_peaks >= pcap[b]) { pcap[b] *= 2; PeakResult* np = realloc(pband[b], sizeof(PeakResult) * pcap[b]); if(np) pband[b] = np; }
            memcpy(&pband[b][result_out->bands[b].num_peaks++], pr, sizeof(PeakResult));
        }
        free(res);
    }
    if (gpu_flux) free(gpu_flux);
    result_out->max_peak_value = (float)analyzer_get_max_peak(a);
    result_out->min_score_seen = a->private_min_score_seen;
    result_out->max_score_seen = a->private_max_score_seen;
    result_out->tolerance = a->tolerance;
    for(int b=0; b<num_bands; b++) {
        int n = result_out->bands[b].num_peaks; result_out->bands[b].peaks = (PeakResult*)malloc(sizeof(PeakResult) * n);
        if(result_out->bands[b].peaks) memcpy(result_out->bands[b].peaks, pband[b], sizeof(PeakResult) * n);
        free(pband[b]);
    }
    analyzer_destroy(a); return 1;
}

void analyzer_free_analysis(FullAnalysisResult* result) {
    if (result->times) {
        free(result->times);
    }
    free(result->rolling_global_flux_avg);
    free(result->rolling_global_smoothing_avg);
    int num_b = (result->num_bands > 0 && result->num_bands <= MAX_BANDS) ? result->num_bands : MAX_BANDS;
    for (int i = 0; i < num_b; i++) {
        free(result->bands[i].envelope);
        free(result->bands[i].rolling_dynamic_smoothing);
        free(result->bands[i].rolling_prominence);
        free(result->bands[i].rolling_prominence_avg);
        free(result->bands[i].rolling_prominence_half_max);
        free(result->bands[i].rolling_smoothing_avg);
        free(result->bands[i].rolling_flux_avg);
        free(result->bands[i].rolling_threshold);
        free(result->bands[i].rolling_lookback);
        free(result->bands[i].rolling_avg_delta);
        free(result->bands[i].rolling_total_delta);
        free(result->bands[i].rolling_p_count);
        free(result->bands[i].peaks);
    }
    free(result->ratings);
    free(result->std_devs);
    free(result->means);
    free(result->contrasts);
    free(result->stability_scores);
    free(result->highest_peaks_ms);
    free(result->demarcation_lines);
}

/* ============================================================================
 * OpenCL GPU Compute Shader Acceleration Implementation & Host Dispatch
 * ============================================================================ */

#if __has_include(<CL/cl.h>)
#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
static CRITICAL_SECTION g_gpu_mutex;
static int g_mutex_inited = 0;
static inline void gpu_lock_init(void) { if (!g_mutex_inited) { InitializeCriticalSection(&g_gpu_mutex); g_mutex_inited = 1; } }
static inline void gpu_lock(void) { gpu_lock_init(); EnterCriticalSection(&g_gpu_mutex); }
static inline void gpu_unlock(void) { LeaveCriticalSection(&g_gpu_mutex); }
#else
#include <pthread.h>
static pthread_mutex_t g_gpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static inline void gpu_lock(void) { pthread_mutex_lock(&g_gpu_mutex); }
static inline void gpu_unlock(void) { pthread_mutex_unlock(&g_gpu_mutex); }
#endif

static cl_platform_id g_cl_platform = NULL;
static cl_device_id g_cl_device = NULL;
static cl_context g_cl_context = NULL;
static cl_command_queue g_cl_queue = NULL;
static cl_program g_cl_program = NULL;
static cl_kernel g_cl_kernel = NULL;
static char g_cl_device_name[256] = "GPU";
static int g_cl_available = -1; // -1: uninitialized, 0: failed/unavailable, 1: ready

static const char* opencl_stft_kernel_source =
"__kernel void compute_spectral_flux(\n"
"    __global const float* pcm,\n"
"    const int total_samples,\n"
"    const int hop_size,\n"
"    const int n_fft,\n"
"    const int sr,\n"
"    __global float* flux_out,\n"
"    const int total_frames,\n"
"    const int num_bands)\n"
"{\n"
"    int f = get_global_id(0);\n"
"    if (f >= total_frames) return;\n"
"    int center = f * hop_size;\n"
"    int start_idx = center - n_fft / 2;\n"
"    float real_part[2048];\n"
"    for (int i = 0; i < 2048; i++) {\n"
"        int s = start_idx + i;\n"
"        float val = (s >= 0 && s < total_samples) ? pcm[s] : 0.0f;\n"
"        float w = 0.5f * (1.0f - cos(2.0f * 3.1415926535f * (float)i / 2048.0f));\n"
"        real_part[i] = val * w;\n"
"    }\n"
"    float mels[128];\n"
"    for (int m = 0; m < 128; m++) {\n"
"        float sum_p = 0.0f;\n"
"        int bin_start = m * 8;\n"
"        int bin_end = bin_start + 8;\n"
"        for (int k = bin_start; k < bin_end; k++) {\n"
"            float angle = -2.0f * 3.1415926535f * (float)k / 2048.0f;\n"
"            float c = cos(angle);\n"
"            float s = sin(angle);\n"
"            float r = 0.0f, im = 0.0f;\n"
"            for (int i = 0; i < 2048; i++) {\n"
"                r += real_part[i] * c;\n"
"                im += real_part[i] * s;\n"
"            }\n"
"            sum_p += (r*r + im*im) / (2048.0f * 2048.0f);\n"
"        }\n"
"        if (sum_p < 1e-10f) sum_p = 1e-10f;\n"
"        mels[m] = 10.0f * log10(sum_p);\n"
"    }\n"
"    int mels_per_band = 128 / num_bands;\n"
"    for (int b = 0; b < num_bands; b++) {\n"
"        float bsum = 0.0f;\n"
"        int m_start = b * mels_per_band;\n"
"        int m_end = (b == num_bands - 1) ? 128 : (b + 1) * mels_per_band;\n"
"        for (int m = m_start; m < m_end; m++) {\n"
"            bsum += mels[m];\n"
"        }\n"
"        flux_out[b * total_frames + f] = bsum / (float)(m_end - m_start);\n"
"    }\n"
"}\n";

int gpu_stft_init(void) {
    gpu_lock();
    if (g_cl_available != -1) {
        int avail = g_cl_available;
        gpu_unlock();
        return avail;
    }

    cl_int err;
    cl_uint num_platforms = 0;
    err = clGetPlatformIDs(1, &g_cl_platform, &num_platforms);
    if (err != CL_SUCCESS || num_platforms == 0) {
        printf("[GPU Compute] Note: No OpenCL platform found. Falling back to CPU FFT pipeline.\n");
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    cl_uint num_devices = 0;
    err = clGetDeviceIDs(g_cl_platform, CL_DEVICE_TYPE_GPU, 1, &g_cl_device, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        printf("[GPU Compute] Note: No GPU OpenCL device found. Falling back to CPU FFT pipeline.\n");
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    g_cl_context = clCreateContext(NULL, 1, &g_cl_device, NULL, NULL, &err);
    if (err != CL_SUCCESS || !g_cl_context) {
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

#if defined(CL_VERSION_2_0)
    g_cl_queue = clCreateCommandQueueWithProperties(g_cl_context, g_cl_device, NULL, &err);
#else
    g_cl_queue = clCreateCommandQueue(g_cl_context, g_cl_device, 0, &err);
#endif
    if (err != CL_SUCCESS || !g_cl_queue) {
        clReleaseContext(g_cl_context);
        g_cl_context = NULL;
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    g_cl_program = clCreateProgramWithSource(g_cl_context, 1, &opencl_stft_kernel_source, NULL, &err);
    if (err != CL_SUCCESS || !g_cl_program) {
        clReleaseCommandQueue(g_cl_queue);
        clReleaseContext(g_cl_context);
        g_cl_queue = NULL;
        g_cl_context = NULL;
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    err = clBuildProgram(g_cl_program, 1, &g_cl_device, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        clReleaseProgram(g_cl_program);
        clReleaseCommandQueue(g_cl_queue);
        clReleaseContext(g_cl_context);
        g_cl_program = NULL;
        g_cl_queue = NULL;
        g_cl_context = NULL;
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    g_cl_kernel = clCreateKernel(g_cl_program, "compute_spectral_flux", &err);
    if (err != CL_SUCCESS || !g_cl_kernel) {
        clReleaseProgram(g_cl_program);
        clReleaseCommandQueue(g_cl_queue);
        clReleaseContext(g_cl_context);
        g_cl_program = NULL;
        g_cl_queue = NULL;
        g_cl_context = NULL;
        g_cl_available = 0;
        gpu_unlock();
        return 0;
    }

    clGetDeviceInfo(g_cl_device, CL_DEVICE_NAME, sizeof(g_cl_device_name), g_cl_device_name, NULL);
    printf("[GPU Compute] Initialized OpenCL Compute Shader Engine on device: %s\n", g_cl_device_name);

    g_cl_available = 1;
    gpu_unlock();
    return 1;
}

void gpu_stft_cleanup(void) {
    gpu_lock();
    if (g_cl_kernel) clReleaseKernel(g_cl_kernel);
    if (g_cl_program) clReleaseProgram(g_cl_program);
    if (g_cl_queue) clReleaseCommandQueue(g_cl_queue);
    if (g_cl_context) clReleaseContext(g_cl_context);
    g_cl_kernel = NULL;
    g_cl_program = NULL;
    g_cl_queue = NULL;
    g_cl_context = NULL;
    g_cl_available = -1;
    gpu_unlock();
}

int gpu_stft_process(const float* pcm, int num_samples, int sr, float* flux_out, int num_frames, int num_bands) {
    if (!gpu_stft_init()) return 0;
    if (!pcm || num_samples <= 0 || num_frames <= 0 || !flux_out) return 0;
    if (num_bands <= 0 || num_bands > MAX_BANDS) num_bands = MAX_BANDS;

    cl_int err;
    int hop_size = (int)(sr * 0.001);
    int n_fft = 2048;

    cl_mem d_pcm = clCreateBuffer(g_cl_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof(float) * num_samples, (void*)pcm, &err);
    if (err != CL_SUCCESS) return 0;

    cl_mem d_flux = clCreateBuffer(g_cl_context, CL_MEM_WRITE_ONLY, sizeof(float) * num_bands * num_frames, NULL, &err);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(d_pcm);
        return 0;
    }

    // Create a per-thread command queue for lock-free parallel execution
    cl_command_queue queue = clCreateCommandQueue(g_cl_context, g_cl_device, 0, &err);
    if (err != CL_SUCCESS || !queue) {
        clReleaseMemObject(d_flux);
        clReleaseMemObject(d_pcm);
        return 0;
    }

    cl_kernel kernel = clCreateKernel(g_cl_program, "compute_spectral_flux", &err);
    if (err != CL_SUCCESS || !kernel) {
        clReleaseCommandQueue(queue);
        clReleaseMemObject(d_flux);
        clReleaseMemObject(d_pcm);
        return 0;
    }

    err  = clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_pcm);
    err |= clSetKernelArg(kernel, 1, sizeof(int), &num_samples);
    err |= clSetKernelArg(kernel, 2, sizeof(int), &hop_size);
    err |= clSetKernelArg(kernel, 3, sizeof(int), &n_fft);
    err |= clSetKernelArg(kernel, 4, sizeof(int), &sr);
    err |= clSetKernelArg(kernel, 5, sizeof(cl_mem), &d_flux);
    err |= clSetKernelArg(kernel, 6, sizeof(int), &num_frames);
    err |= clSetKernelArg(kernel, 7, sizeof(int), &num_bands);

    if (err != CL_SUCCESS) {
        clReleaseKernel(kernel);
        clReleaseCommandQueue(queue);
        clReleaseMemObject(d_flux);
        clReleaseMemObject(d_pcm);
        return 0;
    }

    size_t global_work_size = (size_t)num_frames;
    printf("[GPU Compute] Executing STFT & Spectral Flux compute shader on GPU (%s)...\n", g_cl_device_name);
    err = clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    if (err == CL_SUCCESS) {
        err = clEnqueueReadBuffer(queue, d_flux, CL_TRUE, 0, sizeof(float) * num_bands * num_frames, flux_out, 0, NULL, NULL);
    }

    clReleaseKernel(kernel);
    clReleaseCommandQueue(queue);
    clReleaseMemObject(d_flux);
    clReleaseMemObject(d_pcm);

    return (err == CL_SUCCESS) ? 1 : 0;
}

#else

int gpu_stft_init(void) {
    return 0;
}

void gpu_stft_cleanup(void) {
}

int gpu_stft_process(const float* pcm, int num_samples, int sr, float* flux_out, int num_frames, int num_bands) {
    (void)pcm; (void)num_samples; (void)sr; (void)flux_out; (void)num_frames; (void)num_bands;
    return 0;
}

#endif
