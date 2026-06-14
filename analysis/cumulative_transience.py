import librosa
import numpy as np
import scipy.signal
import os
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
    Generates a video file for the analyzed audio showing a moving playhead over the transient graph
    and an accumulating 10-second buffer.
    Returns the path to the generated MP4 file.
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
        playhead = ax1.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_line = ax1.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')
        ax1.set_title(f"Transient Analysis - {os.path.basename(audio_path)}")
        ax1.set_ylabel("Onset Strength")
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_xlim(-20, 5)
        ax1.set_ylim(0, max(onset_env) * 1.1 if len(onset_env) > 0 else 1)

        # Bottom Plot: Accumulated 5s Historical Buffer
        buffer_len = 5001  # 5 seconds @ 1ms = 5000 steps + current
        accumulated_buffer = np.zeros(buffer_len)
        buffer_times = np.linspace(-5000, 0, buffer_len)
        buffer_line, = ax2.plot(buffer_times, accumulated_buffer, color='#2ecc71', lw=2)
        ax2.set_title("Accumulated 5s Historical Buffer")
        ax2.set_xlabel("Time Relative to Peak (ms)")
        ax2.set_ylabel("Accumulated Energy")
        ax2.grid(True, alpha=0.3)
        ax2.set_xlim(-5000, 0)
        ax2.set_ylim(0, 1)

        # Flash and fade storage
        active_flashes = [] # List of (snapshot, frames_remaining)
        flash_fill_artists = []
        peak_lines = []
        peak_labels = []
        peak_snapshots = {} # frame_idx -> snapshot

        processed_peaks = set()
        cleaned_peaks = set()

        def update(frame):
            current_time = times[frame]
            ax1.set_xlim(current_time - 20, current_time + 5)

            # Update playhead and cleanup sweep
            playhead.set_xdata([current_time, current_time])
            cleanup_time = current_time - 15
            cleanup_line.set_xdata([cleanup_time, cleanup_time])

            # Check for peaks passed since last frame (100ms chunks)
            # frame is the current millisecond index.
            # We look for peaks in [frame-99, frame]
            for p_idx in peak_indices:
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in processed_peaks:
                    processed_peaks.add(p_idx)
                    start = p_idx - 5000
                    end = p_idx

                    # Extract window with padding
                    if start < 0:
                        window = onset_env[0 : end + 1]
                        window = np.pad(window, (abs(start), 0), mode='constant')
                    else:
                        window = onset_env[start : end + 1]

                    if len(window) == buffer_len:
                        peak_val = onset_env[p_idx]
                        normalization = peak_val / max_peak if max_peak > 0 else 1.0
                        snapshot = window * normalization
                        accumulated_buffer[:] += snapshot
                        peak_snapshots[p_idx] = snapshot
                        buffer_line.set_ydata(accumulated_buffer)

                        # Add new flash: (data, initial_lifetime)
                        active_flashes.append([snapshot, 20])

            # Dynamic Y-axis scaling for buffer (excluding peak at 0ms)
            # We ignore the last 100ms (-99ms to 0ms) to avoid scaling by the alignment peak.
            # This is an arbitrary value but works well in practice.
            current_max = np.max(accumulated_buffer[:-100]) if len(accumulated_buffer) > 100 else 0
            ax2.set_ylim(0, max(0.1, current_max * 1.1))

            # Check for peak at cleanup sweep (15 seconds = 15000 frames @ 1ms)
            cleanup_frame_threshold = frame - 15000
            for p_idx in list(peak_snapshots.keys()):
                if p_idx <= cleanup_frame_threshold and p_idx not in cleaned_peaks:
                    accumulated_buffer[:] -= peak_snapshots[p_idx]
                    cleaned_peaks.add(p_idx)
                    del peak_snapshots[p_idx]
                    buffer_line.set_ydata(accumulated_buffer)

            # Handle Flash and Fade
            for artist in flash_fill_artists:
                artist.remove()
            flash_fill_artists.clear()

            for flash in active_flashes[:]:
                snapshot, lifetime = flash
                alpha = (lifetime / 20.0) * 0.5 # Max 0.5 alpha
                # Only plot flash if it has non-zero values
                if np.any(snapshot > 0):
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
                # Find the three largest peaks
                peaks_in_buf, props = scipy.signal.find_peaks(accumulated_buffer, height=0.01, distance=200)
                if len(peaks_in_buf) > 0:
                    # Sort by height and take top 3
                    peak_heights = props['peak_heights']
                    top_indices = np.argsort(peak_heights)[-3:][::-1]

                    peak_styles = [
                        {'color': '#f1c40f', 'lw': 2, 'alpha': 1.0}, # 1st: Gold
                        {'color': '#ecf0f1', 'lw': 1.5, 'alpha': 0.9}, # 2nd: Silver
                        {'color': '#bdc3c7', 'lw': 1, 'alpha': 0.8}, # 3rd: Bronze
                    ]

                    for i, idx in enumerate(top_indices):
                        p_idx = peaks_in_buf[idx]
                        ms_val = int(buffer_times[p_idx])
                        style = peak_styles[i] if i < len(peak_styles) else peak_styles[-1]

                        line = ax2.axvline(x=ms_val, color=style['color'], lw=style['lw'], alpha=style['alpha'], ls='--')
                        label = ax2.text(ms_val, ax2.get_ylim()[1]*0.9 - (i * 0.05 * ax2.get_ylim()[1]),
                                        f"{ms_val}ms", color=style['color'],
                                        fontsize=8, ha='center', fontweight='bold',
                                        bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=1))
                        peak_lines.append(line)
                        peak_labels.append(label)

            return [playhead, cleanup_line, buffer_line] + flash_fill_artists + peak_lines + peak_labels

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
        # Resolution: 1ms chunks
        hop_length = int(sr * 0.001)
        onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)
        times = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr, hop_length=hop_length)

        # Find peaks in the onset strength
        # prominence=0.5 and distance=200 are heuristic values for transient detection
        # distance=200 at 1ms resolution corresponds to 200ms
        peaks, _ = scipy.signal.find_peaks(onset_env, prominence=0.5, distance=200)

        peak_times = times[peaks].tolist()
        peak_values = onset_env[peaks].tolist()
        peak_indices = peaks.tolist()
        max_peak_value = float(np.max(peak_values)) if len(peak_values) > 0 else 1.0

        # Downsample onset_env for SSM calculation (100ms resolution)
        # This keeps the SSM calculation performant.
        ssm_hop = 100
        onset_env_ssm = onset_env[::ssm_hop]
        times_ssm = times[::ssm_hop]

        # Normalize onset_env for weighting
        max_onset = np.max(onset_env_ssm) if np.max(onset_env_ssm) > 0 else 1
        norm_onset = onset_env_ssm / max_onset

        # Calculate SSM at 100ms resolution
        # Compute pairwise distance matrix using broadcasting
        # SSM(i,j) = |onset_env[i] - onset_env[j]|
        dist_matrix = np.abs(onset_env_ssm[:, np.newaxis] - onset_env_ssm[np.newaxis, :])

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
            "time_i": float(times_ssm[i]),
            "time_j": float(times_ssm[j]),
            "onset_i": float(onset_env_ssm[i]),
            "onset_j": float(onset_env_ssm[j]),
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
