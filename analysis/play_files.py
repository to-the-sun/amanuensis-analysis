import argparse
import librosa
import numpy as np
import os
import time
import sys
import subprocess
import traceback
import ct_utils

# Ensure built before attempt import
ct_utils.ensure_extension_built()
try:
    import cumulative_transience
except ImportError:
    cumulative_transience = None

# Try to import sounddevice for real-time playback
SOUNDDEVICE_AVAILABLE = False
SD_ERROR = ""
try:
    import sounddevice as sd
    SOUNDDEVICE_AVAILABLE = True
except ImportError:
    SD_ERROR = "sounddevice module not found. Install with 'pip install sounddevice'."
except OSError as e:
    # This can happen if PortAudio is not found
    SD_ERROR = f"PortAudio library not found or error loading it: {e}"

def play_and_analyze(file_path, mock=False, device=None):
    if cumulative_transience is None:
        raise ImportError("The 'cumulative_transience' extension module could not be loaded.")

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
            try:
                # Try to get device info
                device_info = sd.query_devices(device, kind='output')
                device_name = device_info['name']
                device_sr = device_info['default_samplerate']
                device_channels = device_info['max_output_channels']

                print(f"Device: {device_name}")
                print(f"Device Default Sample Rate: {device_sr} Hz")
                print(f"Device Max Output Channels: {device_channels}")
                print(f"File Sample Rate: {sr} Hz")

                # Check output settings
                try:
                    sd.check_output_settings(device=device, samplerate=sr, channels=1)
                    print(f"Output settings (SR={sr}, Channels=1) are supported.")
                except Exception as check_err:
                    print(f"Warning: Output settings may not be supported: {check_err}")

                print("Starting audible playback...")
                sd.play(y, sr, device=device)
            except Exception as e:
                print(f"Error starting playback: {e}")
                print("Falling back to mock mode.")
                mock = True
        else:
            print(f"Warning: {SD_ERROR}")
            print("Falling back to mock mode (no audio).")
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
                            print(f"{output}  <-- PEAK [{band_names[p['band_idx']]}] Score: {p['total_score']:+.2f}", flush=True)
                    else:
                        print(output, flush=True)

                last_printed_frame = current_frame

            # Sleep a bit to avoid maxing CPU
            time.sleep(0.001)

    except KeyboardInterrupt:
        if not mock and SOUNDDEVICE_AVAILABLE:
            sd.stop()
        print("\nPlayback interrupted by user.", flush=True)
        return

    if not mock and SOUNDDEVICE_AVAILABLE:
        sd.wait()

    print("-" * 65, flush=True)
    print("Analysis Complete.", flush=True)

def main():
    parser = argparse.ArgumentParser(description="Real-time transient analysis and audible playback.")
    parser.add_argument("files", nargs="*", help="Optional list of audio files to process.")
    parser.add_argument("--mock", action="store_true", help="Simulate real-time playback without audio output.")
    parser.add_argument("--list-devices", action="store_true", help="List available audio output devices and exit.")
    parser.add_argument("--device", help="Audio device ID or name substring.")
    args = parser.parse_args()

    if args.list_devices:
        if SOUNDDEVICE_AVAILABLE:
            print("\nAvailable Audio Devices:")
            print(sd.query_devices())
        else:
            print(f"Cannot list devices: {SD_ERROR}")
        return

    # Handle device argument (convert to int if numeric)
    device = args.device
    if device and device.isdigit():
        device = int(device)

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
        try:
            play_and_analyze(f, mock=args.mock, device=device)
        except Exception as e:
            print(f"Error processing {f}:")
            traceback.print_exc()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        traceback.print_exc()
        try:
            input("\nPress Enter to exit...")
        except EOFError:
            pass
