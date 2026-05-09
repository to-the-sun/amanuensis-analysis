import argparse
import os
import sys
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

    # 1. Amplitude Dynamics
    rms = librosa.feature.rms(y=mono_audio)
    print(f"Average RMS Energy: {np.mean(rms):.4f}")
    print(f"Peak RMS Energy: {np.max(rms):.4f}")

    # 2. Spectral Shape
    centroid = librosa.feature.spectral_centroid(y=mono_audio, sr=sr)
    bandwidth = librosa.feature.spectral_bandwidth(y=mono_audio, sr=sr)
    flatness = librosa.feature.spectral_flatness(y=mono_audio)

    print(f"Average Spectral Centroid: {np.mean(centroid):.2f} Hz")
    print(f"Average Spectral Bandwidth: {np.mean(bandwidth):.2f} Hz")
    print(f"Average Spectral Flatness: {np.mean(flatness):.4f} (0=tonal, 1=noisy)")

    # 3. Temporal Features
    zcr = librosa.feature.zero_crossing_rate(y=mono_audio)
    print(f"Average Zero-Crossing Rate: {np.mean(zcr):.4f}")

    # 4. Rhythmic Analysis
    onset_env = librosa.onset.onset_strength(y=mono_audio, sr=sr)
    if np.max(onset_env) > 0:
        tempo, _ = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
        # librosa 0.11+ returns a float or array
        if isinstance(tempo, (np.ndarray, list)):
            tempo_val = tempo[0]
        else:
            tempo_val = tempo
        print(f"Estimated Tempo: {tempo_val:.2f} BPM")

    # 5. MFCCs (Timbral descriptor)
    mfccs = librosa.feature.mfcc(y=mono_audio, sr=sr, n_mfcc=5)
    print("MFCC Means (First 5):", np.mean(mfccs, axis=1))

def main():
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
    sf.write(args.output, audio, args.sr)
    print(f"Audio saved to {args.output}")

    # Analyze
    analyze_audio(audio, args.sr)

if __name__ == "__main__":
    main()
