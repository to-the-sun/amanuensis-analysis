#ifndef CUMULATIVE_TRANSIENCE_H
#define CUMULATIVE_TRANSIENCE_H

#include <stdint.h>
#include <stdbool.h>

#define BUFFER_LEN 15001
#define MAX_BANDS 4
#define MAX_QUALIFIERS 256

typedef struct {
    double ms;
    double val;
    double orig_ms;
} Qualifier;

typedef struct {
    int p_idx;
    int band_idx;
    double time;
    double peak_val;
    double total_score;
    double detected_peak_val;   // The EXACT flux value at detection
    double thresh_val;
    double left_min;
    double right_min;
    double prominence;
    int num_qualifiers;
    Qualifier qualifiers[MAX_QUALIFIERS];
    double snapshot[BUFFER_LEN];
} PeakResult;

typedef struct SnapshotEntry {
    int p_idx;
    double snapshot[BUFFER_LEN];
    struct SnapshotEntry* next;
} SnapshotEntry;

typedef struct {
    double std_dev;
    double mean;
    double contrast;
    double stability_score;
    double rating;
    int buffer_updated;
    double highest_peak_ms;
    int highest_peak_valid;
    double min_score_seen;
    double max_score_seen;
    double demarcation_line;
    double band_midpoints[MAX_BANDS];
    double band_lookbacks[MAX_BANDS];
    double band_avg_deltas[MAX_BANDS];
    double band_total_deltas[MAX_BANDS];
    int band_p_counts[MAX_BANDS];
    double band_prominence_avgs[MAX_BANDS];
    double band_prominence_half_maxes[MAX_BANDS];
    double band_smoothing_avgs[MAX_BANDS];
    double band_flux_avgs[MAX_BANDS];
    double global_flux_avg;
    double global_smoothing_avg;
} AnalyzerMetrics;

#define MAX_PEAKS_PER_CHUNK 64

typedef void (*ct_lock_func)(void* lock_obj);

typedef struct {
    double accumulated_buffer[BUFFER_LEN];
    double max_peak;
    double min_score_seen;
    double max_score_seen;
    double total_score_sum;
    int score_count;
} SharedTransientBuffer;

typedef struct {
    PeakResult peaks[MAX_PEAKS_PER_CHUNK];
    int num_peaks;
} PeakResultList;

typedef struct {
    PeakResultList peak_list;
    AnalyzerMetrics metrics;
    float last_flux[MAX_BANDS][100];
    float last_dynamic_smoothing[MAX_BANDS][100];
    float last_prominence[MAX_BANDS][100];
} ChunkAnalysisResult;

typedef struct {
    SharedTransientBuffer* shared_buffer;
    void* lock_obj;
    ct_lock_func lock_func;
    ct_lock_func unlock_func;

    double private_accumulated_buffer[BUFFER_LEN];
    double buffer_times[BUFFER_LEN];
    double private_max_peak;

    double private_min_score_seen;
    double private_max_score_seen;
    double private_total_score_sum;
    int private_score_count;

    double highest_peak_ms;
    double midpoint_lookback[MAX_BANDS];
    double lookback_avg_delta[MAX_BANDS];
    double lookback_total_delta[MAX_BANDS];
    int lookback_p_count[MAX_BANDS];

    // Bar Length History for Stability
    int bar_length_counts[BUFFER_LEN];

    // Snapshots tracking (queue per band)
    SnapshotEntry* snapshot_heads[MAX_BANDS];
    SnapshotEntry* snapshot_tails[MAX_BANDS];

    double frame_duration_ms;
    int window_ms;

    // Incremental Cache State
    double* mel_spectrogram;    // Mel bands cache
    float* flux_envelopes;      // Flux cache per band
    float* dynamic_smoothings;  // Dynamic smoothing cache per band
    float* prominence_envelopes; // Prominence cache per band
    float smoothing_states[MAX_BANDS];
    double smoothing_avgs[MAX_BANDS];
    double* mel_filters;        // Pre-calculated filters
    double* fft_window;         // Pre-calculated window
    int cache_write_ptr;
    int cache_count;
    int sample_rate;
    double max_mel_db;          // Rolling max mel energy for clamping

    float* overlap_buffer;      // To store the end of the last audio push for seamless FFT
    int overlap_len;
    float* combined_scratch;    // Reuse to avoid realloc
    int combined_scratch_cap;
    double* fft_real;           // Reuse to avoid realloc
    double* fft_imag;

    long long total_frames_pushed; // To track global frame index alignment
    long long total_samples_received;
    double tolerance;
    int num_bands;
} TransientAnalyzer;

TransientAnalyzer* analyzer_create(double max_peak_value, SharedTransientBuffer* shared_buffer, void* lock_obj, ct_lock_func lock_func, ct_lock_func unlock_func, int window_ms, int num_bands);
void analyzer_destroy(TransientAnalyzer* self);
void analyzer_clear(TransientAnalyzer* self);
void analyzer_set_sample_rate(TransientAnalyzer* self, int sr);
double analyzer_get_max_peak(TransientAnalyzer* self);

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int global_p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          double detected_peak_val,
                          double thresh_val,
                          double left_min,
                          double right_min,
                          double prominence,
                          PeakResult* result_out);

bool analyzer_cleanup_snapshots(TransientAnalyzer* self, int frame);
void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out);
double* analyzer_get_buffer(TransientAnalyzer* self);

int analyzer_analyze_chunk(TransientAnalyzer* self,
                           const float* y,
                           int len,
                           int sr,
                           int buffer_start_frame,
                           int active_start_frame,
                           ChunkAnalysisResult* result_out);

void analyzer_push_audio(TransientAnalyzer* self, const float* y, int len, int sr);

// Full analysis structures
typedef struct {
    float* envelope;
    float* rolling_dynamic_smoothing;
    float* rolling_prominence;
    float* rolling_prominence_avg;
    float* rolling_prominence_half_max;
    float* rolling_smoothing_avg;
    float* rolling_flux_avg;
    float* rolling_threshold;
    float* rolling_lookback;
    float* rolling_avg_delta;
    float* rolling_total_delta;
    int* rolling_p_count;
    PeakResult* peaks;
    int num_peaks;
} BandAnalysis;

typedef struct {
    float* times;
    int num_frames;
    float max_peak_value;
    int num_bands;
    BandAnalysis bands[MAX_BANDS];

    // Batch analysis metrics history
    double* ratings;
    double* std_devs;
    double* means;
    double* contrasts;
    double* stability_scores;
    double* highest_peaks_ms;

    double min_score_seen;
    double max_score_seen;
    double* demarcation_lines;
    float* rolling_global_flux_avg;
    float* rolling_global_smoothing_avg;
    double tolerance;
} FullAnalysisResult;

int analyzer_batch_analyze(const float* y, int len, int sr, int window_ms, int num_bands, FullAnalysisResult* result_out);
void analyzer_free_analysis(FullAnalysisResult* result);

// OpenCL GPU Compute Shader Acceleration Interface
int gpu_stft_init(void);
void gpu_stft_cleanup(void);
int gpu_stft_process(const float* pcm, int num_samples, int sr, float* flux_out, int num_frames, int num_bands);

#endif
