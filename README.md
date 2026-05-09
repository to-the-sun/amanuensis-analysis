# Transient Analysis Tool

This tool analyzes audio files to extract transient information and generate an interactive HTML report. It focuses on identifying rhythmic energy and structural patterns within the audio.

## Analysis Pipeline

The script performs the following steps for each audio file:

### 1. Audio Loading
The script uses `librosa` to load audio files. It preserves the original sampling rate (`sr=None`) to ensure high-fidelity analysis of transient peaks.

### 2. Onset Strength (Transient Envelope)
Instead of just looking at raw amplitude, the tool calculates **Onset Strength**. This represents the spectral energy flux across frames. It is a robust way to detect transients because it captures changes in the frequency content (e.g., a drum hit or a note attack) even if the overall volume doesn't change significantly.

The analysis is conducted with a high temporal resolution of **20 milliseconds** per chunk, ensuring that even very rapid transients are captured and represented.

### 3. Peak Detection
Using `scipy.signal.find_peaks`, the script identifies specific points in time where the transient envelope has a local maximum with a minimum prominence of 0.5. These peaks correspond to the "attacks" or "hits" in the audio.

### 4. Self-Similarity Matrix (SSM)
The script generates a high-resolution Self-Similarity Matrix based on the transient envelope.

#### What is a Self-Similarity Matrix?
A Self-Similarity Matrix (SSM) is a powerful tool for visualizing the structure of a signal. It is a square matrix where the entry at position $(i, j)$ represents how "similar" the signal at time $i$ is to the signal at time $j$.

In this tool, we construct the SSM as follows:
1. The transient envelope is analyzed at its full **20ms resolution**.
2. We compute the pairwise distance between every possible pair of time points: $D_{i,j} = |x_i - x_j|$.
3. We convert this distance into a similarity score: $S_{i,j} = 1 - \frac{D_{i,j}}{\max(D)}$.
4. For performance and to prevent browser instability with large datasets, the SSM is rendered as a **Base64-encoded PNG image** directly in the Python script.

#### How to Interpret the SSM
- **The Main Diagonal:** You will always see a bright diagonal line from the bottom-left to the top-right. This represents the signal compared to itself at the same moment ($i=j$), which always has perfect similarity.
- **Off-Diagonal Patterns:** These are the most interesting parts.
    - **Checkerboard/Block Patterns:** Large blocks of similar color indicate segments of the audio with consistent energy levels or textures.
    - **Parallel Diagonals:** Lines parallel to the main diagonal indicate **repetition**. If you see a line offset from the main diagonal, it means the rhythmic pattern at time $i$ is repeating at time $i + \text{offset}$.
    - **Grid-like Dots:** In highly rhythmic or percussive music (like a drum loop), you will see a grid of dots. These dots represent the alignment of transient hits (e.g., every beat or every snare hit).

#### Why it matters
The SSM allows you to see the "DNA" of the audio's rhythm. It reveals structural repetitions and rhythmic consistency that might be hard to see in a standard waveform or transient graph.

## Usage

1. **Install Dependencies:**
   Ensure you have the following Python packages installed:
   - `librosa`
   - `numpy`
   - `scipy`
   - `matplotlib`
   - `soundcard`
   - `soundfile`

   You can install them via pip:
   ```bash
   pip install librosa numpy scipy matplotlib soundcard soundfile
   ```

2. **Run the Script:**
   ```bash
   python analyze_transients.py --dir /path/to/audio --output report.html
   ```

3. **View the Report:**
   Open the generated `.html` file in any modern web browser. Use the dropdown to switch between audio files and use the built-in player to listen while watching the playhead move across the graphs.

## Interactive Features
- **Synchronized Playhead:** The orange dashed line on both the Transient Graph and the SSM moves in real-time as you play the audio.
- **High-Resolution SSM:** By rendering the SSM as an image, we maintain the 20ms analysis resolution without sacrificing browser performance.
- **SSM Crosshair:** On the SSM, the playhead appears as a crosshair, showing you exactly which time-pairs are being compared as the audio progresses.
- **Zoom/Pan:** Both graphs are interactive (powered by Plotly.js), allowing you to zoom into specific sections of the transient envelope.
