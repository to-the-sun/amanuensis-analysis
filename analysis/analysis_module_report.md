# Transient Analysis Module Report: `cumulative_transience.py`

## Overview
The `cumulative_transience.py` module serves as the core engine for audio transient analysis. It has been refactored to separate analysis logic from file management and visualization concerns. The module is designed to be stateful, allowing it to process audio data in chunks and maintain a historical buffer of transient events, making it suitable for both file-based batch processing and potential real-time audio streams.

---

## Core Components

### 1. `TransientAnalyzer` Class
The primary stateful object for managing cumulative transient analysis.

#### **State Management**
- `accumulated_buffer`: A 5001-sample (5 seconds @ 1ms) buffer representing the sum of historical transient snapshots.
- `peak_snapshots`: A collection of active snapshots being tracked, indexed by their trigger frame.
- `score_history`: A temporal record of all resonance scores generated.
- `peak_history`: A record of the temporal positions (ms) of the highest peaks in the accumulated buffer (used for rhythm stability metrics).
- `min_score_seen` / `max_score_seen`: Dynamic tracking of the resonance score range.

#### **Methods**

##### `process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)`
Processes detected peaks that fall within a 100ms window preceding the current `frame`.
- **Inputs**:
    - `frame` (int): Current playhead position in milliseconds.
    - `peak_indices_list` (list of lists): Pre-detected peak indices for each of the 4 spectral bands.
    - `onset_envs` (list of arrays): Raw onset strength envelopes for each band.
    - `all_valid_peak_indices` (set): Union of all peak indices across bands.
    - `times` (array): Time mapping for frames.
- **Outputs**: A list of dictionaries containing:
    - `p_idx`: The index of the peak.
    - `total_score`: The calculated resonance score.
    - `qualifiers`: Individual alignment scores for historical peaks in the buffer.
    - `snapshot`: The normalized 5001-sample window added to the buffer.

##### `update_metrics(frame)`
Performs buffer cleanup and calculates real-time rhythm and scoring metrics.
- **Inputs**: `frame` (int): Current playhead position.
- **Outputs**: A dictionary containing:
    - `std_dev`: Standard deviation of the historical buffer.
    - `contrast`: Max-to-mean ratio of the buffer energy.
    - `peak_std`: Stability of peak positions (Bar Length Deviation).
    - `rating`: Running average of all generated resonance scores.
    - `rolling_score`: 9ms rolling average of recent scores.
    - `buffer_updated` (bool): Whether a cleanup occurred.

---

### 2. Standalone Functions

#### `analyze_audio(y, sr)`
Performs spectral decomposition and peak detection on raw audio.
- **Inputs**:
    - `y` (numpy.ndarray): Mono audio data (float32).
    - `sr` (int): Sample rate.
- **Outputs**: A dictionary containing `times`, `onset_envs` (4 bands), `rolling_thresholds`, and `peaks_list`.

---

## Input/Output Specification

### Expected Inputs
- **Audio Data**: 1D Numpy arrays (`float32`), normalized typically to `[-1.0, 1.0]`.
- **Sample Rate**: Standard rates (e.g., 44.1kHz, 48kHz).
- **Resolution**: The analysis internally operates at a **1ms resolution** (temporal frames).

### Provided Outputs
- **Structured Analysis**: Envelopes and peak indices for Sub-Bass, Bass/Low-Mid, High-Mid, and Treble.
- **Resonance Scores**: Quantitative measures of how well a new peak aligns with the historical energy in the buffer.
- **Rhythmic Stats**: Real-time metrics reflecting the "tightness" and "contrast" of the accumulated transients.

---

## Contexts for Future Use

1. **Real-Time Audio Streams**:
   By feeding 100ms chunks of audio into `analyze_audio` and passing the results to `TransientAnalyzer.process_new_peaks` sequentially, this module can power live dashboards for DJs or performers. This is demonstrated in the `play_files.py` script.

2. **Alternative Frontends**:
   Since the module no longer depends on Matplotlib for analysis, it could be used as a backend for:
   - A React/Web-based visualization using WebGL or D3.js.
   - A VST plugin interface that displays rhythmic stability in real-time within a DAW.
   - A command-line tool that outputs JSON streams of metrics for automated mix grading.

3. **Machine Learning Pre-processing**:
   The `accumulated_buffer` provides a "rhythmic fingerprint" of a section of audio. This could be used as an input feature for classifying genres or detecting rhythmic shifts in a song.
