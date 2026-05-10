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
        # Resolution: 100ms chunks
        hop_length = int(sr * 0.100)
        onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)
        times = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr, hop_length=hop_length)

        # Find peaks in the onset strength
        # prominence=0.5 and distance=2 are heuristic values for transient detection
        # distance=2 at 100ms resolution corresponds to 200ms
        peaks, _ = scipy.signal.find_peaks(onset_env, prominence=0.5, distance=2)

        peak_times = times[peaks].tolist()
        peak_values = onset_env[peaks].tolist()

        # Logarithmic compression for better dynamic range handling in similarity
        log_onset = np.log1p(onset_env)

        # Normalize onset_env for weighting
        max_onset = np.max(log_onset) if np.max(log_onset) > 0 else 1
        norm_onset = log_onset / max_onset

        # Calculate SSM at 100ms resolution
        # Compute pairwise distance matrix using broadcasting
        # SSM(i,j) = |log_onset[i] - log_onset[j]|
        dist_matrix = np.abs(log_onset[:, np.newaxis] - log_onset[np.newaxis, :])

        # Convert to similarity: 1 - normalized distance
        max_dist = np.max(dist_matrix) if np.max(dist_matrix) > 0 else 1
        ssm_base = 1 - (dist_matrix / max_dist)

        # Power scaling (P=12) increases contrast for repeating patterns (diagonals)
        # over general transient intersections (blocks).
        ssm_contrast = ssm_base ** 12

        # Weight by transient strength: multiply by the smaller of the two transiences
        # This ensures that only points with high similarity AND high transience are vibrant.
        transience_weight = np.minimum(norm_onset[:, np.newaxis], norm_onset[np.newaxis, :])
        ssm = ssm_contrast * transience_weight

        # Identity property: Ensure the main diagonal is always solid 1.0
        np.fill_diagonal(ssm, 1.0)

        # Find peak off-diagonal similarity for footnote
        ssm_off_diag = ssm.copy()
        np.fill_diagonal(ssm_off_diag, -1)
        peak_idx = np.unravel_index(np.argmax(ssm_off_diag), ssm_off_diag.shape)
        i, j = peak_idx

        peak_similarity_data = {
            "time_i": float(times[i]),
            "time_j": float(times[j]),
            "onset_i": float(log_onset[i]),
            "onset_j": float(log_onset[j]),
            "max_dist": float(max_dist),
            "max_onset": float(max_onset),
            "final_similarity": float(ssm[i, j])
        }

        # Render SSM to image instead of storing raw JSON data
        # This keeps the HTML report performant even at 100ms resolution
        fig = plt.figure(figsize=(10, 10))
        ax = fig.add_axes([0, 0, 1, 1])
        ax.imshow(ssm, cmap='viridis', origin='lower', aspect='auto', vmin=0, vmax=1, interpolation='nearest')
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
            "ssm_extent": [float(times[0]), float(times[-1])],
            "peak_similarity": peak_similarity_data
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
        .footnote { margin-top: 20px; padding: 15px; background: #fdf6e3; border-left: 5px solid #b58900; font-size: 14px; color: #586e75; }
        .footnote h3 { margin-top: 0; color: #b58900; }
        .math-block { font-family: "Courier New", Courier, monospace; background: #eee; padding: 10px; border-radius: 4px; margin-top: 10px; overflow-x: auto; }
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
        <div id="ssm-footnote" class="footnote"></div>

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
        const footnoteDiv = document.getElementById('ssm-footnote');

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

            // Dummy trace for colorbar
            const colorbarTrace = {
                x: [null],
                y: [null],
                type: 'scatter',
                mode: 'markers',
                marker: {
                    colorscale: 'Viridis',
                    cmin: 0,
                    cmax: 1,
                    showscale: true,
                    colorbar: {
                        title: 'Similarity',
                        thickness: 20,
                        len: 0.9,
                        yanchor: 'middle',
                        y: 0.5
                    }
                },
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

            Plotly.newPlot(ssmDiv, [ssmTrace, colorbarTrace], ssmLayout, config);

            // Update Footnote with Math
            const ps = fileData.peak_similarity;
            const norm_i = ps.onset_i / ps.max_onset;
            const norm_j = ps.onset_j / ps.max_onset;
            const dist = Math.abs(ps.onset_i - ps.onset_j);
            const base_similarity = 1 - (dist / ps.max_dist);
            const weight = Math.min(norm_i, norm_j);

            const contrast_similarity = Math.pow(base_similarity, 12);

            footnoteDiv.innerHTML = `
                <h3>Peak Off-Diagonal Similarity Analysis</h3>
                <p>The point of greatest similarity (excluding the main diagonal) occurs at <strong>t1 = ${ps.time_i.toFixed(3)}s</strong> and <strong>t2 = ${ps.time_j.toFixed(3)}s</strong>. Similarity is calculated using log-compressed onset strength to better represent rhythmic perception.</p>
                <div class="math-block">
                    <strong>1. Logarithmic Normalization:</strong><br>
                    norm_i = log_onset_i / max_log_onset = ${ps.onset_i.toFixed(4)} / ${ps.max_onset.toFixed(4)} = ${norm_i.toFixed(4)}<br>
                    norm_j = log_onset_j / max_log_onset = ${ps.onset_j.toFixed(4)} / ${ps.max_onset.toFixed(4)} = ${norm_j.toFixed(4)}<br><br>
                    <strong>2. Contrast-Enhanced Similarity:</strong><br>
                    dist = |log_onset_i - log_onset_j| = |${ps.onset_i.toFixed(4)} - ${ps.onset_j.toFixed(4)}| = ${dist.toFixed(4)}<br>
                    S_base = 1 - (dist / max_dist) = 1 - (${dist.toFixed(4)} / ${ps.max_dist.toFixed(4)}) = ${base_similarity.toFixed(4)}<br>
                    S_contrast = S_base^12 = ${base_similarity.toFixed(4)}^12 = ${contrast_similarity.toFixed(4)}<br><br>
                    <strong>3. Transience Weighting:</strong><br>
                    W = min(norm_i, norm_j) = min(${norm_i.toFixed(4)}, ${norm_j.toFixed(4)}) = ${weight.toFixed(4)}<br><br>
                    <strong>4. Final Similarity:</strong><br>
                    S_final = S_contrast * W = ${contrast_similarity.toFixed(4)} * ${weight.toFixed(4)} = <strong>${ps.final_similarity.toFixed(4)}</strong>
                </div>
            `;
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
