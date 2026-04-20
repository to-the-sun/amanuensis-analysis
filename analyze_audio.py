import librosa
import librosa.display
import matplotlib.pyplot as plt
import numpy as np
import os
import json
from scipy.signal import find_peaks

def analyze_file(filepath):
    print(f"Deep analyzing {filepath}...")
    y, sr = librosa.load(filepath)
    duration = librosa.get_duration(y=y, sr=sr)

    # --- 1. Harmonic/Percussive Separation ---
    y_harmonic, y_percussive = librosa.effects.hpss(y)

    # --- 2. Amplitude/Temporal Patterns ---
    rms = librosa.feature.rms(y=y)[0]
    times = librosa.times_like(rms, sr=sr)
    peaks, _ = find_peaks(rms, height=np.mean(rms)*1.2, distance=sr//512 * 0.2)
    peak_times = times[peaks]

    zcr = librosa.feature.zero_crossing_rate(y)[0]

    # --- 3. Spectral Patterns ---
    S = np.abs(librosa.stft(y))
    melspec = librosa.feature.melspectrogram(y=y, sr=sr)
    melspec_db = librosa.power_to_db(melspec, ref=np.max)

    spec_centroid = librosa.feature.spectral_centroid(y=y, sr=sr)[0]
    spec_flatness = librosa.feature.spectral_flatness(y=y)[0]
    spec_contrast = librosa.feature.spectral_contrast(S=S, sr=sr)

    # Dominant Frequencies
    f_bins = librosa.fft_frequencies(sr=sr)
    mean_spec = np.mean(S, axis=1)
    top_f_indices = np.argsort(mean_spec)[-10:][::-1]
    top_frequencies = f_bins[top_f_indices]

    # --- 4. Pitch/Harmonic Patterns ---
    chroma = librosa.feature.chroma_cqt(y=y, sr=sr)
    mean_chroma = np.mean(chroma, axis=1)
    pitch_classes = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']

    # --- 5. Visualizations ---
    base_name = os.path.splitext(filepath)[0]
    output_dir = f"analysis_{base_name}"
    os.makedirs(output_dir, exist_ok=True)

    # Plot 1: Waveform & HPSS
    plt.figure(figsize=(12, 6))
    plt.subplot(2, 1, 1)
    librosa.display.waveshow(y_harmonic, sr=sr, alpha=0.8, color='b', label='Harmonic')
    librosa.display.waveshow(y_percussive, sr=sr, alpha=0.5, color='r', label='Percussive')
    plt.title(f"Harmonic-Percussive Separation: {filepath}")
    plt.legend()
    plt.subplot(2, 1, 2)
    plt.plot(times, rms, color='black', label='Loudness (RMS)')
    plt.fill_between(times, rms, alpha=0.3, color='black')
    plt.title("Dynamic Envelope")
    plt.tight_layout()
    plt.savefig(f"{output_dir}/temporal.png")
    plt.close()

    # Plot 2: Spectrogram & Centroid
    plt.figure(figsize=(12, 6))
    librosa.display.specshow(melspec_db, sr=sr, x_axis='time', y_axis='mel')
    plt.plot(times, spec_centroid, color='w', label='Spectral Centroid', alpha=0.6)
    plt.colorbar(format='%+2.0f dB')
    plt.title(f"Mel Spectrogram with Spectral Centroid: {filepath}")
    plt.legend()
    plt.tight_layout()
    plt.savefig(f"{output_dir}/spectrogram.png")
    plt.close()

    # Plot 3: Spectral Contrast
    plt.figure(figsize=(12, 6))
    librosa.display.specshow(spec_contrast, sr=sr, x_axis='time')
    plt.colorbar()
    plt.ylabel('Frequency Bands')
    plt.title("Spectral Contrast (Peak-Valley Difference)")
    plt.tight_layout()
    plt.savefig(f"{output_dir}/contrast.png")
    plt.close()

    # Plot 4: Recurrence
    hop_length = 1024
    mfcc = librosa.feature.mfcc(y=y, sr=sr, hop_length=hop_length)
    R = librosa.segment.recurrence_matrix(mfcc, width=3, mode='affinity', sym=True)
    R_smooth = librosa.segment.path_enhance(R, n=17)
    plt.figure(figsize=(8, 8))
    librosa.display.specshow(R_smooth, x_axis='time', y_axis='time', hop_length=hop_length)
    plt.title("Structural Self-Similarity")
    plt.tight_layout()
    plt.savefig(f"{output_dir}/recurrence.png")
    plt.close()

    # --- Pattern Discovery Logic ---
    patterns = []

    # Harmonic vs Percussive dominance
    h_energy = np.mean(librosa.feature.rms(y=y_harmonic))
    p_energy = np.mean(librosa.feature.rms(y=y_percussive))
    ratio = h_energy / (p_energy + 1e-10)

    if ratio > 2.0:
        patterns.append({
            "name": "Sustained Harmonic Resonance",
            "description": f"The audio is overwhelmingly harmonic (Ratio {ratio:.2f}). It exhibits long-form resonance with stable tonal centers.",
            "prominence": 0.95
        })
    elif ratio < 0.5:
        patterns.append({
            "name": "Transient Percussive Texture",
            "description": f"The signal is dominated by percussive impulses (Ratio {ratio:.2f}). Rapid energy bursts and high-frequency noise transients are the primary features.",
            "prominence": 0.95
        })
    else:
        patterns.append({
            "name": "Balanced Hybrid Texture",
            "description": f"A complex mix of tonal stability and percussive events (Ratio {ratio:.2f}).",
            "prominence": 0.6
        })

    # Tonal Centers
    top_pitches = [pitch_classes[i] for i in np.argsort(mean_chroma)[-3:][::-1]]
    patterns.append({
        "name": "Dominant Pitch Constellation",
        "description": f"Strong energy concentration in {', '.join(top_pitches)}. Fundamental frequencies are anchored around {top_frequencies[0]:.1f}Hz and {top_frequencies[1]:.1f}Hz.",
        "prominence": 0.9
    })

    # Spectral Complexity
    avg_flatness = np.mean(spec_flatness)
    if avg_flatness < 0.01:
        patterns.append({
            "name": "High Spectral Purity",
            "description": "The sound is highly tonal with very low noise floor. Most energy is concentrated in narrow frequency bands (pure tones or harmonics).",
            "prominence": 0.8
        })
    else:
        patterns.append({
            "name": "Broadband Spectral Complexity",
            "description": "The sound contains significant noise or complex, dense frequency distributions (stochastic patterns).",
            "prominence": 0.7
        })

    # Temporal Dynamics
    crest_factor = np.max(np.abs(y)) / (np.sqrt(np.mean(y**2)) + 1e-10)
    if crest_factor > 10:
        patterns.append({
            "name": "Extreme Transience / High Crest Factor",
            "description": f"Presence of very sharp, high-intensity peaks (Crest Factor: {crest_factor:.2f}). These 'micro-patterns' suggest sudden explosive onset events.",
            "prominence": 0.85
        })

    # Structural repetition
    rep_score = np.mean(R_smooth)
    if rep_score > 0.1:
        patterns.append({
            "name": "Iterative Structural Symmetry",
            "description": "The recurrence analysis reveals significant self-similarity over time, indicating repeating motifs or cyclic transformations.",
            "prominence": 0.75
        })

    patterns.sort(key=lambda x: x['prominence'], reverse=True)

    report_data = {
        "filename": filepath,
        "duration": f"{duration:.2f}s",
        "patterns": patterns,
        "metrics": {
            "crest_factor": f"{crest_factor:.2f}",
            "avg_centroid": f"{np.mean(spec_centroid):.1f} Hz",
            "harmonic_ratio": f"{ratio:.2f}",
            "spectral_flatness": f"{avg_flatness:.4f}"
        },
        "output_dir": output_dir
    }

    with open(f"{output_dir}/report_data.json", "w") as f:
        json.dump(report_data, f, indent=4)

    return report_data

def generate_html(report_data):
    html_content = f"""
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>Pattern Analysis Report - {report_data['filename']}</title>
        <style>
            body {{ font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; line-height: 1.6; color: #333; max-width: 1100px; margin: 0 auto; padding: 40px; background: #fafafa; }}
            .container {{ background: #fff; padding: 40px; border-radius: 12px; box-shadow: 0 4px 20px rgba(0,0,0,0.08); }}
            h1 {{ color: #1a2a3a; border-bottom: 3px solid #3498db; padding-bottom: 15px; margin-top: 0; }}
            h2 {{ color: #2c3e50; margin-top: 40px; border-left: 5px solid #3498db; padding-left: 15px; }}
            .metrics-grid {{ display: grid; grid-template-columns: repeat(4, 1fr); gap: 20px; margin: 20px 0; }}
            .metric-box {{ background: #f8f9fa; padding: 15px; border-radius: 8px; text-align: center; border: 1px solid #eee; }}
            .metric-value {{ font-size: 1.4em; font-weight: bold; color: #e67e22; }}
            .metric-label {{ font-size: 0.85em; color: #7f8c8d; text-transform: uppercase; }}
            .pattern-card {{ background: #fff; border: 1px solid #e0e0e0; padding: 20px; margin-bottom: 20px; border-radius: 8px; transition: transform 0.2s; }}
            .pattern-card:hover {{ transform: translateY(-2px); box-shadow: 0 4px 12px rgba(0,0,0,0.05); }}
            .pattern-name {{ font-weight: bold; font-size: 1.25em; color: #2c3e50; margin-bottom: 10px; }}
            .pattern-prominence {{ float: right; background: #3498db; color: #fff; padding: 3px 12px; border-radius: 20px; font-size: 0.8em; }}
            .visualizations {{ display: grid; grid-template-columns: 1fr; gap: 30px; margin-top: 30px; }}
            .vis-item {{ background: #fff; padding: 15px; border: 1px solid #eee; border-radius: 8px; }}
            .vis-item img {{ width: 100%; height: auto; border-radius: 4px; }}
            .vis-caption {{ font-size: 0.9em; color: #666; margin-top: 10px; font-style: italic; }}
            footer {{ margin-top: 60px; text-align: center; color: #95a5a6; font-size: 0.9em; }}
        </style>
    </head>
    <body>
        <div class="container">
            <h1>Audio Pattern Report: {report_data['filename']}</h1>

            <div class="metrics-grid">
                <div class="metric-box"><div class="metric-value">{report_data['duration']}</div><div class="metric-label">Duration</div></div>
                <div class="metric-box"><div class="metric-value">{report_data['metrics']['avg_centroid']}</div><div class="metric-label">Spectral Center</div></div>
                <div class="metric-box"><div class="metric-value">{report_data['metrics']['crest_factor']}</div><div class="metric-label">Crest Factor</div></div>
                <div class="metric-box"><div class="metric-value">{report_data['metrics']['harmonic_ratio']}</div><div class="metric-label">Harmonicity</div></div>
            </div>

            <h2>Prominent Patterns & Observations</h2>
            {"".join([f'''
            <div class="pattern-card">
                <span class="pattern-prominence">Prominence: {int(p['prominence']*100)}%</span>
                <div class="pattern-name">{p['name']}</div>
                <div class="pattern-description">{p['description']}</div>
            </div>
            ''' for p in report_data['patterns']])}

            <h2>Technical Visualizations</h2>
            <div class="visualizations">
                <div class="vis-item">
                    <h3>Temporal & Dynamic Envelope</h3>
                    <img src="temporal.png">
                    <div class="vis-caption">Displays the raw waveform separated into harmonic (blue) and percussive (red) components, with the overall loudness envelope below.</div>
                </div>
                <div class="vis-item">
                    <h3>Spectral Density (Mel-Scale)</h3>
                    <img src="spectrogram.png">
                    <div class="vis-caption">Heatmap of energy across the frequency spectrum. The white line tracks the 'center of mass' of the sound (Spectral Centroid).</div>
                </div>
                <div class="vis-item">
                    <h3>Spectral Contrast Peaks</h3>
                    <img src="contrast.png">
                    <div class="vis-caption">Highlights the difference between spectral peaks and valleys, revealing the 'sharpness' of the tonal content across frequency bands.</div>
                </div>
                <div class="vis-item">
                    <h3>Structural Recurrence</h3>
                    <img src="recurrence.png">
                    <div class="vis-caption">A map of temporal self-similarity. Diagonals and blocks indicate repeating patterns or stable textures over the course of the recording.</div>
                </div>
            </div>

            <footer>
                Analyzed by Jules AI • Sound Pattern Recognition Engine
            </footer>
        </div>
    </body>
    </html>
    """
    with open(f"{report_data['output_dir']}/report.html", "w") as f:
        f.write(html_content)
    print(f"Deep report generated: {report_data['output_dir']}/report.html")

if __name__ == "__main__":
    files = sorted([f for f in os.listdir('.') if f.endswith('.wav')])
    for f in files:
        data = analyze_file(f)
        generate_html(data)
