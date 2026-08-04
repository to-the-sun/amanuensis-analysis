#!/usr/bin/env python3
"""
Procedural Audio Generation and Cluster Analysis Script
Generates 48 structured five-second audio samples spanning 6 archetypes,
extracts timbral, pitch, and rhythmic features, clusters them,
and plots the results in a beautiful 2x2 visualization grid.
"""

import os
import sys
import random
import argparse
import numpy as np
import soundfile as sf
import scipy.signal as signal
from tqdm import tqdm

# Try importing audio analysis and ML libraries
try:
    import librosa
    from sklearn.preprocessing import StandardScaler
    from sklearn.cluster import KMeans
    from sklearn.decomposition import PCA
    from sklearn.manifold import TSNE
    import matplotlib
    import matplotlib.pyplot as plt
except ImportError as e:
    print(f"Error importing libraries: {e}")
    print("Please make sure all dependencies are installed. E.g.,")
    print("pip install librosa scikit-learn matplotlib soundfile numpy scipy tqdm")
    sys.exit(1)

# Ensure matplotlib works in headless environments
matplotlib.use("Agg")

# Set random seed for absolute reproducibility
np.random.seed(42)
random.seed(42)

# ==========================================
# 1. AUDIO SYNTHESIS ENGINE
# ==========================================

def adsr_envelope(length_samples, sr, attack_s, decay_s, sustain_level, release_s):
    """
    Generates a classic ADSR envelope.
    """
    env = np.zeros(length_samples, dtype=np.float32)
    attack_samples = int(attack_s * sr)
    decay_samples = int(decay_s * sr)
    release_samples = int(release_s * sr)

    # Check if ADSR parameters exceed the total length and scale down proportionally if so
    total_adr = attack_samples + decay_samples + release_samples
    if total_adr > length_samples:
        scale = length_samples / max(1, total_adr)
        attack_samples = int(attack_samples * scale)
        decay_samples = int(decay_samples * scale)
        release_samples = int(release_samples * scale)

    sustain_samples = length_samples - attack_samples - decay_samples - release_samples

    # Attack: Linear ramp from 0 to 1
    if attack_samples > 0:
        env[:attack_samples] = np.linspace(0.0, 1.0, attack_samples)

    # Decay: Exponential decay from 1.0 to sustain_level
    if decay_samples > 0:
        decay_vals = np.linspace(0, 1, decay_samples)
        # Fast exponential decay towards sustain
        env[attack_samples:attack_samples+decay_samples] = sustain_level + (1.0 - sustain_level) * np.exp(-4.0 * decay_vals)

    # Sustain: Constant level
    if sustain_samples > 0:
        start_idx = attack_samples + decay_samples
        env[start_idx:start_idx+sustain_samples] = sustain_level

    # Release: Exponential decay from sustain_level to 0
    if release_samples > 0:
        release_vals = np.linspace(0, 1, release_samples)
        env[-release_samples:] = sustain_level * np.exp(-4.0 * release_vals)

    return env


def synthesize_sine(freq, duration_s, sr, attack=0.1, decay=0.1, sustain=0.8, release=0.2):
    """Synthesizes a pure sine wave with ADSR envelope."""
    num_samples = int(duration_s * sr)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)
    wave = np.sin(2 * np.pi * freq * t)
    env = adsr_envelope(num_samples, sr, attack, decay, sustain, release)
    return wave * env


def synthesize_sawtooth(freq, duration_s, sr, attack=0.01, decay=0.15, sustain=0.5, release=0.1):
    """Synthesizes a sawtooth wave with ADSR envelope."""
    num_samples = int(duration_s * sr)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)
    wave = signal.sawtooth(2 * np.pi * freq * t)
    env = adsr_envelope(num_samples, sr, attack, decay, sustain, release)
    return wave * env


def synthesize_fm_bell(freq, duration_s, sr, attack=0.002, decay=0.4, sustain=0.0, release=0.1):
    """
    Synthesizes an FM metallic bell sound.
    Carrier frequency is freq, modulator frequency is freq * 1.414,
    modulation index decays exponentially to give a changing metallic spectrum.
    """
    num_samples = int(duration_s * sr)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)

    # Exponential index of modulation env
    index_env = 6.0 * np.exp(-4.0 * t / duration_s)
    modulator = np.sin(2 * np.pi * (freq * 1.414) * t)

    wave = np.sin(2 * np.pi * freq * t + index_env * modulator)
    env = adsr_envelope(num_samples, sr, attack, decay, sustain, release)
    return wave * env


def synthesize_kick(duration_s, sr):
    """Synthesizes a kick drum (rapid sine pitch sweep)."""
    num_samples = int(duration_s * sr)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)
    # Pitch sweep from 150 Hz down to 45 Hz
    freq_sweep = 45.0 + (150.0 - 45.0) * np.exp(-30.0 * t)
    phase = 2 * np.pi * np.cumsum(freq_sweep) / sr
    wave = np.sin(phase)
    env = np.exp(-12.0 * t)
    return wave * env


def synthesize_snare(duration_s, sr):
    """Synthesizes a snare drum (noise burst mixed with a 180 Hz tone)."""
    num_samples = int(duration_s * sr)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)

    # Noise part
    noise = np.random.normal(0.0, 0.5, num_samples)
    noise_env = np.exp(-10.0 * t)

    # Tone part (180 Hz)
    tone = np.sin(2 * np.pi * 180.0 * t)
    tone_env = np.exp(-20.0 * t)

    wave = (noise * noise_env) + 0.4 * (tone * tone_env)
    return wave


def synthesize_hihat(duration_s, sr):
    """Synthesizes a hi-hat (very short noise burst)."""
    num_samples = int(duration_s * sr)
    noise = np.random.normal(0.0, 0.4, num_samples)
    t = np.linspace(0, duration_s, num_samples, endpoint=False)
    env = np.exp(-40.0 * t)
    return noise * env


def safe_mix(track, wave, start_sample):
    """Mixes a wave into a track starting at start_sample, handling boundaries."""
    if start_sample < 0:
        # If start_sample is negative, truncate wave prefix
        wave = wave[-start_sample:]
        start_sample = 0
    if start_sample >= len(track):
        return
    end_sample = start_sample + len(wave)
    if end_sample > len(track):
        wave = wave[:len(track) - start_sample]
        end_sample = len(track)
    if len(wave) > 0:
        track[start_sample:end_sample] += wave


def create_ambient_flute_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 1: Ambient Flutes
    Sine timbre, slow tempo, Pentatonic minor scale in A.
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)
    pentatonic_a = [440.0, 523.25, 587.33, 659.25, 783.99] # A4, C5, D5, E5, G5

    # Slowly trigger 3-4 notes
    note_duration = 1.2
    # Place at beats 0, 2, 4 (with small random timing offsets)
    beat_dur = 60.0 / bpm
    triggers = [0.0, 2.0 * beat_dur, 4.0 * beat_dur]

    for trig in triggers:
        if trig + note_duration >= total_duration_s:
            continue
        freq = random.choice(pentatonic_a)
        # Pitch jitter
        freq = freq * random.choice([1.0, 2.0]) # allow octaves
        if freq > 1600.0: freq /= 2.0

        note_wave = synthesize_sine(freq, note_duration, sr, attack=0.4, decay=0.2, sustain=0.7, release=0.4)
        start_sample = int((trig + random.uniform(-0.02, 0.02)) * sr)
        safe_mix(track, note_wave, start_sample)

    return track


def create_buzzing_synth_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 2: Buzzing Synth Melodies
    Sawtooth timbre, fast tempo, Major scale in C.
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)
    major_c = [261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25] # C4 to C5

    # Trigger fast notes (8th notes)
    beat_dur = 60.0 / bpm
    note_duration = beat_dur * 0.4  # short buzzy notes
    step = beat_dur * 0.5  # 8th note spacing

    num_steps = int(total_duration_s / step)
    for i in range(num_steps):
        trig = i * step
        if trig + note_duration >= total_duration_s:
            continue
        # Play melodic arpeggio
        freq = random.choice(major_c)
        note_wave = synthesize_sawtooth(freq, note_duration, sr, attack=0.01, decay=0.1, sustain=0.4, release=0.05)
        start_sample = int(trig * sr)
        safe_mix(track, note_wave, start_sample)

    return track


def create_percussive_beat_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 3: Percussive Beats
    Kick/Snare/Hihat, syncopated 120 BPM tempo, no pitched melody content.
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)
    beat_dur = 60.0 / bpm

    # 4/4 syncopated pattern over 2 bars (8 beats)
    # Kick: beats 0, 1.5, 2.0, 3.5, 4.0, 5.5, 6.0, 7.5
    # Snare: beats 1.0, 3.0, 5.0, 7.0
    # Hi-hat: 8th notes (0, 0.5, 1.0, 1.5, ...)

    # Create master beat grid
    for half_beat in range(int(total_duration_s / (beat_dur * 0.5))):
        t_sec = half_beat * (beat_dur * 0.5)
        if t_sec >= total_duration_s:
            break

        beat_idx = half_beat * 0.5
        # Modulo for 8 beats pattern
        pattern_beat = beat_idx % 8.0

        start_sample = int(t_sec * sr)

        # 1. Hihat on every eighth note (with velocity accent variation)
        hat_wave = synthesize_hihat(0.08, sr)
        accent = 1.0 if half_beat % 2 == 0 else 0.5
        safe_mix(track, hat_wave * accent * 0.3, start_sample)

        # 2. Kick Drum
        if pattern_beat in [0.0, 1.5, 2.0, 3.5, 4.0, 5.5, 6.0, 7.5]:
            kick_wave = synthesize_kick(0.18, sr)
            safe_mix(track, kick_wave * 0.9, start_sample)

        # 3. Snare Drum
        if pattern_beat in [1.0, 3.0, 5.0, 7.0]:
            snare_wave = synthesize_snare(0.2, sr)
            safe_mix(track, snare_wave * 0.6, start_sample)

    return track


def create_fm_bell_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 4: FM Bells
    Metallic FM timbre, steady quarter note rhythm (120 BPM), Pentatonic minor scale in A.
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)
    pentatonic_a = [220.0, 261.63, 293.66, 329.63, 392.00] # A3 to G4 (lower octave)

    beat_dur = 60.0 / bpm
    note_duration = beat_dur * 0.8

    num_beats = int(total_duration_s / beat_dur)
    for i in range(num_beats):
        trig = i * beat_dur
        if trig + note_duration >= total_duration_s:
            continue

        freq = random.choice(pentatonic_a)
        note_wave = synthesize_fm_bell(freq, note_duration, sr, attack=0.002, decay=note_duration*0.8, sustain=0.0, release=note_duration*0.1)
        start_sample = int(trig * sr)
        safe_mix(track, note_wave, start_sample)

    return track


def create_hybrid_synth_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 5: Hybrid Synth
    Sawtooth timbre (shares with 2), steady quarter notes (shares rhythm with 4),
    Pentatonic minor in A (shares key with 1 & 4).
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)
    pentatonic_a = [220.0, 261.63, 293.66, 329.63, 392.00] # A3 to G4

    beat_dur = 60.0 / bpm
    note_duration = beat_dur * 0.7

    num_beats = int(total_duration_s / beat_dur)
    for i in range(num_beats):
        trig = i * beat_dur
        if trig + note_duration >= total_duration_s:
            continue

        freq = random.choice(pentatonic_a)
        # Sawtooth wave (timbre shares with 2)
        note_wave = synthesize_sawtooth(freq, note_duration, sr, attack=0.03, decay=note_duration*0.5, sustain=0.3, release=0.1)
        start_sample = int(trig * sr)
        safe_mix(track, note_wave, start_sample)

    return track


def create_percussive_roll_track(bpm, sr, total_duration_s=5.0):
    """
    Archetype 6: Percussive Roll
    Hihat noise burst roll, fast 140 BPM (shares rhythm with 2), plus a low-level 440 Hz sine hum (shares pitch/sine with 1).
    """
    track = np.zeros(int(total_duration_s * sr), dtype=np.float32)

    # 1. Add background 440 Hz sine hum
    t = np.linspace(0, total_duration_s, len(track), endpoint=False)
    hum = np.sin(2 * np.pi * 440.0 * t) * 0.08  # Muted constant hum
    track += hum

    # 2. Add fast 16th-note percussive rolls (rapid noise bursts)
    beat_dur = 60.0 / bpm
    step = beat_dur * 0.25 # 16th note spacing
    note_duration = step * 0.8

    num_steps = int(total_duration_s / step)
    for i in range(num_steps):
        # We can create a galloping or rolling pattern (e.g., skips some steps for flavor, but is fast)
        if i % 4 == 3 and random.random() < 0.3:
            continue # skip occasionally for groove

        trig = i * step
        if trig + note_duration >= total_duration_s:
            continue

        # Hi-hat noise burst
        perc_wave = synthesize_hihat(note_duration, sr)
        start_sample = int(trig * sr)
        safe_mix(track, perc_wave * 0.7, start_sample)

    return track


def generate_all_samples(output_dir, num_per_archetype=8, sr=44100):
    """
    Generates all audio files, organizing them into the 6 structured archetypes.
    Returns a list of dicts with file metadata.
    """
    os.makedirs(output_dir, exist_ok=True)
    metadata = []

    # Archetype specs
    archetypes = [
        {
            "id": 1,
            "name": "Ambient Flute",
            "desc": "Sine wave, slow tempo (~80 BPM), Pentatonic minor in A, slow attack/release.",
            "generator": create_ambient_flute_track,
            "bpm_range": (75, 85)
        },
        {
            "id": 2,
            "name": "Buzzing Synth",
            "desc": "Sawtooth wave, fast tempo (~140 BPM), Major key in C, sharp attack/decay.",
            "generator": create_buzzing_synth_track,
            "bpm_range": (135, 145)
        },
        {
            "id": 3,
            "name": "Percussive Beat",
            "desc": "Kick/Snare/Hi-hat, syncopated tempo (~120 BPM), unpitched/dissonant, rapid decay.",
            "generator": create_percussive_beat_track,
            "bpm_range": (115, 125)
        },
        {
            "id": 4,
            "name": "FM Bell",
            "desc": "FM metallic synthesizer, steady quarter notes (~120 BPM), Pentatonic minor in A, exponential decay.",
            "generator": create_fm_bell_track,
            "bpm_range": (115, 125)
        },
        {
            "id": 5,
            "name": "Hybrid Synth",
            "desc": "Sawtooth wave, steady quarter notes (~120 BPM), Pentatonic minor in A.",
            "generator": create_hybrid_synth_track,
            "bpm_range": (115, 125)
        },
        {
            "id": 6,
            "name": "Percussive Roll",
            "desc": "Noise hi-hat rolls, fast tempo (~140 BPM), plus steady background 440Hz sine hum.",
            "generator": create_percussive_roll_track,
            "bpm_range": (135, 145)
        }
    ]

    print(f"\n--- Generating {num_per_archetype * len(archetypes)} procedural audio samples ---")
    pbar = tqdm(total=len(archetypes) * num_per_archetype, desc="Synthesizing audio")

    for arch in archetypes:
        for idx in range(1, num_per_archetype + 1):
            bpm = random.randint(arch["bpm_range"][0], arch["bpm_range"][1])

            # Synthesize track
            audio_data = arch["generator"](bpm, sr)

            # Peak normalize to avoid clipping (-1dB / 0.89 amplitude)
            max_val = np.max(np.abs(audio_data))
            if max_val > 0:
                audio_data = (audio_data / max_val) * 0.89

            # File name
            safe_name = arch["name"].lower().replace(" ", "_")
            filepath = os.path.join(output_dir, f"arch{arch['id']}_{safe_name}_{idx:02d}.wav")

            # Save audio
            sf.write(filepath, audio_data, sr)

            metadata.append({
                "filepath": filepath,
                "filename": os.path.basename(filepath),
                "archetype_id": arch["id"],
                "archetype_name": arch["name"],
                "bpm": bpm
            })
            pbar.update(1)

    pbar.close()
    print("Audio generation completed successfully.")
    return metadata


# ==========================================
# 2. AUDIO FEATURE EXTRACTION ENGINE
# ==========================================

def extract_features(filepath):
    """
    Loads an audio file and extracts multidimensional features:
    - MFCCs (timbral)
    - Spectral Centroid (brightness)
    - Spectral Flatness (noisiness)
    - Chroma features (harmonic/pitch classes)
    - Onset Envelope Autocorrelation (rhythmic signature & tempo)
    """
    y, sr = librosa.load(filepath, sr=None)

    # 1. Timbral features: MFCCs (13 coefficients)
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13)
    mfcc_mean = np.mean(mfcc, axis=1)
    mfcc_std = np.std(mfcc, axis=1)

    # 2. Timbral features: Spectral Centroid
    centroid = librosa.feature.spectral_centroid(y=y, sr=sr)
    centroid_mean = np.mean(centroid)
    centroid_std = np.std(centroid)

    # 3. Timbral features: Spectral Flatness
    flatness = librosa.feature.spectral_flatness(y=y)
    flatness_mean = np.mean(flatness)
    flatness_std = np.std(flatness)

    # 4. Pitch/Harmonic features: Chroma STFT (12 semitones)
    chroma = librosa.feature.chroma_stft(y=y, sr=sr, n_chroma=12)
    chroma_mean = np.mean(chroma, axis=1)

    # 5. Rhythmic features: Onset strength envelope autocorrelation
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    # Autocorrelation of onset envelope captures rhythmic periods
    autocorr = librosa.autocorrelate(onset_env, max_size=300)
    # Standardize or take the first 120 values
    rhythm_features = autocorr[:120]
    # Normalize rhythm features to peak at 1 to represent pattern, not amplitude
    max_rc = np.max(rhythm_features)
    if max_rc > 0:
        rhythm_features = rhythm_features / max_rc

    # Concatenate all into a single unified feature vector
    feat_vector = np.concatenate([
        mfcc_mean,         # 13
        mfcc_std,          # 13
        [centroid_mean, centroid_std, flatness_mean, flatness_std], # 4
        chroma_mean,       # 12
        rhythm_features    # 120
    ])                     # Total = 162 features

    return feat_vector


# ==========================================
# 3. CLUSTERING AND VISUALIZATION ENGINE
# ==========================================

def run_cluster_analysis(metadata, output_plot_path):
    """
    Runs K-Means clustering, PCA, and t-SNE dimensionality reduction,
    and saves a beautiful 2x2 cluster visualization grid.
    """
    print(f"\n--- Extracting features from {len(metadata)} audio samples ---")
    features_list = []

    for meta in tqdm(metadata, desc="Extracting features"):
        feat = extract_features(meta["filepath"])
        features_list.append(feat)

    features_arr = np.array(features_list)

    # Standardize the feature vectors
    scaler = StandardScaler()
    scaled_feats = scaler.fit_transform(features_arr)

    # Run K-Means Clustering (unsupervised discovery)
    num_clusters = 6
    kmeans = KMeans(n_clusters=num_clusters, random_state=42, n_init=10)
    cluster_labels = kmeans.fit_predict(scaled_feats)

    # Add cluster labels to metadata
    for i, meta in enumerate(metadata):
        meta["cluster_id"] = cluster_labels[i]

    # 1. Dimensionality Reduction: PCA (Linear Projection)
    pca = PCA(n_components=2, random_state=42)
    pca_proj = pca.fit_transform(scaled_feats)

    # 2. Dimensionality Reduction: t-SNE (Non-Linear Projection)
    # Perplexity must be less than number of samples
    tsne = TSNE(n_components=2, perplexity=10, random_state=42, init='pca', learning_rate='auto')
    tsne_proj = tsne.fit_transform(scaled_feats)

    # Prepare colors and labels
    unique_arch_ids = sorted(list(set(m["archetype_id"] for m in metadata)))
    unique_arch_names = {m["archetype_id"]: m["archetype_name"] for m in metadata}

    # Aesthetic color palettes
    # Soft distinct colors for archetypes
    arch_colors = ['#E74C3C', '#3498DB', '#2ECC71', '#9B59B6', '#F1C40F', '#16A085']
    # Soft distinct colors for K-Means clusters
    kmeans_colors = ['#FF6B6B', '#4D96FF', '#6BCB77', '#FFD93D', '#B185DB', '#1A5F7A']

    print("\nGenerating cluster visualization grid...")
    fig, axs = plt.subplots(2, 2, figsize=(18, 14))

    # Layout details:
    # Row 0: PCA Projections
    # Row 1: t-SNE Projections
    # Col 0: Ground-Truth Archetypes
    # Col 1: Unsupervised K-Means Clusters

    projections = [
        ("PCA (Linear Metric Projection)", pca_proj, "PCA 1", "PCA 2"),
        ("t-SNE (Non-Linear Neighborhood Projection)", tsne_proj, "t-SNE Dimension 1", "t-SNE Dimension 2")
    ]

    for row_idx, (proj_name, coords, x_lbl, y_lbl) in enumerate(projections):
        # -------------------------------------------------------------
        # Left Panel: Ground Truth Archetypes
        # -------------------------------------------------------------
        ax_gt = axs[row_idx, 0]

        # Plot each archetype as a distinct group to get clean legend entries
        for arch_id in unique_arch_ids:
            indices = [i for i, m in enumerate(metadata) if m["archetype_id"] == arch_id]
            ax_gt.scatter(
                coords[indices, 0], coords[indices, 1],
                color=arch_colors[arch_id - 1],
                label=f"{arch_id}. {unique_arch_names[arch_id]}",
                s=110, edgecolors='black', alpha=0.85, zorder=3
            )

            # Label individual points with index for tracking
            for idx in indices:
                meta = metadata[idx]
                short_fn = meta["filename"].split("_")[-1].replace(".wav", "")
                ax_gt.annotate(
                    short_fn, (coords[idx, 0], coords[idx, 1]),
                    textcoords="offset points", xytext=(0,6),
                    ha='center', fontsize=8, fontweight='bold', alpha=0.75
                )

        ax_gt.set_title(f"{proj_name} - Ground-Truth Archetypes", fontsize=12, fontweight="bold", pad=10)
        ax_gt.set_xlabel(x_lbl, fontsize=10)
        ax_gt.set_ylabel(y_lbl, fontsize=10)
        ax_gt.grid(True, linestyle=":", alpha=0.5)
        ax_gt.legend(title="Archetype Group", loc="best", framealpha=0.9)

        # -------------------------------------------------------------
        # Right Panel: Unsupervised K-Means Clusters
        # -------------------------------------------------------------
        ax_km = axs[row_idx, 1]

        for k_idx in range(num_clusters):
            indices = [i for i, m in enumerate(metadata) if m["cluster_id"] == k_idx]
            ax_km.scatter(
                coords[indices, 0], coords[indices, 1],
                color=kmeans_colors[k_idx],
                label=f"K-Means Cluster {k_idx + 1}",
                s=110, edgecolors='black', alpha=0.85, zorder=3
            )

            # Label individual points with ground-truth archetype short tag to see mixing
            for idx in indices:
                meta = metadata[idx]
                arch_tag = f"A{meta['archetype_id']}-{meta['filename'].split('_')[-1].replace('.wav', '')}"
                ax_km.annotate(
                    arch_tag, (coords[idx, 0], coords[idx, 1]),
                    textcoords="offset points", xytext=(0,6),
                    ha='center', fontsize=7, alpha=0.7
                )

        ax_km.set_title(f"{proj_name} - Unsupervised K-Means Clusters", fontsize=12, fontweight="bold", pad=10)
        ax_km.set_xlabel(x_lbl, fontsize=10)
        ax_km.set_ylabel(y_lbl, fontsize=10)
        ax_km.grid(True, linestyle=":", alpha=0.5)
        ax_km.legend(title="Discovered Clusters", loc="best", framealpha=0.9)

    plt.suptitle(
        "Procedural Audio Similarity & Cluster Analysis\n"
        "Comparison of Linear (PCA) vs. Non-Linear (t-SNE) Mapping of Timbre, Pitch, and Rhythm Features",
        fontsize=16, fontweight="bold", y=0.98
    )

    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    plt.savefig(output_plot_path, dpi=180, bbox_inches="tight")
    plt.close()

    print(f"\nSuccessfully generated and saved beautiful cluster plot to: {os.path.abspath(output_plot_path)}")

    # Log a helpful summary of clustering performance
    print("\n--- Clustering Analysis Summary ---")
    print("This table maps Ground-Truth Archetypes to the K-Means discovered clusters:")
    print("-----------------------------------------------------------------------------")
    print(f"{'Ground Truth Archetype':<25} | {'Discovered K-Means Clusters (Counts)':<45}")
    print("-----------------------------------------------------------------------------")
    for arch_id in unique_arch_ids:
        counts = {}
        arch_meta = [m for m in metadata if m["archetype_id"] == arch_id]
        for m in arch_meta:
            c_id = m["cluster_id"] + 1
            counts[c_id] = counts.get(c_id, 0) + 1
        count_strs = [f"Cluster {c}: {n}" for c, n in sorted(counts.items())]
        print(f"A{arch_id}. {unique_arch_names[arch_id]:<21} | {', '.join(count_strs)}")
    print("-----------------------------------------------------------------------------")


def main():
    parser = argparse.ArgumentParser(
        description="Procedurally generate dozens of structured audio samples with shared similarities and perform cluster analysis."
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="analysis/generated_samples",
        help="Directory to save generated five-second WAV files (default: %(default)s)."
    )
    parser.add_argument(
        "--num-samples",
        type=int,
        default=48,
        help="Total number of samples to generate. Must be a multiple of 6 (default: %(default)s)."
    )
    parser.add_argument(
        "--plot-path",
        type=str,
        default="analysis/cluster_analysis.png",
        help="Path to save the generated similarity cluster plot (default: %(default)s)."
    )
    parser.add_argument(
        "--sr",
        type=int,
        default=44100,
        help="Sample rate of generated audio in Hz (default: %(default)s)."
    )
    parser.add_argument(
        "--skip-gen",
        action="store_true",
        help="Skip audio generation and perform cluster analysis on existing files in output-dir."
    )

    args = parser.parse_args()

    if args.num_samples % 6 != 0:
        print(f"Error: --num-samples must be a multiple of 6 so we have equal samples per archetype. Setting to 48.")
        args.num_samples = 48

    num_per_archetype = args.num_samples // 6

    metadata = []

    if args.skip_gen:
        print(f"Skipping generation. Scanning existing WAV files in {args.output_dir}...")
        # Re-build metadata by scanning files in output_dir
        if not os.path.exists(args.output_dir):
            print(f"Error: {args.output_dir} does not exist. Cannot skip generation.")
            sys.exit(1)

        wav_files = sorted([f for f in os.listdir(args.output_dir) if f.endswith(".wav")])
        if not wav_files:
            print(f"Error: No .wav files found in {args.output_dir}.")
            sys.exit(1)

        print(f"Found {len(wav_files)} WAV files.")
        for f in wav_files:
            # Parse archetype id from filename: arch[ID]_name_num.wav
            if f.startswith("arch"):
                try:
                    arch_id = int(f[4])
                    # Try to parse name
                    parts = f.split("_")
                    name_parts = []
                    for p in parts[1:-1]:
                        name_parts.append(p.capitalize())
                    arch_name = " ".join(name_parts)
                    metadata.append({
                        "filepath": os.path.join(args.output_dir, f),
                        "filename": f,
                        "archetype_id": arch_id,
                        "archetype_name": arch_name,
                        "bpm": 120 # dummy
                    })
                except Exception as e:
                    print(f"Warning: Could not parse archetype from file {f}: {e}")

        if not metadata:
            print("Error: Could not parse any files matching the procedural naming format.")
            sys.exit(1)
    else:
        # Generate new procedural samples
        metadata = generate_all_samples(args.output_dir, num_per_archetype=num_per_archetype, sr=args.sr)

    # Run the cluster analysis and save plot
    run_cluster_analysis(metadata, args.plot_path)


if __name__ == "__main__":
    main()
