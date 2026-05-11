import argparse
import os
import sys
import shutil
import json
import numpy as np
import soundfile as sf
import librosa
from mido import Message
import sound_design

# Standard MIDI sequence for analysis
DEFAULT_MIDI_SEQUENCE = [
    Message("note_on", note=60, velocity=100, time=0.0),
    Message("note_off", note=60, velocity=0,   time=0.5),
    Message("note_on", note=64, velocity=100, time=0.5),
    Message("note_off", note=64, velocity=0,   time=1.0),
    Message("note_on", note=67, velocity=100, time=1.0),
    Message("note_off", note=67, velocity=0,   time=1.5),
    Message("note_on", note=72, velocity=120, time=1.5),
    Message("note_off", note=72, velocity=0,   time=3.0),
]

def analyze_audio(audio, sr):
    """Perform comprehensive analysis on the generated audio, including temporal tracking."""
    print("\n--- Audio Analysis (Objective Judgments) ---")

    # Ensure audio is mono for analysis
    if len(audio.shape) > 1:
        mono_audio = np.mean(audio, axis=0)
    else:
        mono_audio = audio

    results = {}

    # Analysis parameters
    hop_length = int(sr * 0.050)  # 50ms hop size
    win_length = int(sr * 0.050)  # 50ms window size

    # 1. Amplitude Dynamics
    rms = librosa.feature.rms(y=mono_audio, frame_length=win_length, hop_length=hop_length)
    avg_rms = np.mean(rms).item()
    peak_rms = np.max(rms).item()
    print(f"Average RMS Energy: {avg_rms:.4f}")
    print(f"Peak RMS Energy: {peak_rms:.4f}")
    results['average_rms'] = avg_rms
    results['peak_rms'] = peak_rms

    # 2. Spectral Shape
    centroid = librosa.feature.spectral_centroid(y=mono_audio, sr=sr, hop_length=hop_length)
    bandwidth = librosa.feature.spectral_bandwidth(y=mono_audio, sr=sr, hop_length=hop_length)
    flatness = librosa.feature.spectral_flatness(y=mono_audio, hop_length=hop_length)

    avg_centroid = np.mean(centroid).item()
    avg_bandwidth = np.mean(bandwidth).item()
    avg_flatness = np.mean(flatness).item()

    print(f"Average Spectral Centroid: {avg_centroid:.2f} Hz")
    print(f"Average Spectral Bandwidth: {avg_bandwidth:.2f} Hz")
    print(f"Average Spectral Flatness: {avg_flatness:.4f} (0=tonal, 1=noisy)")

    results['average_spectral_centroid'] = avg_centroid
    results['average_spectral_bandwidth'] = avg_bandwidth
    results['average_spectral_flatness'] = avg_flatness

    # 3. Temporal Features
    zcr = librosa.feature.zero_crossing_rate(y=mono_audio, frame_length=win_length, hop_length=hop_length)
    avg_zcr = np.mean(zcr).item()
    print(f"Average Zero-Crossing Rate: {avg_zcr:.4f}")
    results['average_zero_crossing_rate'] = avg_zcr

    # 4. Rhythmic Analysis
    onset_env = librosa.onset.onset_strength(y=mono_audio, sr=sr, hop_length=hop_length)
    if np.max(onset_env) > 0:
        tempo, _ = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr, hop_length=hop_length)
        if isinstance(tempo, (np.ndarray, list)):
            tempo_val = tempo[0]
        else:
            tempo_val = tempo
        tempo_val = float(tempo_val)
        print(f"Estimated Tempo: {tempo_val:.2f} BPM")
        results['estimated_tempo'] = tempo_val

    # 5. MFCCs (Timbral descriptor)
    mfccs = librosa.feature.mfcc(y=mono_audio, sr=sr, n_mfcc=13, hop_length=hop_length)
    mfcc_means = np.mean(mfccs, axis=1).tolist()
    print("MFCC Means (First 5):", mfcc_means[:5])
    results['mfcc_means'] = mfcc_means

    # 6. Temporal Data
    times = librosa.times_like(rms, sr=sr, hop_length=hop_length).tolist()
    results['temporal_data'] = {
        'times': times,
        'rms': rms[0].tolist(),
        'spectral_centroid': centroid[0].tolist(),
        'spectral_bandwidth': bandwidth[0].tolist(),
        'spectral_flatness': flatness[0].tolist(),
        'zero_crossing_rate': zcr[0].tolist(),
        'mfccs': mfccs.tolist() # Full MFCCs over time
    }

    return results

def calculate_distance(current_results, other_results):
    """Calculate a simple distance metric between two sets of analysis results."""
    # Use normalized features for distance
    # For now, let's use Euclidean distance on mean MFCCs as a starting point
    # and maybe combine with other features.

    m1 = np.array(current_results['mfcc_means'])
    m2 = np.array(other_results['mfcc_means'])

    # Ensure they have the same length (might differ if n_mfcc was changed)
    min_len = min(len(m1), len(m2))
    dist = np.linalg.norm(m1[:min_len] - m2[:min_len])

    return float(dist)

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sounds_dir = os.path.join(script_dir, "sounds")
    os.makedirs(sounds_dir, exist_ok=True)

    # Use version identifier from sound_design for subfolder naming
    version_num = getattr(sound_design, "SOUND_DESIGN_VERSION", 1)
    new_subfolder = os.path.join(sounds_dir, str(version_num))
    os.makedirs(new_subfolder, exist_ok=True)

    parser = argparse.ArgumentParser(description="Sound Design Sandbox: Synthesize audio from MIDI and analyze.")
    parser.add_argument("--output", type=str, default="design_output.wav", help="Path to save the output WAV file.")
    parser.add_argument("--duration", type=float, default=5.0, help="Duration of the recording in seconds.")
    parser.add_argument("--sr", type=int, default=44100, help="Sample rate.")

    args = parser.parse_args()

    print(f"Rendering {args.duration} seconds of audio using sound_design sandbox...")
    try:
        audio = sound_design.render_midi(
            midi_messages=DEFAULT_MIDI_SEQUENCE,
            duration=args.duration,
            sample_rate=args.sr
        )
    except Exception as e:
        print(f"Error during synthesis: {e}")
        sys.exit(1)

    # Save to WAV file
    output_path = os.path.join(new_subfolder, args.output)
    sf.write(output_path, audio, args.sr)
    print(f"Audio saved to {output_path}")

    # Copy current sound_design.py
    shutil.copy2(os.path.join(script_dir, "sound_design.py"), new_subfolder)

    # Analyze
    results = analyze_audio(audio, args.sr)

    # Compare with existing sounds
    distances = {}
    for entry in os.listdir(sounds_dir):
        entry_path = os.path.join(sounds_dir, entry)
        if os.path.isdir(entry_path) and entry != str(version_num):
            other_json = os.path.join(entry_path, "analysis.json")
            if os.path.exists(other_json):
                try:
                    with open(other_json, 'r') as f:
                        other_results = json.load(f)
                    dist = calculate_distance(results, other_results)
                    distances[entry] = dist
                except Exception as e:
                    print(f"Could not compare with {entry}: {e}")

    if distances:
        results['distances'] = distances
        avg_dist = sum(distances.values()) / len(distances)
        print(f"Average distance from other sounds: {avg_dist:.4f}")
    
    # Save results to JSON
    json_path = os.path.join(new_subfolder, "analysis.json")
    with open(json_path, 'w') as f:
        json.dump(results, f, indent=4)
    print(f"Analysis saved to {json_path}")

if __name__ == "__main__":
    main()
