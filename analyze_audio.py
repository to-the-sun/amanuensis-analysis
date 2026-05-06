import os
import base64
import io
import numpy as np
import librosa
import librosa.display
import matplotlib.pyplot as plt
from scipy.ndimage import median_filter

def fig_to_base64(fig):
    buf = io.BytesIO()
    fig.savefig(buf, format='png', bbox_inches='tight')
    buf.seek(0)
    return base64.b64encode(buf.read()).decode('utf-8')

def analyze_file(filepath):
    print(f"Analyzing {filepath}...")
    y, sr = librosa.load(filepath, sr=None)
    duration = librosa.get_duration(y=y, sr=sr)

    # Features
    rms = librosa.feature.rms(y=y)[0]
    times_rms = librosa.times_like(rms, sr=sr)

    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    times_onset = librosa.times_like(onset_env, sr=sr)

    tempogram = librosa.feature.tempogram(onset_envelope=onset_env, sr=sr)

    S = librosa.feature.melspectrogram(y=y, sr=sr)
    S_db = librosa.power_to_db(S, ref=np.max)
    times_spec = librosa.times_like(S_db, sr=sr)

    chroma = librosa.feature.chroma_stft(y=y, sr=sr)
    times_chroma = librosa.times_like(chroma, sr=sr)

    patterns = []

    # 1. Amplitude Pattern (RMS)
    rms_norm = (rms - np.min(rms)) / (np.max(rms) - np.min(rms) + 1e-6)
    # Local consistency of amplitude: variance in a sliding window
    # Default hop_length is 512
    hop_length = 512
    window_size = int(sr * 2 / hop_length) # 2 second window
    if window_size < 1: window_size = 1
    rms_local_var = np.array([np.std(rms_norm[max(0, i-window_size//2):min(len(rms_norm), i+window_size//2)]) for i in range(len(rms_norm))])
    rms_consistency = 1.0 - rms_local_var
    rms_consistency = (rms_consistency - np.min(rms_consistency)) / (np.max(rms_consistency) - np.min(rms_consistency) + 1e-6)

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(times_rms, rms_norm, label='Normalized RMS')
    ax.fill_between(times_rms, rms_consistency, color='orange', alpha=0.3, label='Amplitude Consistency')
    ax.set_title('Amplitude Pattern & Consistency')
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Normalized Value')
    ax.legend()

    patterns.append({
        "name": "Amplitude Dynamics",
        "description": "This pattern reflects the overall volume envelope of the audio. In 'sustained' files, it should show high stability, whereas 'transient' files will show periodic spikes. High consistency indicates a steady volume level or a predictable rhythmic pulse, while low consistency (breaks) indicates sudden changes in intensity or erratic volume shifts.",
        "visualization": fig_to_base64(fig),
        "location_summary": f"The pattern is most consistent when the signal maintains its expected level. Breaks occur during transitions or silences."
    })
    plt.close(fig)

    # 2. Rhythmic Pattern (Tempo/Onsets)
    # Using tempogram to find the most dominant tempo
    # We skip the first few rows of tempogram which often contain DC-like or global energy terms
    # librosa.feature.tempogram rows correspond to lag. Row 0 is lag 0.
    mean_tempo = np.mean(tempogram, axis=1)
    # Look for the peak excluding the very low lag (high tempo) which might be an artifact
    dominant_tempo_idx = np.argmax(mean_tempo[1:]) + 1
    tempo_consistency = tempogram[dominant_tempo_idx, :]
    tempo_consistency = (tempo_consistency - np.min(tempo_consistency)) / (np.max(tempo_consistency) - np.min(tempo_consistency) + 1e-6)

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.plot(times_onset, tempo_consistency, color='green')
    ax.set_title('Rhythmic Pulse Consistency')
    ax.set_xlabel('Time (s)')
    ax.set_ylabel('Consistency (Dominant Tempo Strength)')

    patterns.append({
        "name": "Rhythmic Pulse",
        "description": "Analyzes the regularity of transients. For 'transient' files, this identifies the underlying beat. A high value on the heatmap indicates a rigid, predictable rhythm. A drop in this value suggests a 'break' in the rhythm, such as a missed beat, a syncopation, or a change in the rhythmic structure.",
        "visualization": fig_to_base64(fig),
        "location_summary": "Strongest in sections with repetitive strikes; breaks during pauses or irregular sequences."
    })
    plt.close(fig)

    # 3. Spectral Stability
    # We look at how the mel-spectrogram changes over time (spectral flux-like)
    spec_diff = np.sqrt(np.mean(np.diff(S_db, axis=1)**2, axis=0))
    spec_diff = np.pad(spec_diff, (1, 0), mode='edge')
    spec_stability = 1.0 - (spec_diff / (np.max(spec_diff) + 1e-6))
    spec_stability = median_filter(spec_stability, size=window_size)
    spec_stability = (spec_stability - np.min(spec_stability)) / (np.max(spec_stability) - np.min(spec_stability) + 1e-6)

    fig, ax = plt.subplots(figsize=(10, 4))
    img = librosa.display.specshow(S_db, x_axis='time', y_axis='mel', sr=sr, ax=ax)
    ax2 = ax.twinx()
    ax2.plot(times_spec, spec_stability, color='red', linewidth=2, label='Spectral Stability')
    ax2.set_ylabel('Stability Score')
    ax2.legend(loc='upper right')
    ax.set_title('Spectral Content & Stability')

    patterns.append({
        "name": "Spectral Texture",
        "description": "This pattern tracks the consistency of the frequency distribution. In 'sustained' sounds, this represents the steady drone or harmonic content. High stability means the timbre is unchanging. Breaks in this pattern represent 'spectral shifts'—moments where the tonal quality of the sound changes significantly, even if the volume remains the same.",
        "visualization": fig_to_base64(fig),
        "location_summary": "Consistent during steady tones; breaks at timbral transitions."
    })
    plt.close(fig)

    # 4. Harmonic/Pitch Pattern (Chroma)
    chroma_stability = np.mean(chroma, axis=0) # Simple proxy for 'tonal presence'
    # Or better: correlation of chroma frames with their neighbors
    chroma_corr = np.array([np.corrcoef(chroma[:, i], chroma[:, max(0, i-1)])[0, 1] if i > 0 else 1.0 for i in range(chroma.shape[1])])
    chroma_corr = median_filter(chroma_corr, size=window_size)
    chroma_corr = (chroma_corr - np.min(chroma_corr)) / (np.max(chroma_corr) - np.min(chroma_corr) + 1e-6)

    fig, ax = plt.subplots(figsize=(10, 4))
    librosa.display.specshow(chroma, y_axis='chroma', x_axis='time', ax=ax)
    ax2 = ax.twinx()
    ax2.plot(times_chroma, chroma_corr, color='white', linewidth=2, label='Pitch Consistency')
    ax2.set_ylabel('Pitch Consistency Score')
    ax2.legend()
    ax.set_title('Chroma (Pitch) Consistency')

    patterns.append({
        "name": "Tonal Continuity",
        "description": "Focuses on the pitch content. This pattern captures the 'notes' or harmonic foundation. High consistency indicates a stable pitch or chord. A break occurs when the pitch drifts, changes suddenly, or when the sound becomes inharmonious/noisy.",
        "visualization": fig_to_base64(fig),
        "location_summary": "Most stable during clear melodic or harmonic segments; breaks during pitch shifts or noisy transients."
    })
    plt.close(fig)

    return patterns, duration

def generate_html(filename, patterns, duration):
    report_name = filename.replace('.wav', '_Report.html')
    html_content = f"""
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1.0">
        <title>Audio Analysis Report - {filename}</title>
        <style>
            body {{ font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; line-height: 1.6; color: #333; max-width: 1000px; margin: 0 auto; padding: 20px; background-color: #f4f4f9; }}
            h1, h2 {{ color: #2c3e50; }}
            .summary {{ background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 30px; }}
            .pattern {{ background: #fff; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 40px; }}
            .pattern img {{ max-width: 1000px; height: auto; border: 1px solid #ddd; border-radius: 4px; }}
            .pattern-desc {{ margin-top: 15px; }}
            .location {{ font-weight: bold; color: #e67e22; margin-top: 10px; }}
            .heatmap-info {{ font-style: italic; font-size: 0.9em; color: #7f8c8d; }}
        </style>
    </head>
    <body>
        <h1>Audio Pattern Analysis Report</h1>
        <div class="summary">
            <h2>File: {filename}</h2>
            <p><strong>Duration:</strong> {duration:.2f} seconds</p>
            <p>This report documents the most prominent human-perceptible patterns found in the audio file, analyzed across multiple dimensions including amplitude, rhythm, spectrum, and pitch. Each pattern includes a consistency analysis to show where the pattern holds and where it breaks.</p>
        </div>
    """

    for p in patterns:
        html_content += f"""
        <div class="pattern">
            <h2>Pattern: {p['name']}</h2>
            <img src="data:image/png;base64,{p['visualization']}" alt="{p['name']}">
            <div class="pattern-desc">
                <p>{p['description']}</p>
                <p class="location">Location Analysis: {p['location_summary']}</p>
                <p class="heatmap-info">Note: The overlay/graph line represents the local consistency of the pattern. High values indicate strong adherence to the pattern; dips indicate breaks.</p>
            </div>
        </div>
        """

    html_content += """
    </body>
    </html>
    """

    with open(report_name, 'w') as f:
        f.write(html_content)
    print(f"Report generated: {report_name}")

if __name__ == "__main__":
    wav_files = [f for f in os.listdir('.') if f.endswith('.wav')]
    for file in sorted(wav_files):
        patterns, duration = analyze_file(file)
        generate_html(file, patterns, duration)
