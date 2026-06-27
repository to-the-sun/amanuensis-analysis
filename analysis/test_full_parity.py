import numpy as np
import sys
import os
import importlib.util
import subprocess
import time
import librosa
import ct_utils
import traceback

def load_legacy_ref():
    legacy_path = os.path.join(os.path.dirname(__file__), "legacy", "cumulative_transience.py")
    spec = importlib.util.spec_from_file_location("cumulative_transience_legacy", legacy_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# Ensure built before attempt import
ct_utils.ensure_extension_built()
import cumulative_transience as cumulative_transience_c

def test_full_analysis():
    print("Testing full analysis parity (Python/librosa vs C extension)...")

    cumulative_transience_py = load_legacy_ref()

    # Load a fixed control file
    audio_file = "analysis/01 sustained bass [2026-06-24 181817].wav"
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
    py_res = cumulative_transience_py.analyze_audio(y, sr)
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
    try:
        test_full_analysis()
    except Exception as e:
        traceback_str = "".join(traceback.format_exception(None, e, e.__traceback__))
        print(traceback_str)
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass
