import numpy as np
import cumulative_transience
import cumulative_transience_c
import time
import librosa

def test_full_analysis():
    print("Testing full analysis parity (Python/librosa vs C/Internal FFT)...")

    # Generate some random data since file load is unreliable in this env
    sr = 44100
    y = np.random.rand(sr * 5).astype(np.float32)

    print(f"Analyzing {len(y)/sr:.2f}s of audio...")

    start = time.time()
    py_res = cumulative_transience.analyze_audio(y, sr)
    py_time = time.time() - start
    print(f"Python analysis took: {py_time:.4f}s")

    start = time.time()
    c_res = cumulative_transience_c.analyze_audio(y, sr)
    c_time = time.time() - start
    print(f"C analysis took: {c_time:.4f}s")
    print(f"Speedup: {py_time / c_time:.2f}x")

    print("Comparing envelopes...")
    for i in range(4):
        diff = np.abs(py_res['onset_envs'][i] - c_res['onset_envs'][i])
        print(f"Band {i} envelope mean diff: {np.mean(diff):.6f}")
        corr = np.corrcoef(py_res['onset_envs'][i], c_res['onset_envs'][i])[0, 1]
        print(f"Band {i} correlation: {corr:.6f}")

if __name__ == "__main__":
    test_full_analysis()
