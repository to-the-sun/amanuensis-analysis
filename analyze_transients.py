import librosa
import numpy as np
import scipy.signal
import json
import os
import argparse
import asyncio

def analyze_audio(file_path):
    """
    Analyzes an audio file to extract its transient envelope and identify peaks.
    """
    print(f"Analyzing {file_path}...")
    try:
        # Load audio file. sr=None preserves original sampling rate.
        y, sr = librosa.load(file_path, sr=None)

        # Calculate onset strength (transient envelope)
        # This represents the spectral energy flux across frames.
        onset_env = librosa.onset.onset_strength(y=y, sr=sr)
        times = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr)

        # Find peaks in the onset strength
        # prominence=0.5 and distance=10 are heuristic values for transient detection
        peaks, _ = scipy.signal.find_peaks(onset_env, prominence=0.5, distance=10)

        peak_times = times[peaks].tolist()
        peak_values = onset_env[peaks].tolist()

        return {
            "filename": os.path.basename(file_path),
            "times": times.tolist(),
            "onset_env": onset_env.tolist(),
            "peaks": {
                "times": peak_times,
                "values": peak_values
            }
        }
    except Exception as e:
        print(f"Error analyzing {file_path}: {e}")
        return None

async def generate_screenshot(html_path, output_png):
    """
    Uses Playwright to capture a screenshot of the generated HTML.
    """
    try:
        from playwright.async_api import async_playwright
    except ImportError:
        print("Playwright not found. Automatic screenshot generation is disabled.")
        print("To enable, install with: pip install playwright && playwright install chromium")
        return

    print(f"Generating screenshot: {output_png}...")
    try:
        async with async_playwright() as p:
            browser = await p.chromium.launch(args=["--no-sandbox", "--disable-setuid-sandbox"])
            page = await browser.new_page(viewport={'width': 1200, 'height': 630})
            # Use absolute path for file:// URL
            abs_path = os.path.abspath(html_path)
            await page.goto(f"file://{abs_path}", wait_until="networkidle")
            # Wait for Plotly to render
            try:
                await page.wait_for_selector("#graph .js-plotly-plot", timeout=10000)
            except:
                print("Warning: Plotly graph selector not found, attempting screenshot anyway.")
            # Small delay to ensure everything is stable
            await asyncio.sleep(2)
            await page.screenshot(path=output_png)
            await browser.close()
            print(f"Screenshot saved to {output_png}")
    except Exception as e:
        print(f"Error generating screenshot: {e}")

def main():
    parser = argparse.ArgumentParser(description="Generate a transient analysis report for audio files in a directory.")
    parser.add_argument("--dir", default=".", help="Directory containing audio files (default: current directory)")
    parser.add_argument("--output", default="transient_analysis.html", help="Output HTML file name (default: transient_analysis.html)")
    parser.add_argument("--base-url", default="", help="Base URL where the report and audio files will be hosted (required for Discord embeds)")
    parser.add_argument("--title", default="Transient Analysis Report", help="Title for the report and Discord embed")
    parser.add_argument("--description", default="Interactive transient analysis of audio files.", help="Description for the Discord embed")
    parser.add_argument("--no-screenshot", action="store_true", help="Disable automatic screenshot generation")
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
        result = analyze_audio(file_path)
        if result:
            all_data[f] = result

    if not all_data:
        print("No valid audio data was processed.")
        return

    # Prepare meta tags data
    page_title = args.title
    page_description = args.description

    # Placeholder for image and audio URLs
    image_url = ""
    audio_url = ""

    if args.base_url:
        base_url = args.base_url.rstrip('/')
        image_filename = os.path.splitext(args.output)[0] + ".png"
        image_url = f"{base_url}/{image_filename}"

        # Pick the first audio file as default for og:audio if available
        if audio_files:
            audio_url = f"{base_url}/{audio_files[0]}"

    html_template = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>TITLE_PLACEHOLDER</title>

    <!-- Open Graph / Discord Meta Tags -->
    <meta property="og:type" content="website">
    <meta property="og:title" content="TITLE_PLACEHOLDER">
    <meta property="og:description" content="DESCRIPTION_PLACEHOLDER">
    <meta property="og:image" content="IMAGE_URL_PLACEHOLDER">
    <meta property="og:audio" content="AUDIO_URL_PLACEHOLDER">

    <!-- Twitter Meta Tags -->
    <meta name="twitter:card" content="summary_large_image">
    <meta name="twitter:title" content="TITLE_PLACEHOLDER">
    <meta name="twitter:description" content="DESCRIPTION_PLACEHOLDER">
    <meta name="twitter:image" content="IMAGE_URL_PLACEHOLDER">

    <!-- Using Plotly.js for interactive visualizations -->
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 0; line-height: 1.6; color: #333; background-color: #f9f9f9; }
        .container { max-width: 1200px; min-height: 630px; margin: 0 auto; background: white; padding: 20px; box-sizing: border-box; }
        h1 { color: #2c3e50; text-align: center; margin-top: 0; font-size: 24px; }
        .controls { margin-bottom: 15px; padding: 15px; background: #ecf0f1; border-radius: 5px; }
        .controls label { font-weight: bold; display: block; margin-bottom: 5px; font-size: 14px; }
        #file-select { width: 100%; padding: 8px; font-size: 14px; border-radius: 4px; border: 1px solid #ccc; }
        #audio-player { width: 100%; margin-top: 10px; height: 35px; }
        #graph { width: 100%; height: 420px; margin-top: 10px; }
        .footer { margin-top: 20px; font-size: 11px; text-align: center; color: #7f8c8d; }
    </style>
</head>
<body>
    <div class="container">
        <h1>TITLE_PLACEHOLDER</h1>

        <div class="controls">
            <label for="file-select">Select Audio File:</label>
            <select id="file-select"></select>

            <audio id="audio-player" controls></audio>
        </div>

        <div id="graph"></div>

        <div class="footer">
            Generated by Transient Analysis Tool
        </div>
    </div>

    <script>
        const data = DATA_PLACEHOLDER;
        const fileSelect = document.getElementById('file-select');
        const audioPlayer = document.getElementById('audio-player');
        const graphDiv = document.getElementById('graph');

        // Populate dropdown with available audio files
        Object.keys(data).forEach(filename => {
            const option = document.createElement('option');
            option.value = filename;
            option.textContent = filename;
            fileSelect.appendChild(option);
        });

        let currentFile = fileSelect.value;

        function updateGraph(filename) {
            const fileData = data[filename];

            const traceTransient = {
                x: fileData.times,
                y: fileData.onset_env,
                mode: 'lines',
                name: 'Transient Envelope',
                line: { color: '#3498db', width: 2 },
                hoverinfo: 'x+y'
            };

            const tracePeaks = {
                x: fileData.peaks.times,
                y: fileData.peaks.values,
                mode: 'markers',
                name: 'Peaks',
                marker: { color: '#e74c3c', size: 8, symbol: 'cross' },
                hoverinfo: 'x+y'
            };

            const layout = {
                title: {
                    text: 'Transient Analysis - ' + filename,
                    font: { size: 18 }
                },
                xaxis: { title: 'Time (seconds)', gridcolor: '#eee' },
                yaxis: { title: 'Onset Strength (Energy Flux)', gridcolor: '#eee' },
                plot_bgcolor: '#fff',
                paper_bgcolor: '#fff',
                shapes: [{
                    type: 'line',
                    x0: 0,
                    x1: 0,
                    y0: 0,
                    y1: 1,
                    yref: 'paper',
                    line: {
                        color: '#e67e22',
                        width: 2,
                        dash: 'dash'
                    },
                    name: 'playhead'
                }],
                dragmode: 'zoom',
                hovermode: 'closest',
                showlegend: true,
                legend: { x: 1, xanchor: 'right', y: 1 }
            };

            const config = {
                responsive: true,
                displaylogo: false
            };

            Plotly.newPlot(graphDiv, [traceTransient, tracePeaks], layout, config);
        }

        function updateAudio(filename) {
            // Assumes audio files are in the same relative path as the HTML report
            audioPlayer.src = filename;
            audioPlayer.load();
        }

        fileSelect.addEventListener('change', (e) => {
            currentFile = e.target.value;
            updateGraph(currentFile);
            updateAudio(currentFile);
        });

        // Synchronize playhead on the graph with audio playback
        audioPlayer.addEventListener('timeupdate', () => {
            const currentTime = audioPlayer.currentTime;
            const update = {
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime
            };
            Plotly.relayout(graphDiv, update);
        });

        // Initial load
        if (currentFile) {
            updateGraph(currentFile);
            updateAudio(currentFile);
        }

    </script>
</body>
</html>
    """

    # Injecting data and meta tags
    report_content = html_template.replace("DATA_PLACEHOLDER", json.dumps(all_data))
    report_content = report_content.replace("TITLE_PLACEHOLDER", page_title)
    report_content = report_content.replace("DESCRIPTION_PLACEHOLDER", page_description)
    report_content = report_content.replace("IMAGE_URL_PLACEHOLDER", image_url)
    report_content = report_content.replace("AUDIO_URL_PLACEHOLDER", audio_url)

    output_path = args.output
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(report_content)

    print(f"Report generated successfully: {os.path.abspath(output_path)}")

    # Generate screenshot
    if not args.no_screenshot:
        screenshot_filename = os.path.splitext(output_path)[0] + ".png"
        asyncio.run(generate_screenshot(output_path, screenshot_filename))

if __name__ == "__main__":
    main()
