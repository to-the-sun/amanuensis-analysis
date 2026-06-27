# Cumulative Transient Analysis: Snapshot & Resonance Score Logic

This report details the technical operations that occur when a "snapshot" is taken for calculating the resonance score of a primary peak in the `cumulative_transience` extension module.

## 1. Triggering and Window Extraction

The process is initiated within the `update` function of the video generator whenever the animation playhead reaches a frame containing a pre-detected peak index ($p_{idx}$) from any of the four frequency bands (Sub-Bass, Bass/Low-Mid, High-Mid, or Treble).

*   **Temporal Windowing**: A **5001-sample window** is extracted from the onset strength envelope of the triggering band.
*   **Resolution**: 1ms per sample (representing 5000ms of history + the current 1ms at the peak).
*   **Alignment & Padding**: The window is aligned to end exactly at the peak ($p_{idx}$). If the peak occurs within the first 5 seconds of the audio ($p_{idx} < 5000$), the window is **zero-padded** at the beginning to maintain the fixed 5001-sample length.

## 2. Normalization

To ensure that rhythmic influence is relative across different audio files and sections, each window is normalized before being processed further.

*   **Normalization Factor**: Calculated as $\frac{peak\_val}{max\_peak}$, where:
    *   `peak_val` is the raw onset strength at the current peak ($p_{idx}$).
    *   `max_peak` is the global maximum onset strength detected across all four bands for the entire duration of the audio.
*   **The Snapshot**: The resulting scaled array is designated as the **`snapshot`**.

## 3. Resonance Score Calculation

The **Resonance Score** measures how well the rhythmic "history" contained in the new snapshot aligns with the collective history of all previous transients stored in the `accumulated_buffer`.

### A. Internal Peak Detection
The module identifies all internal peaks within the new 5-second `snapshot` using `scipy.signal.find_peaks` with a dynamic minimum height threshold equal to the **average of every point in the snapshot** (`np.mean(snapshot)`).

### B. Historical Context (The 99ms Offset)
Baseline statistics (`avg`, `max_v`, and `min_v`) are calculated from the current state of the `accumulated_buffer`. Crucially, the **last 99ms of the buffer are excluded** (`accumulated_buffer[:-99]`) from these calculations. This prevents the primary peak—which is always located at the 0ms mark in every snapshot—from artificially inflating the resonance score or biasing the rhythmic average.

### C. The Qualifier ($Q$)
For every peak identified in the new snapshot (at relative index $sp_{idx}$), the module checks the energy level ($val$) in the `accumulated_buffer` at that same relative offset:

*   **Positive Alignment (Reward)**: If $val > avg$, then $Q = \frac{val - avg}{max\_v - avg}$.
*   **Negative Alignment (Penalty)**: If $val < avg$, then $Q = \frac{val - avg}{avg - min\_v}$.

### D. Final Score Determination
Unlike legacy versions that utilized only the single best qualifier, the current module calculates a **`qualifier_sum`** by adding all individual qualifiers calculated for every valid historical peak within the window. The final **Total Score** is the product of the primary `peak_val` (the scalar) and this sum:
$$\text{Total Score} = peak\_val \times \sum Q$$

## 4. Accumulation and Visual Feedback

*   **Buffer Update**: After score calculation, the `snapshot` is added to the `accumulated_buffer` via element-wise addition.
*   **Visual Flash**: A green fill (`#2ecc71`) is rendered on the historical buffer plot, briefly visualizing the snapshot's shape as it merges into the history.
*   **Score Animation**: The calculated score is displayed as floating text (e.g., `+0.45`). Its color is dynamically mapped based on the overall range of scores seen:
    *   **Bright Green**: High positive resonance.
    *   **Bright Red**: High negative resonance (rhythmic clash).
    *   **Subdued Gray**: Neutral/Zero alignment ($#808080$).

## 5. Sliding Window Cleanup

To prevent the `accumulated_buffer` from growing indefinitely and to ensure the "rhythmic memory" reflects the recent context of the audio:

*   **Snapshot Storage**: Each snapshot is stored in a tracking dictionary indexed by its $p_{idx}$.
*   **Subtraction Sweep**: Exactly **15 seconds** (15,000 frames) after the peak was processed, the module **subtracts** that specific snapshot from the `accumulated_buffer`.

## 6. Post-Processing and Metric Recording

Upon completion of the analysis and video generation, the script records the final aggregated metrics to a persistent storage location.

*   **Target Location**: `D:\[Library]\[Audio]\[Works]\[Projects]\{SongName}\ratings.txt`
*   **Recorded Metrics**:
    *   **Rating**: The mean of all generated resonance scores.
    *   **Standard Deviation**: The running standard deviation of the energy in the accumulated buffer (excluding the last 99ms).
    *   **Contrast**: The ratio of the maximum energy to the mean energy in the accumulated buffer.
    *   **Bar Length Deviation**: The standard deviation of the temporal position (in ms) of the highest detected peaks in the historical buffer, tracking rhythmic stability.
