# Additive Synthesis: Acoustic Analysis & Iteration Guide

When designing additive synthesizer presets, adding sinusoids without discipline leads to **muddiness, phase cancellation, and sensory roughness**.

Since we can't always audibly monitor the sound in head-less / sandbox environments, we must "hear" the sound through quantitative analysis metrics. This guide outlines how to run our diagnostic test suite, understand its metrics, and iteratively perfect any sound preset.

---

## 1. Quantitative Acoustic Metrics

To analyze synthesized sounds, we calculate four primary psychoacoustic metrics:

### A. Sensory Roughness (Sethares Model)
- **What it is:** Measures the rapid, jarring amplitude roughness perceived when two or more frequencies fall within a single critical band.
- **Goal:** Keep the total roughness score low ($< 0.25$ for complex chords, $< 0.15$ for simple chimes) to maintain clarity and consonance.

### B. Lower Interval Limits (LIL)
- **What it is:** The physical limit of human hearing to distinguish intervals in low registers. Chords played below 250 Hz become muddy and lose clear pitch definition if intervals are too small.
- **Rule of Thumb:**
  - Below 100 Hz: Minimum interval is a Perfect 5th (7 semitones) or Octave (12 semitones).
  - 100 Hz – 150 Hz: Minimum interval is a Major 3rd (4 semitones).
  - 150 Hz – 200 Hz: Minimum interval is a Minor 3rd (3 semitones).
  - 200 Hz – 250 Hz: Minimum interval is a Major 2nd (2 semitones).

### C. Low-Frequency Beating
- **What it is:** Slow, unstable phase interference caused by two active low frequencies (below 250 Hz) being placed extremely close to each other (less than 8 Hz difference).
- **Goal:** Avoid low-frequency beating entirely. For bass frequencies, use clear harmonic spacings or simple unison octave separations rather than sub-10 Hz offsets.

### D. Decay-Frequency Coupling Correlation
- **What it is:** The correlation coefficient between frequency and exponential decay rate.
- **Goal:** Ensure higher-frequency sparkles decay faster (positive Pearson correlation coefficient $\ge 0.3$). This prevents high-pitched, inharmonic elements from ringing out forever and sounding metallic, synthetic, or fatiguing.

---

## 2. Using the Test Suite

Run the automated test suite to diagnose presets:
```bash
python3 -m unittest additive_synthesis/test_suite.py
```

### Reading Failures
- **Muddy Low-Frequency Beating:** Indicates two low-register sinusoids are too close. Expand their spacing or remove one of the beating partners.
- **LIL Violations:** Indicates an interval in the bass register is too small (e.g., a major/minor third below 100 Hz). Invert or transpose the upper tone to a higher octave.
- **Decay-Frequency Coupling Correlation Failures:** Indicates high-frequency components lack sufficient decay rates. Increase the `decay` property for high-frequency elements.

---

## 3. How to Iterate on Chords/Presets

To fix a "muddy mess" like the legacy `complex.json`, follow these systematic steps:
1. **Clear the Bass (Below 150 Hz):** Ensure there is only one clear sub-bass foundation (e.g., around 55 Hz or 110 Hz) and no close detuned copies below 150 Hz.
2. **Space Bass Intervals:** Keep bass intervals extremely wide (fifths or octaves).
3. **Move Detuning/Chorus Higher:** Detuned unison copies (for warm chorus texture) should only be placed in the midrange and treble (above 300 Hz) where our ears handle small frequency offsets as a pleasant chorusing effect rather than mud.
4. **Link Highs to Quick Decays:** For any partials above 400 Hz, assign a decay rate proportional to $\approx \sqrt{\text{freq}}$, ensuring they quickly die out and act as "transient sparkles" rather than sustained ringouts.
