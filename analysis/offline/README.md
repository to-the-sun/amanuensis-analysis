# Standalone Offline Cumulative Transience Analyzer & Exporter

This directory contains the pure C implementation of the offline transience analyzer and HTML report exporter (`ct_analyze_headless`).

## Architecture

The C pipeline operates in two passes without Python dependencies:

1. **Pass 1 (15,000ms Rolling Window)**:
   - Performs transience analysis across 4 frequency bands using a 15,000ms window (`pass1_window_ms = 15000`).
   - Determines the longest stable high point (`best_bar_length_ms`).
   - Tracks the accumulated transience contour histogram across the song to determine the segment length high point.

2. **Pass 2 (Adaptive Window = `max(5000, min(15000, best_bar_length * 2))`)**:
   - Re-analyzes the audio with the optimized window size for peak detection, pattern identification, and segment scoring.
   - Copies the target audio file into the destination folder so the interactive HTML report plays the local copied instance.
   - Scans for and copies any matching text files (located in the audio file folder) whose names match the audio file except starting with "passes" instead of "palette" into the destination folder.
   - Exports binary assets (`.ctbin`, `snapshots.bin`), Base64 snapshot fallbacks (`snapshots.js`), structured JSON data (`manifest.json`, `report_data.json`, `report_data.js`), pattern WAV slices, and an interactive HTML report (`<stem>.html`).

## Grouped Analysis & Multithreading Architecture (`--group` / `-g`)

When invoked with `--group` (or `-g`), the analyzer executes multi-file grouped stem analysis across all WAV files in a target palette directory, computing shared cross-stem transience metrics while maximizing multi-core CPU and GPU utilization. An optional argument `--original <path>` (or `-o <path>`) allows specifying the absolute or relative path to an original audio file.

When searching for original audio files to extract audio from for creating `[stems]` and `[loops]`, the analyzer inspects parent folders level-by-level starting at the parent directory above the folder containing `ct_analyze_headless.exe`. It looks for a WAV file whose whole name (minus extension) matches the beginning of the stem WAV file in question (case-insensitive). If a match is found in a parent directory level, higher parent directories are forgone. Matching `passes` text files associated with dynamically located original audio files are scanned and copied into destination palette directories.

1. **Parallel Stem WAV Decoding**:
   - Spawns CPU worker threads (`_beginthreadex` on Windows, `pthread_create` on POSIX) to decode PCM audio frames and perform stereo-to-mono downmixing across all stems concurrently.
2. **Parallel STFT Envelope Pre-Computation**:
   - Dispatches STFT spectral flux envelope calculation for all stems in parallel across CPU worker threads and OpenCL GPU compute queues (`gpu_stft_process`).
3. **Thread-Safe Lockstep Chunk Processing**:
   - Pass 1 and Pass 2 time-step chunk loops execute concurrently across stems, with thread synchronization (`GroupMutex`: `CRITICAL_SECTION` / `pthread_mutex_t`) protecting shared accumulated transience updates (`SharedTransientBuffer`).
4. **Parallel Per-Stem Asset Export & Group Pattern Combination**:
   - Concurrently exports `.ctbin`, `snapshots.bin`, `snapshots.js`, `manifest.json`, `report_data.json/js`, and `<stem>.html` for all stems (omitting common stem name prefixes from created stem output directories to keep path lengths short).
   - Collects all patterns derived by individual audio files (stems) and groups stems sharing the same original audio file. Overlapping pattern intervals are combined only among stems that share an original audio file.
   - Slicing of combined isolated loop WAVs (`[palettes]/[loops]/pattern_N.wav`) and pass-aligned stem WAVs (`[palettes]/[stems]/pattern_N.wav`) is performed directly into the overall `[loops]` and `[stems]` folders inside `[palettes]`, with audio samples extracted from each group's original audio file (or summed fallback for that group) without audio doubling. Dynamic amplitude-based crossfading (`crossfade_process`) is applied to pattern stem WAV generation in `[stems]` using `s1` as the active pattern audio signal and `s2` as silence to ensure smooth fade-in and trailing fade-out without muting output samples. Individual stem output folders do not create separate `[loops]` or `[stems]` subdirectories, and pattern WAV slicing at the group level report is omitted.
   - Companion raw flux WAV files (`<basename>_pitch_<03d>_<note_name>_flux.wav`), modified flux WAV files (`<basename>_pitch_<03d>_<note_name>_modified_flux.wav`), and recombined modified flux WAV files (`<basename>_recombined_modified_flux.wav`) are exported to the `[flux]` subdirectory (normalized against the individual stem's peak flux or recombined peak amplitude, with modified flux envelopes scaled by thinned percentile rank peak salience) along with an interactive report (`[flux]/index.html`) displaying peak (top) and trough (bottom) hash marks.
5. **Combined Group Mix & Report**:
   - Uses an original audio file if explicitly specified via `--original <path>` for `group_<name>.wav` and group report analysis (`group_<name>_pattern_analysis.html`). If `--original` is not specified or cannot be located, it falls back to reconstructing the group audio signal by summing all available input stems and normalizing the peak amplitude.
6. **Thread Task Delegation & Fine-Grained Console Progress Indicators**:
   - Thread task delegation logging outputs real-time console messages (`[Thread X/Y] Starting task: ...`) as worker threads pick up tasks.
   - Thread-safe console progress bar rendering (`print_progress_bar`) displays fine-grained step/frame-level percentage indicators across all pipeline stages (Decoding, Pass 1, STFT, Pass 2, Asset Export, Group Mixing, Group Report).

## Generated HTML Report Layout

The interactive report (`<audio_stem>.html`) features:

1. **Section 1: Interactive Waveform & Pattern Map**:
   - Overall audio waveform, color-coded pattern spans, and audio seek interactions.

2. **Section 2: Zoomed-In Segment Inspector**:
   - Vertically stacked segment panels (`boxPrev`, `boxCurr`, `boxNext`).
   - Dynamic pattern color-coding: segment borders dynamically highlight using the color of the pattern they belong to, and cleanly revert back to neutral borders when a pattern ends.

3. **Section 3: Real-Time Accumulated History Buffer**:
   - Real-time history buffer graph updating during audio playback with peak markers, qualification lines, tolerance bands, and drag-to-zoom controls.

## Compilation

To compile the standalone analyzer executables:

- **Linux**: `make` (produces `ct_analyze_headless`)
- **Windows (64-bit Cross-Compilation)**: `x86_64-w64-mingw32-gcc -O3 -I. -Wall -Wextra ct_analyze_headless.c ct_exporter.c cumulative_transience.c -o ct_analyze_headless.exe -lm`
