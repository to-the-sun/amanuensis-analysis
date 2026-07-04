# Peak Detection Process Report

This report provides a detailed technical overview of the peak detection and transient analysis pipeline implemented in the unified C-based engine and its Cython integration.

## 1. Overview
The peak detection system is designed to identify significant rhythmic and percussive events in audio signals across four frequency bands. It employs a multi-stage pipeline: spectral analysis, envelope extraction, adaptive thresholding, and a greedy peak selection algorithm refined by prominence and temporal distance constraints.

## 2. Signal Pre-processing
The engine performs high-resolution spectral analysis to convert raw time-domain audio into a perceptually relevant representation.

*   **Short-Time Fourier Transform (STFT)**:
    *   **Frame Size (`n_fft`)**: 2048 samples.
    *   **Hop Length**: 1ms (e.g., 44 samples at 44.1 kHz), providing high temporal resolution.
    *   **Windowing**: Hann window.
    *   **Centering**: The signal is padded at both ends to ensure that the $t$-th frame is centered at sample $t \times \text{hop\_length}$.
*   **Mel Filterbank**:
    *   The power spectrogram is mapped to the **Slaney Mel scale** with 128 bands.
*   **Logarithmic Scaling**:
    *   Values are converted to decibels: $10 \times \log_{10}(\text{power})$.
    *   The dynamic range is clipped to a `top_db` of 80.0 dB relative to the maximum spectral energy.

## 3. Envelope Generation (Spectral Flux)
The 128 Mel bands are divided into four equal segments (32 bins each) to capture transients in specific frequency ranges:
1.  **Sub-Bass**: 0–31 bins
2.  **Bass/Low-Mid**: 32–63 bins
3.  **High-Mid**: 64–95 bins
4.  **Treble**: 96–127 bins

For each band, a **Spectral Flux** envelope is calculated by summing the positive-only differences (half-wave rectification) between successive frames across its 32 Mel bins, then normalized by the bin count.

### Temporal Alignment
To maintain parity with Librosa's `onset_strength`, the envelope is shifted by a constant offset of $n\_fft // (2 \times \text{hop\_length})$ frames to compensate for STFT centering.

## 4. Adaptive Thresholding
To account for varying loudness and sustained energy, the system applies a dynamic rolling threshold:
*   **Window**: 15 seconds (15,000 frames).
*   **Logic**: Each frame's threshold is the mean of the envelope values within the preceding 15-second window.

## 5. Peak Selection Logic
The engine identifies peaks using a multi-pass approach within the C function `analyzer_analyze_audio`:

1.  **Local Maxima**: A frame $f$ is a candidate if $\text{env}[f] > \text{env}[f-1]$ and $\text{env}[f] > \text{env}[f+1]$.
2.  **Adaptive Filtering**: The candidate must exceed the 15-second rolling threshold at that frame.
3.  **Greedy Distance Constraint**:
    *   A minimum distance of **200ms** is enforced.
    *   If a new candidate is found within 200ms of a previously accepted peak, the system performs a "greedy replacement": it keeps only the candidate with the higher magnitude.
4.  **Topological Prominence**:
    *   The peak must have a prominence of at least **0.5**.
    *   Prominence is calculated as the difference between the peak height and the higher of the two lowest valleys on either side before encountering a higher peak.

## 6. Resonance Scoring (TransientAnalyzer)
Once peaks are identified, they are processed by the `TransientAnalyzer` to calculate resonance scores, which measure the "rhythmic consistency" of a peak relative to recent history.

### Snapshot Accumulation
*   For every peak at index $p\_idx$, a **5-second snapshot** (5001ms window: $[-5000, 0]$) is extracted.
*   The snapshot is normalized by the global maximum peak value seen in the entire file.
*   The snapshot is added to a global `accumulated_buffer`.

### Qualifier Calculation
For a new peak at $p\_idx$, the analyzer identifies all previously validated peaks within a 5-second window prior (excluding the final 99ms to avoid self-feedback).
*   For each historical peak at index $s\_idx$ (where $p\_idx - 5000 \le s\_idx \le p\_idx - 99$):
    *   The value at that relative offset in the `accumulated_buffer` is compared to the buffer's average, minimum, and maximum.
    *   **Formula**:
        *   If $\text{val} > \text{avg}$: $\text{qualifier} = \frac{\text{val} - \text{avg}}{\text{max\_v} - \text{avg}}$
        *   If $\text{val} < \text{avg}$: $\text{qualifier} = \frac{\text{val} - \text{avg}}{\text{avg} - \text{min\_v}}$
*   **Total Score**: The peak's own magnitude multiplied by the sum of these qualifiers.

## 7. Metrics and Cleanup
*   **Cleanup**: Snapshots are removed from the `accumulated_buffer` 15 seconds after they occur to keep the analysis relevant to the current musical context.
*   **Rolling Score**: A 9ms rolling average of recent resonance scores.
*   **Peak Std (Bar Length Deviation)**: Standard deviation of the temporal position of the highest peak within the 5-second accumulated buffer, used to measure rhythmic stability.

## 8. Implementation Details
*   **C Analysis Engine**: High-performance core (`cumulative_transience.c`) handles FFT, Mel filtering, and the iterative peak selection.
*   **Cython Extension**: The `ct_extension.pyx` module provides a native Python interface, allowing `analyze_files.py` and `play_files.py` to access the C structures and functions with minimal overhead.
