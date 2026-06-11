import librosa
import numpy as np
import scipy.signal
import json
import os
import argparse
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import io
import base64
import subprocess
import tempfile
import shutil

def generate_video(audio_path, data):
    """
    Generates a video file for the analyzed audio showing a moving playhead over the transient graph
    and an accumulating 10-second buffer.
    """
    print(f"Generating video for {audio_path}...")
    try:
        times = data['times']
        onset_env = np.array(data['onset_env'])
        peak_times = data['peaks']['times']
        peak_values = data['peaks']['values']
        peak_indices = set(data['peaks']['indices'])
        max_peak = data['max_peak_value']

        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10), gridspec_kw={'height_ratios': [1, 1]})

        # Top Plot: Transient Envelope
        ax1.plot(times, onset_env, color='#3498db', lw=2, label='Transient Envelope')
        ax1.scatter(peak_times, peak_values, color='#e74c3c', marker='x', s=50, label='Peaks')
        playhead = ax1.axvline(x=0, color='#e67e22', lw=2, ls='--')
        ax1.set_title(f"Transient Analysis - {os.path.basename(audio_path)}")
        ax1.set_ylabel("Onset Strength")
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_xlim(times[0], times[-1])
        ax1.set_ylim(0, max(onset_env) * 1.1 if len(onset_env) > 0 else 1)

        # Bottom Plot: Accumulated 5s Historical Buffer
        buffer_len = 51  # 5 seconds @ 100ms = 50 steps + current
        accumulated_buffer = np.zeros(buffer_len)
        buffer_times = np.linspace(-5000, 0, buffer_len)
        buffer_line, = ax2.plot(buffer_times, accumulated_buffer, color='#2ecc71', lw=2)
        ax2.set_title("Accumulated 5s Historical Buffer")
        ax2.set_xlabel("Time Relative to Peak (ms)")
        ax2.set_ylabel("Accumulated Energy")
        ax2.grid(True, alpha=0.3)
        ax2.set_xlim(-5000, 0)
        ax2.set_ylim(0, 1)

        # Use the first half of a 10s Hanning window so it peaks at the end
        hanning = np.hanning(101)[:51]

        # Flash and fade storage
        active_flashes = [] # List of (snapshot, frames_remaining)
        flash_fill_artists = []
        peak_lines = []
        peak_labels = []

        def update(frame):
            # Update playhead
            playhead.set_xdata([times[frame], times[frame]])

            # Check for peak
            if frame in peak_indices:
                start = frame - 50
                end = frame

                # Extract window with padding
                if start < 0:
                    window = onset_env[0 : end + 1]
                    window = np.pad(window, (abs(start), 0), mode='constant')
                else:
                    window = onset_env[start : end + 1]

                if len(window) == buffer_len:
                    peak_val = onset_env[frame]
                    normalization = peak_val / max_peak if max_peak > 0 else 1.0
                    snapshot = window * hanning * normalization
                    accumulated_buffer[:] += snapshot
                    buffer_line.set_ydata(accumulated_buffer)

                    # Add new flash: (data, initial_lifetime)
                    active_flashes.append([snapshot, 20])

                    # Dynamic Y-axis scaling for buffer
                    current_max = np.max(accumulated_buffer)
                    if current_max > ax2.get_ylim()[1]:
                        ax2.set_ylim(0, current_max * 1.1)

            # Handle Flash and Fade
            for artist in flash_fill_artists:
                artist.remove()
            flash_fill_artists.clear()

            for flash in active_flashes[:]:
                snapshot, lifetime = flash
                alpha = (lifetime / 20.0) * 0.5 # Max 0.5 alpha
                fill = ax2.fill_between(buffer_times, 0, snapshot, color='#2ecc71', alpha=alpha)
                flash_fill_artists.append(fill)
                flash[1] -= 1
                if flash[1] <= 0:
                    active_flashes.remove(flash)

            # Analyze peaks in the accumulated buffer
            for line in peak_lines:
                line.remove()
            for label in peak_labels:
                label.remove()
            peak_lines.clear()
            peak_labels.clear()

            if np.max(accumulated_buffer) > 0.1:
                # distance=5 ensures we don't have too many labels (500ms apart)
                peaks_in_buf, _ = scipy.signal.find_peaks(accumulated_buffer, height=np.max(accumulated_buffer)*0.3, distance=5)
                for p_idx in peaks_in_buf:
                    ms_val = int(buffer_times[p_idx])
                    line = ax2.axvline(x=ms_val, color='white', lw=1, alpha=0.6, ls=':')
                    label = ax2.text(ms_val, ax2.get_ylim()[1]*0.9, f"{ms_val}ms", color='white',
                                    fontsize=8, ha='center', fontweight='bold',
                                    bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=1))
                    peak_lines.append(line)
                    peak_labels.append(label)

            return [playhead, buffer_line] + flash_fill_artists + peak_lines + peak_labels

        # 100ms resolution = 10 FPS
        ani = animation.FuncAnimation(fig, update, frames=len(times), blit=False, interval=100)

        with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as tmp:
            temp_video_path = tmp.name

        writer = animation.FFMpegWriter(fps=10, metadata=dict(artist='Transient Analysis Tool'), bitrate=2000)
        ani.save(temp_video_path, writer=writer)
        plt.close(fig)

        # Merge with original audio using ffmpeg
        output_video = os.path.splitext(audio_path)[0] + ".mp4"

        if not shutil.which("ffmpeg"):
            print("Error: ffmpeg is not installed. Skipping video-audio merge.")
            return

        cmd = [
            'ffmpeg', '-y',
            '-i', temp_video_path,
            '-i', audio_path,
            '-c:v', 'copy',
            '-c:a', 'aac',
            '-shortest',
            output_video
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        if os.path.exists(temp_video_path):
            os.remove(temp_video_path)

        print(f"Video generated: {output_video}")
    except Exception as e:
        print(f"Error generating video for {audio_path}: {e}")

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
        peak_indices = peaks.tolist()
        max_peak_value = float(np.max(peak_values)) if len(peak_values) > 0 else 1.0

        # Normalize onset_env for weighting
        max_onset = np.max(onset_env) if np.max(onset_env) > 0 else 1
        norm_onset = onset_env / max_onset

        # Calculate SSM at 100ms resolution
        # Compute pairwise distance matrix using broadcasting
        # SSM(i,j) = |onset_env[i] - onset_env[j]|
        dist_matrix = np.abs(onset_env[:, np.newaxis] - onset_env[np.newaxis, :])

        # Convert to similarity: 1 - normalized distance
        max_dist = np.max(dist_matrix) if np.max(dist_matrix) > 0 else 1
        ssm = 1 - (dist_matrix / max_dist)

        # Weight by transient strength: multiply by the smaller of the two transiences
        # This ensures that only points with high similarity AND high transience are vibrant.
        transience_weight = np.minimum(norm_onset[:, np.newaxis], norm_onset[np.newaxis, :])
        ssm = ssm * transience_weight

        # Find peak off-diagonal similarity for footnote
        ssm_off_diag = ssm.copy()
        np.fill_diagonal(ssm_off_diag, -1)
        peak_idx = np.unravel_index(np.argmax(ssm_off_diag), ssm_off_diag.shape)
        i, j = peak_idx

        peak_similarity_data = {
            "time_i": float(times[i]),
            "time_j": float(times[j]),
            "onset_i": float(onset_env[i]),
            "onset_j": float(onset_env[j]),
            "max_dist": float(max_dist),
            "max_onset": float(max_onset),
            "final_similarity": float(ssm[i, j])
        }

        # Render SSM to image instead of storing raw JSON data
        # This keeps the HTML report performant even at 100ms resolution
        fig = plt.figure(figsize=(10, 10))
        ax = fig.add_axes([0, 0, 1, 1])
        ax.imshow(ssm, cmap='viridis', origin='lower', aspect='auto', vmin=0, vmax=1)
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
                "values": peak_values,
                "indices": peak_indices
            },
            "max_peak_value": max_peak_value,
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
            generate_video(file_path, result)

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
        #graph { width: 100%; height: 400px; margin-top: 20px; }
        #buffer-graph { width: 100%; height: 400px; margin-top: 20px; }
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
        <div id="buffer-graph"></div>
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
        const bufferDiv = document.getElementById('buffer-graph');
        const ssmDiv = document.getElementById('ssm-graph');
        const footnoteDiv = document.getElementById('ssm-footnote');

        let accumulatedBuffer = new Array(51).fill(0);
        let processedPeaks = new Set();
        let activeFlashes = []; // {snapshot: [], lifetime: 20}
        let lastFrameIdx = -1;

        function getHanningWindow(size) {
            let window = new Array(size);
            for (let i = 0; i < size; i++) {
                window[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / (size - 1)));
            }
            return window;
        }
        // Use first half of 10s Hanning (101 points)
        const hanning = getHanningWindow(101).slice(0, 51);

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

            // Initialize Buffer Graph
            const bufferTimes = [];
            for (let i = -50; i <= 0; i++) bufferTimes.push(i * 100);

            const traceBuffer = {
                x: bufferTimes,
                y: accumulatedBuffer,
                mode: 'lines',
                name: 'Accumulated Buffer',
                line: { color: '#2ecc71', width: 2 },
                fill: 'tozeroy'
            };

            const bufferLayout = {
                title: 'Accumulated 5s Historical Buffer (Real-time)',
                xaxis: { title: 'Time Relative to Peak (ms)' },
                yaxis: { title: 'Accumulated Energy', autorange: true },
                plot_bgcolor: '#fff',
                paper_bgcolor: '#fff'
            };

            Plotly.newPlot(bufferDiv, [traceBuffer], bufferLayout, config);

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

            footnoteDiv.innerHTML = `
                <h3>Peak Off-Diagonal Similarity Analysis</h3>
                <p>The point of greatest similarity (excluding the main diagonal) occurs at <strong>t1 = ${ps.time_i.toFixed(3)}s</strong> and <strong>t2 = ${ps.time_j.toFixed(3)}s</strong>.</p>
                <div class="math-block">
                    <strong>1. Normalization:</strong><br>
                    norm_i = onset_i / max_onset = ${ps.onset_i.toFixed(4)} / ${ps.max_onset.toFixed(4)} = ${norm_i.toFixed(4)}<br>
                    norm_j = onset_j / max_onset = ${ps.onset_j.toFixed(4)} / ${ps.max_onset.toFixed(4)} = ${norm_j.toFixed(4)}<br><br>
                    <strong>2. Base Similarity:</strong><br>
                    dist = |onset_i - onset_j| = |${ps.onset_i.toFixed(4)} - ${ps.onset_j.toFixed(4)}| = ${dist.toFixed(4)}<br>
                    S_base = 1 - (dist / max_dist) = 1 - (${dist.toFixed(4)} / ${ps.max_dist.toFixed(4)}) = ${base_similarity.toFixed(4)}<br><br>
                    <strong>3. Transience Weighting:</strong><br>
                    W = min(norm_i, norm_j) = min(${norm_i.toFixed(4)}, ${norm_j.toFixed(4)}) = ${weight.toFixed(4)}<br><br>
                    <strong>4. Final Similarity:</strong><br>
                    S_final = S_base * W = ${base_similarity.toFixed(4)} * ${weight.toFixed(4)} = <strong>${ps.final_similarity.toFixed(4)}</strong>
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
            accumulatedBuffer.fill(0);
            processedPeaks.clear();
            lastFrameIdx = -1;
            updateGraph(currentFile);
            updateAudio(currentFile);
        });

        // Synchronize playhead on the graph with audio playback
        audioPlayer.addEventListener('timeupdate', () => {
            const currentTime = audioPlayer.currentTime;
            const fileData = data[currentFile];
            const frameIdx = Math.floor(currentTime * 10); // 100ms resolution

            // Reset if seeking back
            if (frameIdx < lastFrameIdx) {
                accumulatedBuffer.fill(0);
                processedPeaks.clear();
            }

            // Check for peaks passed since last update
            const peakIndices = fileData.peaks.indices;
            let bufferUpdated = false;

            for (let i = 0; i < peakIndices.length; i++) {
                const peakIdx = peakIndices[i];
                if (peakIdx <= frameIdx && !processedPeaks.has(peakIdx)) {
                    processedPeaks.add(peakIdx);

                    const start = peakIdx - 50;
                    const end = peakIdx;
                    const window = new Array(51).fill(0);

                    for (let j = 0; j <= 50; j++) {
                        const envIdx = start + j;
                        if (envIdx >= 0 && envIdx < fileData.onset_env.length) {
                            window[j] = fileData.onset_env[envIdx];
                        }
                    }

                    const peakVal = fileData.onset_env[peakIdx];
                    const normalization = peakVal / fileData.max_peak_value;

                    const snapshot = new Array(51);
                    for (let j = 0; j < 51; j++) {
                        snapshot[j] = window[j] * hanning[j] * normalization;
                        accumulatedBuffer[j] += snapshot[j];
                    }
                    activeFlashes.push({snapshot: snapshot, lifetime: 20});
                    bufferUpdated = true;
                }
            }

            if (bufferUpdated || activeFlashes.length > 0) {
                // Handle Flash and Fade
                activeFlashes = activeFlashes.filter(f => f.lifetime > 0);
                activeFlashes.forEach(f => f.lifetime--);

                // Update Buffer Graph with Flashes and Peaks
                const traces = [
                    {
                        x: bufferTimes,
                        y: accumulatedBuffer,
                        mode: 'lines',
                        name: 'Accumulated Buffer',
                        line: { color: '#2ecc71', width: 2 },
                        fill: 'tozeroy'
                    }
                ];

                activeFlashes.forEach((f, idx) => {
                    traces.push({
                        x: bufferTimes,
                        y: f.snapshot,
                        mode: 'lines',
                        name: 'Flash ' + idx,
                        line: { color: 'rgba(46, 204, 113, ' + (f.lifetime / 20 * 0.5) + ')', width: 0 },
                        fill: 'tozeroy',
                        showlegend: false,
                        hoverinfo: 'none'
                    });
                });

                // Peak Detection in Buffer
                const shapes = [];
                const annotations = [];
                const maxVal = Math.max(...accumulatedBuffer);
                if (maxVal > 0.1) {
                    // Heuristic peak detection: higher than neighbors and > 30% of max
                    for (let i = 1; i < accumulatedBuffer.length - 1; i++) {
                        if (accumulatedBuffer[i] > accumulatedBuffer[i-1] &&
                            accumulatedBuffer[i] > accumulatedBuffer[i+1] &&
                            accumulatedBuffer[i] > maxVal * 0.3) {

                            const msVal = bufferTimes[i];
                            shapes.push({
                                type: 'line',
                                x0: msVal,
                                x1: msVal,
                                y0: 0,
                                y1: 1,
                                yref: 'paper',
                                line: { color: 'white', width: 1, dash: 'dot', opacity: 0.6 }
                            });
                            annotations.push({
                                x: msVal,
                                y: 1,
                                yref: 'paper',
                                text: msVal + 'ms',
                                showarrow: false,
                                font: { color: 'white', size: 10, weight: 'bold' },
                                bgcolor: 'rgba(0,0,0,0.5)',
                                yshift: -10
                            });
                        }
                    }
                }

                Plotly.react(bufferDiv, traces, {
                    title: 'Accumulated 5s Historical Buffer (Real-time)',
                    xaxis: { title: 'Time Relative to Peak (ms)' },
                    yaxis: { title: 'Accumulated Energy', autorange: true },
                    plot_bgcolor: '#333', // Darker background for better contrast with white lines
                    paper_bgcolor: '#fff',
                    shapes: shapes,
                    annotations: annotations
                });
            }

            lastFrameIdx = frameIdx;

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
