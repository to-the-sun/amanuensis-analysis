# Transient Analysis Module Report: `cumulative_transience` (Cython Extension)

## Overview
The `cumulative_transience` module serves as the high-performance core engine for audio transient analysis. It is implemented as a native Cython-based Python extension, wrapping optimized C code (`cumulative_transience.c`). This architecture separates heavy numerical analysis from high-level orchestration, ensuring that calculations are performant enough for real-time applications.

---

## Core Components

### 1. `TransientAnalyzer` Class
The stateful object for managing cumulative transient analysis.

#### **State Management**
- `accumulated_buffer`: A 5001-sample (5 seconds @ 1ms) buffer representing the sum of historical transient snapshots.
- `min_score_seen` / `max_score_seen`: Dynamic tracking of the resonance score range encountered.

#### **Methods**

##### `process_new_peaks(frame, peak_indices_list, onset_envs, all_valid_peak_indices, times)`
Processes detected peaks that fall within a 100ms window preceding the current `frame`.
- **Inputs**:
    - `frame` (int): Current playhead position in milliseconds.
    - `peak_indices_list` (list of lists): Pre-detected peak indices for each of the 4 spectral bands.
    - `onset_envs` (list of arrays): Raw onset strength envelopes for each band.
    - `all_valid_peak_indices` (set/list): Union of all peak indices across bands.
    - `times` (array): Time mapping for frames.
- **Outputs**: A list of dictionaries containing peak data, total resonance scores, individual qualifiers, and snapshots added to the buffer.

##### `update_metrics(frame)`
Performs buffer cleanup (removing snapshots older than 15 seconds) and calculates rhythm and scoring metrics.
- **Inputs**: `frame` (int): Current playhead position.
- **Outputs**: A dictionary containing `std_dev`, `mean`, `contrast`, `peak_std`, `rating`, `rolling_score`, and state flags.

---

### 2. Standalone Functions

#### `analyze_audio(y, sr)`
Performs full spectral decomposition, spectral flux calculation, and peak detection on raw audio using optimized C loops.
- **Inputs**:
    - `y` (numpy.ndarray): Mono audio data (float32).
    - `sr` (int): Sample rate.
- **Outputs**: A dictionary containing `times`, `onset_envs`, `rolling_thresholds`, `peaks_list`, and historical metric arrays.

---

## Architecture: Cython vs. C
The module is built from two primary source files:
1.  **`cumulative_transience.c`**: Contains the raw numerical algorithms (FFT, Mel-Filterbank, Spectral Flux, Peak Detection). This file is standalone and compatible with external C projects (like Max MSP).
2.  **`ct_extension.pyx`**: A Cython wrapper that provides the Pythonic interface, handling NumPy array memory management and dictionary creation for seamless integration with Python scripts.

---

## Input/Output Specification

### Expected Inputs
- **Audio Data**: 1D Numpy arrays (`float32`), mono.
- **Sample Rate**: Standard rates (e.g., 44.1kHz).
- **Resolution**: The analysis internally operates at a **1ms resolution** (temporal frames).

### Provided Outputs
- **Direct Import**: Unlike previous versions using `ctypes`, this module is imported directly as `import cumulative_transience`.
- **Structured Results**: Returns native Python types (lists, dictionaries, NumPy arrays) while maintaining C performance for the inner loops.
