import argparse
import librosa
import numpy as np
import os
import time
import sys
import cumulative_transience

# Try to import sounddevice for real-time playback
try:
    import sounddevice as sd
    SOUNDDEVICE_AVAILABLE = True
except ImportError:
    SOUNDDEVICE_AVAILABLE = False
except OSError:
    # This can happen if PortAudio is not found
    SOUNDDEVICE_AVAILABLE = False

def play_and_analyze(file_path, mock=False):
    print(f"\n--- Playing and Analyzing: {os.path.basename(file_path)} ---")

    # Load audio
    y, sr = librosa.load(file_path, sr=None, mono=True)
    duration = librosa.get_duration(y=y, sr=sr)

    # Pre-analyze (as analyze_files.py does before rendering)
    print("Pre-calculating transient envelopes...")
    data = cumulative_transience.analyze_audio(y, sr)

    times = data['times']
    onset_envs = data['onset_envs']
    peak_indices_list = data['peaks_list']
    max_peak = data['max_peak_value']
    all_valid_peak_indices = set().union(*peak_indices_list)

    # Initialize Real-time Analyzer
    analyzer = cumulative_transience.TransientAnalyzer(max_peak_value=max_peak)

    # Start playback if not mocking
    if not mock:
        if SOUNDDEVICE_AVAILABLE:
            print("Starting audible playback...")
            sd.play(y, sr)
        else:
            print("Warning: sounddevice or PortAudio not found. Falling back to mock mode (no audio).")
            mock = True
    else:
        print("Mock mode enabled: Simulating real-time playback.")

    start_time = time.time()
    last_printed_frame = -1

    print(f"{'Time':>8} | {'Score':>8} | {'Rating':>8} | {'Std Dev':>8} | {'Contrast':>8} | {'Peak Std':>8}")
    print("-" * 65)

    try:
        while True:
            elapsed = time.time() - start_time
            if elapsed >= duration:
                break

            # Current frame index (1ms resolution)
            current_frame = int(elapsed * 1000)

            if current_frame > last_printed_frame:
                # Process peaks for the current frame
                # Note: process_new_peaks expects a frame index
                new_peak_data = analyzer.process_new_peaks(current_frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)

                # Update metrics
                metrics = analyzer.update_metrics(current_frame)

                # Print results if a new peak was found or at regular intervals
                # To avoid flooding, we can print every 100ms or when a peak happens
                if new_peak_data or current_frame % 500 == 0:
                    time_str = f"{int(elapsed // 60)}:{elapsed % 60:05.2f}"
                    score_str = f"{metrics['rolling_score']:+8.2f}"
                    rating_str = f"{metrics['rating']:8.2f}"
                    std_str = f"{metrics['std_dev']:8.3f}"
                    contrast_str = f"{metrics['contrast']:8.3f}"
                    pstd_str = f"{metrics['peak_std']:8.3f}"

                    output = f"{time_str:>8} | {score_str:>8} | {rating_str:>8} | {std_str:>8} | {contrast_str:>8} | {pstd_str:>8}"

                    if new_peak_data:
                        # Highlight peak events
                        for p in new_peak_data:
                            band_names = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']
                            print(f"{output}  <-- PEAK [{band_names[p['band_idx']]}] Score: {p['total_score']:+.2f}")
                    else:
                        print(output)

                last_printed_frame = current_frame

            # Sleep a bit to avoid maxing CPU
            time.sleep(0.001)

    except KeyboardInterrupt:
        if not mock and SOUNDDEVICE_AVAILABLE:
            sd.stop()
        print("\nPlayback interrupted by user.")
        return

    if not mock and SOUNDDEVICE_AVAILABLE:
        sd.wait()

    print("-" * 65)
    print("Analysis Complete.")

def main():
    parser = argparse.ArgumentParser(description="Real-time transient analysis and audible playback.")
    parser.add_argument("files", nargs="*", help="Optional list of audio files to process.")
    parser.add_argument("--mock", action="store_true", help="Simulate real-time playback without audio output.")
    args = parser.parse_args()

    audio_files = []
    if args.files:
        audio_files = args.files
    else:
        extensions = ('.wav', '.mp3', '.m4a', '.flac', '.ogg', '.aiff')
        # Look in current directory or analysis directory
        search_dirs = [os.getcwd(), os.path.dirname(os.path.abspath(__file__))]
        for d in search_dirs:
            audio_files.extend([os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(extensions)])

        # Remove duplicates and sort
        audio_files = list(set(audio_files))
        audio_files.sort()

    if not audio_files:
        print("No audio files found to process.")
        return

    for f in audio_files:
        if not os.path.exists(f):
            print(f"File not found: {f}")
            continue
        play_and_analyze(f, mock=args.mock)

if __name__ == "__main__":
    main()
