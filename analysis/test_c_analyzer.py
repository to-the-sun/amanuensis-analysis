import numpy as np
import cumulative_transience
import cumulative_transience_c
import time

def test_comparison():
    print("Testing C implementation against Python implementation...")

    # Create some synthetic data
    sr = 44100
    duration = 10.0 # seconds
    times = np.linspace(0, duration, int(duration * 1000))

    # Synthetic onset envelopes (4 bands)
    onset_envs = [np.random.rand(len(times)).astype(np.float32) for _ in range(4)]

    # Synthetic peaks
    np.random.seed(42)
    peak_indices_list = [[] for _ in range(4)]
    all_valid_peak_indices = set()
    for i in range(4):
        peaks = np.sort(np.random.choice(len(times), 20, replace=False))
        peak_indices_list[i] = peaks
        for p in peaks:
            all_valid_peak_indices.add(p)

    max_peak_value = max([np.max(env) for env in onset_envs])

    py_analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak_value)
    c_analyzer = cumulative_transience_c.TransientAnalyzer(max_peak_value=max_peak_value)

    # Process frame by frame
    for frame in range(0, len(times), 100):
        py_results = py_analyzer.process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)
        c_results = c_analyzer.process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)

        for pr, cr in zip(py_results, c_results):
            if not np.allclose(pr['total_score'], cr['total_score'], atol=1e-5):
                print(f"total_score mismatch at {pr['p_idx']}: Py={pr['total_score']}, C={cr['total_score']}")

        py_metrics = py_analyzer.update_metrics(frame)
        c_metrics = c_analyzer.update_metrics(frame)

        for key in py_metrics:
            if isinstance(py_metrics[key], (int, float)):
                if not np.allclose(py_metrics[key], c_metrics[key], atol=1e-5):
                    print(f"Metric {key} mismatch at frame {frame}: Py={py_metrics[key]}, C={c_metrics[key]}")

    print("Comparison test finished.")

if __name__ == "__main__":
    test_comparison()
