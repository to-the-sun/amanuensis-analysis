# Audio Analysis, Transcription, and Synthesis Suite

This repository contains a comprehensive suite of tools for audio signal processing, transient analysis, continuous pitch tracking, stem alignment, real-time Discord voice transcription, and additive sound synthesis.

The project is organized into three primary subdirectories and root utility scripts:
1. `analysis/` – Rhythmic energy tracking, spectral transient analysis, dominant pitch tracking, audio feature clustering, and Discord bot automation.
2. `transcription/` – Real-time Discord voice-to-text transcription (Aqua Voice API), phonetic syllabification, custom IPA weak-vowel overrides, backward vowel histogram analysis, and AI-driven poem generation (TypeSafe Jev API).
3. `additive_synthesis/` – Interactive additive sound synthesis engine, LaTeX/Sigma notation displays, preset management, psychoacoustic analysis test suite, and generative sound creation.
4. `tune_stems.py` – Continuous interval-relative pitch alignment for song stems.

---

## 1. Stem Tuning & Alignment (`tune_stems.py`)

Performs high-quality, continuous, time-varying pitch alignment of song stems relative to a "00" prepended base stem (e.g. `00_bass.wav`). Uses `librosa` for continuous pitch tracking, temporal Gaussian smoothing, and calls `rubberband` with `--pitchmap` to execute time-varying pitch shifting while preserving multi-channel layouts and song duration.

**Usage:**
```bash
python3 tune_stems.py --stem-dir "path/to/stems" [options]
```

---

## 2. Audio Analysis Suite (`analysis/`)

Focuses on identifying rhythmic transient energy, dominant pitch tracking, structural audio patterns, and feature clustering.

### Interactive Transient Reports (`analyze_files.py` & `cumulative_transience.c`)
Generates high-resolution HTML reports and MP4 videos featuring real-time transient tracking. Uses a centered STFT (2048 window, 1ms hop) mapped to a 128-band Mel scale across 4 frequency bands (Sub-Bass, Bass/Low-Mid, High-Mid, Treble).

**Usage:**
```bash
python3 analysis/analyze_files.py "path/to/audio.wav"
```

### Real-Time Transient Playback (`play_files.py`)
Audibly plays audio files while running the C/Cython transient analysis engine in real time and displaying results in the console.

**Usage:**
```bash
python3 analysis/play_files.py "path/to/audio.wav"
```

### Pitch Tracking & Tuning Regression (`pitch_tracker.py`)
Analyzes polyphonic recordings for continuous dominant pitch and steady tuning regression over time using YIN, pYIN, or PipTrack algorithms. Outputs high-resolution 2-panel visual plots (`.png`) with spectrogram overlays and fractional MIDI gridlines, as well as detailed CSV exports with cent deviations (`+12c`, `-34c`).

**Usage:**
```bash
python3 analysis/pitch_tracker.py "path/to/audio.wav" --algo piptrack
```

### Feature Extraction & Archetype Clustering (`generate_and_cluster.py`)
Generates structured test audio files, extracts MFCCs, spectral centroid, spectral flatness, chroma, and onset-envelope autocorrelation features, and exports PCA and t-SNE projection visualizations (`cluster_analysis.png`).

### Amanuensis Automation Bot (`Amanuensis.py`)
A Discord bot that monitors local directories for WAV files, automatically downmixes and compresses them to MP3s, uploads them to `#works-in-progress`, and triggers background transient analysis video generation.

---

## 3. Real-Time Transcription & Poetic Analysis Suite (`transcription/`)

Provides real-time voice-to-text capabilities for Discord voice channels with phonetic syllabification, custom IPA weak-vowel overrides, and AI-driven poetic analysis.

### Aqua Voice Transcription Bot (`transcription_bot_aqua.py`)
- **DAVE Decryption**: Handles Discord's End-to-End Encryption (DAVE) using custom `discord.ext.voice_recv` patches with AES-GCM decryption and sequence number handling.
- **Voice Activity Detection**: Uses RMS-based VAD with a 300ms pre-roll lookahead buffer and a 10-second accumulated audio threshold.
- **Phonetic IPA & Syllabification**: Uses `eng-to-ipa` and Maximal Onset Principle (MOP) syllabification, enhanced by over 8,000 custom weak-vowel IPA overrides (`ipa_overrides.json`) to correct standard CMU schwa reductions.
- **Formatting**: Interleaves phonetic IPA transcriptions below text messages and caps message sizes under 1950 characters to prevent Discord HTTP 400 errors.

### Local & Desktop Transcribers (`desktop_transcriber.py` & `desktop_audio.py`)
- `desktop_transcriber.py`: Captures local desktop loopback audio via `soundcard`, downsamples to 16kHz mono, transcribes via Aqua Voice API, and posts to Discord `#world`.
- `desktop_audio.py`: Streams system desktop audio directly into Discord voice channels via a custom `discord.AudioSource`.

### Poetic Analysis & Jev Grammar Reordering (`analyze_transcript.py` & `jev_api.py`)
- **Vowel Histogram & Syllable Matching**: Tracks syllable vowel sounds backward to discover optimal line repetition distances.
- **Couplet Construction & Jev Evaluation**: Reconstructs plain candidate couplets without capitalizations, saves them to `unordered_poem_lines.txt`, and evaluates their grammatical coherence via TypeSafe Jev API (`jev_api.py`).
- **Rhyme Formatting & Confidence Scores**: Rhyming syllables are converted to `UPPERCASE` and non-rhyming syllables to `lowercase` **after** Jev API reorders the poem couplets into `ordered_poem_lines.txt`. Each displayed couplet includes its Jev confidence score e.g. `(Jev Confidence: XX.X%)`.
- **Slash Commands**: `/analyze` generates the ranked poem along with Jev confidence scores, `/purge` resets channel history and in-memory histogram state.

### Batch Launcher (`run_transcription_bot.bat`)
Single double-click Windows batch launcher that verifies dependencies, resolves API keys (`TYPESAFE_API_KEY` / `JEV_API_KEY`), launches `transcription_bot_aqua.py`, and maintains interactive command line context on exit or error.

---

## 4. Additive Sound Synthesis Suite (`additive_synthesis/`)

An interactive additive sound synthesis engine and psychoacoustic analysis framework (`additive_synthesis.py`).

**Features:**
- **Step-by-Step Synthesis**: Synthesizes and visualizes evolving sinusoids step-by-step with LaTeX and compact Sigma notation (`\sum`) displays.
- **Presets**: Includes built-in mathematical presets (`square.json`, `bell.json`, `complex.json`, 'cosmic', 'generative', and 'juicy').
- **Psychoacoustic Analysis Suite**: Includes `test_suite.py` to evaluate Sethares Roughness, Lower Interval Limit (LIL) violations, low-frequency beating, and decay-frequency coupling.

**Usage:**
```bash
python3 additive_synthesis/additive_synthesis.py
python3 -m unittest additive_synthesis/test_suite.py
```

---

## Installation & Dependencies

### System Dependencies
```bash
sudo apt-get update && sudo apt-get install -y libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev ffmpeg rubberband-cli
```

### Python Environment
```bash
pip install librosa numpy scipy matplotlib soundcard soundfile discord.py[voice] discord-ext-voice-recv davey cryptography faster-whisper google-genai torch transformers mido plotly playwright openai requests tqdm nltk syllables SoundsLike pydub eng-to-ipa scikit-learn Cython send2trash
```

### Linguistic Data
```bash
python3 -m nltk.downloader cmudict averaged_perceptron_tagger
```

---

## Configuration

Place a `credentials.json` file in the root or `transcription/` directory:
```json
{
  "token": "YOUR_DISCORD_BOT_TOKEN",
  "aqua_key": "YOUR_AQUAVOICE_API_KEY",
  "typesafe_api_key": "YOUR_TYPESAFE_JEV_API_KEY"
}
```
Or set environment variables: `TYPESAFE_API_KEY`, `JEV_API_KEY`, `JULES_API_KEY`, `AQUA_KEY`.
