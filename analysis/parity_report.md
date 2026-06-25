# Audio Analysis Parity Report: C vs. Python

This document serves as the primary technical log and roadmap for achieving **100% numerical parity** between the C-based `cumulative_transience.c` analysis engine and the reference Python implementation (`cumulative_transience.py`).

## Mission Objective: Absolute Parity
The goal of this project has been elevated from "near-parity" to **absolute 100% verification-verified parity**. Every aspect of the C implementation must produce outputs that are mathematically identical to the Python reference implementation across multiple dimensions.

## Current Status (As of Jan 2024)
Recent verification tests using `analysis/test_full_parity.py` on control WAV files show significant discrepancies that require urgent attention.

### Latest Parity Metrics (Pearson Correlation)
- **Band 0 (Sub-Bass):** 0.395608
- **Band 1 (Bass/Low-Mid):** 0.631372
- **Band 2 (High-Mid):** 0.569843
- **Band 3 (Treble):** 0.046021

*Note: Previous reports of 99% parity were premature or based on incomplete alignment logic. The current implementation is significantly out of sync with the reference Python logic.*

## Multidimensional Verification
To ensure 100% parity, verification must go beyond simple envelope correlation. The following dimensions must match exactly:
1.  **Onset Envelopes:** Pearson correlation of 1.000000 across all spectral bands.
2.  **Peak Detection Timing:** Sample-accurate alignment of detected transient peaks.
3.  **Peak Magnitudes:** Identical floating-point values for peak heights in the onset envelope.
4.  **Resonance Scores:** The final calculated resonance scores and qualifiers must be identical.
5.  **Historical Buffers:** The state of the 5-second accumulation buffer after identical processing steps.

## System-Level Verification (The "Rating" Test)
The repository contains four primary control WAV files for verification:
- `analysis/01 sustained bass [2026-06-24 181817].wav`
- `analysis/02 sustained treble [2026-06-24 181817].wav`
- `analysis/03 transient bass [2026-06-24 181817].wav`
- `analysis/04 transient treble [2026-06-24 181817].wav`

An ideal verification test is to run `analysis/analyze_files.py` (which uses the C engine) and `analysis/legacy/analyze_files.py` (which uses the original Python engine) on these files. **The final "Rating" metric produced by both must be exactly identical.**

## Strategy & Critique
Continued iteration is the core philosophy. We must constantly critique existing emulation strategies. If a C-based FFT or Mel-filterbank cannot match Librosa's output exactly, we must identify why (e.g., windowing, scaling, padding) and adjust until it does.

**Valid critiques and areas for investigation:**
- **Alignment Offsets:** Is the `n_fft // (2 * hop_length)` shift exactly right for all sample rates?
- **Slaney Normalization:** Does the C Mel-scale implementation include the area-normalization used by Librosa?
- **FFT Scaling:** Does the C FFT output require different scaling to match the unscaled Numpy FFT?
- **Topological Prominence:** Is the C implementation handling the "higher base" edge cases in SciPy's `find_peaks` correctly?

## Autonomous Execution
This work is to be conducted autonomously. The agent is instructed to:
1.  **Trust Intuition:** Make informed decisions based on the reference code and mathematical principles.
2.  **Iterate Rapidly:** Use the `test_full_parity.py` script and the Rating test to verify every change.
3.  **No Human-in-the-Loop:** Do not pause for approval or ask for clarification. The mandate is clear: 100% parity.
4.  **Continuous Improvement:** Keep working until the verification suite confirms absolute identity between the two implementations.

## References
- `analysis/cumulative_transience.py`: The "Source of Truth" for the desired behavior.
- `analysis/legacy/cumulative_transience.py`: The original Python implementation.
- `analysis/test_full_parity.py`: The verification tool.
- `analysis/cumulative_transience.c`: The target for optimization.
