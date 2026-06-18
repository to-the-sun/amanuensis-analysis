import os
import argparse
import cumulative_transience

def main():
    parser = argparse.ArgumentParser(description="Generate a transient analysis report for audio files in a directory.")
    parser.add_argument("--dir", default=".", help="Directory containing audio files (default: current directory)")
    parser.add_argument("--output", default="transient_analysis.html", help="Output HTML file name (default: transient_analysis.html)")
    args = parser.parse_args()

    # Supported audio extensions
    extensions = ('.wav', '.mp3', '.m4a', '.flac', '.ogg', '.aiff')
    audio_files = [f for f in os.listdir(args.dir) if f.lower().endswith(extensions)]
    audio_files.sort()

    if not audio_files:
        print(f"No audio files found in {os.path.abspath(args.dir)}")
        return

    all_data = {}
    for f in audio_files:
        file_path = os.path.join(args.dir, f)
        result = cumulative_transience.analyze_audio(file_path)
        if result:
            all_data[f] = result
            cumulative_transience.generate_video(file_path, result)

    if not all_data:
        print("No valid audio data was processed.")
        return

    cumulative_transience.render_html_report(all_data, args.output)

if __name__ == "__main__":
    main()
