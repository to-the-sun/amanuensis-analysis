# Continuous Phase-Coherence Transient Enhancement Implementation Plan

## Executive Summary & Background

In the Perfect Reconstruction Pitch Stem Separator (`analysis/pitch_separator.c`), separating audio into pitch-specific stems using STFT filter banks captures the sustained tonal sinusoids, but most of a note's transient attack energy (pick strike, hammer impact, or onset transient noise) resides in the non-tonal residual signal (`nontonal_data`).

Previously, non-tonal residual audio was added to pitch stems using a strict binary threshold check: if a pitch stem's momentary transience flux exceeded 89% of the peak flux at sample $t$, 100% of the non-tonal audio was added; otherwise, 0% was added. This caused two major issues:
1. **Audio Artifacts:** Non-tonal noise and high-frequency attack bursts "snapped" abruptly in and out whenever the threshold was crossed.
2. **Soft Pitch Attacks:** Isolated pure pitch stems lacked attack punch when flux missed the binary threshold.

This specification details a replacement algorithm: **Continuous Phase-Coherence Transient Injection**. Instead of binary snapping, non-tonal residual audio is continuously blended into each pitch stem in direct proportion to how **in-phase** that pitch stem's local waveform movement is relative to the **total original audio mix**.

---

## Core Algorithm & Mathematical Logic

### 1. Fundamental Principle
When a pitch (e.g. C#4) is struck, its initial acoustic attack movement in the stem array `stem_audio[k]` is directly in phase with the major excursions of the total original audio mix `orig_audio`. A lingering ringing note from a previous strike (e.g. D4) is out of phase or destructively interfering with `orig_audio`.

We calculate a localized normalized cross-correlation (Pearson correlation) between each pitch stem and the original mix over a sliding window $W$.

### 2. Zero-Mean Normalized Cross-Correlation (Pearson Correlation)
To prevent low-frequency carrier waves (such as a deep bass note C2 at 65 Hz) from modulating high-frequency pitch correlations (such as C4 at 261 Hz), local window means are subtracted prior to calculating cross-correlation:

```c
// Local window calculation centered at sample t for stem k
double mean_stem = 0.0;
double mean_orig = 0.0;
int window_count = 0;

for (int m = -W / 2; m <= W / 2; m++) {
    int idx = t + m;
    if (idx >= 0 && idx < total_samples) {
        mean_stem += stem_audio[k][idx];
        mean_orig += orig_audio[idx];
        window_count++;
    }
}
mean_stem /= window_count;
mean_orig /= window_count;

double numerator = 0.0;
double sum_sq_stem = 0.0;
double sum_sq_orig = 0.0;

for (int m = -W / 2; m <= W / 2; m++) {
    int idx = t + m;
    if (idx >= 0 && idx < total_samples) {
        double s_val = stem_audio[k][idx] - mean_stem;
        double o_val = orig_audio[idx] - mean_orig;
        numerator += s_val * o_val;
        sum_sq_stem += s_val * s_val;
        sum_sq_orig += o_val * o_val;
    }
}

double denom = sqrt(sum_sq_stem * sum_sq_orig);
double correlation = (denom > 1e-12) ? (numerator / denom) : 0.0;
```

### 3. Rectification and Window Smoothing
* **Half-Wave Rectification & Squaring:** Negative correlations (out-of-phase components) are zeroed out. Squaring emphasizes strong in-phase alignment ($> 0.70$):

```c
double raw_weight = (correlation > 0.0) ? (correlation * correlation) : 0.0;
```

* **Asymmetric Attack/Release Envelope Follower & Pitch-Adaptive Windowing:**
  To prevent fundamental cycle-by-cycle oscillations (intra-cycle correlation beating occurring at fundamental pitch rates) while preserving sharp transient attacks, two key refinements are applied:
  1. **Pitch-Adaptive Windowing ($W_k$):** The correlation window size scales adaptively with the pitch frequency $f_k$:
     $$W_k = \max(12\text{ms}, \min(40\text{ms}, 4 / f_k))$$
     This guarantees that at least 4 full cycles of the pitch's fundamental frequency are covered within the correlation window, smoothing out intra-cycle phase variations.
  2. **Asymmetric Envelope Follower:** An asymmetric lowpass follower is applied to the squared correlation weight `raw`:
     * **Attack Constant ($\tau_{\text{attack}} = 2\text{ms}$):** Fast attack captures onset transients instantly without smearing.
     * **Release Constant ($\tau_{\text{release}} = 25\text{ms}$):** Slower release eliminates pitch-rate ripple during sustained tonal notes.

```c
// Asymmetric time smoothing (2ms attack, 25ms release)
double alpha_attack = 1.0 - exp(-1.0 / (0.002 * sample_rate));
double alpha_release = 1.0 - exp(-1.0 / (0.025 * sample_rate));

if (raw > smooth) {
    smooth += alpha_attack * (raw - smooth);
} else {
    smooth += alpha_release * (raw - smooth);
}
```

### 4. Multi-Stem Weight Normalization
In polyphonic sections (chords), multiple pitch stems (e.g. C#4 and G#4) may simultaneously exhibit high phase coherence with `orig_audio`. To ensure the total injected non-tonal energy across all stems does not exceed 100% of the original non-tonal audio `nontonal_audio[t]`, weights are normalized across active stems at each sample $t$:

```c
double total_weight = 0.0;
for (int k = 0; k < num_audible_stems; k++) {
    total_weight += smooth_weight[k][t];
}

for (int k = 0; k < num_audible_stems; k++) {
    double norm_weight = smooth_weight[k][t];
    if (total_weight > 1.0) {
        norm_weight /= total_weight;
    }
    enhanced_stem_audio[k][t] = stem_audio[k][t] + (norm_weight * nontonal_audio[t]);
}
```

---

## Detailed C Implementation Steps (`analysis/pitch_separator.c`)

### Step 1: Replace Binary Flux Pass
Locate Pass 2 in `process_audio()` inside `analysis/pitch_separator.c`:
* Remove the binary threshold block:
  ```c
  if (audible_stems[idx].flux_data[k] > thresh) {
      audible_stems[idx].audio_data[k] += nontonal_data[k];
  }
  ```

### Step 2: Implement Correlation & Weight Calculation Function
Add a dedicated helper function in `pitch_separator.c`:
```c
void apply_continuous_phase_coherence_enhancement(
    AudibleStem *stems,
    int stem_count,
    const double *orig_audio,
    const double *nontonal_audio,
    uint32_t total_samples,
    uint32_t num_channels,
    uint32_t sample_rate
) {
    // Window size W: ~8ms (352 samples at 44.1kHz)
    int W = (int)(0.008 * sample_rate);
    if (W % 2 == 0) W++; // Ensure odd window size for symmetric centering

    // Allocate memory for weight arrays per stem
    double **weights = (double**)malloc(stem_count * sizeof(double*));
    for (int k = 0; k < stem_count; k++) {
        weights[k] = (double*)calloc(total_samples * num_channels, sizeof(double));
    }

    // 1. Calculate raw correlation weights per channel and sample
    for (uint32_t c = 0; c < num_channels; c++) {
        for (int k = 0; k < stem_count; k++) {
            const double *s_buf = stems[k].audio_data;

            double smooth = 0.0;
            double alpha = 1.0 - exp(-1.0 / (0.002 * sample_rate)); // 2ms time constant

            for (uint32_t t = 0; t < total_samples; t++) {
                // Compute local zero-mean cross-correlation over window W
                double mean_s = 0.0, mean_o = 0.0;
                int count = 0;
                int half_w = W / 2;

                int start_m = ((int)t - half_w < 0) ? -(int)t : -half_w;
                int end_m = ((int)t + half_w >= (int)total_samples) ? ((int)total_samples - 1 - (int)t) : half_w;

                for (int m = start_m; m <= end_m; m++) {
                    uint32_t idx = (t + m) * num_channels + c;
                    mean_s += s_buf[idx];
                    mean_o += orig_audio[idx];
                    count++;
                }
                if (count > 0) {
                    mean_s /= count;
                    mean_o /= count;
                }

                double num = 0.0, sum_s = 0.0, sum_o = 0.0;
                for (int m = start_m; m <= end_m; m++) {
                    uint32_t idx = (t + m) * num_channels + c;
                    double sv = s_buf[idx] - mean_s;
                    double ov = orig_audio[idx] - mean_o;
                    num += sv * ov;
                    sum_s += sv * sv;
                    sum_o += ov * ov;
                }

                double denom = sqrt(sum_s * sum_o);
                double corr = (denom > 1e-12) ? (num / denom) : 0.0;
                double raw = (corr > 0.0) ? (corr * corr) : 0.0;

                // Apply time smoothing
                smooth += alpha * (raw - smooth);
                weights[k][t * num_channels + c] = smooth;
            }
        }

        // 2. Normalize across stems and inject non-tonal residual
        for (uint32_t t = 0; t < total_samples; t++) {
            uint32_t idx = t * num_channels + c;
            double sum_w = 0.0;
            for (int k = 0; k < stem_count; k++) {
                sum_w += weights[k][idx];
            }

            for (int k = 0; k < stem_count; k++) {
                double w = weights[k][idx];
                if (sum_w > 1.0) {
                    w /= sum_w;
                }
                stems[k].audio_data[idx] += w * nontonal_audio[idx];
            }
        }
    }

    // Cleanup memory
    for (int k = 0; k < stem_count; k++) {
        free(weights[k]);
    }
    free(weights);
}
```

---

## Edge Case Handling & Performance Guidelines

1. **Phase Coherence Target:**
   * Always calculate correlation against `orig_audio` (the original audio mix), never against `nontonal_audio`.
2. **Zero-Mean Subtraction:**
   * Mandatory inside the window calculation to prevent low-frequency notes (e.g. C2) from distorting high-frequency note correlations (e.g. C4).
3. **Execution Speed Optimization:**
   * Calculate moving sums for `mean_s`, `mean_o`, `sum_s`, `sum_o`, and `num` across adjacent samples $t$ to convert $O(N \cdot W)$ window iteration to $O(N)$ running updates if needed for maximum speed.
4. **Cross-Compilation:**
   * Whenever `analysis/pitch_separator.c` is updated, cross-compile `analysis/pitch_separator.exe` for Windows using `x86_64-w64-mingw32-gcc`:
     ```bash
     x86_64-w64-mingw32-gcc -O3 analysis/pitch_separator.c -o analysis/pitch_separator.exe -lm
     ```

---

## Verification & Testing Plan

1. **Compilation:**
   * Compile on Linux (`gcc -O3 analysis/pitch_separator.c -o analysis/pitch_separator -lm`) and Windows (`x86_64-w64-mingw32-gcc`).
2. **Test Run:**
   * Execute `pitch_separator` on a test polyphonic audio WAV file containing rapid note transitions and overlapping sustain tails.
3. **Report & Waveform Verification:**
   * Open the generated `index.html` report.
   * Zoom in to sample level at note onset moments:
     * Confirm that freshly struck notes (in-phase with the mix) display crisp, sharp transient attack envelopes.
     * Confirm that ringing notes (out-of-phase with the mix) stay clean without sudden noise spikes or atonal audio snapping.
