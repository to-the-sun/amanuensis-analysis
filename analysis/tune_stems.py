#!/usr/bin/env python3
"""
Tuning Stems Tool
Aligns and corrects pitch variations in song stems continuously over time relative to a '00' prepended base stem.
"""

import os
import sys
import argparse
import subprocess
import tempfile
import numpy as np
import librosa
import soundfile as sf
from scipy.ndimage import gaussian_filter1d

def parse_args():
    parser = argparse.ArgumentParser(
        description="Continuously tune song stems relative to a base stem (starting with '00')."
    )
    parser.add_argument(
        "stems",
        nargs="+",
        help="Path to two or more WAV stem files. One must start with '00' (the base stem)."
    )
    parser.add_argument(
        "--algo",
        type=str,
        choices=["yin", "pyin", "piptrack"],
        default="pyin",
        help="Pitch tracking algorithm to use (default: %(default)s)."
    )
    parser.add_argument(
        "--fmin",
        type=float,
        default=50.0,
        help="Minimum frequency to track in Hz (default: %(default)s)."
    )
    parser.add_argument(
        "--fmax",
        type=float,
        default=1000.0,
        help="Maximum frequency to track in Hz (default: %(default)s)."
    )
    parser.add_argument(
        "--hop-length",
        type=int,
        default=512,
        help="Hop length in samples for pitch analysis (default: %(default)s)."
    )
    parser.add_argument(
        "--strength",
        type=float,
        default=1.0,
        help="Pitch correction strength multiplier (default: %(default)s)."
    )
    parser.add_argument(
        "--max-correction",
        type=float,
        default=0.5,
        help="Maximum pitch correction in semitones (default: %(default)s)."
    )
    parser.add_argument(
        "--smoothing",
        type=float,
        default=0.15,
        help="Gaussian smoothing standard deviation in seconds for the correction curve (default: %(default)s)."
    )
    parser.add_argument(
        "--formant",
        action="store_true",
        help="Enable formant preservation in Rubber Band (highly recommended for vocals)."
    )
    parser.add_argument(
        "--engine",
        type=str,
        choices=["fine", "fast"],
        default="fine",
        help="Rubber Band engine: 'fine' (R3 engine, higher quality) or 'fast' (R2 engine) (default: %(default)s)."
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default=None,
        help="Directory to save tuned stems. If not specified, saves in the same directory as input files with '_tuned' suffix."
    )
    parser.add_argument(
        "--confidence-threshold",
        type=float,
        default=0.1,
        help="Minimum confidence threshold for pitch tracking (default: %(default)s)."
    )
    return parser.parse_args()

def identify_base_and_targets(stems):
    """Identifies the '00' base stem and the target stems."""
    base_stem = None
    target_stems = []

    for stem in stems:
        filename = os.path.basename(stem)
        if filename.startswith("00"):
            if base_stem is not None:
                print(f"Error: Multiple base stems starting with '00' found: '{base_stem}' and '{stem}'", file=sys.stderr)
                sys.exit(1)
            base_stem = stem
        else:
            target_stems.append(stem)

    if base_stem is None:
        print("Error: Could not find base stem starting with '00' among arguments.", file=sys.stderr)
        sys.exit(1)

    if not target_stems:
        print("Error: No target stems found to tune. Provide at least one target stem alongside the base '00' stem.", file=sys.stderr)
        sys.exit(1)

    return base_stem, target_stems

def track_pitch(audio_mono, sr, algo, fmin, fmax, hop_length):
    """Tracks pitch using YIN, pYIN, or PipTrack."""
    print(f"  Tracking pitch using {algo.upper()} (fmin={fmin}, fmax={fmax})...")
    if algo == "yin":
        f0 = librosa.yin(audio_mono, fmin=fmin, fmax=fmax, sr=sr, hop_length=hop_length)
        times = librosa.frames_to_time(range(len(f0)), sr=sr, hop_length=hop_length)
        confidence = np.ones_like(f0)
        invalid = np.isnan(f0) | (f0 < fmin) | (f0 > fmax)
        f0[invalid] = np.nan
        confidence[invalid] = 0.0
    elif algo == "pyin":
        f0, voiced_flag, voiced_probs = librosa.pyin(
            audio_mono, fmin=fmin, fmax=fmax, sr=sr, hop_length=hop_length, fill_na=np.nan
        )
        times = librosa.frames_to_time(range(len(f0)), sr=sr, hop_length=hop_length)
        confidence = np.nan_to_num(voiced_probs)
        f0[~voiced_flag] = np.nan
    elif algo == "piptrack":
        n_fft = 2048 if fmin >= 80 else 4096
        pitches, magnitudes = librosa.piptrack(
            y=audio_mono, sr=sr, hop_length=hop_length, fmin=fmin, fmax=fmax, n_fft=n_fft
        )
        n_frames = pitches.shape[1]
        f0 = np.zeros(n_frames, dtype=float)
        confidence = np.zeros(n_frames, dtype=float)

        for t in range(n_frames):
            mag_t = magnitudes[:, t]
            max_bin = np.argmax(mag_t)
            max_mag = mag_t[max_bin]

            if max_mag > 0:
                pitch_val = pitches[max_bin, t]
                if fmin <= pitch_val <= fmax:
                    f0[t] = pitch_val
                    confidence[t] = max_mag
                else:
                    f0[t] = np.nan
            else:
                f0[t] = np.nan

        f0[f0 == 0] = np.nan
        times = librosa.frames_to_time(range(n_frames), sr=sr, hop_length=hop_length)
        max_conf = np.max(confidence)
        if max_conf > 0:
            confidence = confidence / max_conf
    else:
        raise ValueError(f"Unknown pitch algorithm: {algo}")

    return times, f0, confidence

def get_continuous_midi(times, f0, confidence, grid_times, confidence_threshold=0.1):
    """Interpolates fractional MIDI values onto a regular grid times array, with voiced masking."""
    midi = np.full_like(f0, np.nan)
    valid = (~np.isnan(f0)) & (f0 > 0)
    if np.any(valid):
        midi[valid] = librosa.hz_to_midi(f0[valid])

    voiced_mask = valid & (confidence >= confidence_threshold)

    if not np.any(voiced_mask):
        return np.full_like(grid_times, np.nan), np.zeros_like(grid_times, dtype=bool)

    voiced_times = times[voiced_mask]
    voiced_midi = midi[voiced_mask]

    # Interpolate onto grid_times
    grid_midi = np.interp(grid_times, voiced_times, voiced_midi)

    # Voice proximity check: within 0.25 seconds of any voiced frame
    grid_voiced = np.zeros_like(grid_times, dtype=bool)
    for vt in voiced_times:
        grid_voiced |= (np.abs(grid_times - vt) <= 0.25)

    grid_midi[~grid_voiced] = np.nan
    return grid_midi, grid_voiced

def process_tuning(base_path, target_path, args):
    """Analyzes base and target, generates pitch map, and runs Rubber Band to tune target."""
    print(f"\nProcessing stem: '{os.path.basename(target_path)}'")

    # Load audio files
    y_base, sr_base = librosa.load(base_path, sr=None, mono=True)
    y_target, sr_target = librosa.load(target_path, sr=None, mono=True)

    # Check length
    duration_base = len(y_base) / sr_base
    duration_target = len(y_target) / sr_target
    print(f"  Base Duration: {duration_base:.2f}s (SR: {sr_base} Hz)")
    print(f"  Target Duration: {duration_target:.2f}s (SR: {sr_target} Hz)")

    if abs(duration_base - duration_target) > 0.5:
         print(f"  Warning: Durations differ by more than 0.5s ({duration_base:.2f}s vs {duration_target:.2f}s). Aligning offsets.")

    # Track pitch on base and target
    times_base, f0_base, conf_base = track_pitch(
        y_base, sr_base, args.algo, args.fmin, args.fmax, args.hop_length
    )
    times_target, f0_target, conf_target = track_pitch(
        y_target, sr_target, args.algo, args.fmin, args.fmax, args.hop_length
    )

    # Build common time grid
    max_duration = max(duration_base, duration_target)
    dt = args.hop_length / sr_base
    grid_times = np.arange(0, max_duration, dt)

    # Get continuous MIDI representations
    grid_midi_base, grid_voiced_base = get_continuous_midi(
        times_base, f0_base, conf_base, grid_times, args.confidence_threshold
    )
    grid_midi_target, grid_voiced_target = get_continuous_midi(
        times_target, f0_target, conf_target, grid_times, args.confidence_threshold
    )

    # Compute continuous pitch deviation where both are voiced
    both_voiced = grid_voiced_base & grid_voiced_target
    raw_corr = np.zeros_like(grid_times)

    if np.any(both_voiced):
        diff_midi = grid_midi_target[both_voiced] - grid_midi_base[both_voiced]
        # Calculate interval-relative deviation: diff_midi - round(diff_midi)
        # E.g. if target is 3.12 semitones above base, deviation is +0.12 (it's 12 cents sharp of a minor third)
        # To correct it, we shift the target by -0.12 semitones
        dev = diff_midi - np.round(diff_midi)

        # Clip deviation to maximum correction limit
        dev = np.clip(dev, -args.max_correction, args.max_correction)

        # Scale by strength
        corr_at_voiced = -dev * args.strength
        raw_corr[both_voiced] = corr_at_voiced

        # Interpolate correction over unvoiced/silent frames to prevent sudden jumps
        valid_idx = np.where(both_voiced)[0]
        all_idx = np.arange(len(grid_times))
        filled_corr = np.interp(all_idx, valid_idx, raw_corr[both_voiced])
    else:
        print("  Warning: No overlapping voiced frames detected. Applying zero pitch correction.")
        filled_corr = np.zeros_like(grid_times)

    # Smooth the correction curve using Gaussian filtering to prevent transient artifacts
    sigma_pixels = args.smoothing / dt
    smoothed_corr = gaussian_filter1d(filled_corr, sigma=sigma_pixels)

    # Print correction statistics
    if np.any(both_voiced):
        avg_abs_corr = np.mean(np.abs(smoothed_corr[both_voiced]))
        max_abs_corr = np.max(np.abs(smoothed_corr[both_voiced]))
        print(f"  Pitch Correction Stats (voiced regions):")
        print(f"    Average absolute correction: {avg_abs_corr * 100:.1f} cents")
        print(f"    Maximum absolute correction: {max_abs_corr * 100:.1f} cents")

    # Generate the Rubber Band pitch map file
    # Format: sample_frame pitch_offset_in_semitones
    temp_map = tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.txt')
    try:
        # Write first frame at sample 0
        temp_map.write(f"0 {smoothed_corr[0]:.6f}\n")

        num_samples_target = int(round(duration_target * sr_target))
        for idx, (t, corr) in enumerate(zip(grid_times, smoothed_corr)):
            sample_frame = int(round(t * sr_target))
            if 0 < sample_frame < num_samples_target:
                temp_map.write(f"{sample_frame} {corr:.6f}\n")

        # Write final sample frame
        temp_map.write(f"{num_samples_target} {smoothed_corr[-1]:.6f}\n")
        temp_map.close()

        # Build output filepath
        target_dir = args.output_dir or os.path.dirname(target_path)
        base_name, ext = os.path.splitext(os.path.basename(target_path))
        output_path = os.path.join(target_dir, f"{base_name}_tuned{ext}")
        os.makedirs(os.path.dirname(output_path), exist_ok=True)

        # Run rubberband
        print(f"  Running Rubber Band to pitch shift stem...")
        cmd = ["rubberband"]
        if args.engine == "fine":
            cmd.append("--fine")
        else:
            cmd.append("--fast")

        if args.formant:
            cmd.append("--formant")

        cmd.extend(["--pitchmap", temp_map.name, target_path, output_path])

        print(f"    Command: {' '.join(cmd)}")
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        if result.returncode != 0:
            print(f"  Error: Rubber Band failed with return code {result.returncode}", file=sys.stderr)
            print(f"  Stderr:\n{result.stderr}", file=sys.stderr)
            sys.exit(1)

        print(f"  Successfully tuned and saved to: '{output_path}'")
    finally:
        # Ensure temporary file is cleaned up
        if os.path.exists(temp_map.name):
            os.remove(temp_map.name)

import traceback

def main():
    args = parse_args()
    base_stem, target_stems = identify_base_and_targets(args.stems)

    print("=== Continuous Stem Tuning Pipeline ===")
    print(f"Base Stem (00 reference): '{base_stem}'")
    print(f"Stems to tune: {len(target_stems)} file(s)")

    for target in target_stems:
        process_tuning(base_stem, target, args)

    print("\nAll target stems successfully processed!")

if __name__ == "__main__":
    try:
        main()
        print("\nTuning complete.")
    except Exception as e:
        print("\n" + "="*60)
        print("CRITICAL ERROR DURING STEM TUNING")
        print("="*60)
        traceback.print_exc()
        print("="*60)
    finally:
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass
