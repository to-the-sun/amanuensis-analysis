import ctypes
import os
import numpy as np

# Define C structures to match cumulative_transience.h
BUFFER_LEN = 5001
MAX_QUALIFIERS = 256

class Qualifier(ctypes.Structure):
    _fields_ = [
        ("ms", ctypes.c_double),
        ("val", ctypes.c_double)
    ]

class PeakResult(ctypes.Structure):
    _fields_ = [
        ("p_idx", ctypes.c_int),
        ("band_idx", ctypes.c_int),
        ("time", ctypes.c_double),
        ("peak_val", ctypes.c_double),
        ("total_score", ctypes.c_double),
        ("num_qualifiers", ctypes.c_int),
        ("qualifiers", Qualifier * MAX_QUALIFIERS),
        ("snapshot", ctypes.c_double * BUFFER_LEN)
    ]

class AnalyzerMetrics(ctypes.Structure):
    _fields_ = [
        ("std_dev", ctypes.c_double),
        ("mean", ctypes.c_double),
        ("contrast", ctypes.c_double),
        ("peak_std", ctypes.c_double),
        ("rating", ctypes.c_double),
        ("buffer_updated", ctypes.c_bool),
        ("highest_peak_ms", ctypes.c_double),
        ("highest_peak_valid", ctypes.c_bool),
        ("rolling_score", ctypes.c_double),
        ("min_score_seen", ctypes.c_double),
        ("max_score_seen", ctypes.c_double)
    ]

class BandAnalysis(ctypes.Structure):
    _fields_ = [
        ("envelope", ctypes.POINTER(ctypes.c_float)),
        ("rolling_threshold", ctypes.POINTER(ctypes.c_float)),
        ("peaks", ctypes.POINTER(ctypes.c_int)),
        ("num_peaks", ctypes.c_int)
    ]

class FullAnalysisResult(ctypes.Structure):
    _fields_ = [
        ("times", ctypes.POINTER(ctypes.c_float)),
        ("num_frames", ctypes.c_int),
        ("max_peak_value", ctypes.c_float),
        ("bands", BandAnalysis * 4),
        ("rolling_scores", ctypes.POINTER(ctypes.c_double)),
        ("ratings", ctypes.POINTER(ctypes.c_double)),
        ("std_devs", ctypes.POINTER(ctypes.c_double)),
        ("contrasts", ctypes.POINTER(ctypes.c_double)),
        ("peak_stds", ctypes.POINTER(ctypes.c_double))
    ]

class TransientAnalyzer:
    def __init__(self, max_peak_value=1.0):
        # Load the shared library
        lib_name = "libtransience.so" if os.name != 'nt' else "libtransience.dll"
        lib_path = os.path.join(os.path.dirname(__file__), lib_name)

        if not os.path.exists(lib_path):
            print(f"Warning: {lib_name} not found in {os.path.dirname(__file__)}. Attempting to compile...")
            current_dir = os.getcwd()
            try:
                os.chdir(os.path.dirname(__file__))
                # Attempt to run make; this works on Linux/macOS and Windows if a build env is present
                ret = os.system("make all")
                if ret != 0 and os.name == 'nt':
                    print("Compilation failed. Ensure you have 'make' and 'gcc' installed (e.g., via MinGW or MSYS2).")
            except Exception as compile_err:
                print(f"Auto-compilation failed: {compile_err}")
            finally:
                os.chdir(current_dir)

        if not os.path.exists(lib_path):
            raise FileNotFoundError(f"Shared library {lib_name} could not be found or compiled at {lib_path}")

        self.lib = ctypes.CDLL(lib_path)

        # Configure function signatures
        self.lib.analyzer_create.argtypes = [ctypes.c_double]
        self.lib.analyzer_create.restype = ctypes.c_void_p

        self.lib.analyzer_destroy.argtypes = [ctypes.c_void_p]
        self.lib.analyzer_destroy.restype = None

        self.lib.analyzer_process_peak.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_double,
            ctypes.POINTER(ctypes.c_float), ctypes.c_int,
            ctypes.POINTER(ctypes.c_int), ctypes.c_int,
            ctypes.POINTER(PeakResult)
        ]
        self.lib.analyzer_process_peak.restype = ctypes.c_int

        self.lib.analyzer_update_metrics.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(AnalyzerMetrics)]
        self.lib.analyzer_update_metrics.restype = None

        self.lib.analyzer_get_buffer.argtypes = [ctypes.c_void_p]
        self.lib.analyzer_get_buffer.restype = ctypes.POINTER(ctypes.c_double)

        # Initialize the C analyzer
        self.obj = self.lib.analyzer_create(max_peak_value)

        self.accumulated_buffer = np.zeros(BUFFER_LEN, dtype=np.float64)
        self.buffer_times = np.linspace(-5000, 0, BUFFER_LEN)
        self.min_score_seen = 0.0
        self.max_score_seen = 0.0

    def __del__(self):
        if hasattr(self, 'obj') and self.obj:
            self.lib.analyzer_destroy(self.obj)

    def process_new_peaks(self, frame, peak_indices_list, onset_envs, all_valid_peak_indices, times):
        if not hasattr(self, 'processed_peaks'):
            self.processed_peaks = [set() for _ in range(4)]

        new_peaks_to_proc = []
        for band_idx in range(4):
            for p_idx in peak_indices_list[band_idx]:
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in self.processed_peaks[band_idx]:
                    new_peaks_to_proc.append((p_idx, band_idx))

        new_peaks_to_proc.sort()
        results = []

        all_valid_arr = (ctypes.c_int * len(all_valid_peak_indices))(*sorted(list(all_valid_peak_indices)))

        for p_idx, band_idx in new_peaks_to_proc:
            self.processed_peaks[band_idx].add(p_idx)
            env = np.ascontiguousarray(onset_envs[band_idx], dtype=np.float32)
            env_ptr = env.ctypes.data_as(ctypes.POINTER(ctypes.c_float))

            res = PeakResult()
            ret = self.lib.analyzer_process_peak(
                self.obj, p_idx, band_idx, times[p_idx], env_ptr, len(env),
                all_valid_arr, len(all_valid_arr), ctypes.byref(res)
            )

            if ret:
                peak_results = {
                    'p_idx': res.p_idx, 'band_idx': res.band_idx, 'time': res.time,
                    'peak_val': res.peak_val, 'total_score': res.total_score,
                    'qualifiers': [], 'snapshot': np.array(res.snapshot)
                }
                for i in range(res.num_qualifiers):
                    peak_results['qualifiers'].append({'ms': res.qualifiers[i].ms, 'val': res.qualifiers[i].val})
                results.append(peak_results)
                ctypes.memmove(self.accumulated_buffer.ctypes.data, self.lib.analyzer_get_buffer(self.obj), BUFFER_LEN * 8)

        return results

    def update_metrics(self, frame):
        m = AnalyzerMetrics()
        self.lib.analyzer_update_metrics(self.obj, frame, ctypes.byref(m))
        ctypes.memmove(self.accumulated_buffer.ctypes.data, self.lib.analyzer_get_buffer(self.obj), BUFFER_LEN * 8)
        self.min_score_seen = m.min_score_seen
        self.max_score_seen = m.max_score_seen

        return {
            'std_dev': m.std_dev, 'mean': m.mean, 'contrast': m.contrast, 'peak_std': m.peak_std,
            'rating': m.rating, 'buffer_updated': m.buffer_updated,
            'highest_peak_ms': m.highest_peak_ms if m.highest_peak_valid else None,
            'rolling_score': m.rolling_score, 'min_score_seen': m.min_score_seen, 'max_score_seen': m.max_score_seen
        }

def analyze_audio(y, sr):
    lib_name = "libtransience.so" if os.name != 'nt' else "libtransience.dll"
    lib_path = os.path.join(os.path.dirname(__file__), lib_name)

    if not os.path.exists(lib_path):
        # Trigger compilation via constructor logic if called standalone
        TransientAnalyzer()

    lib = ctypes.CDLL(lib_path)

    lib.analyzer_batch_analyze.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.c_int, ctypes.c_int, ctypes.POINTER(FullAnalysisResult)]
    lib.analyzer_batch_analyze.restype = ctypes.c_int
    lib.analyzer_free_analysis.argtypes = [ctypes.POINTER(FullAnalysisResult)]
    lib.analyzer_free_analysis.restype = None

    y_c = np.ascontiguousarray(y, dtype=np.float32)
    res = FullAnalysisResult()
    # Using batch_analyze to get both envelopes and metrics
    ret = lib.analyzer_batch_analyze(y_c.ctypes.data_as(ctypes.POINTER(ctypes.c_float)), len(y_c), sr, ctypes.byref(res))

    if not ret:
        return None

    num_frames = res.num_frames
    times = np.ctypeslib.as_array(res.times, shape=(num_frames,)).copy()

    onset_envs = []
    rolling_thresholds = []
    peaks_list = []

    for i in range(4):
        onset_envs.append(np.ctypeslib.as_array(res.bands[i].envelope, shape=(num_frames,)).copy())
        rolling_thresholds.append(np.ctypeslib.as_array(res.bands[i].rolling_threshold, shape=(num_frames,)).copy())
        peaks_list.append(np.ctypeslib.as_array(res.bands[i].peaks, shape=(res.bands[i].num_peaks,)).copy())

    rolling_scores = np.ctypeslib.as_array(res.rolling_scores, shape=(num_frames,)).copy()
    ratings = np.ctypeslib.as_array(res.ratings, shape=(num_frames,)).copy()

    max_peak_value = res.max_peak_value
    lib.analyzer_free_analysis(ctypes.byref(res))

    return {
        "times": times,
        "max_peak_value": float(max_peak_value),
        "onset_envs": onset_envs,
        "rolling_thresholds": rolling_thresholds,
        "peaks_list": peaks_list,
        "rolling_scores": rolling_scores,
        "ratings": ratings
    }
