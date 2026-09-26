# Transient Analysis Module Report: `cumulative_transience` & `pattern_finder`

## Overview
The `analysis` module provides high-performance audio transient analysis, pitch tracking, and structural pattern recognition.

---

## Pattern Recognition (`analysis/pattern_finder.py`)

`pattern_finder.py` extracts frame-level MFCC, Delta-MFCC, and Chroma features to analyze audio similarity using Dynamic Time Warping (DTW).

### Core Architecture
- **Global Variables:**
  - `MIN_SEGMENT_LEN_MS = 100`: Minimum initial segment length in milliseconds.
  - `ATOM_ITERATION_MS = 50`: Increment added to segment length on each iteration.
  - `SIMILARITY_THRESHOLD = 0.75`: Normalized similarity threshold for DTW matches.
- **Workflow:**
  1. Audio feature matrix extraction at 16kHz resolution (10ms frame hop).
  2. Iterative segmentation from `MIN_SEGMENT_LEN_MS` increasing by `ATOM_ITERATION_MS`.
  3. Pairwise Dynamic Time Warping (DTW) cosine distance calculation between segment feature matrices.
  4. Identification and contiguous grouping of similar segments into patterns.
  5. Bar length selection based on maximum total duration of identified patterns.
  6. Exporting each identified pattern as an individual WAV file in the input audio folder.
  7. Real-time visualization via Tkinter/Matplotlib canvas (`analysis/pattern_gui.py`).

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
