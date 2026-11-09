# Analyze Palettes

Single high-performance C executable for analyzing audio palettes, finding repeated patterns and loop segment periodicities, and exporting interactive HTML waveform reports.

## Features

- **Single Executable (`analyze_palettes.exe`)**: Written in C for maximum efficiency and execution speed.
- **Drag-and-Drop Single File**: Drag any audio WAV file directly onto the executable to generate its pattern analysis report.
- **Drag-and-Drop Multiple Files**: Drag multiple WAV files simultaneously to process all of them concurrently across CPU cores using worker threads.
- **Double-Click Execution**: Double-clicking the executable automatically scans the directory where the executable is located for all WAV audio files and analyzes them.
- **Interactive Console Window**: Automatically pauses with a prompt upon completion or error so output remains visible when launched via GUI or double-clicking.
- **Interactive Multi-Tier Reports**:
  - Full-file waveform with interactive drag-zooming and right-click zoom-out history.
  - Pattern regions and segment playback chips.
  - Native audio player with byte-range seeking.
  - Base64 int16 multi-tier chunking (`summary.js` at coarse step 4096; fine tiers `l8`, `l64`, `l512` in 65536-bin chunks).
  - Comprehensive index page linking all generated reports.

## Build

To compile with MinGW cross-compiler for Windows:

```bash
make analyze_palettes.exe
```

Or natively on Linux:

```bash
make analyze_palettes
```
