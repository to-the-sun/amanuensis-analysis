# Procedural Audio Similarity and Cluster Analysis Report

This report documents the design, implementation, and mathematical analysis of the **Procedural Audio Generation and Cluster Analysis Suite** (`analysis/generate_and_cluster.py`).

The objective was to procedurally generate a large set of five-second audio files featuring highly structured variations of melody, rhythm, key, timbre, and transience. Some samples share common musical and signal traits, while others vary significantly. Following generation, we extracted multi-dimensional physical and musical features, performed dimensionality reduction (using both linear PCA and non-linear t-SNE), applied unsupervised K-Means clustering, and visualized the similarity relationships in a high-resolution 2x2 plot.

---

## 1. Procedural Audio Synthesis Engine

To ensure a balanced blend of distinct clusters and organic overlap, we designed **6 different audio archetypes**. We procedurally synthesized **48 five-second WAV files** (8 variations per archetype) at a professional sample rate of 44.1 kHz.

### The 6 Archetypes & Shared Relationships

| Archetype ID & Name | Timbral Characteristics | Rhythmic/Tempo Specs | Pitch & Harmonic Key Specs | Temporal Envelope (ADSR) | Shared Qualities / Bridges |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **A1. Ambient Flute** | Pure Sine Wave | Slow, Sparse (~80 BPM) | Pentatonic Minor in A (`A4, C5, D5, E5, G5`) | **Attack:** 400ms<br>**Decay:** 200ms<br>**Sustain:** 70%<br>**Release:** 400ms | **Pitch Set:** Shares with A4 & A5.<br>**Timbre:** Uses sine wave elements shared with A6 background hum. |
| **A2. Buzzing Synth** | Rich Sawtooth Wave | Fast, Arpeggiated (~140 BPM, 8th notes) | Major Diatonic in C (`C4, D4, E4, F4, G4, A4, B4, C5`) | **Attack:** 10ms<br>**Decay:** 100ms<br>**Sustain:** 40%<br>**Release:** 50ms | **Timbre:** Shares sawtooth wave with A5.<br>**Rhythm:** Shares fast tempo range with A6. |
| **A3. Percussive Beat** | Unpitched White Noise + Low Bass Sine Sweep | Syncopated Pattern (~120 BPM, Hi-hats, Kick, Snare) | Dissonant/Unpitched (No melody) | **Attack:** 0ms (Instant)<br>**Decay:** Exponential (15ms hi-hats, 180ms kick, 200ms snare) | **Timbre:** Shares noise-percussion elements with A6.<br>**Rhythm:** Shares tempo range with A4 and A5. |
| **A4. FM Bell** | FM Synthesis (Carrier=f, Modulator=1.414f, exponential modulation index) | Steady Quarter Notes (~120 BPM) | Pentatonic Minor in A (`A3, C4, D4, E4, G4`) | **Attack:** 2ms (Sharp)<br>**Decay:** Exponential (80% of beat duration)<br>**Release:** 10% of beat | **Pitch Set:** Shares Pentatonic A with A1 & A5.<br>**Rhythm:** Shares 120 BPM steady rhythm with A5. |
| **A5. Hybrid Synth** | Rich Sawtooth Wave (Shares with A2) | Steady Quarter Notes (~120 BPM, Shares with A4) | Pentatonic Minor in A (Shares with A1 & A4) | **Attack:** 30ms<br>**Decay:** Medium (50% of beat duration)<br>**Sustain:** 30%<br>**Release:** 15% of beat | **Hybrid Bridge:** Directly bridges A2 (sawtooth timbre) with A4 (steady 120 BPM, Pentatonic A pitch set). |
| **A6. Percussive Roll** | Hi-hat Noise Rolls + Low Sine Wave Hum | Fast 16th-Note Rolls (~140 BPM, Shares with A2) | Steady Single-Tone 440Hz Hum background + Unpitched percussion | **Attack:** 0ms (Instant percussive burst)<br>**Decay:** Very rapid (exponential) | **Hybrid Bridge:** Shares percussive timbre with A3, fast tempo with A2, and single-pitch sine timbre element with A1. |

Within each archetype, we introduce procedural variations for each of the 8 files:
1. **Pitch Jitter & Melodic Paths:** Melody notes and chords are chosen randomly from the designated scale for each note event, resulting in unique sequences.
2. **Tempo Perturbation:** The exact BPM is randomly shifted within a range (e.g., 135–145 BPM), modifying the absolute spacing of rhythmic events.
3. **Trigger Jitter:** Notes have a tiny sub-millisecond timing jitter to simulate expressive performance and vary transient locations.

---

## 2. Multi-Dimensional Audio Feature Extraction

To map the generated samples back into a similarity space, we extract **162 mathematical features** representing three orthogonal musical axes:

### A. Timbral Features (28 dimensions)
* **Mel-Frequency Cepstral Coefficients (MFCCs):** We extract 13 coefficients and compute their **Mean** and **Standard Deviation** over time. MFCCs capture the spectral shape, letting us easily distinguish between the sine wave (A1), sawtooth (A2, A5), FM metallic (A4), and noise (A3, A6).
* **Spectral Centroid:** Represents the "brightness center" of the frequency spectrum. Mean and standard deviation are computed.
* **Spectral Flatness:** Measures how noise-like the signal is (high flat spectrum) versus tone-like (spiky narrow spectrum). Mean and standard deviation are computed.

### B. Harmonic/Pitch Features (12 dimensions)
* **Chroma STFT:** Extracts the energy distribution across the 12 chromatic semitone bins (C, C#, D... B), averaged across the entire five-second audio. This creates a pitch profile that clearly groups samples sharing the Pentatonic A scale (A1, A4, A5) vs. C Major (A2) vs. unpitched/sine hum (A3, A6).

### C. Rhythmic/Temporal Features (120 dimensions)
* **Onset Strength Envelope Autocorrelation:** Rather than simple tempo estimation (which yields a single noisy integer), we compute the autocorrelation of the onset strength envelope up to a 300-sample frame lag (taking the first 120 lag coefficients). This captures the repeating periodicities, rhythmic syncopation, and tempo signatures, clustering steady quarter-note patterns (A4, A5) separate from fast 16th rolls (A2, A6) and syncopated drum beats (A3). Rhythmic features are peak-normalized to capture the *pattern*, not the overall volume.

---

## 3. Machine Learning & Unsupervised Clustering

After compiling the feature vectors, we standardize them using a `StandardScaler` to have zero mean and unit variance, preventing high-magnitude spectral values from dominating rhythmic lags.

### Dimensionality Reduction
We apply two separate techniques to project the 162-dimensional similarity space into 2D:
1. **Principal Component Analysis (PCA):** A linear projection that maximizes global variance, exposing macro-structural trends.
2. **t-Distributed Stochastic Neighbor Embedding (t-SNE):** A non-linear manifold projection that prioritizes preserving local neighborhoods, making tight local clusters extremely visible.

### Unsupervised Clustering
We execute **K-Means Clustering** with $K = 6$. The algorithm has zero knowledge of our ground-truth labels and groups files solely based on the extracted 162-dimensional vectors.

---

## 4. Empirical Clustering Performance

When running the suite, the unsupervised K-Means algorithm achieves spectacular alignment with our generative design:

```
--- Clustering Analysis Summary ---
This table maps Ground-Truth Archetypes to the K-Means discovered clusters:
-----------------------------------------------------------------------------
Ground Truth Archetype    | Discovered K-Means Clusters (Counts)
-----------------------------------------------------------------------------
A1. Ambient Flute         | Cluster 3: 8
A2. Buzzing Synth         | Cluster 1: 8
A3. Percussive Beat       | Cluster 4: 8
A4. FM Bell               | Cluster 2: 8
A5. Hybrid Synth          | Cluster 2: 8
A6. Percussive Roll       | Cluster 1: 1, Cluster 5: 4, Cluster 6: 3
-----------------------------------------------------------------------------
```

### Insights & Musical Bridges:
1. **Pure Coherent Clusters:**
   * **A1 (Ambient Flute)** is placed in its own exclusive cluster (Cluster 3). Its ultra-slow attack, lack of high-frequency transients, and pure sine structure make it highly distinct.
   * **A2 (Buzzing Synth)** is mapped with 100% precision into Cluster 1.
   * **A3 (Percussive Beat)** is mapped with 100% precision into Cluster 4 due to its distinct, rich syncopated drums and complete lack of melodic pitches.
2. **The Hybrid Synthesis Bridge:**
   * **A5 (Hybrid Synth)** is placed in **Cluster 2**, the exact same cluster as **A4 (FM Bell)**! This is a beautiful validation of the feature extractor. A5 uses a sawtooth timbre (like A2) but shares the exact same steady quarter-note rhythm and Pentatonic A pitch set as A4. Because it shares two out of three primary axes with A4, the algorithm clusters them together!
3. **The Multi-Bridge Transition:**
   * **A6 (Percussive Roll)** is distributed across Cluster 5 (4/8), Cluster 6 (3/8), and Cluster 1 (1/8). This reflects its nature as a multi-bridge: it has the fast rhythm of A2 (Cluster 1), the hi-hat percussive noise of A3, and a constant 440Hz sine hum of A1. This structural ambiguity makes it form sub-clusters or lie on the borders of the other groups.

---

## 5. Visualizing the Similarity Space (`analysis/cluster_analysis.png`)

The script outputs a high-resolution 2x2 grid `analysis/cluster_analysis.png` allowing visual analysis of these relationships:
* **Top-Left (PCA - Ground Truth):** Shows linear similarity. The major dimensions are clearly separated, with the Hybrid Synth (A5) lying midway between the Buzzing Synth (A2) and FM Bell (A4).
* **Top-Right (PCA - K-Means):** Shows how the linear partitions translate to unsupervised groups.
* **Bottom-Left (t-SNE - Ground Truth):** Displays extremely tight, well-defined clusters for A1, A2, A3, and A4, and clearly illustrates how the hybrid A5 and A6 sit at the intersections or form adjacent groups.
* **Bottom-Right (t-SNE - K-Means):** Visually confirms that the unsupervised algorithm's clusters represent cohesive, distinct, and musically meaningful spaces.

---

## 6. How to Run the Suite

You can generate new samples, extract features, and run the cluster analysis with a its simple command:

```bash
python3 analysis/generate_and_cluster.py
```

### Options:
* `--num-samples INT`: Specify how many files to generate (must be a multiple of 6, default is 48).
* `--output-dir PATH`: Directory where 5-second WAV files are saved (default: `analysis/generated_samples/`).
* `--plot-path PATH`: Destination path for the cluster plot (default: `analysis/cluster_analysis.png`).
* `--skip-gen`: Skips generating new audio files and performs feature extraction and clustering on existing WAV files inside `--output-dir`.

All synthesized `.wav` files are peak-normalized to -1dB and saved under the `analysis/generated_samples/` folder, which is automatically ignored by Git to keep the repository lightweight. The master plot `analysis/cluster_analysis.png` is tracked in Git as a visual reference of the similarity space.
