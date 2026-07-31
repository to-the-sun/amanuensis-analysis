#!/usr/bin/env python3
"""
Pitch Tracker and Regression Analysis Tool
Analyzes polyphonic WAV files (e.g., guitar) for continuous dominant pitch.
"""

import os
import sys
import argparse

def parse_args():
    parser = argparse.ArgumentParser(
        description="Analyze continuous dominant pitch and tuning regression in polyphonic WAV files."
    )
    parser.add_argument(
        "audio_path",
        type=str,
        help="Path to the polyphonic WAV file to analyze."
    )
    parser.add_argument(
        "--algo",
        type=str,
        choices=["yin", "pyin", "piptrack"],
        default="yin",
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
        "--hop_length",
        type=int,
        default=512,
        help="Hop length in samples (default: %(default)s)."
    )
    parser.add_argument(
        "--output-img",
        type=str,
        default=None,
        help="Path to save the output visualization image. Defaults to [audio_name]_pitch.png."
    )
    parser.add_argument(
        "--output-csv",
        type=str,
        default=None,
        help="Path to save the output data CSV. Defaults to [audio_name]_pitch.csv."
    )
    parser.add_argument(
        "--no-plot",
        action="store_true",
        help="Disable generating and saving the visualization image."
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Display the interactive Matplotlib plot window."
    )
    return parser.parse_args()

def load_audio(audio_path):
    """Loads the audio file using librosa, converting to mono if polyphonic/stereo."""
    if not os.path.exists(audio_path):
        print(f"Error: Audio file not found at '{audio_path}'", file=sys.stderr)
        sys.exit(1)

    print(f"Loading audio: {audio_path}")
    try:
        import librosa
        # Load audio with original sample rate to preserve high-res components if possible,
        # or fall back to default librosa sampling (22050 Hz).
        y, sr = librosa.load(audio_path, sr=None, mono=True)
        print(f"Successfully loaded audio. Duration: {len(y)/sr:.2f} seconds (sr={sr} Hz, mono=True)")
        return y, sr
    except Exception as e:
        print(f"Error loading audio file: {e}", file=sys.stderr)
        sys.exit(1)

import numpy as np

def track_pitch_yin(y, sr, fmin, fmax, hop_length):
    """Tracks pitch using YIN algorithm."""
    import librosa
    print("Tracking pitch using YIN...")
    f0 = librosa.yin(y, fmin=fmin, fmax=fmax, sr=sr, hop_length=hop_length)
    frames = range(len(f0))
    times = librosa.frames_to_time(frames, sr=sr, hop_length=hop_length)

    # YIN doesn't return confidence directly, we will initialize with ones
    confidence = np.ones_like(f0)

    # Mask out values that are outside boundaries or NaN
    invalid_mask = np.isnan(f0) | (f0 < fmin) | (f0 > fmax)
    f0 = f0.astype(float)
    f0[invalid_mask] = np.nan
    confidence[invalid_mask] = 0.0
    return times, f0, confidence

def track_pitch_pyin(y, sr, fmin, fmax, hop_length):
    """Tracks pitch using Probabilistic YIN (pYIN) algorithm."""
    import librosa
    print("Tracking pitch using pYIN...")
    f0, voiced_flag, voiced_probs = librosa.pyin(
        y, fmin=fmin, fmax=fmax, sr=sr, hop_length=hop_length, fill_value=np.nan
    )
    frames = range(len(f0))
    times = librosa.frames_to_time(frames, sr=sr, hop_length=hop_length)

    # Clean up voiced probs as confidence
    confidence = np.nan_to_num(voiced_probs)
    # If the voiced flag is False, set f0 to NaN
    f0[~voiced_flag] = np.nan
    return times, f0, confidence

def track_pitch_piptrack(y, sr, fmin, fmax, hop_length):
    """Tracks pitch using librosa.piptrack (spectral peak tracking)."""
    import librosa
    print("Tracking pitch using PipTrack (dominant spectral peak tracker)...")

    # We can use a standard n_fft that is appropriate for the fmin.
    # To resolve lower frequencies well, we need a larger n_fft.
    n_fft = 2048 if fmin >= 80 else 4096

    pitches, magnitudes = librosa.piptrack(
        y=y, sr=sr, hop_length=hop_length, fmin=fmin, fmax=fmax, n_fft=n_fft
    )

    # pitches shape: (n_bins, n_frames)
    # magnitudes shape: (n_bins, n_frames)
    n_frames = pitches.shape[1]
    f0 = np.zeros(n_frames, dtype=float)
    confidence = np.zeros(n_frames, dtype=float)

    for t in range(n_frames):
        # find the bin with the maximum magnitude at frame t
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
                confidence[t] = max_mag
        else:
            f0[t] = np.nan
            confidence[t] = 0.0

    # Convert zero elements to NaN
    f0[f0 == 0] = np.nan

    times = librosa.frames_to_time(range(n_frames), sr=sr, hop_length=hop_length)

    # Normalize confidence (magnitudes) to 0-1 range for better plotting/visualization
    max_conf = np.max(confidence)
    if max_conf > 0:
        confidence = confidence / max_conf

    return times, f0, confidence

def hz_to_midi_safe(f0):
    """Converts frequency array (Hz) to fractional MIDI values safely."""
    import librosa
    midi_vals = np.full_like(f0, np.nan, dtype=float)
    valid_mask = (~np.isnan(f0)) & (f0 > 0)
    if np.any(valid_mask):
        midi_vals[valid_mask] = librosa.hz_to_midi(f0[valid_mask])
    return midi_vals

def midi_to_note_and_cents(midi_val):
    """Converts a fractional MIDI value to a note name and cent deviation."""
    if np.isnan(midi_val):
        return "", ""

    notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
    closest_midi = int(round(midi_val))
    cents = int(round((midi_val - closest_midi) * 100))

    octave = (closest_midi // 12) - 1
    note_name = f"{notes[closest_midi % 12]}{octave}"
    deviation_str = f"{cents:+=d}c" if cents != 0 else "0c"
    return note_name, deviation_str

def generate_visualization(times, f0, confidence, midi_vals, y, sr, args, output_img):
    """Generates a beautiful 2-panel pitch tracking visualization."""
    import matplotlib
    if not args.interactive:
        matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import librosa.display

    print("Generating visualization...")

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 10), sharex=True)

    # --- Panel 1: Spectrogram and Hz Overlay ---
    # Compute spectrogram
    stft_matrix = librosa.stft(y, hop_length=args.hop_length)
    stft_db = librosa.amplitude_to_db(np.abs(stft_matrix), ref=np.max)

    # Show spectrogram as background
    img1 = librosa.display.specshow(
        stft_db, sr=sr, hop_length=args.hop_length,
        x_axis='time', y_axis='log', ax=ax1, cmap='inferno'
    )

    # Restrict y-axis limits to tracked range with some headroom
    ax1.set_ylim(args.fmin * 0.8, min(args.fmax * 1.2, sr / 2))

    # Overlay tracked pitch
    # Filter out nan values for plotting continuous lines cleanly
    valid_idx = ~np.isnan(f0)
    if np.any(valid_idx):
        ax1.plot(
            times[valid_idx], f0[valid_idx],
            color='cyan', linewidth=2.5, alpha=0.9,
            label=f'Tracked Pitch ({args.algo.upper()})'
        )
        # Highlight high-confidence points
        sc = ax1.scatter(
            times[valid_idx], f0[valid_idx],
            c=confidence[valid_idx], cmap='cool',
            s=12, zorder=3, alpha=0.7
        )

    ax1.set_title(f"Polyphonic Spectrogram & Tracked Pitch (Algorithm: {args.algo.upper()})", fontsize=12, fontweight='bold')
    ax1.set_ylabel("Frequency (Hz - Log Scale)", fontsize=10)
    ax1.legend(loc='upper right')

    # --- Panel 2: Linear MIDI Pitch & Tuning Regression ---
    if np.any(valid_idx):
        valid_midi = midi_vals[valid_idx]
        valid_times = times[valid_idx]
        valid_conf = confidence[valid_idx]

        # Plot continuous thin line
        ax2.plot(valid_times, valid_midi, color='#888888', linestyle='-', alpha=0.5, linewidth=1)

        # Scatter plot colored by confidence
        sc2 = ax2.scatter(
            valid_times, valid_midi,
            c=valid_conf, cmap='viridis',
            s=15, alpha=0.8, edgecolors='none', label='Detected Pitch'
        )
        fig.colorbar(sc2, ax=ax2, label="Confidence / Magnitude")

        # Add horizontal reference lines for semitones in range
        min_m = int(np.floor(np.min(valid_midi)))
        max_m = int(np.ceil(np.max(valid_midi)))

        # Determine step size for note labels based on pitch range
        midi_range = max_m - min_m
        if midi_range <= 24:
            # Show every semitone
            semitones_to_show = range(min_m, max_m + 1)
        elif midi_range <= 48:
            # Show natural semitones
            semitones_to_show = [m for m in range(min_m, max_m + 1) if (m % 12) in [0, 2, 4, 5, 7, 9, 11]]
        else:
            # Show only octaves (C notes)
            semitones_to_show = [m for m in range(min_m, max_m + 1) if (m % 12) == 0]

        # Draw the note horizontal lines and label them
        notes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
        tick_locs = []
        tick_labels = []
        for m in semitones_to_show:
            freq = librosa.midi_to_hz(m)
            ax2.axhline(y=m, color='gray', linestyle=':', alpha=0.4, linewidth=0.8)
            note_name = f"{notes[m % 12]}{(m // 12) - 1}"
            tick_locs.append(m)
            tick_labels.append(f"{note_name} ({m})")

        ax2.set_yticks(tick_locs)
        ax2.set_yticklabels(tick_labels, fontsize=8)

        # Set y-limits with some padding
        ax2.set_ylim(min_m - 1, max_m + 1)
    else:
        ax2.text(0.5, 0.5, "No pitch detected", ha='center', va='center', transform=ax2.transAxes)

    ax2.set_title("Tuning Deviation & Linear Pitch Regression (MIDI scale)", fontsize=12, fontweight='bold')
    ax2.set_ylabel("MIDI Pitch (Equal Temperament Ref)", fontsize=10)
    ax2.set_xlabel("Time (seconds)", fontsize=10)
    ax2.grid(True, axis='x', linestyle='--', alpha=0.5)

    plt.tight_layout()

    # Save image
    plt.savefig(output_img, dpi=300, bbox_inches='tight')
    print(f"Saved visualization to: {output_img}")

    if args.interactive:
        plt.show()
    plt.close()

def export_to_csv(times, f0, midi_vals, confidence, output_csv):
    """Saves the tracked pitch and tuning data to a CSV file."""
    import csv
    print(f"Exporting pitch data to CSV: {output_csv}")
    try:
        with open(output_csv, mode='w', newline='', encoding='utf-8') as f:
            writer = csv.writer(f)
            writer.writerow([
                "time_s",
                "frequency_hz",
                "midi_value",
                "closest_note",
                "tuning_deviation_cents",
                "confidence"
            ])

            for t, f_val, m_val, conf in zip(times, f0, midi_vals, confidence):
                if np.isnan(f_val):
                    writer.writerow([
                        f"{t:.4f}",
                        "",
                        "",
                        "",
                        "",
                        f"{conf:.4f}"
                    ])
                else:
                    note_name, deviation_str = midi_to_note_and_cents(m_val)
                    writer.writerow([
                        f"{t:.4f}",
                        f"{f_val:.2f}",
                        f"{m_val:.2f}",
                        note_name,
                        deviation_str,
                        f"{conf:.4f}"
                    ])
        print(f"Successfully saved CSV data.")
    except Exception as e:
        print(f"Error saving CSV: {e}", file=sys.stderr)

def main():
    args = parse_args()
    y, sr = load_audio(args.audio_path)

    # Resolve default output paths if not provided
    base_name, _ = os.path.splitext(args.audio_path)
    output_img = args.output_img or f"{base_name}_pitch.png"
    output_csv = args.output_csv or f"{base_name}_pitch.csv"

    # Run the selected algorithm
    if args.algo == "yin":
        times, f0, confidence = track_pitch_yin(y, sr, args.fmin, args.fmax, args.hop_length)
    elif args.algo == "pyin":
        times, f0, confidence = track_pitch_pyin(y, sr, args.fmin, args.fmax, args.hop_length)
    elif args.algo == "piptrack":
        times, f0, confidence = track_pitch_piptrack(y, sr, args.fmin, args.fmax, args.hop_length)
    else:
        print(f"Error: Unknown algorithm '{args.algo}'", file=sys.stderr)
        sys.exit(1)

    midi_vals = hz_to_midi_safe(f0)

    print(f"Analyzed {len(times)} frames.")
    valid_pitches = np.count_nonzero(~np.isnan(f0))
    print(f"Detected valid pitches in {valid_pitches} / {len(times)} frames.")

    # Save CSV data first
    export_to_csv(times, f0, midi_vals, confidence, output_csv)

    # Generate visualization
    if not args.no_plot:
        generate_visualization(times, f0, confidence, midi_vals, y, sr, args, output_img)

if __name__ == "__main__":
    main()
