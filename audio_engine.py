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

def analyze_audio(audio, sr):
    """Perform comprehensive analysis on the generated audio."""
    print("\n--- Audio Analysis (Objective Judgments) ---")

    # Ensure audio is mono for analysis
    if len(audio.shape) > 1:
        mono_audio = np.mean(audio, axis=0)
    else:
        mono_audio = audio

    results = {}

    # 1. Amplitude Dynamics
    rms = librosa.feature.rms(y=mono_audio)
    avg_rms = np.mean(rms).item()
    peak_rms = np.max(rms).item()
    print(f"Average RMS Energy: {avg_rms:.4f}")
    print(f"Peak RMS Energy: {peak_rms:.4f}")
    results['average_rms'] = avg_rms
    results['peak_rms'] = peak_rms

    # 2. Spectral Shape
    centroid = librosa.feature.spectral_centroid(y=mono_audio, sr=sr)
    bandwidth = librosa.feature.spectral_bandwidth(y=mono_audio, sr=sr)
    flatness = librosa.feature.spectral_flatness(y=mono_audio)

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
    zcr = librosa.feature.zero_crossing_rate(y=mono_audio)
    avg_zcr = np.mean(zcr).item()
    print(f"Average Zero-Crossing Rate: {avg_zcr:.4f}")
    results['average_zero_crossing_rate'] = avg_zcr

    # 4. Rhythmic Analysis
    onset_env = librosa.onset.onset_strength(y=mono_audio, sr=sr)
    if np.max(onset_env) > 0:
        tempo, _ = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
        # librosa 0.11+ returns a float or array
        if isinstance(tempo, (np.ndarray, list)):
            tempo_val = tempo[0]
        else:
            tempo_val = tempo
        tempo_val = float(tempo_val)
        print(f"Estimated Tempo: {tempo_val:.2f} BPM")
        results['estimated_tempo'] = tempo_val

    # 5. MFCCs (Timbral descriptor)
    mfccs = librosa.feature.mfcc(y=mono_audio, sr=sr, n_mfcc=5)
    mfcc_means = np.mean(mfccs, axis=1).tolist()
    print("MFCC Means (First 5):", mfcc_means)
    results['mfcc_means'] = mfcc_means

    return results

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

    # Define a simple MIDI sequence: a C major arpeggio
    # mido.Message uses absolute 'time' here for our engine
    midi_messages = [
        Message("note_on", note=60, velocity=100, time=0.0),
        Message("note_off", note=60, velocity=0,   time=0.5),
        Message("note_on", note=64, velocity=100, time=0.5),
        Message("note_off", note=64, velocity=0,   time=1.0),
        Message("note_on", note=67, velocity=100, time=1.0),
        Message("note_off", note=67, velocity=0,   time=1.5),
        Message("note_on", note=72, velocity=120, time=1.5),
        Message("note_off", note=72, velocity=0,   time=3.0),
    ]

    print(f"Rendering {args.duration} seconds of audio using sound_design sandbox...")
    try:
        audio = sound_design.render_midi(
            midi_messages=midi_messages,
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
    
    # Save results to JSON
    json_path = os.path.join(new_subfolder, "analysis.json")
    with open(json_path, 'w') as f:
        json.dump(results, f, indent=4)
    print(f"Analysis saved to {json_path}")

if __name__ == "__main__":
    main()
