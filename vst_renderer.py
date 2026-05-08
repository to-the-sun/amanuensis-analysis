import argparse
import os
import sys
import numpy as np
import soundfile as sf
import librosa
from mido import Message
from pedalboard import load_plugin

def analyze_audio(audio, sr):
    """Perform basic analysis on the generated audio."""
    print("\n--- Audio Analysis ---")

    # Ensure audio is mono for some analyses if it's stereo
    if len(audio.shape) > 1:
        mono_audio = np.mean(audio, axis=0)
    else:
        mono_audio = audio

    # RMS Energy
    rms = librosa.feature.rms(y=mono_audio)
    avg_rms = np.mean(rms)
    max_rms = np.max(rms)
    print(f"Average RMS Energy: {avg_rms:.4f}")
    print(f"Peak RMS Energy: {max_rms:.4f}")

    # Spectral Centroid
    centroid = librosa.feature.spectral_centroid(y=mono_audio, sr=sr)
    avg_centroid = np.mean(centroid)
    print(f"Average Spectral Centroid: {avg_centroid:.2f} Hz")

    # Spectral Bandwidth
    bandwidth = librosa.feature.spectral_bandwidth(y=mono_audio, sr=sr)
    avg_bandwidth = np.mean(bandwidth)
    print(f"Average Spectral Bandwidth: {avg_bandwidth:.2f} Hz")

    # Estimate Tempo (if applicable)
    onset_env = librosa.onset.onset_strength(y=mono_audio, sr=sr)
    tempo, _ = librosa.beat.beat_track(onset_envelope=onset_env, sr=sr)
    print(f"Estimated Tempo: {tempo} BPM")

def main():
    parser = argparse.ArgumentParser(description="Load a VST3 plugin, play notes, and analyze the output.")
    parser.add_argument("--plugin", type=str, required=True, help="Path to the VST3 plugin file.")
    parser.add_argument("--output", type=str, default="vst_output.wav", help="Path to save the output WAV file.")
    parser.add_argument("--duration", type=float, default=5.0, help="Duration of the recording in seconds.")
    parser.add_argument("--sr", type=int, default=44100, help="Sample rate.")

    args = parser.parse_args()

    if not os.path.exists(args.plugin):
        print(f"Error: Plugin not found at {args.plugin}")
        sys.exit(1)

    print(f"Loading plugin: {args.plugin}")
    try:
        plugin = load_plugin(args.plugin)
    except Exception as e:
        print(f"Failed to load plugin: {e}")
        sys.exit(1)

    if not plugin.is_instrument:
        print("Error: The provided plugin is not an instrument (it's likely an effect).")
        sys.exit(1)

    print(f"Plugin name: {plugin.name}")

    # Define a simple MIDI sequence: a C major arpeggio
    # mido.Message(type, note, velocity, time)
    # The 'time' attribute is relative for mido, but Pedalboard uses it as an absolute timestamp
    # if it's the only timing info provided.
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

    print(f"Rendering {args.duration} seconds of audio...")

    # Try 2 channels first, fall back to 1 if it fails
    try:
        audio = plugin.process(
            midi_messages=midi_messages,
            duration=args.duration,
            sample_rate=args.sr,
            num_channels=2
        )
    except Exception as e:
        if "does not support 2-channel output" in str(e):
            print("Plugin does not support 2 channels, falling back to mono...")
            audio = plugin.process(
                midi_messages=midi_messages,
                duration=args.duration,
                sample_rate=args.sr,
                num_channels=1
            )
        else:
            print(f"Error during processing: {e}")
            sys.exit(1)

    # Save to WAV file
    # Pedalboard returns audio in shape (channels, samples)
    # soundfile expects (samples, channels)
    audio_to_save = audio.T
    sf.write(args.output, audio_to_save, args.sr)
    print(f"Audio saved to {args.output}")

    # Analyze
    analyze_audio(audio, args.sr)

if __name__ == "__main__":
    main()
