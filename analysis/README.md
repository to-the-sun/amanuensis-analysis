# Standalone Offline Cumulative Transience Analyzer & Exporter

This directory contains the pure C implementation of the offline transience analyzer and HTML report exporter (`analyze`), pitch stem spectral flux separator (`flux_separator`, with `flux_separator_debug.bat` for debug runs), on-demand spectral flux report builder (`flux_report`), pattern song correlation builder (`song_builder`), and group analysis launcher (`analyze_group.bat`).

## Architecture

The C pipeline operates in two passes without Python dependencies:

1. **Pass 1 (15,000ms Rolling Window)**:
   - Performs transience analysis across 4 frequency bands using a 15,000ms window (`pass1_window_ms = 15000`).
   - Determines the longest stable high point (`best_bar_length_ms`).
   - Tracks the accumulated transience contour histogram across the song to determine the segment length high point.

2. **Pass 2 (Adaptive Window = `max(5000, min(15000, best_bar_length * 2))`)**:
   - Re-analyzes the audio with the optimized window size for peak detection, pattern identification, and segment scoring.
   - Leaves the audio where it is and writes no copy of the target WAV into the destination folder; the interactive HTML report plays the resolved original audio file in place (a relative reference to the on-disk file) whenever `find_original_audio_for_stem` locates one. If no original audio is resolved, the report is written with an empty audio player and a warning is printed, since there is no local copy to fall back on.
   - Scans for and copies any matching text files (located in the audio file folder) whose names match the audio file except starting with "passes" instead of "palette" into the destination folder.
   - Evaluates pattern overlaps against parsed pass interval boundaries (`[passes[i].loop_point_ms, passes[i+1].loop_point_ms]`), assigning each pattern to the pass with maximum temporal overlap (breaking ties in favor of earlier passes).
   - Computes raw pattern start offsets relative to assigned pass loop points/pads and applies an unconditional offset shift (`shift_ms = -min_blank_space`) so the earliest pattern aligns with sample zero in exported stem WAV files in `[stems]` while preserving exact relative cross-pattern timing alignment.
   - Exports binary assets (`.ctbin`, `snapshots.bin`), Base64 snapshot fallbacks (`snapshots.js`), structured JSON data (`manifest.json`, `report_data.json`, `report_data.js`), pattern WAV slices, and an interactive HTML report (`<stem>.html`).

## Grouped Analysis & Multithreading Architecture (`--group` / `-g`)

When invoked with `--group` (or `-g`), the analyzer executes multi-file grouped stem analysis across all WAV files in a target palette directory, computing shared cross-stem transience metrics while maximizing multi-core CPU and GPU utilization. An optional argument `--original <path>` (or `-o <path>`) allows specifying the absolute or relative path to an original audio file.

When searching for original audio files to extract audio from for creating `[stems]` and `[loops]`, the analyzer inspects parent folders level-by-level starting at the parent directory above the folder containing `analyze.exe`. It looks for a WAV file whose whole name (minus extension) matches the beginning of the stem WAV file in question (case-insensitive). If a match is found in a parent directory level, higher parent directories are forgone. Matching `passes` text files associated with dynamically located original audio files are scanned and copied next to the report that consumes them (the group report folder, or the standalone report folder); individual per-stem palette folders do not receive copies, because their pass intervals come from the group report.

1. **Parallel Stem WAV Decoding**:
   - Spawns CPU worker threads (`_beginthreadex` on Windows, `pthread_create` on POSIX) to decode PCM audio frames and perform stereo-to-mono downmixing across all stems concurrently.
2. **Parallel STFT Envelope Pre-Computation**:
   - Dispatches STFT spectral flux envelope calculation for all stems in parallel across CPU worker threads and OpenCL GPU compute queues (`gpu_stft_process`).
3. **Thread-Safe Lockstep Chunk Processing**:
   - Pass 1 and Pass 2 time-step chunk loops execute concurrently across stems, with thread synchronization (`GroupMutex`: `CRITICAL_SECTION` / `pthread_mutex_t`) protecting shared accumulated transience updates (`SharedTransientBuffer`).
4. **Parallel Per-Stem Asset Export & Group Pattern Combination**:
   - Concurrently exports `.ctbin`, `snapshots.bin`, `snapshots.js`, `manifest.json`, `report_data.json/js`, and `<stem>.html` for all stems (omitting common stem name prefixes from created stem output directories to keep path lengths short).
   - Collects all patterns derived by individual audio files (stems) and groups stems sharing the same original audio file. Overlapping pattern intervals are combined only among stems that share an original audio file.
   - Slicing of combined isolated loop WAVs (`[palettes]/[loops]/pattern_N.wav`) and pass-aligned stem WAVs (`[palettes]/[stems]/pattern_N.wav`) is performed directly into the overall `[loops]` and `[stems]` folders inside `[palettes]`, with audio samples extracted from each group's original audio file (or summed fallback for that group) without audio doubling. Individual stem output folders do not create separate `[loops]` or `[stems]` subdirectories, and pattern WAV slicing at the group level report is omitted.
   - Modified flux envelopes are computed for every stem and recombined into `<basename>_recombined_modified_flux.wav` in the `[flux]` subdirectory, normalized to their recombined peak amplitude; that file is the only `[flux]` output a normal run produces. The per-stem companions (`<basename>_pitch_<03d>_<note_name>_modified_flux.wav`, normalized against the individual stem's peak flux and scaled by thinned percentile rank peak salience) and the stored raw envelopes are debug artifacts, written only when `--debug` is passed, which `flux_separator_debug.bat` does when double-clicked. A debug run also stores each source file's raw envelopes and the peak/trough sample indices detected on them in `[flux]/<basename>_flux_envelopes.bin` (`FLUXENV1` header: magic, sample rate, total samples, channel count, stem count; then per stem its pitch, peak indices, trough indices and 16-bit mono raw envelope), paying the extra marker detection and writes that needs. The interactive report and its `<basename>_flux_waveform_data.js` data are written during neither mode: `flux_report.exe` builds them on demand from the stored data, the `_modified_flux.wav` companions and the original audio (see below).
5. **Per-Group HTML Reports Keyed to Original Audio**:
   - Stems are grouped by the original audio file located for each of them with `find_original_audio_for_stem` (the same parent-directory lookup used for `[stems]`/`[loops]` slicing). If that lookup fails, the `--original <path>` argument is used; if that supplies no path either, an error is printed for the group and no group report is written for it.
   - One interactive HTML report is generated per original audio file, uniquely named `group_<original audio basename>_pattern_analysis.html` (suffixed with a number if a name would collide) inside its own `[palettes]/group_<original audio basename>_pattern_analysis/` folder so per-group companion assets never overwrite each other.
   - Group reports play the original audio in place: the report's `<audio>` source is a relative reference to the located original audio file on disk, so no copy or re-rendered group WAV is written next to the HTML report.
   - The original audio file for every stem is resolved once, up front, before any stem assets are exported; stem exports, pattern slicing and group reports all reuse those resolved paths.
   - Each unique resolved audio path copies its matching `passes` text files exactly once into the folder whose report parses them (the group report folder; per-stem palette folders are skipped because their exports do not parse passes), and each unique original audio file is decoded once into a shared mono buffer reused by every stem and group report that references it (no repeated WAV reads or allocations).
   - The summed-stem fallback audio is only reconstructed when a group actually has no resolved original audio file, so `Original audio file not found. Reconstructing group audio by summing stems...` no longer appears when originals were found.
   - Individual per-stem HTML reports also play their resolved original audio file in place, even though the rest of each report still analyzes that stem's flux audio. Per-stem report folders no longer receive a copy of the stem WAV, and a stem whose audio cannot be resolved gets a report with an empty audio player instead of a local-file fallback.
6. **Thread Task Delegation & Fine-Grained Console Progress Indicators**:
   - Thread task delegation logging outputs real-time console messages (`[Thread X/Y] Starting task: ...`) as worker threads pick up tasks.
   - Thread-safe console progress bar rendering (`print_progress_bar`) displays fine-grained step/frame-level percentage indicators across all pipeline stages (Decoding, Pass 1, STFT, Pass 2, Asset Export, Group Mixing, Group Report).

## Executable Helper File Copying Workflow

1. **`flux_separator.exe`**:
   - When executed, `flux_separator.exe` searches its folder for `analyze_group.bat`, `analyze.exe`, `song_builder.exe`, and `flux_report.exe`, and copies any of these helper files found into the newly created `pitch_stems` directory. The originals are left in place.

2. **`analyze.exe` (group mode)**:
   - `analyze.exe --group` creates `[palettes]` and `[palettes]/[stems]`, and copies `song_builder.exe` from next to `analyze.exe` into `[palettes]/[stems]` as soon as that folder exists. The original is left in place, and if no `song_builder.exe` is found the run notes it and continues. `analyze_group.bat` only launches the analyzer and no longer copies anything.

## On-Demand Flux Reports (`flux_report` / `flux_report.exe`)

Separating audio never writes the multi-gigabyte `[flux]/<basename>_flux_waveform_data.js` data file or the report that loads it. A normal run leaves only the recombined flux WAV in `[flux]`; a `--debug` run additionally stores each source file's raw flux envelopes and peak/trough markers in `[flux]/<basename>_flux_envelopes.bin`. `flux_report.exe` is copied into `pitch_stems/` either way, so it can be used as soon as there is report data to read.

Double-click `flux_report.exe` inside `pitch_stems/` (or run `./flux_report` there) when the reports are wanted. For every source file it finds stored report data for, it reads that data, the stem's `_modified_flux.wav` companion and the original audio (looked for in `pitch_stems/` and one folder up), then writes `[flux]/<basename>_flux_waveform_data.js`, `[flux]/<basename>_index.html`, and finally the `[flux]/index.html` batch dashboard. Running it again rebuilds the reports from the same on-disk sources; the stored report data itself is not rewritten.

## Existing Pitch Stem Skip Check

Before a WAV file is decoded, `flux_separator` checks whether that file's output already exists. A normal run writes only `[flux]/<basename>_recombined_modified_flux.wav`, so that file alone marks the work as done and the file is skipped. A `--debug` run writes the per-stem `_modified_flux.wav` companions and `[flux]/<basename>_flux_envelopes.bin` as well, and requires both before skipping, so a folder separated without debug is separated again when the debug artifacts are wanted. The console reports the file as skipped rather than complete or failed, and files without existing output in the same batch are still processed normally.

## Compilation

To compile the analysis executables:

- **Linux**: `make` (produces `analyze`, `song_builder`, `flux_separator`, and `flux_report`)
- **Windows (64-bit Cross-Compilation)**:
  - `x86_64-w64-mingw32-gcc -O3 -I. -Wall -Wextra analyze.c exporter.c cumulative_transience.c crossfade.c -o analyze.exe -lm -lpthread -lOpenCL`
  - `x86_64-w64-mingw32-gcc -O3 -I. -Wall -Wextra song_builder.c -o song_builder.exe -lm`
  - `x86_64-w64-mingw32-gcc -O3 -Wall -std=c99 -I. -pthread flux_separator.c flux_shared.c flux_envelope.c -o flux_separator.exe -lm`
  - `x86_64-w64-mingw32-gcc -O3 -Wall -std=c99 -I. flux_report.c flux_report_html.c flux_shared.c -o flux_report.exe -lm`

## Executables Summary

- **`flux_separator` / `flux_separator.exe`**: Multi-threaded C pitch separator writing each source file's recombined modified flux WAV. Pass `--debug` (or double-click `flux_separator_debug.bat`) to also write the per-stem modified flux WAVs and the stored raw-envelope report data that `flux_report` consumes. Automatically copies `analyze_group.bat`, `analyze.exe`, `song_builder.exe`, and `flux_report.exe` into `pitch_stems/`.
- **`flux_separator_debug.bat`**: Double-click launcher for `flux_separator.exe --debug`. It runs the separator from its own folder, so every WAV beside it is separated with the debug artifacts enabled; keep it next to `flux_separator.exe`.
- **`flux_report` / `flux_report.exe`**: Builds the on-demand `[flux]` reports (JS waveform data, per-file HTML reports, batch dashboard) from the stored envelopes, modified flux WAVs, and original audio. Double-clicked inside `pitch_stems/`.
- **`analyze` / `analyze.exe`**: C cumulative transience analyzer generating palette pattern maps, loop/stem WAV slices, JSON manifests, and interactive HTML reports.
- **`analyze_group.bat`**: Batch launcher for group stem analysis. Launches `analyze.exe --group`, which copies `song_builder.exe` into `[palettes]/[stems]/` itself, leaving the original in place.
- **`song_builder` / `song_builder.exe`**: Evaluates correlation across candidate patterns using recombined flux audio envelopes. All prospective patterns in a potential song are added together sample index by sample index to form a summed envelope. The standard deviation across samples where more than one pattern is present (a pattern alone with itself does not count towards standard deviation) serves as the overall correlation metric. Patterns are sorted descending by active length and evaluated sequentially in a unified loop: starting with the longest pattern (zero correlation being better than null/uninitialized correlation), each subsequent candidate pattern in descending length order is tested and accepted if it increases overall standard deviation correlation, or rejected if it decreases or maintains correlation. Selected pattern WAV files are copied into `[stems]/[song]`, mixed into `[song]/finished_song.wav` normalized to peak 1.0 (omitting leading zero samples prior to the first non-zero sample so that the first non-zero sample is at index zero), and exported with an interactive HTML report (`song_correlation_report.html`, displaying patterns in the order they were judged, descending from longest to shortest) and companion JS data file (`song_correlation_waveform_data.js`).
