# Pitch Separator Architecture & HTML Report Specification

## Executive Summary

The Perfect Reconstruction Pitch Stem Separator (`analysis/pitch_separator.c`) processes audio WAV files using an STFT COLA (Constant Overlap-Add) filter bank (8192 FFT size, 75% overlap) to separate audio into individual MIDI pitch stems (0–127). The system applies relative noise floor thresholding (19.0% of peak signal amplitude / -14.4 dB) and two-pass transience flux gating to distinguish tonal pitch stems from residual non-tonal acoustic energy.

When the pitch separator runs, it automatically constructs an interactive HTML report (`index.html`) in the output pitch stems directory (`<basename>_pitch_stems/`).

---

## Output Directory & HTML Report Generation

Upon execution, `pitch_separator` creates an isolated stem directory `<basename>_pitch_stems/` containing:
1. Exported MIDI pitch stem WAV files (`<basename>_pitch_<03d>_<note_name>.wav`).
2. Companion transience flux envelope WAV files (`<basename>_pitch_<03d>_<note_name>_flux.wav`).
3. Non-tonal residual stem WAV file (`<basename>_nontonal.wav`).
4. **Interactive HTML Waveform Report** (`index.html`).

---

## HTML Report Features & Waveform Visualization

The generated `index.html` report provides interactive browser-based analysis for all produced pitch stems:

### Key Features:
- **Individual Stem Rows**:
  - Each produced pitch stem (and non-tonal stem) is rendered on its own dedicated row card.
  - Displays stem metadata: MIDI pitch number, note name, fundamental frequency (Hz), peak amplitude, RMS amplitude, active duration (ms), and filename.
  - Integrated `<audio controls>` player per row for direct audio listening.

- **Background Original Peak Envelope Overlay**:
  - In the background of each stem's waveform canvas, a soft translucent region (`rgba(180, 190, 210, 0.35)`) renders the downsampled peak amplitude envelope of the original audio file ($+E_{orig}(t)$ to $-E_{orig}(t)$).
  - This allows instant visual comparison between the amplitude envelope of the full original mix and the isolated stem signal.

- **Interactive Click and Drag to Zoom**:
  - **Horizontal Click & Drag**: Select any time range $[t_{start}, t_{end}]$ on any waveform canvas to zoom in down to millisecond-level detail.
  - **Synchronized Zoom**: Optional "Sync Zoom Across Rows" mode (enabled by default) ensures zooming on one stem row simultaneously updates all other stem rows, preserving precise time alignment across all frequency bands.
  - **Reset Zoom**: One-click "Reset Zoom" button (or double-clicking any graph) instantly restores the full timeline view ($0.0\text{s}$ to $T_{duration}$).
  - **Hover Tooltip & Crosshair**: Hovering over any canvas displays a vertical crosshair cursor and tooltip readout showing exact timestamp, original peak amplitude, and stem peak amplitude.

---

## Technical Specifications

| Parameter | Value / Implementation |
|---|---|
| **STFT Window** | 8192 FFT, 2048 Hop (75% Overlap, Hann Window) |
| **Noise Floor Threshold** | 19.0% of overall signal peak (-14.4 dB) |
| **Minimum Active Duration** | 99 ms above noise floor |
| **Transience Flux Window** | 2048 FFT, 512 Hop |
| **Flux Gating** | 9.0% peak global flux noise floor, 89.0% momentary peak threshold |
| **Report Filename** | `<basename>_pitch_stems/index.html` |
| **Downsampled Bins** | Adaptive 2,000–10,000 bins (500 bins/sec) embedded as JSON |
