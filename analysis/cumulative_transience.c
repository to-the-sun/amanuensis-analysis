#define _USE_MATH_DEFINES
#include "cumulative_transience.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

TransientAnalyzer* analyzer_create(double max_peak_value) {
    TransientAnalyzer* self = (TransientAnalyzer*)calloc(1, sizeof(TransientAnalyzer));
    if (!self) return NULL;

    self->max_peak = max_peak_value;
    for (int i = 0; i < BUFFER_LEN; i++) {
        self->buffer_times[i] = -5000.0 + i;
    }

    self->min_score_seen = 0.0;
    self->max_score_seen = 0.0;
    self->last_score_avg = 0.0;

    return self;
}

void analyzer_destroy(TransientAnalyzer* self) {
    for (int b = 0; b < MAX_BANDS; b++) {
        SnapshotEntry* curr = self->snapshot_heads[b];
        while (curr) {
            SnapshotEntry* next = curr->next;
            free(curr);
            curr = next;
        }
    }
    free(self);
}

static int compare_events(const void* a, const void* b) {
    ScoreEvent* e1 = (ScoreEvent*)a;
    ScoreEvent* e2 = (ScoreEvent*)b;
    if (e1->frame != e2->frame)
        return e1->frame - e2->frame;
    if (e1->type != e2->type)
        return e1->type - e2->type;
    if (e1->score < e2->score) return -1;
    if (e1->score > e2->score) return 1;
    return 0;
}

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          PeakResult* result_out) {

    result_out->p_idx = p_idx;
    result_out->band_idx = band_idx;
    result_out->time = time;
    result_out->peak_val = (double)env_ptr[p_idx];
    result_out->total_score = 0;
    result_out->num_qualifiers = 0;

    int start = p_idx - 5000;

    // Fill snapshot
    for (int i = 0; i < BUFFER_LEN; i++) {
        int idx = start + i;
        if (idx < 0 || idx >= env_len) {
            result_out->snapshot[i] = 0.0;
        } else {
            result_out->snapshot[i] = (double)env_ptr[idx];
        }
    }

    double normalization = (self->max_peak > 0) ? (result_out->peak_val / self->max_peak) : 1.0;
    for (int i = 0; i < BUFFER_LEN; i++) {
        result_out->snapshot[i] *= normalization;
    }

    // Calculate Resonance Score
    double qualifier_sum = 0.0;
    bool found_peak = false;

    // data_to_measure is accumulated_buffer[:-99]
    int m_len = BUFFER_LEN - 99;
    double sum = 0.0;
    double max_v = -DBL_MAX;
    double min_v = DBL_MAX;

    if (m_len > 0) {
        max_v = self->accumulated_buffer[0];
        min_v = self->accumulated_buffer[0];
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            sum += val;
            if (val > max_v) max_v = val;
            if (val < min_v) min_v = val;
        }
    }
    double avg = (m_len > 0) ? (sum / (double)m_len) : 0.0;

    for (int i = 0; i < all_valid_count; i++) {
        int s_idx = all_valid_peak_indices[i];
        if (s_idx >= p_idx - 5000 && s_idx <= p_idx - 99) {
            int sp_idx = 5000 - (p_idx - s_idx);
            double val = self->accumulated_buffer[sp_idx];
            double qualifier = 0.0;
            if (val > avg) {
                if (max_v > avg) qualifier = (val - avg) / (max_v - avg);
            } else if (val < avg) {
                if (avg > min_v) qualifier = (val - avg) / (avg - min_v);
            }

            if (result_out->num_qualifiers < MAX_QUALIFIERS) {
                result_out->qualifiers[result_out->num_qualifiers].ms = self->buffer_times[sp_idx];
                result_out->qualifiers[result_out->num_qualifiers].val = qualifier;
                result_out->num_qualifiers++;
            }
            qualifier_sum += qualifier;
            found_peak = true;
        }
    }

    if (found_peak) {
        result_out->total_score = result_out->peak_val * qualifier_sum;
    }

    // Update dynamic range
    if (result_out->total_score < self->min_score_seen) self->min_score_seen = result_out->total_score;
    if (result_out->total_score > self->max_score_seen) self->max_score_seen = result_out->total_score;

    self->total_score_sum += result_out->total_score;
    self->score_count++;

    // Add to event queue
    if (self->event_count + 2 <= MAX_EVENTS) {
        ScoreEvent new_evts[2];
        new_evts[0].frame = p_idx;
        new_evts[0].type = 0; // ADD
        new_evts[0].score = result_out->total_score;
        new_evts[1].frame = p_idx + 10;
        new_evts[1].type = 1; // REMOVE
        new_evts[1].score = result_out->total_score;

        for (int k = 0; k < 2; k++) {
            int j = self->event_count - 1;
            while (j >= self->event_read_ptr && compare_events(&self->upcoming_events[j], &new_evts[k]) > 0) {
                self->upcoming_events[j+1] = self->upcoming_events[j];
                j--;
            }
            self->upcoming_events[j+1] = new_evts[k];
            self->event_count++;
        }
        qsort(self->upcoming_events + self->event_read_ptr, self->event_count - self->event_read_ptr, sizeof(ScoreEvent), compare_events);
    }

    // Update accumulated buffer
    for (int i = 0; i < BUFFER_LEN; i++) {
        self->accumulated_buffer[i] += result_out->snapshot[i];
    }

    // Store snapshot for later cleanup (queue based)
    SnapshotEntry* entry = (SnapshotEntry*)malloc(sizeof(SnapshotEntry));
    entry->p_idx = p_idx;
    memcpy(entry->snapshot, result_out->snapshot, sizeof(double) * BUFFER_LEN);
    entry->next = NULL;

    if (self->snapshot_tails[band_idx]) {
        self->snapshot_tails[band_idx]->next = entry;
        self->snapshot_tails[band_idx] = entry;
    } else {
        self->snapshot_heads[band_idx] = entry;
        self->snapshot_tails[band_idx] = entry;
    }

    return 1;
}

void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out) {
    int cleanup_frame_threshold = frame - 15000;
    bool buffer_updated = false;

    for (int b = 0; b < MAX_BANDS; b++) {
        while (self->snapshot_heads[b] && self->snapshot_heads[b]->p_idx <= cleanup_frame_threshold) {
            SnapshotEntry* entry = self->snapshot_heads[b];
            for (int j = 0; j < BUFFER_LEN; j++) {
                self->accumulated_buffer[j] -= entry->snapshot[j];
            }
            self->snapshot_heads[b] = entry->next;
            if (!self->snapshot_heads[b]) self->snapshot_tails[b] = NULL;
            free(entry);
            buffer_updated = true;
        }
    }

    int m_len = BUFFER_LEN - 99;
    double sum = 0.0, sum_sq = 0.0;
    double max_v = -DBL_MAX;

    if (m_len > 0) {
        max_v = self->accumulated_buffer[0];
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            sum += val;
            sum_sq += val * val;
            if (val > max_v) max_v = val;
        }
    }

    double mean = (m_len > 0) ? (sum / (double)m_len) : 0.0;
    double variance = (m_len > 0) ? (sum_sq / (double)m_len - mean * mean) : 0.0;
    if (variance < 0) variance = 0;
    double std_dev = sqrt(variance);

    metrics_out->std_dev = std_dev;
    metrics_out->mean = mean;
    metrics_out->contrast = (mean > 0) ? (max_v / mean) : 0;
    metrics_out->rating = (self->score_count > 0) ? (self->total_score_sum / self->score_count) : 0;
    metrics_out->buffer_updated = buffer_updated;
    metrics_out->min_score_seen = self->min_score_seen;
    metrics_out->max_score_seen = self->max_score_seen;
    metrics_out->highest_peak_valid = false;

    if (max_v > 0.1) {
        int highest_idx = -1;
        double highest_val = -1;
        for (int i = 0; i < m_len; i++) {
            double val = self->accumulated_buffer[i];
            if (val > mean && val > highest_val) {
                highest_val = val;
                highest_idx = i;
            }
        }

        if (highest_idx != -1) {
            metrics_out->highest_peak_ms = self->buffer_times[highest_idx];
            metrics_out->highest_peak_valid = true;

            if (self->peak_history_count < MAX_PEAK_HISTORY) {
                self->peak_history[self->peak_history_count++] = metrics_out->highest_peak_ms;
            }
        }
    }

    double ph_sum = 0, ph_sum_sq = 0;
    for (int i = 0; i < self->peak_history_count; i++) {
        ph_sum += self->peak_history[i];
        ph_sum_sq += self->peak_history[i] * self->peak_history[i];
    }
    double ph_mean = (self->peak_history_count > 0) ? (ph_sum / self->peak_history_count) : 0;
    double ph_var = (self->peak_history_count > 0) ? (ph_sum_sq / self->peak_history_count - ph_mean * ph_mean) : 0;
    if (ph_var < 0) ph_var = 0;
    metrics_out->peak_std = sqrt(ph_var);

    // Optimized rolling score
    while (self->event_read_ptr < self->event_count && self->upcoming_events[self->event_read_ptr].frame <= frame) {
        ScoreEvent evt = self->upcoming_events[self->event_read_ptr];
        self->event_read_ptr++;

        if (evt.type == 0) { // ADD
            if (self->current_window_count < MAX_EVENTS) {
                self->current_window_scores[self->current_window_count++] = evt.score;
            }
        } else { // REMOVE
            for (int i = 0; i < self->current_window_count; i++) {
                if (fabs(self->current_window_scores[i] - evt.score) < 1e-12) {
                    for (int j = i; j < self->current_window_count - 1; j++) {
                        self->current_window_scores[j] = self->current_window_scores[j+1];
                    }
                    self->current_window_count--;
                    break;
                }
            }
        }

        if (self->current_window_count > 0) {
            double win_sum = 0;
            for (int i = 0; i < self->current_window_count; i++) win_sum += self->current_window_scores[i];
            self->last_score_avg = win_sum / (double)self->current_window_count;
        }
    }
    metrics_out->rolling_score = self->last_score_avg;
}

double* analyzer_get_buffer(TransientAnalyzer* self) {
    return self->accumulated_buffer;
}

// FFTW Forward Declarations
typedef struct fftw_plan_s *fftw_plan;
extern fftw_plan fftw_plan_dft_r2c_1d(int n, double *in, double *out, unsigned flags);
extern void fftw_execute(fftw_plan p);
extern void fftw_destroy_plan(fftw_plan p);
#define FFTW_ESTIMATE (1U << 6)

// Constants for STFT
#define N_FFT 2048
#define N_MELS 128

static double* create_mel_filterbank(int sr, int n_fft, int n_mels) {
    double* filters = (double*)malloc(sizeof(double) * n_mels * (n_fft / 2 + 1));
    memset(filters, 0, sizeof(double) * n_mels * (n_fft / 2 + 1));

    double f_min = 0;
    double f_max = sr / 2.0;

    #define HZ_TO_MEL(hz) (2595.0 * log10(1.0 + (hz) / 700.0))
    #define MEL_TO_HZ(mel) (700.0 * (pow(10.0, (mel) / 2595.0) - 1.0))

    double mel_min = HZ_TO_MEL(f_min);
    double mel_max = HZ_TO_MEL(f_max);

    double* mel_points = (double*)malloc(sizeof(double) * (n_mels + 2));
    for (int i = 0; i < n_mels + 2; i++) {
        double mel = mel_min + i * (mel_max - mel_min) / (n_mels + 1);
        mel_points[i] = MEL_TO_HZ(mel);
    }

    int* bin_points = (int*)malloc(sizeof(int) * (n_mels + 2));
    for (int i = 0; i < n_mels + 2; i++) {
        bin_points[i] = (int)floor((n_fft + 1) * mel_points[i] / sr);
    }

    for (int j = 0; j < n_mels; j++) {
        for (int i = bin_points[j]; i < bin_points[j+1]; i++) {
            filters[j * (n_fft / 2 + 1) + i] = (i - bin_points[j]) / (double)(bin_points[j+1] - bin_points[j]);
        }
        for (int i = bin_points[j+1]; i < bin_points[j+2]; i++) {
            filters[j * (n_fft / 2 + 1) + i] = (bin_points[j+2] - i) / (double)(bin_points[j+2] - bin_points[j+1]);
        }
    }

    for (int i = 0; i < n_mels; i++) {
        double enorm = 2.0 / (mel_points[i+2] - mel_points[i]);
        for (int j = 0; j < (n_fft/2+1); j++) {
            filters[i * (n_fft/2+1) + j] *= enorm;
        }
    }

    free(mel_points);
    free(bin_points);
    return filters;
}

int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out) {
    result_out->rolling_scores = NULL;
    result_out->ratings = NULL;
    result_out->std_devs = NULL;
    result_out->contrasts = NULL;
    result_out->peak_stds = NULL;

    int hop_length = sr / 1000;
    int n_fft = N_FFT;
    int n_mels = N_MELS;
    int num_frames = (len + hop_length - 1) / hop_length;

    result_out->num_frames = num_frames;
    result_out->times = (float*)malloc(sizeof(float) * num_frames);
    for (int i = 0; i < num_frames; i++) {
        result_out->times[i] = i * 0.001f;
    }

    double* mel_filters = create_mel_filterbank(sr, n_fft, n_mels);
    double* fft_in = (double*)malloc(sizeof(double) * n_fft);
    double* fft_out = (double*)malloc(sizeof(double) * (n_fft / 2 + 1) * 2);
    fftw_plan p = fftw_plan_dft_r2c_1d(n_fft, fft_in, fft_out, FFTW_ESTIMATE);

    double* window = (double*)malloc(sizeof(double) * n_fft);
    for (int i = 0; i < n_fft; i++) {
        window[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (n_fft - 1)));
    }

    float* mel_spectrogram = (float*)malloc(sizeof(float) * n_mels * num_frames);
    memset(mel_spectrogram, 0, sizeof(float) * n_mels * num_frames);

    for (int f = 0; f < num_frames; f++) {
        int center = f * hop_length;
        int start = center;

        for (int i = 0; i < n_fft; i++) {
            int idx = start + i;
            if (idx >= 0 && idx < len) {
                fft_in[i] = y[idx] * window[i];
            } else {
                fft_in[i] = 0;
            }
        }
        fftw_execute(p);

        for (int m = 0; m < n_mels; m++) {
            double mel_val = 0;
            for (int i = 0; i < (n_fft / 2 + 1); i++) {
                double re = fft_out[i * 2];
                double im = fft_out[i * 2 + 1];
                mel_val += (re * re + im * im) * mel_filters[m * (n_fft / 2 + 1) + i];
            }
            mel_spectrogram[m * num_frames + f] = (float)mel_val;
        }
    }

    float max_power = 0;
    for (int i = 0; i < n_mels * num_frames; i++) {
        if (mel_spectrogram[i] > max_power) max_power = mel_spectrogram[i];
    }
    float ref = max_power;
    for (int i = 0; i < n_mels * num_frames; i++) {
        float val = mel_spectrogram[i];
        if (val < 1e-10) val = 1e-10;
        mel_spectrogram[i] = 10.0f * log10f(val / ref);
        if (mel_spectrogram[i] < -80.0f) mel_spectrogram[i] = -80.0f;
    }

    for (int b = 0; b < MAX_BANDS; b++) {
        result_out->bands[b].envelope = (float*)malloc(sizeof(float) * num_frames);
        memset(result_out->bands[b].envelope, 0, sizeof(float) * num_frames);

        for (int f = 1; f < num_frames; f++) {
            float flux = 0;
            for (int m = b * 32; m < (b + 1) * 32; m++) {
                float diff = mel_spectrogram[m * num_frames + f] - mel_spectrogram[m * num_frames + f - 1];
                if (diff > 0) flux += diff;
            }
            result_out->bands[b].envelope[f] = flux / 32.0f;
        }

        result_out->bands[b].rolling_threshold = (float*)malloc(sizeof(float) * num_frames);
        double current_sum = 0;
        int window_size = 15000;
        for (int f = 0; f < num_frames; f++) {
            current_sum += result_out->bands[b].envelope[f];
            if (f >= window_size) {
                current_sum -= result_out->bands[b].envelope[f - window_size];
                result_out->bands[b].rolling_threshold[f] = (float)(current_sum / window_size);
            } else {
                result_out->bands[b].rolling_threshold[f] = (float)(current_sum / (f + 1));
            }
        }
    }

    float global_max = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int f = 0; f < num_frames; f++) {
            if (result_out->bands[b].envelope[f] > global_max) global_max = result_out->bands[b].envelope[f];
        }
    }
    result_out->max_peak_value = global_max;

    for (int b = 0; b < MAX_BANDS; b++) {
        float* env = result_out->bands[b].envelope;
        float* thresh = result_out->bands[b].rolling_threshold;
        int* temp_peaks = (int*)malloc(sizeof(int) * num_frames);
        int peak_count = 0;

        for (int f = 1; f < num_frames - 1; f++) {
            if (env[f] > env[f-1] && env[f] > env[f+1] && env[f] > thresh[f]) {
                bool too_close = false;
                if (peak_count > 0 && f - temp_peaks[peak_count-1] < 200) {
                    if (env[f] > env[temp_peaks[peak_count-1]]) {
                        temp_peaks[peak_count-1] = f;
                    }
                    too_close = true;
                }

                if (!too_close) {
                    if (env[f] - env[f-1] > 0.5 || env[f] - env[f+1] > 0.5) {
                        temp_peaks[peak_count++] = f;
                    }
                }
            }
        }

        result_out->bands[b].peaks = (int*)malloc(sizeof(int) * peak_count);
        memcpy(result_out->bands[b].peaks, temp_peaks, sizeof(int) * peak_count);
        result_out->bands[b].num_peaks = peak_count;
        free(temp_peaks);
    }

    fftw_destroy_plan(p);
    free(fft_in);
    free(fft_out);
    free(window);
    free(mel_filters);
    free(mel_spectrogram);

    return 1;
}

int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out) {
    if (!analyzer_analyze_audio(y, len, sr, result_out)) return 0;

    int num_frames = result_out->num_frames;
    result_out->rolling_scores = (double*)malloc(sizeof(double) * num_frames);
    result_out->ratings = (double*)malloc(sizeof(double) * num_frames);
    result_out->std_devs = (double*)malloc(sizeof(double) * num_frames);
    result_out->contrasts = (double*)malloc(sizeof(double) * num_frames);
    result_out->peak_stds = (double*)malloc(sizeof(double) * num_frames);

    TransientAnalyzer* analyzer = analyzer_create(result_out->max_peak_value);

    int total_peaks = 0;
    for (int b = 0; b < MAX_BANDS; b++) total_peaks += result_out->bands[b].num_peaks;
    int* all_valid_peaks = (int*)malloc(sizeof(int) * total_peaks);
    int curr = 0;
    for (int b = 0; b < MAX_BANDS; b++) {
        for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
            all_valid_peaks[curr++] = result_out->bands[b].peaks[i];
        }
    }

    for (int f = 0; f < num_frames; f++) {
        for (int b = 0; b < MAX_BANDS; b++) {
            for (int i = 0; i < result_out->bands[b].num_peaks; i++) {
                int p_idx = result_out->bands[b].peaks[i];
                if (p_idx == f) {
                    PeakResult pr;
                    analyzer_process_peak(analyzer, p_idx, b, result_out->times[f], result_out->bands[b].envelope, num_frames, all_valid_peaks, total_peaks, &pr);
                }
            }
        }

        AnalyzerMetrics m;
        analyzer_update_metrics(analyzer, f, &m);
        result_out->rolling_scores[f] = m.rolling_score;
        result_out->ratings[f] = m.rating;
        result_out->std_devs[f] = m.std_dev;
        result_out->contrasts[f] = m.contrast;
        result_out->peak_stds[f] = m.peak_std;
    }

    free(all_valid_peaks);
    analyzer_destroy(analyzer);
    return 1;
}

void analyzer_free_analysis(FullAnalysisResult* result) {
    if (result->times) free(result->times);
    for (int i = 0; i < MAX_BANDS; i++) {
        if (result->bands[i].envelope) free(result->bands[i].envelope);
        if (result->bands[i].rolling_threshold) free(result->bands[i].rolling_threshold);
        if (result->bands[i].peaks) free(result->bands[i].peaks);
    }
    if (result->rolling_scores) free(result->rolling_scores);
    if (result->ratings) free(result->ratings);
    if (result->std_devs) free(result->std_devs);
    if (result->contrasts) free(result->contrasts);
    if (result->peak_stds) free(result->peak_stds);
}
