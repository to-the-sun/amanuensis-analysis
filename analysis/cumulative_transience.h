#ifndef CUMULATIVE_TRANSIENCE_H
#define CUMULATIVE_TRANSIENCE_H

#include <stdint.h>
#include <stdbool.h>

#define BUFFER_LEN 5001
#define MAX_BANDS 4
#define MAX_QUALIFIERS 256
#define MAX_PEAK_HISTORY 8192
#define MAX_EVENTS 32768

typedef struct {
    double ms;
    double val;
} Qualifier;

typedef struct {
    int p_idx;
    int band_idx;
    double time;
    double peak_val;
    double total_score;
    int num_qualifiers;
    Qualifier qualifiers[MAX_QUALIFIERS];
    double snapshot[BUFFER_LEN];
} PeakResult;

typedef struct {
    double std_dev;
    double mean;
    double contrast;
    double peak_std;
    double rating;
    bool buffer_updated;
    double highest_peak_ms;
    bool highest_peak_valid;
    double rolling_score;
    double min_score_seen;
    double max_score_seen;
} AnalyzerMetrics;

typedef struct SnapshotEntry {
    int p_idx;
    double snapshot[BUFFER_LEN];
    struct SnapshotEntry* next;
} SnapshotEntry;

typedef struct {
    int frame;
    int type; // 0 for ADD, 1 for REMOVE
    double score;
} ScoreEvent;

typedef struct {
    double accumulated_buffer[BUFFER_LEN];
    double buffer_times[BUFFER_LEN];
    double max_peak;

    double min_score_seen;
    double max_score_seen;
    double total_score_sum;
    int score_count;

    // Rolling score
    ScoreEvent upcoming_events[MAX_EVENTS];
    int event_count;
    int event_read_ptr;

    double current_window_scores[MAX_EVENTS];
    int current_window_count;
    double last_score_avg;

    // Peak history
    double peak_history[MAX_PEAK_HISTORY];
    int peak_history_count;

    // Snapshots tracking (queue per band)
    SnapshotEntry* snapshot_heads[MAX_BANDS];
    SnapshotEntry* snapshot_tails[MAX_BANDS];
} TransientAnalyzer;

TransientAnalyzer* analyzer_create(double max_peak_value);
void analyzer_destroy(TransientAnalyzer* self);

int analyzer_process_peak(TransientAnalyzer* self,
                          int p_idx,
                          int band_idx,
                          double time,
                          const float* env_ptr,
                          int env_len,
                          const int* all_valid_peak_indices,
                          int all_valid_count,
                          PeakResult* result_out);

void analyzer_update_metrics(TransientAnalyzer* self, int frame, AnalyzerMetrics* metrics_out);
double* analyzer_get_buffer(TransientAnalyzer* self);

// Full analysis structures
typedef struct {
    float* envelope;
    float* rolling_threshold;
    int* peaks;
    int num_peaks;
} BandAnalysis;

typedef struct {
    float* times;
    int num_frames;
    float max_peak_value;
    BandAnalysis bands[MAX_BANDS];

    // Batch analysis metrics history
    double* rolling_scores;
    double* ratings;
    double* std_devs;
    double* contrasts;
    double* peak_stds;
} FullAnalysisResult;

int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out);
int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out);
void analyzer_free_analysis(FullAnalysisResult* result);
void analyzer_debug_mel_filters(int sr, int n_fft, int n_mels, double* filters_out);

#endif
