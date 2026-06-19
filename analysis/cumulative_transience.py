import argparse
import librosa
import numpy as np
import scipy.signal
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.animation as animation
import matplotlib.ticker as ticker
from tqdm import tqdm
import io
import base64
import subprocess
import tempfile
import shutil

def get_score_color(score, min_score, max_score):
    """
    Returns a hex color string based on the resonance score relative to min/max seen.
    score == min_score (negative): bright red (#ff0000)
    score == 0: subdued gray (#808080)
    score == max_score (positive): bright green (#00ff00)
    Interpolates linearly in between, anchoring zero as gray.
    """
    if score == 0:
        return "#808080"
    
    if score < 0:
        # Interpolate between Red (#ff0000) and Gray (#808080)
        # t=1 at min_score, t=0 at 0
        t = score / min_score if min_score < 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0xff - 0x80) * t)
        g = int(0x80 + (0x00 - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)
    else:
        # Interpolate between Gray (#808080) and Green (#00ff00)
        # t=0 at 0, t=1 at max_score
        t = score / max_score if max_score > 0 else 0.0
        t = max(0, min(1, t))
        r = int(0x80 + (0x00 - 0x80) * t)
        g = int(0x80 + (0xff - 0x80) * t)
        b = int(0x80 + (0x00 - 0x80) * t)
        
    return f"#{r:02x}{g:02x}{b:02x}"

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
        rolling_thresholds = [np.array(data[f'rolling_threshold_{i}']) for i in range(4)]
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
        threshold_lines = []
        for i in range(4):
            line, = ax_transient.plot(times, onset_envs[i], color=colors[i], lw=2, alpha=alphas[i], label=labels[i])
            transient_lines.append(line)
            # Add horizontal line for rolling threshold
            t_line, = ax_transient.plot([times[0], times[-1]], [0, 0], color=colors[i], lw=1, ls='--', alpha=0.5)
            threshold_lines.append(t_line)
            ax_transient.scatter(peak_times_list[i], peak_values_list[i], color='#e74c3c', marker='x', s=30, alpha=alphas[i])

        playhead_transient = ax_transient.axvline(x=0, color='#e67e22', lw=2, ls='--', label='Playhead')
        cleanup_transient = ax_transient.axvline(x=-15, color='#9b59b6', lw=2, ls=':', label='Cleanup Sweep')

        ax_transient.set_title(f"4-Band Transient Analysis - {os.path.basename(audio_path)}")
        ax_transient.set_ylabel("Onset Strength")
        ax_transient.legend(loc='upper right')
        ax_transient.grid(True, alpha=0.3)
        ax_transient.set_xlim(-20, 5)

        def format_time(x, pos):
            m = int(abs(x) // 60)
            s = int(abs(x) % 60)
            prefix = "-" if x < 0 else ""
            return f"{prefix}{m}:{s:02d}"
        ax_transient.xaxis.set_major_formatter(ticker.FuncFormatter(format_time))

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
        active_scores = [] # List of [text_artist, lifetime, initial_y]
        active_qualifiers = [] # List of [line, label, lifetime, val]
        all_generated_scores = []
        min_score_seen = 0.0
        max_score_seen = 0.0

        # Rating text in the corner of the first graph (ax_transient)
        rating_text = ax_transient.text(0.02, 0.98, 'Rating: 0.00', transform=ax_transient.transAxes,
                                        verticalalignment='top', fontsize=12, color='#f1c40f',
                                        fontweight='bold', bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=2))

        # Rhythm Metrics text initialization
        metrics_text = ax_buf.text(0.02, 0.95, '', transform=ax_buf.transAxes,
                                   verticalalignment='top', fontsize=10, color='#f1c40f',
                                   fontweight='bold', bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=2))

        peak_history = []

        # Track snapshots for cleanup
        peak_snapshots = [{} for _ in range(4)] # List of dictionaries (peak_idx -> snapshot) for each band
        processed_peaks = [set() for _ in range(4)]
        cleaned_peaks = [set() for _ in range(4)]
        all_valid_peak_indices = set().union(*peak_indices_list)

        def update(frame):
            nonlocal min_score_seen, max_score_seen
            current_time = times[frame]
            ax_transient.set_xlim(current_time - 20, current_time + 5)

            # Update threshold lines to current rolling average at playhead
            for i in range(4):
                threshold_lines[i].set_ydata([rolling_thresholds[i][frame], rolling_thresholds[i][frame]])
                threshold_lines[i].set_xdata([current_time - 20, current_time + 5])

            # Update playheads and cleanup sweeps
            playhead_transient.set_xdata([current_time, current_time])
            cleanup_time = current_time - 15
            cleanup_transient.set_xdata([cleanup_time, cleanup_time])

            buffer_updated = False

            # Process Peaks for all 4 bands
            new_peaks = []
            for band_idx in range(4):
                for p_idx in peak_indices_list[band_idx]:
                    if p_idx > frame - 100 and p_idx <= frame and p_idx not in processed_peaks[band_idx]:
                        new_peaks.append((p_idx, band_idx))

            # Sort peaks chronologically to clear markers and flip to newest
            new_peaks.sort()

            for p_idx, band_idx in new_peaks:
                processed_peaks[band_idx].add(p_idx)

                # Clear existing qualifiers when a new peak is processed
                for q in active_qualifiers:
                    q[0].remove()
                    q[1].remove()
                active_qualifiers.clear()

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

                    # Calculate Resonance Score
                    total_score = 0
                    data_to_measure = accumulated_buffer[:-99]
                    if len(data_to_measure) > 0:
                        avg = np.mean(data_to_measure)
                        max_v = np.max(data_to_measure)
                        min_v = np.min(data_to_measure)

                        qualifier_sum = 0.0
                        found_peak = False

                        # Identify secondary peaks in the 5s window preceding p_idx (ignoring last 99ms)
                        secondary_indices = [idx for idx in all_valid_peak_indices if p_idx - 5000 <= idx <= p_idx - 99]

                        for s_idx in secondary_indices:
                            sp_idx = 5000 - (p_idx - s_idx)
                            val = accumulated_buffer[sp_idx]
                            qualifier = 0
                            if val > avg:
                                if max_v > avg:
                                    qualifier = (val - avg) / (max_v - avg)
                            elif val < avg:
                                if avg > min_v:
                                    qualifier = (val - avg) / (avg - min_v)

                            qualifier_sum += qualifier
                            found_peak = True

                            # Create individual qualifier markers on ax_buf
                            q_ms = buffer_times[sp_idx]
                            # Use fixed range -1 to 1 for qualifier colors
                            q_color = get_score_color(qualifier, -1.0, 1.0)
                            q_line = ax_buf.axvline(x=q_ms, color=q_color, lw=3.0, ls=':', alpha=0.8)
                            q_label = ax_buf.text(q_ms, accumulated_buffer[sp_idx], f"{qualifier:+.2f}",
                                                  color=q_color, fontsize=8, ha='center', va='bottom')
                            active_qualifiers.append([q_line, q_label, 20, qualifier])

                        if found_peak:
                            # Use the scalar of the primary original peak from the transient graph
                            scalar = peak_val
                            total_score = scalar * qualifier_sum

                    # Update dynamic range
                    min_score_seen = min(min_score_seen, total_score)
                    max_score_seen = max(max_score_seen, total_score)

                    all_generated_scores.append(total_score)
                    avg_rating = np.mean(all_generated_scores)
                    rating_text.set_text(f"Rating: {avg_rating:.2f}")

                    # Create score animation
                    score_text = ax_transient.text(times[p_idx], peak_val, f"{total_score:+.2f}",
                                                color=get_score_color(total_score, min_score_seen, max_score_seen),
                                                fontsize=10, fontweight='bold',
                                                ha='center', va='bottom')
                    active_scores.append([score_text, 20, peak_val, total_score])

                    accumulated_buffer[:] += snapshot
                    peak_snapshots[band_idx][p_idx] = snapshot
                    buffer_updated = True
                    active_flashes.append([snapshot, 20])

            # Dynamic Y-axis scaling for buffer (ignoring last 99ms)
            current_max = np.max(accumulated_buffer[:-99]) if len(accumulated_buffer) > 99 else 0
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

            # Calculate Rhythm Metrics (ignoring last 99ms)
            data_to_measure = accumulated_buffer[:-99]
            if len(data_to_measure) > 0:
                std_dev = np.std(data_to_measure)
                mean_metrics = np.mean(data_to_measure)
                contrast = np.max(data_to_measure) / mean_metrics if mean_metrics > 0 else 0
                peak_std = np.std(peak_history) if peak_history else 0.0
                metrics_text.set_text(f"Std Dev: {std_dev:.3f}\nContrast: {contrast:.3f}\nPeak Std: {peak_std:.3f}")
            else:
                metrics_text.set_text("Std Dev: 0.000\nContrast: 0.000\nPeak Std: 0.000")

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

            # Handle Score Animations
            current_ylim = ax_transient.get_ylim()[1]
            for score in active_scores[:]:
                txt, lifetime, initial_y, val = score
                lifetime -= 1
                if lifetime <= 0:
                    txt.remove()
                    active_scores.remove(score)
                else:
                    score[1] = lifetime
                    # Float upward
                    progress = (20 - lifetime) / 20.0
                    new_y = initial_y + (progress * 0.1 * current_ylim)
                    txt.set_position((txt.get_position()[0], new_y))
                    txt.set_alpha(lifetime / 20.0)
                    # Update color based on current known range
                    txt.set_color(get_score_color(val, min_score_seen, max_score_seen))

            # Analyze peaks in the accumulated buffer
            for line in peak_lines:
                line.remove()
            for label in peak_labels:
                label.remove()
            peak_lines.clear()
            peak_labels.clear()

            # Analyze peaks in the accumulated buffer (ignoring last 99ms)
            data_to_measure = accumulated_buffer[:-99]
            if np.max(data_to_measure) > 0.1:
                peaks_in_buf, props = scipy.signal.find_peaks(data_to_measure, height=np.mean(data_to_measure), distance=200)
                if len(peaks_in_buf) > 0:
                    peak_heights = props['peak_heights']
                    top_indices = np.argsort(peak_heights)[-1:][::-1] # Just keep the highest peak

                    # Track highest peak's X-value stability (ms)
                    highest_peak_idx = peaks_in_buf[top_indices[0]]
                    peak_history.append(float(buffer_times[highest_peak_idx]))

                    peak_styles = [
                        {'color': '#f1c40f', 'lw': 2, 'alpha': 1.0}, # 1st: Gold
                    ]

                    for i, idx in enumerate(top_indices):
                        p_idx = peaks_in_buf[idx]
                        ms_val = int(buffer_times[p_idx])
                        style = peak_styles[i] if i < len(peak_styles) else peak_styles[-1]

                        line = ax_buf.axvline(x=ms_val, color=style['color'], lw=style['lw'], alpha=style['alpha'], ls='--')
                        label = ax_buf.text(ms_val, ax_buf.get_ylim()[1]*0.9,
                                        f"{ms_val}ms", color=style['color'],
                                        fontsize=8, ha='center', fontweight='bold',
                                        bbox=dict(facecolor='black', alpha=0.5, edgecolor='none', pad=1))
                        peak_lines.append(line)
                        peak_labels.append(label)

            # Handle Qualifier Animations
            for q in active_qualifiers[:]:
                q_line, q_label, lifetime, val = q
                lifetime -= 1
                if lifetime <= 0:
                    q_line.remove()
                    q_label.remove()
                    active_qualifiers.remove(q)
                else:
                    q[2] = lifetime
                    alpha = lifetime / 20.0
                    q_line.set_alpha(alpha * 0.8)
                    q_label.set_alpha(alpha)

            score_artists = [s[0] for s in active_scores]
            qualifier_artists = []
            for q in active_qualifiers:
                qualifier_artists.append(q[0])
                qualifier_artists.append(q[1])

            return [playhead_transient, cleanup_transient, buffer_line, metrics_text, rating_text] + threshold_lines + flash_fill_artists + peak_lines + peak_labels + score_artists + qualifier_artists

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
        rolling_thresholds = []

        window_size = 15000 # 15 seconds at 1ms resolution

        for i in range(4):
            S_band = S_db[i*32 : (i+1)*32, :]
            env = librosa.onset.onset_strength(S=S_band, sr=sr, hop_length=hop_length)
            onset_envs.append(env)

            # Calculate 15-second rolling average for thresholding
            cumsum = np.cumsum(env)
            rolling_avg = np.zeros_like(env)
            # Expanding window for the first window_size samples
            actual_window = min(window_size, len(env))
            rolling_avg[:actual_window] = cumsum[:actual_window] / np.arange(1, actual_window + 1)
            # Rolling window for the rest
            if len(env) > window_size:
                rolling_avg[window_size:] = (cumsum[window_size:] - cumsum[:-window_size]) / window_size
            rolling_thresholds.append(rolling_avg)

            # Detect peaks that are above the rolling average threshold
            peaks, _ = scipy.signal.find_peaks(env, prominence=0.5, distance=200, height=rolling_avg)
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
            result[f"rolling_threshold_{i}"] = rolling_thresholds[i].tolist()
            result[f"peaks_{i}"] = {
                "times": times[peaks_list[i]].tolist(),
                "values": onset_envs[i][peaks_list[i]].tolist(),
                "indices": peaks_list[i].tolist()
            }

        return result
    except Exception as e:
        print(f"Error analyzing {file_path}: {e}")
        return None

def main():
    parser = argparse.ArgumentParser(description="Standalone transient analysis and video generation.")
    parser.add_argument("files", nargs="*", help="Optional list of audio files to process.")
    args = parser.parse_args()

    audio_files = []
    if args.files:
        audio_files = args.files
    else:
        # Scan current directory for audio files
        extensions = ('.wav', '.mp3', '.m4a', '.flac', '.ogg', '.aiff')
        # Use absolute path to ensure files are found correctly if script is run from elsewhere
        current_dir = os.path.dirname(os.path.abspath(__file__))
        audio_files = [os.path.join(current_dir, f) for f in os.listdir(current_dir) if f.lower().endswith(extensions)]
        audio_files.sort()

    if not audio_files:
        print("No audio files found to process.")
        return

    for f in audio_files:
        if not os.path.exists(f):
            print(f"File not found: {f}")
            continue
        result = analyze_audio(f)
        if result:
            generate_video(f, result)

if __name__ == "__main__":
    main()
