import librosa
import numpy as np
import scipy.signal
import json
import os
import argparse
import matplotlib.pyplot as plt
import io
import base64

def analyze_audio(file_path):
    """
    Analyzes an audio file to extract its transient envelope and identify peaks.
    Generates a high-resolution SSM image (Base64).
    """
    print(f"Analyzing {file_path}...")
    try:
        # Load audio file. sr=None preserves original sampling rate.
        y, sr = librosa.load(file_path, sr=None)

        # Calculate onset strength (transient envelope)
        # Resolution: 20ms chunks
        hop_length = int(sr * 0.020)
        onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)
        times = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr, hop_length=hop_length)

        # Find peaks in the onset strength
        # prominence=0.5 and distance=10 are heuristic values for transient detection
        peaks, _ = scipy.signal.find_peaks(onset_env, prominence=0.5, distance=10)

        peak_times = times[peaks].tolist()
        peak_values = onset_env[peaks].tolist()

        # Calculate SSM at full 20ms resolution
        # Compute pairwise distance matrix using broadcasting
        # SSM(i,j) = |onset_env[i] - onset_env[j]|
        dist_matrix = np.abs(onset_env[:, np.newaxis] - onset_env[np.newaxis, :])

        # Convert to similarity: 1 - normalized distance
        max_dist = np.max(dist_matrix) if np.max(dist_matrix) > 0 else 1
        ssm = 1 - (dist_matrix / max_dist)

        # Render SSM to image instead of storing raw JSON data
        # This keeps the HTML report performant even at high resolution (20ms chunks)
        fig = plt.figure(figsize=(10, 10))
        ax = fig.add_axes([0, 0, 1, 1])
        ax.imshow(ssm, cmap='viridis', origin='lower', aspect='auto')
        ax.axis('off')

        buf = io.BytesIO()
        fig.savefig(buf, format='png', dpi=150)
        plt.close(fig)
        buf.seek(0)
        ssm_base64 = base64.b64encode(buf.read()).decode('utf-8')

        return {
            "filename": os.path.basename(file_path),
            "times": times.tolist(),
            "onset_env": onset_env.tolist(),
            "peaks": {
                "times": peak_times,
                "values": peak_values
            },
            "ssm_image": ssm_base64,
            "ssm_extent": [float(times[0]), float(times[-1])]
        }
    except Exception as e:
        print(f"Error analyzing {file_path}: {e}")
        return None

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
        result = analyze_audio(file_path)
        if result:
            all_data[f] = result

    if not all_data:
        print("No valid audio data was processed.")
        return

    html_template = """
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Transient Analysis Report</title>
    <!-- Using Plotly.js for interactive visualizations -->
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 40px; line-height: 1.6; color: #333; background-color: #f9f9f9; }
        .container { max-width: 1200px; margin: auto; background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #2c3e50; text-align: center; }
        .controls { margin-bottom: 30px; padding: 20px; background: #ecf0f1; border-radius: 5px; }
        .controls label { font-weight: bold; display: block; margin-bottom: 10px; }
        #file-select { width: 100%; padding: 10px; font-size: 16px; border-radius: 4px; border: 1px solid #ccc; }
        #audio-player { width: 100%; margin-top: 20px; }
        #graph { width: 100%; height: 500px; margin-top: 20px; }
        #ssm-graph { width: 100%; height: 600px; margin-top: 30px; }
        .footer { margin-top: 40px; font-size: 12px; text-align: center; color: #7f8c8d; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Transient Analysis Report</h1>

        <div class="controls">
            <label for="file-select">Select Audio File:</label>
            <select id="file-select"></select>

            <audio id="audio-player" controls></audio>
        </div>

        <div id="graph"></div>
        <div id="ssm-graph"></div>

        <div class="footer">
            Generated by Transient Analysis Tool
        </div>
    </div>

    <script>
        const data = DATA_PLACEHOLDER;
        const fileSelect = document.getElementById('file-select');
        const audioPlayer = document.getElementById('audio-player');
        const graphDiv = document.getElementById('graph');
        const ssmDiv = document.getElementById('ssm-graph');

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

            // Update SSM Visualization using Base64 Image
            const ssmExtent = fileData.ssm_extent;

            // Dummy trace to establish axes
            const ssmTrace = {
                x: ssmExtent,
                y: ssmExtent,
                mode: 'markers',
                marker: { opacity: 0 },
                showlegend: false,
                hoverinfo: 'none'
            };

            const ssmLayout = {
                title: {
                    text: 'Transient Self-Similarity Matrix',
                    font: { size: 18 }
                },
                xaxis: {
                    title: 'Time (seconds)',
                    range: ssmExtent,
                    fixedrange: false
                },
                yaxis: {
                    title: 'Time (seconds)',
                    range: ssmExtent,
                    scaleanchor: 'x',
                    fixedrange: false
                },
                images: [{
                    source: 'data:image/png;base64,' + fileData.ssm_image,
                    xref: 'x',
                    yref: 'y',
                    x: ssmExtent[0],
                    y: ssmExtent[1],
                    sizex: ssmExtent[1] - ssmExtent[0],
                    sizey: ssmExtent[1] - ssmExtent[0],
                    sizing: 'stretch',
                    opacity: 1,
                    layer: 'below'
                }],
                shapes: [{
                    type: 'line',
                    x0: 0,
                    x1: 0,
                    y0: ssmExtent[0],
                    y1: ssmExtent[1],
                    xref: 'x',
                    yref: 'y',
                    line: { color: '#e67e22', width: 2, dash: 'dash' },
                    name: 'playhead-x'
                }, {
                    type: 'line',
                    y0: 0,
                    y1: 0,
                    x0: ssmExtent[0],
                    x1: ssmExtent[1],
                    xref: 'x',
                    yref: 'y',
                    line: { color: '#e67e22', width: 2, dash: 'dash' },
                    name: 'playhead-y'
                }]
            };

            Plotly.newPlot(ssmDiv, [ssmTrace], ssmLayout, config);
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

            // Update Transient Graph Playhead
            Plotly.relayout(graphDiv, {
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime
            });

            // Update SSM Crosshair Playhead
            Plotly.relayout(ssmDiv, {
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime,
                'shapes[1].y0': currentTime,
                'shapes[1].y1': currentTime
            });
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

    # Injecting data as JSON. Using json.dumps ensures proper escaping.
    report_content = html_template.replace("DATA_PLACEHOLDER", json.dumps(all_data))

    output_path = args.output
    with open(output_path, "w", encoding="utf-8") as f:
        f.write(report_content)

    print(f"Report generated successfully: {os.path.abspath(output_path)}")

if __name__ == "__main__":
    main()
