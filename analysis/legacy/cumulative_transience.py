import librosa
import numpy as np
import scipy.signal

class TransientAnalyzer:
    def __init__(self, max_peak_value=1.0):
        self.buffer_len = 5001
        self.accumulated_buffer = np.zeros(self.buffer_len)
        self.buffer_times = np.linspace(-5000, 0, self.buffer_len)
        self.max_peak = max_peak_value
        
        self.min_score_seen = 0.0
        self.max_score_seen = 0.0
        self.all_generated_scores = []
        
        # rolling score calculation optimization
        self.upcoming_events = [] # List of (p_idx, type, score) where type is 'ADD' or 'REMOVE'
        self.current_window_scores = [] # Current scores in the 9ms window
        self.last_score_avg = 0.0
        
        self.peak_history = []
        self.peak_snapshots = [{} for _ in range(4)] # List of dictionaries (peak_idx -> snapshot) for each band
        self.processed_peaks = [set() for _ in range(4)]
        self.cleaned_peaks = [set() for _ in range(4)]

    def process_new_peaks(self, frame, peak_indices_list, onset_envs, all_valid_peak_indices, times):
        """
        Processes peaks for all 4 bands that fall within the current frame window.
        Returns a list of processed peak data.
        """
        new_peaks = []
        for band_idx in range(4):
            for p_idx in peak_indices_list[band_idx]:
                # frame is expected to be the current time index (ms)
                if p_idx > frame - 100 and p_idx <= frame and p_idx not in self.processed_peaks[band_idx]:
                    new_peaks.append((p_idx, band_idx))

        # Sort peaks chronologically
        new_peaks.sort()
        results = []

        for p_idx, band_idx in new_peaks:
            self.processed_peaks[band_idx].add(p_idx)
            
            start = p_idx - 5000
            end = p_idx
            if start < 0:
                window = onset_envs[band_idx][0 : end + 1]
                window = np.pad(window, (abs(start), 0), mode='constant')
            else:
                window = onset_envs[band_idx][start : end + 1]

            peak_results = {
                'p_idx': p_idx,
                'band_idx': band_idx,
                'time': times[p_idx],
                'peak_val': onset_envs[band_idx][p_idx],
                'total_score': 0,
                'qualifiers': [],
                'snapshot': None
            }

            if len(window) == self.buffer_len:
                peak_val = onset_envs[band_idx][p_idx]
                normalization = peak_val / self.max_peak if self.max_peak > 0 else 1.0
                snapshot = window * normalization
                peak_results['snapshot'] = snapshot

                # Calculate Resonance Score
                total_score = 0
                data_to_measure = self.accumulated_buffer[:-99]
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
                        val = self.accumulated_buffer[sp_idx]
                        qualifier = 0
                        if val > avg:
                            if max_v > avg:
                                qualifier = (val - avg) / (max_v - avg)
                        elif val < avg:
                            if avg > min_v:
                                qualifier = (val - avg) / (avg - min_v)

                        qualifier_sum += qualifier
                        found_peak = True
                        
                        peak_results['qualifiers'].append({
                            'ms': self.buffer_times[sp_idx],
                            'val': qualifier
                        })

                    if found_peak:
                        # Use the scalar of the primary original peak from the transient graph
                        scalar = peak_val
                        total_score = scalar * qualifier_sum

                # Update dynamic range
                self.min_score_seen = min(self.min_score_seen, total_score)
                self.max_score_seen = max(self.max_score_seen, total_score)
                self.all_generated_scores.append(total_score)
                
                peak_results['total_score'] = total_score
                
                # Add to event queue for optimized rolling average
                self.upcoming_events.append((p_idx, 'ADD', total_score))
                self.upcoming_events.append((p_idx + 10, 'REMOVE', total_score))
                
                self.accumulated_buffer[:] += snapshot
                self.peak_snapshots[band_idx][p_idx] = snapshot
            
            results.append(peak_results)
        
        return results

    def update_metrics(self, frame):
        """
        Handles cleanup of old snapshots and calculates current rhythm metrics.
        Returns a dictionary of metrics.
        """
        # Check for peak at cleanup sweep (15 seconds = 15000 frames @ 1ms)
        cleanup_frame_threshold = frame - 15000
        buffer_updated = False

        for band_idx in range(4):
            for p_idx in list(self.peak_snapshots[band_idx].keys()):
                if p_idx <= cleanup_frame_threshold and p_idx not in self.cleaned_peaks[band_idx]:
                    self.accumulated_buffer[:] -= self.peak_snapshots[band_idx][p_idx]
                    self.cleaned_peaks[band_idx].add(p_idx)
                    del self.peak_snapshots[band_idx][p_idx]
                    buffer_updated = True
        
        # Calculate Rhythm Metrics (ignoring last 99ms)
        data_to_measure = self.accumulated_buffer[:-99]
        metrics = {
            'std_dev': 0.0,
            'mean': 0.0,
            'contrast': 0.0,
            'peak_std': 0.0,
            'rating': np.mean(self.all_generated_scores) if self.all_generated_scores else 0.0,
            'buffer_updated': buffer_updated,
            'highest_peak_ms': None,
            'rolling_score': 0.0,
            'min_score_seen': self.min_score_seen,
            'max_score_seen': self.max_score_seen
        }

        if len(data_to_measure) > 0:
            metrics['std_dev'] = np.std(data_to_measure)
            metrics['mean'] = np.mean(data_to_measure)
            metrics['contrast'] = np.max(data_to_measure) / metrics['mean'] if metrics['mean'] > 0 else 0
            
            # Analyze peaks in the accumulated buffer (ignoring last 99ms)
            if np.max(data_to_measure) > 0.1:
                peaks_in_buf, props = scipy.signal.find_peaks(data_to_measure, height=metrics['mean'], distance=200)
                if len(peaks_in_buf) > 0:
                    peak_heights = props['peak_heights']
                    top_indices = np.argsort(peak_heights)[-1:][::-1]
                    highest_peak_idx = peaks_in_buf[top_indices[0]]
                    ms_val = float(self.buffer_times[highest_peak_idx])
                    metrics['highest_peak_ms'] = ms_val
                    self.peak_history.append(ms_val)
            
            metrics['peak_std'] = np.std(self.peak_history) if self.peak_history else 0.0
        
        # Optimized rolling score calculation using event queue
        # Sort and process all events up to the current frame
        self.upcoming_events.sort()
        while self.upcoming_events and self.upcoming_events[0][0] <= frame:
            evt_time, evt_type, score = self.upcoming_events.pop(0)
            if evt_type == 'ADD':
                self.current_window_scores.append(score)
            elif evt_type == 'REMOVE':
                if score in self.current_window_scores:
                    self.current_window_scores.remove(score)
            
            # Update average if window is non-empty; otherwise persist last avg
            if self.current_window_scores:
                self.last_score_avg = np.mean(self.current_window_scores)
        
        metrics['rolling_score'] = self.last_score_avg
        
        return metrics

def analyze_audio(y, sr):
    """
    Analyzes raw audio data to extract its transient envelope (4-band analysis)
    and identify peaks. Returns a dictionary with all analysis data.
    """
    # Resolution: 1ms chunks
    hop_length = int(sr * 0.001)

    # Compute Mel Spectrogram
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

    # Use first band for time calculation (all bands have same length)
    times = librosa.frames_to_time(np.arange(len(onset_envs[0])), sr=sr, hop_length=hop_length)

    # Shared normalization factor (max across all bands)
    all_peak_vals = []
    for i in range(4):
        if len(peaks_list[i]) > 0:
            all_peak_vals.extend(onset_envs[i][peaks_list[i]])

    max_peak_value = float(np.max(all_peak_vals)) if len(all_peak_vals) > 0 else 1.0

    return {
        "times": times,
        "max_peak_value": max_peak_value,
        "onset_envs": onset_envs,
        "rolling_thresholds": rolling_thresholds,
        "peaks_list": peaks_list
    }
