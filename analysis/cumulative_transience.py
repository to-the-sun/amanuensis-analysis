import librosa
import numpy as np
import scipy.signal
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
    (bass and treble) and an accumulating 10-second buffer.
    Returns the path to the generated MP4 file.
    """
    print(f"Generating video for {audio_path}...")
    try:
        times = data['times']
        onset_env_bass = np.array(data['onset_env_bass'])
        onset_env_treble = np.array(data['onset_env_treble'])

        peak_indices_bass = set(data['peaks_bass']['indices'])
        peak_indices_treble = set(data['peaks_treble']['indices'])

        max_peak = data['max_peak_value']

        fig, (ax_bass, ax_treble, ax_buf) = plt.subplots(3, 1, figsize=(12, 14), gridspec_kw={'height_ratios': [1, 1, 1]})

        # Bass Plot
        ax_bass.plot(times, onset_env_bass, color='#3498db', lw=2, label='Bass Transient')
        ax_bass.scatter(data['peaks_bass']['times'], data['peaks_bass']['values'], color='#e74c3c', marker='x', s=50, label='Bass Peaks')
        playhead_bass = ax_bass.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_bass = ax_bass.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')
        ax_bass.set_title(f"Bass Transient Analysis - {os.path.basename(audio_path)}")
        ax_bass.set_ylabel("Onset Strength")
        ax_bass.legend()
        ax_bass.grid(True, alpha=0.3)
        ax_bass.set_xlim(-20, 5)
        ax_bass.set_ylim(0, max(onset_env_bass) * 1.1 if len(onset_env_bass) > 0 else 1)

        # Treble Plot
        ax_treble.plot(times, onset_env_treble, color='#2ecc71', lw=2, label='Treble Transient')
        ax_treble.scatter(data['peaks_treble']['times'], data['peaks_treble']['values'], color='#e74c3c', marker='x', s=50, label='Treble Peaks')
        playhead_treble = ax_treble.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_treble = ax_treble.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')
        ax_treble.set_title(f"Treble Transient Analysis")
        ax_treble.set_ylabel("Onset Strength")
        ax_treble.legend()
        ax_treble.grid(True, alpha=0.3)
        ax_treble.set_xlim(-20, 5)
        ax_treble.set_ylim(0, max(onset_env_treble) * 1.1 if len(onset_env_treble) > 0 else 1)

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

        # We need to track which band the peak came from to use the correct envelope for the snapshot
        # But wait, the snapshot is taken from the envelope at the time of the peak.
        # If a peak happens in Bass, we take a snapshot of onset_env_bass?
        # The prompt says: "add (and subtract at the cleanup sweep) them both into the accumulated historical buffer"
        # This implies that a peak in EITHER band triggers an accumulation of THAT band's transient profile.

        peak_snapshots_bass = {} # frame_idx -> snapshot
        peak_snapshots_treble = {} # frame_idx -> snapshot

        processed_peaks_bass = set()
        processed_peaks_treble = set()
        cleaned_peaks_bass = set()
        cleaned_peaks_treble = set()

        def update(frame):
            current_time = times[frame]
            ax_bass.set_xlim(current_time - 20, current_time + 5)
            ax_treble.set_xlim(current_time - 20, current_time + 5)

            # Update playheads and cleanup sweeps
            playhead_bass.set_xdata([current_time, current_time])
            playhead_treble.set_xdata([current_time, current_time])
            cleanup_time = current_time - 15
            cleanup_bass.set_xdata([cleanup_time, cleanup_time])
            cleanup_treble.set_xdata([cleanup_time, cleanup_time])

            # Process Bass Peaks
            for p_idx in peak_indices_bass:
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in processed_peaks_bass:
                    processed_peaks_bass.add(p_idx)
                    start = p_idx - 5000
                    end = p_idx
                    if start < 0:
                        window = onset_env_bass[0 : end + 1]
                        window = np.pad(window, (abs(start), 0), mode='constant')
                    else:
                        window = onset_env_bass[start : end + 1]

                    if len(window) == buffer_len:
                        peak_val = onset_env_bass[p_idx]
                        normalization = peak_val / max_peak if max_peak > 0 else 1.0
                        snapshot = window * normalization
                        accumulated_buffer[:] += snapshot
                        peak_snapshots_bass[p_idx] = snapshot
                        buffer_line.set_ydata(accumulated_buffer)
                        active_flashes.append([snapshot, 20])

            # Process Treble Peaks
            for p_idx in peak_indices_treble:
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in processed_peaks_treble:
                    processed_peaks_treble.add(p_idx)
                    start = p_idx - 5000
                    end = p_idx
                    if start < 0:
                        window = onset_env_treble[0 : end + 1]
                        window = np.pad(window, (abs(start), 0), mode='constant')
                    else:
                        window = onset_env_treble[start : end + 1]

                    if len(window) == buffer_len:
                        peak_val = onset_env_treble[p_idx]
                        normalization = peak_val / max_peak if max_peak > 0 else 1.0
                        snapshot = window * normalization
                        accumulated_buffer[:] += snapshot
                        peak_snapshots_treble[p_idx] = snapshot
                        buffer_line.set_ydata(accumulated_buffer)
                        active_flashes.append([snapshot, 20])

            # Dynamic Y-axis scaling for buffer (excluding peak at 0ms)
            # We ignore the last 100ms (-99ms to 0ms) to avoid scaling by the alignment peak.
            # This is an arbitrary value but works well in practice.
            current_max = np.max(accumulated_buffer[:-100]) if len(accumulated_buffer) > 100 else 0
            ax_buf.set_ylim(0, max(0.1, current_max * 1.1))

            # Check for peak at cleanup sweep (15 seconds = 15000 frames @ 1ms)
            cleanup_frame_threshold = frame - 15000

            # Cleanup Bass
            for p_idx in list(peak_snapshots_bass.keys()):
                if p_idx <= cleanup_frame_threshold and p_idx not in cleaned_peaks_bass:
                    accumulated_buffer[:] -= peak_snapshots_bass[p_idx]
                    cleaned_peaks_bass.add(p_idx)
                    del peak_snapshots_bass[p_idx]
                    buffer_line.set_ydata(accumulated_buffer)

            # Cleanup Treble
            for p_idx in list(peak_snapshots_treble.keys()):
                if p_idx <= cleanup_frame_threshold and p_idx not in cleaned_peaks_treble:
                    accumulated_buffer[:] -= peak_snapshots_treble[p_idx]
                    cleaned_peaks_treble.add(p_idx)
                    del peak_snapshots_treble[p_idx]
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

                        line = ax_buf.axvline(x=ms_val, color=style['color'], lw=style['lw'], alpha=style['alpha'], ls='--')
                        label = ax_buf.text(ms_val, ax_buf.get_ylim()[1]*0.9 - (i * 0.05 * ax_buf.get_ylim()[1]),
                                        f"{ms_val}ms", color=style['color'],
                                        fontsize=8, ha='center', fontweight='bold',
                                        bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=1))
                        peak_lines.append(line)
                        peak_labels.append(label)

            return [playhead_bass, playhead_treble, cleanup_bass, cleanup_treble, buffer_line] + flash_fill_artists + peak_lines + peak_labels

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

def analyze_audio(file_path):
    """
    Analyzes an audio file to extract its transient envelope (bass and treble bands)
    and identify peaks. Generates a high-resolution SSM image (Base64).
    """
    print(f"Analyzing {file_path}...")
    try:
        # Load audio file. sr=None preserves original sampling rate.
        y, sr = librosa.load(file_path, sr=None, mono=True)

        # Resolution: 1ms chunks
        hop_length = int(sr * 0.001)

        # Compute Mel Spectrogram
        # n_mels=128 provides enough resolution to split into two perceptible bands
        S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128, hop_length=hop_length)
        S_db = librosa.power_to_db(S, ref=np.max)

        # Split into Bass (0-63) and Treble (64-127) Mel bands
        S_bass = S_db[:64, :]
        S_treble = S_db[64:, :]

        # Calculate onset strength for each band
        onset_env_bass = librosa.onset.onset_strength(S=S_bass, sr=sr, hop_length=hop_length)
        onset_env_treble = librosa.onset.onset_strength(S=S_treble, sr=sr, hop_length=hop_length)

        # Combined envelope for SSM calculation
        onset_env_combined = librosa.onset.onset_strength(S=S_db, sr=sr, hop_length=hop_length)

        times = librosa.frames_to_time(np.arange(len(onset_env_combined)), sr=sr, hop_length=hop_length)

        # Find peaks in each band
        # distance=200 at 1ms resolution corresponds to 200ms
        peaks_bass, _ = scipy.signal.find_peaks(onset_env_bass, prominence=0.5, distance=200)
        peaks_treble, _ = scipy.signal.find_peaks(onset_env_treble, prominence=0.5, distance=200)

        # Shared normalization factor (max across both bands)
        all_peak_vals = np.concatenate([onset_env_bass[peaks_bass], onset_env_treble[peaks_treble]])
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

        return {
            "filename": os.path.basename(file_path),
            "times": times.tolist(),
            "onset_env_bass": onset_env_bass.tolist(),
            "onset_env_treble": onset_env_treble.tolist(),
            "peaks_bass": {
                "times": times[peaks_bass].tolist(),
                "values": onset_env_bass[peaks_bass].tolist(),
                "indices": peaks_bass.tolist()
            },
            "peaks_treble": {
                "times": times[peaks_treble].tolist(),
                "values": onset_env_treble[peaks_treble].tolist(),
                "indices": peaks_treble.tolist()
            },
            "max_peak_value": max_peak_value,
            "ssm_image": ssm_base64,
            "ssm_extent": [float(times[0]), float(times[-1])],
            "peak_similarity": peak_similarity_data
        }
    except Exception as e:
        print(f"Error analyzing {file_path}: {e}")
        return None
