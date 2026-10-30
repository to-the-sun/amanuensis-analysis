# Audio Analysis, Transcription, and Synthesis Suite

This repository contains a comprehensive suite of tools for audio signal processing, transient analysis, continuous pitch tracking, stem alignment, real-time Discord voice transcription, and additive sound synthesis.

The project is organized into three primary subdirectories and root utility scripts:
1. `analysis/` – Rhythmic energy tracking, spectral transient analysis, pitch stem separation, group stem pattern analysis, dominant pitch tracking, audio feature clustering, and Discord bot automation.
2. `transcription/` – Real-time Discord voice-to-text transcription (Aqua Voice API), phonetic syllabification, custom IPA weak-vowel overrides, backward vowel histogram analysis, and AI-driven poem generation (TypeSafe Jev API).
3. `additive_synthesis/` – Interactive additive sound synthesis engine, LaTeX/Sigma notation displays, preset management, psychoacoustic analysis test suite, and generative sound creation.
4. `analysis/flux_separator.c` / `analysis/flux_separator.exe` – C-based Spectral Flux Pitch Separator focusing strictly on deriving per-pitch-stem flux envelopes, modified flux envelopes, peak/trough identification, companion flux WAV exports, and interactive HTML flux reports (when `--debug` argument is enabled). Supports single-file, multi-file drag-and-drop, and automatic directory scanning with multi-threaded concurrent CPU execution.
5. `tune_stems.py` – Continuous interval-relative pitch alignment for song stems.

---

## 1. Perfect Reconstruction Pitch Stem Separator (`analysis/flux_separator.c`)

Separates an audio file into individual MIDI integer pitch stems ($0..127$, representing C-1 to G9) using a Short-Time Fourier Transform (STFT) COLA filter bank. Every detected pitch present in the audio file is exported to a separate 16-bit PCM WAV file inside a `pitch_stems/` directory.

**Key Features:**
- **Perfect Reconstruction & Active Duration Filtering**: Summing all exported pitch stems sample-by-sample reconstructs the original audible notes perfectly. Applies a segmented noise gate on each separated pitch stem audio, zeroing out any audio segment bounded on either side by at least 1 millisecond of silence if it never rises above the global 19% noise floor ratio (`g_noise_floor_ratio`), and exporting WAV files for stems with at least $99\text{ ms}$ of active frames.
- **Companion Transience (Flux) WAV Output (`--debug`)**: When the `--debug` flag is passed, `flux_separator` calculates frame-by-frame spectral flux throughout time for every exported pitch stem (`<basename>_pitch_<03d>_<note_name>.wav`) and exports a matching companion WAV file stem (`<basename>_pitch_<03d>_<note_name>_flux.wav`) containing the transience measure waveform normalized against the individual stem's peak flux. In addition, `flux_separator` evaluates peak salience scores (counting peaks iterated in either direction before finding an equal or greater peak), normalizes salience scores using a thinned percentile rank within the ordered list of unique salience integers (`rank / num_unique`), and multiplies each sample in the hill between troughs of its respective peak by the peak's normalized salience score to generate `<basename>_pitch_<03d>_<note_name>_modified_flux.wav`. For each analyzed audio file, `flux_separator` also recombines all modified flux pitch stems into a single buffer, normalizes its peak amplitude to 1.0, and exports it as `<basename>_recombined_modified_flux.wav` in the `[flux]` subfolder. All generated outputs land in a unified `pitch_stems/` directory and `pitch_stems/[flux]/` subdirectory, using `<basename>` in output filenames (`<basename>_pitch_<03d>_<note_name>_modified_flux.wav`, `<basename>_pitch_<03d>_<note_name>_flux.wav`, `<basename>_recombined_modified_flux.wav`, `<basename>_flux_waveform_data.js`, `<basename>_index.html`) to prevent filename collisions. In multi-file or directory mode, a master batch dashboard report is generated in `pitch_stems/[flux]/index.html`. If `--debug` is omitted (default), the `[flux]` subfolder and associated flux calculations/reports are not created.
- **Double-Click & Drag-and-Drop (Multi-File & Multithreaded)**:
  - **Multi-File Drag & Drop**: Drag single or multiple WAV files (or folders) directly onto `flux_separator.exe` or pass multiple file paths as arguments.
  - **Double-Click Folder Scanning**: Double-clicking `flux_separator.exe` without arguments automatically scans its current folder for all WAV files (ignoring generated stems, `preview.wav`, and `glued.wav`) and processes them simultaneously using POSIX worker threads across all available CPU cores.
  - **Automatic Helper File Moving**:
    - When `flux_separator.exe` runs, it searches its folder for `analyze_group.bat`, `analyze.exe`, and `song_builder.exe` and automatically moves these files into the newly created `pitch_stems/` folder if found.
    - When `analyze_group.bat` is executed inside `pitch_stems/`, it executes group stem transience analysis and then searches its folder for `song_builder.exe` and moves it into the newly created `[palettes]/[stems]/` folder.

**Compilation & Usage:**
```bash
# Build native Linux/macOS binaries and cross-compile Windows executables in analysis/:
make

# Usage:
./analysis/flux_separator file1.wav file2.wav ... # Pitch Stem Separation (Linux / macOS)
analysis/flux_separator.exe file1.wav file2.wav   # Pitch Stem Separation (Windows)
analysis/flux_separator.exe file1.wav --debug     # Enable [flux] subfolder & HTML reports
./analysis/flux_separator                         # Double-click / scan current directory for all WAVs
```

---

## 2. Stem Tuning & Alignment (`tune_stems.py`)

Performs high-quality, continuous, time-varying pitch alignment of song stems relative to a "00" prepended base stem (e.g. `00_bass.wav`). Uses `librosa` for continuous pitch tracking, temporal Gaussian smoothing, and calls `rubberband` with `--pitchmap` to execute time-varying pitch shifting while preserving multi-channel layouts and song duration.

**Usage:**
```bash
python3 tune_stems.py --stem-dir "path/to/stems" [options]
```

---

## 3. Audio Analysis Suite (`analysis/`)

Focuses on identifying rhythmic transient energy, dominant pitch tracking, structural audio patterns, stem pattern correlation, and feature clustering.

### Interactive Transient Reports & Pattern Analysis (`analyze`)
Pure C offline cumulative transience analyzer (`analyze` / `analyze.exe`) generating palette pattern maps, loop/stem WAV slices, JSON manifests, and interactive HTML reports.

### Group Analysis & Song Building (`analyze_group.bat` & `song_builder`)
- `analyze_group.bat`: Executes multi-file grouped stem analysis across all stems in a palette directory, automatically moving `song_builder.exe` into `[palettes]/[stems]/`.
- `song_builder`: Evaluates cross-stem correlation metrics inside `[stems]/`, copies selected pattern stems into `[song]/`, builds `[song]/finished_song.wav`, and exports an interactive correlation report (`song_correlation_report.html`).

### Pitch Tracking & Tuning Regression (`pitch_tracker.py`)
Analyzes polyphonic recordings for continuous dominant pitch and steady tuning regression over time using YIN, pYIN, or PipTrack algorithms. Outputs high-resolution 2-panel visual plots (`.png`) with spectrogram overlays and fractional MIDI gridlines, as well as detailed CSV exports with cent deviations (`+12c`, `-34c`).

**Usage:**
```bash
python3 analysis/pitch_tracker.py "path/to/audio.wav" --algo piptrack
```

### Feature Extraction & Archetype Clustering (`generate_and_cluster.py`)
Generates structured test audio files, extracts MFCCs, spectral centroid, spectral flatness, chroma, and onset-envelope autocorrelation features, and exports PCA and t-SNE projection visualizations (`cluster_analysis.png`).

---

## 4. Real-Time Transcription & Poetic Analysis Suite (`transcription/`)

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

## 5. Additive Sound Synthesis Suite (`additive_synthesis/`)

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
