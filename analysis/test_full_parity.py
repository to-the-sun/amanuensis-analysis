import numpy as np
import cumulative_transience
import cumulative_transience_c
import time
import librosa
import os

def test_full_analysis():
    print("Testing full analysis parity (Python/librosa vs C/Internal FFT)...")

    # Load a fixed control file
    audio_file = "analysis/01 sustained bass [2025-12-29-22-19-46].wav"
    if not os.path.exists(audio_file):
        print(f"Error: {audio_file} not found. Using random data with seed.")
        np.random.seed(42)
        sr = 44100
        y = np.random.rand(sr * 5).astype(np.float32)
    else:
        print(f"Loading {audio_file}...")
        y, sr = librosa.load(audio_file, sr=None)
        # Use only first 10 seconds for speed
        y = y[:sr*10]

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
        py_env = py_res['onset_envs'][i]
        c_env = c_res['onset_envs'][i]

        # Trim to match lengths if they differ slightly due to framing
        min_len = min(len(py_env), len(c_env))
        py_env = py_env[:min_len]
        c_env = c_env[:min_len]

        diff = np.abs(py_env - c_env)
        print(f"Band {i} envelope mean diff: {np.mean(diff):.6f}")
        corr = np.corrcoef(py_env, c_env)[0, 1]
        print(f"Band {i} correlation: {corr:.6f}")

if __name__ == "__main__":
    test_full_analysis()
