# Transient Analysis Module Report: `cumulative_transience` & `pattern_finder`

## Overview
The `analysis` module provides high-performance audio transient analysis, pitch tracking, and structural pattern recognition.

---

## Pattern Recognition (`analysis/pattern_finder.py`)

`pattern_finder.py` extracts frame-level MFCC, Delta-MFCC, and Chroma features to analyze audio similarity using Dynamic Time Warping (DTW).

### Core Architecture
- **Global Variables:**
  - `MIN_SEGMENT_LEN_MS = 100`: Minimum initial segment length in milliseconds.
  - `ATOM_ITERATION_MS = 50`: Atom of iteration used for rounding inter-transient time intervals.
  - `SIMILARITY_THRESHOLD = 0.75`: Normalized similarity threshold for DTW matches.
- **Workflow:**
  1. Full-file onset transient detection (`librosa.onset.onset_detect`).
  2. Pairwise time difference calculation between all detected transients.
  3. Rounding inter-transient durations to the nearest `ATOM_ITERATION_MS` (50ms) to produce a plausible set of candidate segment lengths.
  4. Dynamic Time Warping (DTW) feature comparison across contiguous audio segments for each candidate length.
  5. Contiguous grouping of matching segments into patterns.
  6. Bar length selection based on maximum total duration of identified patterns.
  7. Exporting each identified pattern as an individual WAV file in the input audio folder.
  8. Real-time visualization via Tkinter/Matplotlib canvas (`analysis/pattern_gui.py`) with update throttling (~20 FPS) for visual smoothness.

---

## Cython Extension: `cumulative_transience`

The `cumulative_transience` module serves as the high-performance core engine for audio transient analysis. It is implemented as a native Cython-based Python extension, wrapping optimized C code (`cumulative_transience.c`).

### Core Components

#### 1. `TransientAnalyzer` Class
The stateful object for managing cumulative transient analysis.

##### **State Management**
- `accumulated_buffer`: A 5001-sample (5 seconds @ 1ms) buffer representing the sum of historical transient snapshots.
- `min_score_seen` / `max_score_seen`: Dynamic tracking of the resonance score range encountered.

##### **Methods**
- `process_new_peaks(...)`: Processes detected peaks that fall within a 100ms window preceding the current frame.
- `update_metrics(...)`: Performs buffer cleanup (removing snapshots older than 15 seconds) and calculates rhythm and scoring metrics.

#### 2. Standalone Functions
- `analyze_audio(y, sr)`: Performs full spectral decomposition, spectral flux calculation, and peak detection on raw audio using optimized C loops.
