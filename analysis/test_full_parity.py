import numpy as np
import sys
import os
import importlib.util
import subprocess

def load_legacy_ref():
    legacy_path = os.path.join(os.path.dirname(__file__), "legacy", "cumulative_transience.py")
    spec = importlib.util.spec_from_file_location("cumulative_transience_legacy", legacy_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

cumulative_transience_py = load_legacy_ref()

def ensure_extension_built():
    """Checks if the extension is built and builds it if necessary."""
    current_dir = os.path.dirname(os.path.abspath(__file__))
    ext_file = None
    for f in os.listdir(current_dir):
        if f.startswith("cumulative_transience.") and (f.endswith(".so") or f.endswith(".pyd")):
            ext_file = os.path.join(current_dir, f)
            break

    source_pyx = os.path.join(current_dir, "ct_extension.pyx")
    source_c = os.path.join(current_dir, "cumulative_transience.c")

    needs_build = False
    if ext_file is None:
        needs_build = True
    else:
        ext_mtime = os.path.getmtime(ext_file)
        if os.path.exists(source_pyx) and os.path.getmtime(source_pyx) > ext_mtime:
            needs_build = True
        elif os.path.exists(source_c) and os.path.getmtime(source_c) > ext_mtime:
            needs_build = True

    if needs_build:
        print("Notice: Extension module is missing or outdated. Attempting to build...")
        old_cwd = os.getcwd()
        os.chdir(current_dir)
        try:
            python_cmd = "python" if os.name == "nt" else "python3"
            subprocess.run([python_cmd, "setup.py", "build_ext", "--inplace"], check=True)
            print("Extension module built successfully.")
        except Exception as e:
            print(f"Warning: Failed to build extension module: {e}")
        finally:
            os.chdir(old_cwd)

ensure_extension_built()
import cumulative_transience as cumulative_transience_c
import time
import librosa
import os
import subprocess
import sys

def ensure_extension_built():
    """Checks if the extension is built and builds it if necessary."""
    current_dir = os.path.dirname(os.path.abspath(__file__))
    ext_file = None
    for f in os.listdir(current_dir):
        if f.startswith("cumulative_transience.") and (f.endswith(".so") or f.endswith(".pyd")):
            ext_file = os.path.join(current_dir, f)
            break

    source_pyx = os.path.join(current_dir, "ct_extension.pyx")
    source_c = os.path.join(current_dir, "cumulative_transience.c")

    needs_build = False
    if ext_file is None:
        needs_build = True
    else:
        ext_mtime = os.path.getmtime(ext_file)
        if os.path.exists(source_pyx) and os.path.getmtime(source_pyx) > ext_mtime:
            needs_build = True
        elif os.path.exists(source_c) and os.path.getmtime(source_c) > ext_mtime:
            needs_build = True

    if needs_build:
        print("Notice: Extension module is missing or outdated. Attempting to build...")
        old_cwd = os.getcwd()
        os.chdir(current_dir)
        try:
            python_cmd = "python" if os.name == "nt" else "python3"
            subprocess.run([python_cmd, "setup.py", "build_ext", "--inplace"], check=True)
            print("Extension module built successfully.")
        except Exception as e:
            print(f"Warning: Failed to build extension module: {e}")
        finally:
            os.chdir(old_cwd)

def test_full_analysis():
    print("Testing full analysis parity (Python/librosa vs C extension)...")

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
    test_full_analysis()
