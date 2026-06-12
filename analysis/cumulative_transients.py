import librosa
import numpy as np
import scipy.signal
import os
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
        cleanup_line = ax1.axvline(x=-20, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')
        ax1.set_title(f"Transient Analysis - {os.path.basename(audio_path)}")
        ax1.set_ylabel("Onset Strength")
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_xlim(-20, 5)
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
        peak_snapshots = {} # frame_idx -> snapshot

        def update(frame):
            current_time = times[frame]
            ax1.set_xlim(current_time - 20, current_time + 5)

            # Update playhead and cleanup sweep
            playhead.set_xdata([current_time, current_time])
            cleanup_time = current_time - 20
            cleanup_line.set_xdata([cleanup_time, cleanup_time])

            # Check for peak at playhead
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
                    peak_snapshots[frame] = snapshot
                    buffer_line.set_ydata(accumulated_buffer)

                    # Add new flash: (data, initial_lifetime)
                    active_flashes.append([snapshot, 20])

                    # Dynamic Y-axis scaling for buffer
                    current_max = np.max(accumulated_buffer)
                    if current_max > ax2.get_ylim()[1]:
                        ax2.set_ylim(0, current_max * 1.1)

            # Check for peak at cleanup sweep (20 seconds = 200 frames @ 10fps)
            cleanup_frame = frame - 200
            if cleanup_frame in peak_snapshots:
                accumulated_buffer[:] -= peak_snapshots[cleanup_frame]
                del peak_snapshots[cleanup_frame]
                buffer_line.set_ydata(accumulated_buffer)

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

            return [playhead, cleanup_line, buffer_line] + flash_fill_artists + peak_lines + peak_labels

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
