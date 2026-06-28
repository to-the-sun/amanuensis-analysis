# Peak Distance Implementation Comparison: C vs. Python

This report details the differences in the **Minimum Peak Distance** logic between the high-performance C engine (`analysis/cumulative_transience.c`) and the original Python reference (`analysis/legacy/cumulative_transience.py`).

## 1. Core Logic & Implementation Method
The primary difference lies in the algorithmic approach to identifying and filtering peaks:

*   **Python (Reference)**: Utilizes `scipy.signal.find_peaks(..., distance=200, prominence=0.5)`. This is a **multi-pass, global optimization** algorithm. It first identifies all local maxima, then applies prominence filtering, and finally enforces the distance constraint by processing peaks in order of their magnitude.
*   **C (Engine)**: Implements a **single-pass, greedy linear scan**. It processes frames sequentially (0 to N) and makes immediate decisions about whether to keep or replace a peak based on its temporal proximity to the previously accepted peak.

## 2. Selection Strategy: "Knock-out" Logic
The two approaches handle "peak competition" (multiple peaks within a 200ms window) differently:

*   **Python (Global Sorting)**: SciPy sorts all candidate peaks by magnitude first. It keeps the absolute highest peak in the signal and invalidates any neighbors within 200ms. It then repeats this process for the next highest remaining peak. This ensures that the **mathematically optimal** set of highest peaks is preserved across the entire timeline.
*   **C (Sequential Replacement)**: The C engine uses a greedy "replace if higher" logic.
    - If a new peak (B) is found within 200ms of the previous peak (A), and B is higher than A, B **replaces** A.
    - However, because it is sequential, if B replaces A, but a third peak (C) later arrives and is even higher than B (but still within 200ms of where A was), the algorithm only ever "remembers" the most recent peak.
    - This can lead to different results in dense transient areas compared to SciPy's global view.

## 3. Interaction with Prominence
Prominence filtering, which ensures a peak stands out from its local background, is handled differently:

*   **Python**: The `prominence=0.5` check is applied to **all** candidate peaks *before* the distance filter. Every peak in the final output is guaranteed to meet the 0.5 threshold.
*   **C**: The prominence check is only explicitly performed when a peak is "new" (i.e., not within 200ms of the previous one). If a peak is a **replacement** for a previous peak within the 200ms window, the C code currently assumes it is valid if it is higher than the peak it is replacing, potentially bypassing a fresh topological prominence check for the replacement peak.

## 4. Technical Constants & Resolution
Despite the logic differences, the fundamental constants are aligned for parity:

*   **Temporal Resolution**: Both operate on **1ms frames**.
    - Python: `hop_length = int(sr * 0.001)`
    - C: `hop_length = sr / 1000`
*   **Distance Threshold**: Both use a **200ms window**.
    - Python: `distance=200`
    - C: `f - temp_peaks[peak_count-1] < 200` (where `f` is the current frame index).

## 5. Summary Comparison

| Feature | Python (Original) | C (Current) | Impact |
| :--- | :--- | :--- | :--- |
| **Algorithm** | Multi-pass (SciPy) | Single-pass (Custom) | Performance vs. Precision trade-off |
| **Strategy** | Global Priority (Highest wins) | Sequential Greedy (Latest/Highest wins) | Slight differences in peak timing in dense areas |
| **Prominence** | Strict Pre-filter | Conditional (Bypassed on replacement) | C might accept slightly "flatter" replacements |
| **Resolution** | 1ms | 1ms | Identical |
| **Distance** | 200ms | 200ms | Identical |

## Conclusion
The C implementation is designed for streaming performance and avoids the memory/computational overhead of sorting all peaks globally. While it effectively replicates the *intent* of the 200ms minimum distance, users may notice subtle differences in peak selection in complex audio where multiple high-energy transients occur in very close proximity.
