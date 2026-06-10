# Instructions for Designing a New Sound

When asked to "design a new sound by following the instructions in the `new_sound.md` file," follow this process to ensure the new sound is perceptually distinct and correctly integrated into the library. Work entirely within the `sound_design/` directory. Everything you need can be found within it.

## Goal
The primary objective is to design a sound that is **as perceptually different as possible** from all existing sounds in the `sounds/` library. Focus on "human-perceptible" differences in timbre, dynamics, and texture.

## Step-by-Step Process

### 1. Survey the Existing Library
- Browse the `sounds/` directory.
- Review the `analysis.json` files in each subfolder.
- Pay attention to the `average_spectral_centroid` (brightness), `average_spectral_bandwidth`, and `mfcc_means` (timbral fingerprint).
- Look at the `temporal_data` arrays to understand how existing sounds evolve over time.

### 2. Formulate a Distinct Timbre
- Choose a synthesis strategy that departs from existing ones (e.g., if existing sounds are mostly additive, try FM, subtractive with resonant filters, etc.).
- Aim for a different area of the frequency spectrum or a different temporal envelope.

### 3. Implement the Design
- **Crucial:** Only modify the top-level `sound_design.c` file. **Never** modify the `sound_design.c` files stored inside the `sounds/` subfolders.
- Update the `SOUND_DESIGN_VERSION` macro in `sound_design.h` to the next increment (find the highest numbered folder in `sounds/` and add 1).
- Implement your synthesis logic in the `render_midi` function within `sound_design.c`.

### 4. Standardized Analysis
- Compile the tools using `make`.
- Run `./audio_engine`. This will:
    1. Render the standardized MIDI sequence.
    2. Perform analysis every 50ms (RMS, Spectral Centroid, Bandwidth, Kurtosis, ZCR, MFCCs).
    3. Calculate the "Distance" from other sounds in the library.
    4. Save the `.wav`, the `sound_design.c` copy, and `analysis.json` into the new versioned subfolder.

### 5. Reciprocal Library Maintenance
- After generating a new sound, the older sounds' `analysis.json` files will not yet know their distance to this new sound.
- Run `./migrate_analysis` to re-analyze the entire library. This ensures every sound's `analysis.json` contains a complete `distances` dictionary reflecting its relationship to all other versions, including the one you just created.

## Technical Constraints & Format
- **Language:** C (C99 or later).
- **Dependencies:** `libsndfile`, `aubio`, `json-c`, `fftw3`.
  - On Debian/Ubuntu: `sudo apt-get install libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev`
- **Temporal Analysis:** 50ms hop/window.
- **Data Format:** `temporal_data` must be a dictionary of arrays (e.g., `{"times": [...], "rms": [...]}`).
- **MIDI Consistency:** Always use the same MIDI sequence for all sounds to ensure a fair "timbre" comparison.
- **Distance Metric:** The system uses Euclidean distance on MFCC means as the primary "difference" score.
- **MIDI Handling:** Modules must be polyphonic and correctly handle sustained notes (e.g., by rendering any notes remaining in `active_notes` at the end of the `duration` without a release phase). This ensures compatibility with both listed sequences and potential live MIDI streams.

## Subjective Judgment
While the distance metric provides a quantitative guide, prioritize **human perception**. If two sounds have a high statistical distance but sound similar to a person, iterate further on the design to achieve true variety.
