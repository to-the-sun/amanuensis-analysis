import numpy as np
cimport numpy as cnp
from libc.string cimport memcpy

cnp.import_array()

cdef extern from "cumulative_transience.h":
    ctypedef struct Qualifier:
        double ms
        double val

    ctypedef struct PeakResult:
        int p_idx
        int band_idx
        double time
        double peak_val
        double total_score
        int num_qualifiers
        Qualifier qualifiers[256]
        double snapshot[5001]

    ctypedef struct AnalyzerMetrics:
        double std_dev
        double mean
        double contrast
        double peak_std
        double rating
        bint buffer_updated
        double highest_peak_ms
        bint highest_peak_valid
        double rolling_score
        double min_score_seen
        double max_score_seen

    ctypedef struct TransientAnalyzer_c "TransientAnalyzer":
        pass

    TransientAnalyzer_c* analyzer_create(double max_peak_value)
    void analyzer_destroy(TransientAnalyzer_c* self)
    int analyzer_process_peak(TransientAnalyzer_c* self,
                              int p_idx,
                              int band_idx,
                              double time,
                              const float* env_ptr,
                              int env_len,
                              const int* all_valid_peak_indices,
                              int all_valid_count,
                              PeakResult* result_out)
    void analyzer_update_metrics(TransientAnalyzer_c* self, int frame, AnalyzerMetrics* metrics_out)
    double* analyzer_get_buffer(TransientAnalyzer_c* self)

    ctypedef struct BandAnalysis:
        float* envelope
        float* rolling_threshold
        int* peaks
        int num_peaks

    ctypedef struct FullAnalysisResult:
        float* times
        int num_frames
        float max_peak_value
        BandAnalysis bands[4]
        double* rolling_scores
        double* ratings
        double* std_devs
        double* contrasts
        double* peak_stds

    int analyzer_analyze_audio(const float* y, int len, int sr, FullAnalysisResult* result_out)
    int analyzer_batch_analyze(const float* y, int len, int sr, FullAnalysisResult* result_out)
    void analyzer_free_analysis(FullAnalysisResult* result)

cdef class TransientAnalyzer:
    cdef TransientAnalyzer_c* _c_analyzer
    cdef public double min_score_seen
    cdef public double max_score_seen
    cdef object _processed_peaks

    def __cinit__(self, double max_peak_value=1.0):
        self._c_analyzer = analyzer_create(max_peak_value)
        if self._c_analyzer == NULL:
            raise MemoryError()
        self.min_score_seen = 0.0
        self.max_score_seen = 0.0
        self._processed_peaks = [set() for _ in range(4)]

    def __dealloc__(self):
        if self._c_analyzer != NULL:
            analyzer_destroy(self._c_analyzer)

    @property
    def accumulated_buffer(self):
        cdef double* buf_ptr = analyzer_get_buffer(self._c_analyzer)
        cdef cnp.ndarray[double, ndim=1] res = np.zeros(5001, dtype=np.float64)
        memcpy(res.data, buf_ptr, 5001 * sizeof(double))
        return res

    def process_new_peaks(self, int frame, list peak_indices_list, list onset_envs, object all_valid_peak_indices, object times):
        cdef list new_peaks_to_proc = []
        cdef int band_idx, p_idx
        for band_idx in range(4):
            for p_idx in peak_indices_list[band_idx]:
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in self._processed_peaks[band_idx]:
                    new_peaks_to_proc.append((p_idx, band_idx))

        new_peaks_to_proc.sort()

        cdef list results = []
        cdef list sorted_valid = sorted(list(all_valid_peak_indices))
        cdef int all_valid_count = len(sorted_valid)
        cdef cnp.ndarray[int, ndim=1] all_valid_arr = np.array(sorted_valid, dtype=np.int32)

        cdef PeakResult res
        cdef cnp.ndarray[float, ndim=1] env
        cdef int ret

        for p_idx, band_idx in new_peaks_to_proc:
            self._processed_peaks[band_idx].add(p_idx)
            env = np.ascontiguousarray(onset_envs[band_idx], dtype=np.float32)

            ret = analyzer_process_peak(
                self._c_analyzer, p_idx, band_idx, <double>times[p_idx],
                <float*>env.data, len(env),
                <int*>all_valid_arr.data, all_valid_count, &res
            )

            if ret:
                peak_res = {
                    'p_idx': res.p_idx,
                    'band_idx': res.band_idx,
                    'time': res.time,
                    'peak_val': res.peak_val,
                    'total_score': res.total_score,
                    'qualifiers': [],
                    'snapshot': np.zeros(5001, dtype=np.float64)
                }
                for i in range(res.num_qualifiers):
                    peak_res['qualifiers'].append({'ms': res.qualifiers[i].ms, 'val': res.qualifiers[i].val})

                memcpy(cnp.PyArray_DATA(peak_res['snapshot']), res.snapshot, 5001 * sizeof(double))

                results.append(peak_res)

        return results

    def update_metrics(self, int frame):
        cdef AnalyzerMetrics m
        analyzer_update_metrics(self._c_analyzer, frame, &m)
        self.min_score_seen = m.min_score_seen
        self.max_score_seen = m.max_score_seen

        return {
            'std_dev': m.std_dev,
            'mean': m.mean,
            'contrast': m.contrast,
            'peak_std': m.peak_std,
            'rating': m.rating,
            'buffer_updated': m.buffer_updated,
            'highest_peak_ms': m.highest_peak_ms if m.highest_peak_valid else None,
            'rolling_score': m.rolling_score,
            'min_score_seen': m.min_score_seen,
            'max_score_seen': m.max_score_seen
        }

def analyze_audio(cnp.ndarray[float, ndim=1] y, int sr):
    cdef FullAnalysisResult res
    cdef int ret = analyzer_batch_analyze(<float*>y.data, len(y), sr, &res)

    if not ret:
        return None

    cdef int num_frames = res.num_frames
    cdef cnp.ndarray[float, ndim=1] times = np.zeros(num_frames, dtype=np.float32)
    memcpy(times.data, res.times, num_frames * sizeof(float))

    cdef list onset_envs = []
    cdef list rolling_thresholds = []
    cdef list peaks_list = []

    cdef cnp.ndarray[float, ndim=1] env
    cdef cnp.ndarray[float, ndim=1] thresh
    cdef cnp.ndarray[int, ndim=1] peaks

    for i in range(4):
        env = np.zeros(num_frames, dtype=np.float32)
        memcpy(env.data, res.bands[i].envelope, num_frames * sizeof(float))
        onset_envs.append(env)

        thresh = np.zeros(num_frames, dtype=np.float32)
        memcpy(thresh.data, res.bands[i].rolling_threshold, num_frames * sizeof(float))
        rolling_thresholds.append(thresh)

        peaks = np.zeros(res.bands[i].num_peaks, dtype=np.int32)
        memcpy(peaks.data, res.bands[i].peaks, res.bands[i].num_peaks * sizeof(int))
        peaks_list.append(peaks)

    cdef cnp.ndarray[double, ndim=1] rolling_scores = np.zeros(num_frames, dtype=np.float64)
    memcpy(rolling_scores.data, res.rolling_scores, num_frames * sizeof(double))

    cdef cnp.ndarray[double, ndim=1] ratings = np.zeros(num_frames, dtype=np.float64)
    memcpy(ratings.data, res.ratings, num_frames * sizeof(double))

    cdef float max_peak_value = res.max_peak_value

    analyzer_free_analysis(&res)

    return {
        "times": times,
        "max_peak_value": float(max_peak_value),
        "onset_envs": onset_envs,
        "rolling_thresholds": rolling_thresholds,
        "peaks_list": peaks_list,
        "rolling_scores": rolling_scores,
        "ratings": ratings
    }
