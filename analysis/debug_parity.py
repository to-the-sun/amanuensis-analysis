import numpy as np
import librosa
import ctypes
import os
import sys
import importlib.util

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

# Load modules
legacy = load_module('legacy', 'analysis/legacy/cumulative_transience.py')
current_c = load_module('current_c', 'analysis/cumulative_transience_c.py')

def compare_full_analysis(audio_file="analysis/01 sustained bass [2026-06-24 181817].wav"):
    print(f"Comparing Legacy vs C for: {audio_file}")
    y, sr = librosa.load(audio_file, sr=None)
    y = y[:sr*10] # 10 seconds for speed

    print("Running Legacy analysis...")
    res_leg = legacy.analyze_audio(y, sr)

    print("Running C analysis...")
    res_c = current_c.analyze_audio(y, sr)

    print("\n--- Onset Envelope Parity ---")
    for i in range(4):
        env_l = res_leg['onset_envs'][i]
        env_c = res_c['onset_envs'][i]
        min_len = min(len(env_l), len(env_c))
        env_l = env_l[:min_len]
        env_c = env_c[:min_len]

        corr = np.corrcoef(env_l, env_c)[0, 1]
        mean_diff = np.mean(np.abs(env_l - env_c))
        max_diff = np.max(np.abs(env_l - env_c))
        print(f"Band {i}: Correlation={corr:.10f}, Mean Diff={mean_diff:.10f}, Max Diff={max_diff:.10f}")

    print("\n--- Peak Detection Parity ---")
    for i in range(4):
        p_l = sorted(list(res_leg['peaks_list'][i]))
        p_c = sorted(list(res_c['peaks_list'][i]))

        print(f"Band {i}: Legacy={p_l[:5]}, C={p_c[:5]}")
        if len(p_l) == len(p_c):
            if len(p_l) > 0:
                diff = np.array(p_l) - np.array(p_c)
                max_p_diff = np.max(np.abs(diff))
                print(f"  Count Match ({len(p_l)}), Max Index Diff={max_p_diff}")
            else:
                print(f"Band {i}: Count Match (0)")
        else:
            print(f"Band {i}: Count Mismatch! Legacy={len(p_l)}, C={len(p_c)}")
            # Show first few to see shift
            print(f"  Legacy: {p_l[:5]}")
            print(f"  C:      {p_c[:5]}")

    print("\n--- Max Peak Value ---")
    print(f"Legacy: {res_leg['max_peak_value']:.10f}")
    print(f"C:      {res_c['max_peak_value']:.10f}")

if __name__ == "__main__":
    compare_full_analysis()
