import librosa
import numpy as np
import scipy.signal
import json
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from tqdm import tqdm
import io
import base64
import subprocess
import tempfile
import shutil

def generate_video(audio_path, data):
    """
    Generates a video file for the analyzed audio showing a moving playhead over the transient graphs
    (overlapping 4-band analysis) and an accumulating 10-second buffer.
    Returns the path to the generated MP4 file.
    """
    print(f"Generating video for {audio_path}...")
    try:
        times = data['times']

        # Load the 4 bands
        onset_envs = [np.array(data[f'onset_env_{i}']) for i in range(4)]
        peak_indices_list = [set(data[f'peaks_{i}']['indices']) for i in range(4)]
        peak_times_list = [data[f'peaks_{i}']['times'] for i in range(4)]
        peak_values_list = [data[f'peaks_{i}']['values'] for i in range(4)]

        max_peak = data['max_peak_value']

        fig, (ax_transient, ax_buf) = plt.subplots(2, 1, figsize=(12, 12), gridspec_kw={'height_ratios': [1, 1]})

        # Colors and Alphas for the 4 bands
        # Band 0: Bassiest, Band 3: Trebliest
        colors = ['#1b4f72', '#3498db', '#2ecc71', '#a9dfbf']
        alphas = [1.0, 0.8, 0.6, 0.4]
        labels = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble']

        transient_lines = []
        for i in range(4):
            line, = ax_transient.plot(times, onset_envs[i], color=colors[i], lw=2, alpha=alphas[i], label=labels[i])
            transient_lines.append(line)
            ax_transient.scatter(peak_times_list[i], peak_values_list[i], color='#e74c3c', marker='x', s=30, alpha=alphas[i])

        playhead_transient = ax_transient.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_transient = ax_transient.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')

        ax_transient.set_title(f"4-Band Transient Analysis - {os.path.basename(audio_path)}")
        ax_transient.set_ylabel("Onset Strength")
        ax_transient.legend(loc='upper right')
        ax_transient.grid(True, alpha=0.3)
        ax_transient.set_xlim(-20, 5)

        # Determine global max for Y-axis
        all_onset_vals = np.concatenate(onset_envs)
        ax_transient.set_ylim(0, max(all_onset_vals) * 1.1 if len(all_onset_vals) > 0 else 1)

        # Bottom Plot: Accumulated 5s Historical Buffer
        buffer_len = 5001  # 5 seconds @ 1ms = 5000 steps + current
        accumulated_buffer = np.zeros(buffer_len)
        buffer_times = np.linspace(-5000, 0, buffer_len)
        buffer_line, = ax_buf.plot(buffer_times, accumulated_buffer, color='#f1c40f', lw=2)
        ax_buf.set_title("Accumulated 5s Historical Buffer")
        ax_buf.set_xlabel("Time Relative to Peak (ms)")
        ax_buf.set_ylabel("Accumulated Energy")
        ax_buf.grid(True, alpha=0.3)
        ax_buf.set_xlim(-5000, 0)
        ax_buf.set_ylim(0, 1)

        # Flash and fade storage
        active_flashes = [] # List of (snapshot, frames_remaining)
        flash_fill_artists = []
        peak_lines = []
        peak_labels = []

        # Rhythm Metrics text initialization
        metrics_text = ax_buf.text(0.02, 0.95, '', transform=ax_buf.transAxes,
                                   verticalalignment='top', fontsize=10, color='#f1c40f',
                                   fontweight='bold', bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=2))

        average_line, = ax_buf.plot(buffer_times, np.zeros(len(buffer_times)), color='#3498db', lw=1.5, ls='--', alpha=0.9, label='Average Energy')

        peak_history = []

        # Track snapshots for cleanup
        peak_snapshots = [{} for _ in range(4)] # List of dictionaries (peak_idx -> snapshot) for each band
        processed_peaks = [set() for _ in range(4)]
        cleaned_peaks = [set() for _ in range(4)]

        def update(frame):
            current_time = times[frame]
            ax_transient.set_xlim(current_time - 20, current_time + 5)

            # Update playheads and cleanup sweeps
            playhead_transient.set_xdata([current_time, current_time])
            cleanup_time = current_time - 15
            cleanup_transient.set_xdata([cleanup_time, cleanup_time])

            buffer_updated = False

            # Process Peaks for all 4 bands
            for band_idx in range(4):
                for p_idx in peak_indices_list[band_idx]:
                    if p_idx > frame - 100 and p_idx <= frame and p_idx not in processed_peaks[band_idx]:
                        processed_peaks[band_idx].add(p_idx)
                        start = p_idx - 5000
                        end = p_idx
                        if start < 0:
                            window = onset_envs[band_idx][0 : end + 1]
                            window = np.pad(window, (abs(start), 0), mode='constant')
                        else:
                            window = onset_envs[band_idx][start : end + 1]

                        if len(window) == buffer_len:
                            peak_val = onset_envs[band_idx][p_idx]
                            normalization = peak_val / max_peak if max_peak > 0 else 1.0
                            snapshot = window * normalization
                            accumulated_buffer[:] += snapshot
                            peak_snapshots[band_idx][p_idx] = snapshot
                            buffer_updated = True
                            active_flashes.append([snapshot, 20])

            # Dynamic Y-axis scaling for buffer (excluding peak at 0ms)
            current_max = np.max(accumulated_buffer[:-100]) if len(accumulated_buffer) > 100 else 0
            ax_buf.set_ylim(0, max(0.1, current_max * 1.1))

            # Check for peak at cleanup sweep (15 seconds = 15000 frames @ 1ms)
            cleanup_frame_threshold = frame - 15000

            for band_idx in range(4):
                for p_idx in list(peak_snapshots[band_idx].keys()):
                    if p_idx <= cleanup_frame_threshold and p_idx not in cleaned_peaks[band_idx]:
                        accumulated_buffer[:] -= peak_snapshots[band_idx][p_idx]
                        cleaned_peaks[band_idx].add(p_idx)
                        del peak_snapshots[band_idx][p_idx]
                        buffer_updated = True

            if buffer_updated:
                buffer_line.set_ydata(accumulated_buffer)

            # Calculate Rhythm Metrics (excluding peak at 0ms)
            data_to_measure = accumulated_buffer[:-100]
            if len(data_to_measure) > 0:
                std_dev = np.std(data_to_measure)
                mean_val = np.mean(data_to_measure)
                average_line.set_ydata(np.full(len(buffer_times), mean_val))
                contrast = np.max(data_to_measure) / mean_val if mean_val > 0 else 0
                peak_std = np.std(peak_history) if peak_history else 0.0
                metrics_text.set_text(f"Std Dev: {std_dev:.3f}\nContrast: {contrast:.3f}\nPeak Std: {peak_std:.3f}")
            else:
                metrics_text.set_text("Std Dev: 0.000\nContrast: 0.000\nPeak Std: 0.000")
                average_line.set_ydata(np.zeros(len(buffer_times)))

            # Handle Flash and Fade
            for artist in flash_fill_artists:
                artist.remove()
            flash_fill_artists.clear()

            for flash in active_flashes[:]:
                snapshot, lifetime = flash
                alpha = (lifetime / 20.0) * 0.5 # Max 0.5 alpha
                if np.any(snapshot > 0):
                    fill = ax_buf.fill_between(buffer_times, 0, snapshot, color='#2ecc71', alpha=alpha)
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
                peaks_in_buf, props = scipy.signal.find_peaks(accumulated_buffer, height=0.01, distance=200)
                if len(peaks_in_buf) > 0:
                    peak_heights = props['peak_heights']
                    top_indices = np.argsort(peak_heights)[-3:][::-1]

                    # Track highest peak's X-value stability (ms)
                    highest_peak_idx = peaks_in_buf[top_indices[0]]
                    peak_history.append(float(buffer_times[highest_peak_idx]))

                    peak_styles = [
                        {'color': '#f1c40f', 'lw': 2, 'alpha': 1.0}, # 1st: Gold
                        {'color': '#ecf0f1', 'lw': 1.5, 'alpha': 0.9}, # 2nd: Silver
                        {'color': '#bdc3c7', 'lw': 1, 'alpha': 0.8}, # 3rd: Bronze
                    ]

                    for i, idx in enumerate(top_indices):
                        p_idx = peaks_in_buf[idx]
                        ms_val = int(buffer_times[p_idx])
                        style = peak_styles[i] if i < len(peak_styles) else peak_styles[-1]

                        line = ax_buf.axvline(x=ms_val, color=style['color'], lw=style['lw'], alpha=style['alpha'], ls='--')
                        label = ax_buf.text(ms_val, ax_buf.get_ylim()[1]*0.9 - (i * 0.05 * ax_buf.get_ylim()[1]),
                                        f"{ms_val}ms", color=style['color'],
                                        fontsize=8, ha='center', fontweight='bold',
                                        bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=1))
                        peak_lines.append(line)
                        peak_labels.append(label)

            return [playhead_transient, cleanup_transient, buffer_line, metrics_text, average_line] + flash_fill_artists + peak_lines + peak_labels

        # Update every 100ms, stepping 100 frames (1ms each)
        frame_indices = range(0, len(times), 100)
        num_frames = len(frame_indices)
        ani = animation.FuncAnimation(fig, update, frames=frame_indices, blit=False, interval=100)

        with tempfile.NamedTemporaryFile(suffix=".mp4", delete=False) as tmp:
            temp_video_path = tmp.name

        pbar = tqdm(total=num_frames, desc="Rendering Video", unit="frame")
        def progress_callback(i, n):
            pbar.n = i + 1
            pbar.refresh()

        writer = animation.FFMpegWriter(fps=10, metadata=dict(artist='Transient Analysis Tool'), bitrate=2000)
        ani.save(temp_video_path, writer=writer, progress_callback=progress_callback)
        pbar.close()
        plt.close(fig)

        # Merge with original audio using ffmpeg
        output_video = os.path.splitext(audio_path)[0] + ".mp4"

        if not shutil.which("ffmpeg"):
            print("Error: ffmpeg is not installed. Skipping video-audio merge.")
            return None

        cmd = [
            'ffmpeg', '-y',
            '-i', temp_video_path,
            '-i', audio_path,
            '-c:v', 'copy',
            '-c:a', 'aac',
            '-ac', '1',
            '-shortest',
            output_video
        ]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        if os.path.exists(temp_video_path):
            os.remove(temp_video_path)

        print(f"Video generated: {output_video}")
        return output_video
    except Exception as e:
        print(f"Error generating video for {audio_path}: {e}")
        return None

def render_html_report(all_data, output_path):
    """
    Generates an interactive HTML report for the analyzed audio data.
    """
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
        #transient-graph { width: 100%; height: 500px; margin-top: 20px; }
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

        <div id="transient-graph"></div>
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
        const transientGraphDiv = document.getElementById('transient-graph');
        const bufferDiv = document.getElementById('buffer-graph');
        const ssmDiv = document.getElementById('ssm-graph');
        const footnoteDiv = document.getElementById('ssm-footnote');

        let accumulatedBuffer = new Array(5001).fill(0);
        let peakHistory = [];

        let processedPeaks = [new Set(), new Set(), new Set(), new Set()];
        let cleanedPeaks = [new Set(), new Set(), new Set(), new Set()];
        let peakSnapshots = [{}, {}, {}, {}]; // peakIdx -> snapshot for each band

        let activeFlashes = []; // {snapshot: [], lifetime: 20}
        let lastFrameIdx = -1;

        const bufferTimes = [];
        for (let i = -5000; i <= 0; i++) bufferTimes.push(i);

        const bandColors = ['#1b4f72', '#3498db', '#2ecc71', '#a9dfbf'];
        const bandAlphas = [1.0, 0.8, 0.6, 0.4];
        const bandLabels = ['Sub-Bass', 'Bass/Low-Mid', 'High-Mid', 'Treble'];

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

            const config = {
                responsive: true,
                displaylogo: false
            };

            const commonLayout = {
                xaxis: { title: 'Time (seconds)', gridcolor: '#eee' },
                yaxis: { title: 'Onset Strength', gridcolor: '#eee' },
                plot_bgcolor: '#fff',
                paper_bgcolor: '#fff',
                shapes: [{
                    type: 'line',
                    x0: 0,
                    x1: 0,
                    y0: 0,
                    y1: 1,
                    yref: 'paper',
                    line: { color: '#e67e22', width: 2, dash: 'dash' },
                    name: 'playhead'
                }, {
                    type: 'line',
                    x0: -15,
                    x1: -15,
                    y0: 0,
                    y1: 1,
                    yref: 'paper',
                    line: { color: '#9b59b6', width: 2, dash: 'dot' },
                    name: 'cleanup'
                }],
                dragmode: 'zoom',
                hovermode: 'closest',
                showlegend: true,
                legend: { x: 1, xanchor: 'right', y: 1 }
            };

            // Transient Graph (4 Bands Overlapping)
            const traces = [];
            for (let i = 0; i < 4; i++) {
                traces.push({
                    x: fileData.times,
                    y: fileData['onset_env_' + i],
                    mode: 'lines',
                    name: bandLabels[i],
                    line: { color: bandColors[i], width: 2 },
                    opacity: bandAlphas[i],
                    hoverinfo: 'x+y'
                });
                traces.push({
                    x: fileData['peaks_' + i].times,
                    y: fileData['peaks_' + i].values,
                    mode: 'markers',
                    name: bandLabels[i] + ' Peaks',
                    marker: { color: '#e74c3c', size: 6, symbol: 'cross', opacity: bandAlphas[i] },
                    hoverinfo: 'x+y',
                    showlegend: false
                });
            }

            const layoutTransient = JSON.parse(JSON.stringify(commonLayout));
            layoutTransient.title = { text: '4-Band Transient Analysis - ' + filename, font: { size: 18 } };
            Plotly.newPlot(transientGraphDiv, traces, layoutTransient, config);

            // Initialize Buffer Graph
            const traceBuffer = {
                x: bufferTimes,
                y: accumulatedBuffer,
                mode: 'lines',
                name: 'Accumulated Buffer',
                line: { color: '#f1c40f', width: 2 },
                fill: 'tozeroy'
            };

            const bufferLayout = {
                title: 'Accumulated 5s Historical Buffer (Real-time)',
                xaxis: { title: 'Time Relative to Peak (ms)' },
                yaxis: { title: 'Accumulated Energy', range: [0, 0.1], autorange: false },
                plot_bgcolor: '#fff',
                paper_bgcolor: '#fff',
                shapes: [{
                    type: 'line',
                    x0: -5000,
                    x1: 0,
                    y0: 0,
                    y1: 0,
                    line: { color: '#3498db', width: 1.5, dash: 'dash' },
                    name: 'average-line'
                }]
            };

            Plotly.newPlot(bufferDiv, [traceBuffer], bufferLayout, config);

            // Update SSM Visualization using Base64 Image
            const ssmExtent = fileData.ssm_extent;

            const ssmTrace = {
                x: ssmExtent,
                y: ssmExtent,
                mode: 'markers',
                marker: { opacity: 0 },
                showlegend: false,
                hoverinfo: 'none'
            };

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
            audioPlayer.src = filename;
            audioPlayer.load();
        }

        function resetState() {
            accumulatedBuffer.fill(0);
            peakHistory = [];
            processedPeaks = [new Set(), new Set(), new Set(), new Set()];
            cleanedPeaks = [new Set(), new Set(), new Set(), new Set()];
            peakSnapshots = [{}, {}, {}, {}];
            lastFrameIdx = -1;
        }

        fileSelect.addEventListener('change', (e) => {
            currentFile = e.target.value;
            resetState();
            updateGraph(currentFile);
            updateAudio(currentFile);
        });

        audioPlayer.addEventListener('timeupdate', () => {
            const currentTime = audioPlayer.currentTime;
            const fileData = data[currentFile];
            const frameIdx = Math.floor(currentTime * 1000); // 1ms resolution
            const cleanupFrameIdx = frameIdx - 15000; // 15 seconds behind

            if (Math.abs(frameIdx - lastFrameIdx) > 100) {
                resetState();
            }

            let bufferUpdated = false;

            for (let bandIdx = 0; bandIdx < 4; bandIdx++) {
                const peaks = fileData['peaks_' + bandIdx];
                const peakIndices = peaks.indices;
                const env = fileData['onset_env_' + bandIdx];

                for (let i = 0; i < peakIndices.length; i++) {
                    const peakIdx = peakIndices[i];
                    if (peakIdx <= frameIdx && peakIdx > cleanupFrameIdx && !processedPeaks[bandIdx].has(peakIdx)) {
                        processedPeaks[bandIdx].add(peakIdx);
                        const start = peakIdx - 5000;
                        const snapshot = new Array(5001).fill(0);
                        for (let j = 0; j <= 5000; j++) {
                            const envIdx = start + j;
                            if (envIdx >= 0 && envIdx < env.length) {
                                snapshot[j] = (env[envIdx] * (peaks.values[i] / fileData.max_peak_value));
                                accumulatedBuffer[j] += snapshot[j];
                            }
                        }
                        peakSnapshots[bandIdx][peakIdx] = snapshot;
                        activeFlashes.push({snapshot: snapshot, lifetime: 20});
                        bufferUpdated = true;
                    }
                    if (peakIdx <= cleanupFrameIdx && processedPeaks[bandIdx].has(peakIdx) && !cleanedPeaks[bandIdx].has(peakIdx)) {
                        const snapshot = peakSnapshots[bandIdx][peakIdx];
                        if (snapshot) {
                            for (let j = 0; j < 5001; j++) accumulatedBuffer[j] -= snapshot[j];
                            delete peakSnapshots[bandIdx][peakIdx];
                        }
                        cleanedPeaks[bandIdx].add(peakIdx);
                        bufferUpdated = true;
                    }
                }
            }

            if (bufferUpdated || activeFlashes.length > 0) {
                activeFlashes = activeFlashes.filter(f => f.lifetime > 0);
                activeFlashes.forEach(f => f.lifetime--);

                const traces = [
                    {
                        x: bufferTimes,
                        y: accumulatedBuffer,
                        mode: 'lines',
                        name: 'Accumulated Buffer',
                        line: { color: '#f1c40f', width: 2 },
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

                const shapes = [];
                const annotations = [];
                const maxVal = Math.max(...accumulatedBuffer);
                const detectedPeaks = [];
                if (maxVal > 0.01) {
                    const localMaxima = [];
                    for (let i = 1; i < accumulatedBuffer.length - 1; i++) {
                        if (accumulatedBuffer[i] > accumulatedBuffer[i-1] &&
                            accumulatedBuffer[i] > accumulatedBuffer[i+1] &&
                            accumulatedBuffer[i] > 0.01) {
                            localMaxima.push({idx: i, val: accumulatedBuffer[i]});
                        }
                    }
                    localMaxima.sort((a, b) => b.val - a.val);

                    for (let i = 0; i < localMaxima.length && detectedPeaks.length < 3; i++) {
                        const peak = localMaxima[i];
                        let tooClose = false;
                        for (const existing of detectedPeaks) {
                            if (Math.abs(existing.idx - peak.idx) < 200) {
                                tooClose = true;
                                break;
                            }
                        }
                        if (!tooClose) {
                            detectedPeaks.push(peak);
                        }
                    }

                    if (detectedPeaks.length > 0) {
                        peakHistory.push(bufferTimes[detectedPeaks[0].idx]);
                    }
                }

                const peakStyles = [
                    { color: '#f1c40f', width: 2, opacity: 1.0 },
                    { color: '#ecf0f1', width: 1.5, opacity: 0.9 },
                    { color: '#bdc3c7', width: 1, opacity: 0.8 }
                ];

                detectedPeaks.forEach((peak, i) => {
                    const msVal = bufferTimes[peak.idx];
                    const style = peakStyles[i] || peakStyles[2];
                    shapes.push({
                        type: 'line',
                        x0: msVal,
                        x1: msVal,
                        y0: 0,
                        y1: 1,
                        yref: 'paper',
                        line: { color: style.color, width: style.width, dash: 'dash', opacity: style.opacity }
                    });
                    annotations.push({
                        x: msVal,
                        y: 1 - (i * 0.05),
                        yref: 'paper',
                        text: msVal + 'ms',
                        showarrow: false,
                        font: { color: style.color, size: 10, weight: 'bold' },
                        bgcolor: 'rgba(0,0,0,0.5)',
                        yshift: -10
                    });
                });

                const dataToMeasure = accumulatedBuffer.slice(0, -100);
                const mean = dataToMeasure.reduce((a, b) => a + b, 0) / (dataToMeasure.length || 1);

                shapes.push({
                    type: 'line',
                    x0: -5000,
                    x1: 0,
                    y0: mean,
                    y1: mean,
                    line: { color: '#3498db', width: 1.5, dash: 'dash' },
                    name: 'average-line'
                });

                const stdDev = Math.sqrt(dataToMeasure.map(x => Math.pow(x - mean, 2)).reduce((a, b) => a + b, 0) / (dataToMeasure.length || 1));
                const contrast = Math.max(...dataToMeasure) / (mean || 1);

                const peakMean = peakHistory.reduce((a, b) => a + b, 0) / (peakHistory.length || 1);
                const peakStd = Math.sqrt(peakHistory.map(x => Math.pow(x - peakMean, 2)).reduce((a, b) => a + b, 0) / (peakHistory.length || 1));

                annotations.push({
                    xref: 'paper', yref: 'paper',
                    x: 0.02, y: 0.98,
                    text: `Std Dev: ${stdDev.toFixed(3)}<br>Contrast: ${contrast.toFixed(3)}<br>Peak Std: ${peakStd.toFixed(3)}`,
                    showarrow: false,
                    font: { color: '#f1c40f', size: 12, weight: 'bold' },
                    bgcolor: 'rgba(0,0,0,0.5)',
                    xanchor: 'left', yanchor: 'top'
                });

                const maxValExcludingZero = Math.max(...dataToMeasure);
                const bufferYMax = Math.max(0.1, maxValExcludingZero * 1.1);

                Plotly.react(bufferDiv, traces, {
                    title: 'Accumulated 5s Historical Buffer (Real-time)',
                    xaxis: { title: 'Time Relative to Peak (ms)' },
                    yaxis: { title: 'Accumulated Energy', range: [0, bufferYMax], autorange: false },
                    plot_bgcolor: '#333',
                    paper_bgcolor: '#fff',
                    shapes: shapes,
                    annotations: annotations
                });
            }

            lastFrameIdx = frameIdx;

            const layoutUpdates = {
                'xaxis.range': [currentTime - 20, currentTime + 5],
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime,
                'shapes[1].x0': currentTime - 15,
                'shapes[1].x1': currentTime - 15
            };
            Plotly.relayout(transientGraphDiv, layoutUpdates);

            Plotly.relayout(ssmDiv, {
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime,
                'shapes[1].y0': currentTime,
                'shapes[1].y1': currentTime
            });
        });

        if (currentFile) {
            updateGraph(currentFile);
            updateAudio(currentFile);
        }

    </script>
</body>
</html>
    """

    report_content = html_template.replace("DATA_PLACEHOLDER", json.dumps(all_data))

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(report_content)

    print(f"Report generated successfully: {os.path.abspath(output_path)}")

def analyze_audio(file_path):
    """
    Analyzes an audio file to extract its transient envelope (4-band analysis)
    and identify peaks. Generates a high-resolution SSM image (Base64).
    """
    print(f"Analyzing {file_path}...")
    try:
        # Load audio file. sr=None preserves original sampling rate.
        y, sr = librosa.load(file_path, sr=None, mono=True)

        # Resolution: 1ms chunks
        hop_length = int(sr * 0.001)

        # Compute Mel Spectrogram
        # n_mels=128
        S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128, hop_length=hop_length)
        S_db = librosa.power_to_db(S, ref=np.max)

        # Split into 4 bands (32 bins each)
        onset_envs = []
        peaks_list = []

        for i in range(4):
            S_band = S_db[i*32 : (i+1)*32, :]
            env = librosa.onset.onset_strength(S=S_band, sr=sr, hop_length=hop_length)
            onset_envs.append(env)

            peaks, _ = scipy.signal.find_peaks(env, prominence=0.5, distance=200)
            peaks_list.append(peaks)

        # Combined envelope for SSM calculation
        onset_env_combined = librosa.onset.onset_strength(S=S_db, sr=sr, hop_length=hop_length)

        times = librosa.frames_to_time(np.arange(len(onset_env_combined)), sr=sr, hop_length=hop_length)

        # Shared normalization factor (max across all bands)
        all_peak_vals = []
        for i in range(4):
            if len(peaks_list[i]) > 0:
                all_peak_vals.extend(onset_envs[i][peaks_list[i]])

        max_peak_value = float(np.max(all_peak_vals)) if len(all_peak_vals) > 0 else 1.0

        # Downsample for SSM calculation (100ms resolution)
        ssm_hop = 100
        onset_env_ssm = onset_env_combined[::ssm_hop]
        times_ssm = times[::ssm_hop]

        # Normalize onset_env for weighting
        max_onset = np.max(onset_env_ssm) if np.max(onset_env_ssm) > 0 else 1
        norm_onset = onset_env_ssm / max_onset

        # Calculate SSM at 100ms resolution
        dist_matrix = np.abs(onset_env_ssm[:, np.newaxis] - onset_env_ssm[np.newaxis, :])
        max_dist = np.max(dist_matrix) if np.max(dist_matrix) > 0 else 1
        ssm = 1 - (dist_matrix / max_dist)
        transience_weight = np.minimum(norm_onset[:, np.newaxis], norm_onset[np.newaxis, :])
        ssm = ssm * transience_weight

        # Find peak off-diagonal similarity for footnote
        ssm_off_diag = ssm.copy()
        np.fill_diagonal(ssm_off_diag, -1)
        peak_idx = np.unravel_index(np.argmax(ssm_off_diag), ssm_off_diag.shape)
        i, j = peak_idx

        peak_similarity_data = {
            "time_i": float(times_ssm[i]),
            "time_j": float(times_ssm[j]),
            "onset_i": float(onset_env_ssm[i]),
            "onset_j": float(onset_env_ssm[j]),
            "max_dist": float(max_dist),
            "max_onset": float(max_onset),
            "final_similarity": float(ssm[i, j])
        }

        # Render SSM to image
        fig = plt.figure(figsize=(10, 10))
        ax = fig.add_axes([0, 0, 1, 1])
        ax.imshow(ssm, cmap='viridis', origin='lower', aspect='auto', vmin=0, vmax=1)
        ax.axis('off')

        buf = io.BytesIO()
        fig.savefig(buf, format='png', dpi=150)
        plt.close(fig)
        buf.seek(0)
        ssm_base64 = base64.b64encode(buf.read()).decode('utf-8')

        result = {
            "filename": os.path.basename(file_path),
            "times": times.tolist(),
            "max_peak_value": max_peak_value,
            "ssm_image": ssm_base64,
            "ssm_extent": [float(times[0]), float(times[-1])],
            "peak_similarity": peak_similarity_data
        }

        # Add the 4 bands to the result
        for i in range(4):
            result[f"onset_env_{i}"] = onset_envs[i].tolist()
            result[f"peaks_{i}"] = {
                "times": times[peaks_list[i]].tolist(),
                "values": onset_envs[i][peaks_list[i]].tolist(),
                "indices": peaks_list[i].tolist()
            }

        return result
    except Exception as e:
        print(f"Error analyzing {file_path}: {e}")
        return None
