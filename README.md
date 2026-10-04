# Audio Analysis, Synthesis, & Transcription Suite

A comprehensive repository for advanced audio signal processing, transient analysis, polyphonic pitch tracking, stem tuning, additive synthesis, and real-time Discord voice transcription with AI-driven poetic parsing.

---

## Repository Architecture

```
├── analysis/              # Transient analysis, pitch tracking, clustering & Amanuensis bot
├── additive_synthesis/    # Additive synthesis engine, presets & psychoacoustic test suite
├── transcription/         # Aqua Voice transcription bots, IPA overrides & Jev API ranking
└── tune_stems.py          # Continuous stem pitch alignment utility via Rubber Band
```

---

## 1. Analysis Suite (`analysis/`)

The analysis suite provides high-resolution transient detection, polyphonic pitch tracking, feature clustering, and Discord automation.

### C/Cython Transient Engine (`analyze_files.py`, `cumulative_transience.c`, `ct_extension.pyx`)
Features a high-performance signal processing engine using a centered Short-Time Fourier Transform (STFT, `n_fft=2048`, `1ms` hop size) mapped to a 128-band Slaney Mel scale.
* **Spectral Division:** Onset strength is evaluated across 4 Mel sub-bands (32 bins per band): Sub-Bass (0–1.02 kHz), Bass/Low-Mid (0.99–2.85 kHz), High-Mid (2.76–7.93 kHz), and Treble (7.68–22.05 kHz).
* **High Temporal Resolution:** Onset strength is evaluated at a 1 ms frame resolution with a 5-second historical cumulative buffer and automatic 15-second decay sweeps.
* **Outputs:** High-resolution interactive HTML reports and MP4 visualization videos with synchronized transient tracking.

**Usage:**
```bash
python3 analysis/analyze_files.py "path/to/audio.wav"
```

### Real-Time Playback Analysis (`play_files.py`)
Plays WAV files audibly while running the Cython transient analysis engine simultaneously and logging live peak metrics to the console.

**Usage:**
```bash
python3 analysis/play_files.py "path/to/audio.wav" [--device INDEX_OR_NAME] [--mock]
```

### Pitch Tracking & Tuning Regression (`pitch_tracker.py`)
Tracks continuous dominant pitch and tuning drift in polyphonic audio without requiring source separation.

**Usage:**
```bash
python3 analysis/pitch_tracker.py "path/to/audio.wav" --algo piptrack --fmin 50 --fmax 1000
```
* **Algorithms:** Supports `yin`, `pyin`, and `piptrack` (spectral peak tracking recommended for polyphonic mixes).
* **Outputs:** Generates CSV datasets (timestamps, Hz, fractional MIDI, note names, cents deviation) and high-resolution 2-panel visualization plots (spectrogram with pitch overlay and linear fractional MIDI timeline with equal-tempered gridlines).

### Audio Feature Clustering (`generate_and_cluster.py`)
Generates audio samples across musical archetypes, extracts features (MFCCs, spectral centroid, spectral flatness, chroma, onset autocorrelation), performs clustering, and exports 2x2 comparison projection grids (`analysis/cluster_analysis.png`).

### Amanuensis Automation Bot (`Amanuensis.py`)
A Discord bot that monitors local directories for WAV uploads, converts them to MP3 (dynamically adjusting bitrates under 10MB), runs transient analysis in background threads, and posts synchronized MP3s and MP4 videos to designated channels (`#works-in-progress`, `#general`).

---

## 2. Continuous Stem Tuning (`tune_stems.py`)

Performs high-quality, continuous, time-varying pitch alignment of song stems relative to a prepended reference stem (`00_base_stem.wav`).

**Usage:**
```bash
python3 tune_stems.py "path/to/00_reference.wav" "path/to/vocal_stem.wav" "path/to/synth_stem.wav" --algo pyin --formant
```
* **Interval-Relative Correction:** Calculates relative semitone deviations, applies temporal Gaussian smoothing, and passes time-varying pitch maps to `rubberband-cli` (`--pitchmap`).
* **Multi-Channel & Formant Support:** Preserves multi-channel audio layout, duration, and optional vocal formant structures (`--formant`).

---

## 3. Additive Synthesis (`additive_synthesis/`)

A mathematical additive synthesis synthesizer supporting custom sinusoids, time-varying dynamic envelopes, interactive wave shapes, and automated psychoacoustic diagnostics.

**Usage:**
```bash
python3 additive_synthesis/additive_synthesis.py [preset.json]
```
* **Built-in Presets:** Includes `square.json`, `bell.json`, `complex.json`, `cosmic` (16-step spatial detuned texture), and `generative` (deterministic algorithmic sound generator).
* **Symbolic Equations:** Displays live updated mathematical equations in text, LaTeX, and Sigma ($\sum$) sum notation.
* **Automated Psychoacoustic Test Suite:** Runs quantitative diagnostics checking Sethares Roughness, Lower Interval Limit (LIL) violations, low-frequency beating, and decay-frequency coupling:
  ```bash
  python3 -m unittest additive_synthesis/test_suite.py
  ```

---

## 4. Voice Transcription & Poetic Parsing (`transcription/`)

Real-time Discord voice transcription, IPA syllabification, weak vowel override resolution, and AI grammatical poem ranking.

### Transcription Bots (`transcription_bot_aqua.py` & `desktop_transcriber.py`)
* **Aqua Voice Integration:** Connects to Avalon (`avalon-v1.5`) API. Speech utterances are accumulated in memory buffers and submitted when total speech reaches $\ge 10.0$ seconds or after 49 seconds of channel inactivity.
* **DAVE Decryption:** Patches `discord.ext.voice_recv` to handle Discord's End-to-End Encryption (AES-GCM decryption with sequence number and ROC tracking).
* **Desktop Audio Capture:** `desktop_transcriber.py` uses `soundcard` loopback audio recording with RMS Voice Activity Detection (300ms pre-roll, 1.0s silence timeout).
* **Punctuation & Sentence Splitting:** `poetic_parse` removes periods after uppercase abbreviations and splits text on sentence punctuation (`.`, `!`, `?`), placing each phrase on its own line.
* **IPA & Phonetic Syllabification:** Interleaves syllable-segmented IPA transcriptions (e.g., `/syl1/syl2/ /syl3/`) below each line, incorporating over 8,000 weak-vowels IPA overrides (`ipa_overrides.json`).
* **Slash Commands (`/analyze`, `/purge`):**
  * `/analyze`: Builds backward syllable histogram chains, identifies repeating vowel sounds at the optimal syllable distance, formats rhyming syllables in ALL CAPS, temporarily restores original word capitalization for TypeSafe Jev API (`jev_api.py`) grammatical evaluation, and posts the reordered poem.
  * `/purge`: Clears channel messages and resets in-memory syllable histogram state.

---

## Installation & Setup

### 1. System Dependencies
```bash
sudo apt-get update && sudo apt-get install -y \
    libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev \
    ffmpeg rubberband-cli
```

### 2. Python Dependencies
```bash
pip install librosa numpy scipy matplotlib soundcard soundfile \
    discord.py[voice] discord-ext-voice-recv davey cryptography \
    requests tqdm nltk syllables SoundsLike eng-to-ipa pydub Cython
```

### 3. Linguistic Data
```bash
python3 -m nltk.downloader cmudict averaged_perceptron_tagger
```

### 4. Credentials Configuration
Create `credentials.json` in the project root:
```json
{
  "token": "YOUR_DISCORD_BOT_TOKEN",
  "aqua_key": "YOUR_AQUAVOICE_API_KEY",
  "typesafe_api_key": "YOUR_TYPESAFE_JEV_API_KEY"
}
```
