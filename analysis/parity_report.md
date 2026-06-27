# Audio Analysis Parity Report: C vs. Python

This document serves as the primary technical log and roadmap for achieving **100% numerical parity** between the C-based `cumulative_transience.c` analysis engine and the reference Python implementation (`analysis/legacy/cumulative_transience.py`).

## Mission Objective: Absolute Parity
The goal of this project is **absolute 100% verification-verified parity**. Every aspect of the C implementation must produce outputs that are mathematically identical to the Python reference implementation across all dimensions (envelopes, peaks, scores, ratings).

## Current Status (June 2026)
We are prioritizing the largest algorithmic dissimilarities first. Recent exploration has identified a critical temporal alignment mismatch that explains the previously low correlation scores.

### Algorithmic Comparison & Alignment Status

| Component | Python Reference | C Implementation Status | Action Taken / Planned |
| :--- | :--- | :--- | :--- |
| **STFT Framing** | Librosa `center=True`, Reflect padding | **In Progress** | Aligning reflect padding and frame count logic. |
| **Windowing** | Hann, `fftbins=True` (N divisor) | **Aligned** | divisor is `n_fft`, matching Librosa. |
| **Mel Scale** | Slaney scale | **In Progress** | Implementing Slaney Hz-to-Mel formula. |
| **Mel Normalization** | Area-normalization ('slaney') | **In Progress** | Applying `2.0 / (f_high - f_low)` weights. |
| **Onset Alignment** | `onset_strength(S=S)` (-5ms shift) | **Action Required** | **Emulating** the -5ms shift found in Librosa. |
| **Spectral Flux** | `np.mean` across 32 bins | **Aligned** | Dividing sum of positive diffs by 32.0. |
| **Peak Detection** | `scipy.signal.find_peaks` | **In Progress** | Refining prominence and distance logic. |
| **Resonance Score** | Scalar * Qualifier Sum | **In Progress** | Synchronizing buffer update order. |

### Evaluation of Emulation Strategies
- **Temporal Alignment**:
    - **Original C State**: Applied a +23ms "correction" shift (`n_fft // (2 * hop_length)`) attempting to match `librosa.onset_strength(y=y)`.
    - **Python Reference**: Uses `onset_strength(S=S)`, which Librosa aligns differently, resulting in a **-5ms shift** relative to the spectral peak.
    - **Resolution**: The +23ms shift has been removed. The next step is to exactly replicate the internal Librosa difference logic to achieve the identical -5ms offset.
- **Mel Filterbank**: Discrepancies (~1.5e-9) exist due to the current custom HTK-style Mel formula. Transitioning to the exact Slaney formula used by Librosa will eliminate this error source.

### Pick-up Instructions for Autonomous Iteration
1.  **Define "Frame"**: In this engine, a "frame" is a **1ms slice of audio** (1ms resolution).
2.  **Envelopes First**: Achieve 1.000000 correlation on all 4 bands. Focus on the 5ms offset and the Mel scale coefficients.
3.  **Peak Identity**: Once envelopes match, ensure `analyzer_analyze_audio` produces identical peak indices to `scipy.signal.find_peaks`.
4.  **Scoring & Metrics**: Align the order of operations in `analyzer_process_peak`. The `total_score` and `rating` must be identical to the Python results.
5.  **Verification**: Always run `python3 analysis/test_full_parity.py` after changes.

## Multidimensional Verification
1.  **Onset Envelopes:** Pearson correlation of 1.000000 across all spectral bands.
2.  **Peak Detection Timing:** Sample-accurate alignment of detected transient peaks.
3.  **Peak Magnitudes:** Identical floating-point values for peak heights.
4.  **Resonance Scores:** Identical calculated resonance scores and qualifiers.
5.  **Historical Buffers:** Identical buffer state after processing.

## System-Level Verification (The "Rating" Test)
The final "Rating" metric produced by `analysis/analyze_files.py` (C) and `analysis/legacy/analyze_files.py` (Python) must be exactly identical across all control files.
