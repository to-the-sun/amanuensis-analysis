import os
import base64

def get_image_base64(path):
    if not os.path.exists(path):
        return ""
    with open(path, "rb") as image_file:
        return base64.b64encode(image_file.read()).decode('utf-8')

html_template = """
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Audio Analysis Report</title>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; line-height: 1.6; color: #333; max-width: 1000px; margin: 0 auto; padding: 20px; background-color: #f4f4f9; }}
        h1, h2, h3 {{ color: #2c3e50; }}
        .file-section {{ background: #fff; padding: 30px; margin-bottom: 40px; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }}
        .pattern-box {{ background: #f9f9f9; border-left: 5px solid #3498db; padding: 15px; margin: 20px 0; }}
        .break-list {{ color: #e74c3c; font-weight: bold; }}
        img {{ max-width: 100%; height: auto; border: 1px solid #ddd; margin: 20px 0; border-radius: 4px; }}
        .stats {{ background: #ecf0f1; padding: 10px; border-radius: 4px; font-size: 0.9em; }}
    </style>
</head>
<body>
    <h1>Comprehensive Audio Pattern Analysis Report</h1>
    <p>This report documents human-perceptible patterns found across four distinct audio files. The analysis focuses on temporal, spectral, and pitch-based characteristics.</p>

    <!-- File 01 -->
    <div class="file-section">
        <h2>01 sustained bass [2025-12-29-22-19-46].wav</h2>
        <div class="stats">
            <strong>Stats:</strong> Mean RMS: 0.3172 | Max RMS: 0.5050
        </div>
        <img src="data:image/png;base64,{img01}" alt="Analysis for 01 sustained bass">

        <h3>Prominent Patterns</h3>

        <div class="pattern-box">
            <h4>1. Persistent Low-Frequency Foundation (Sub-Bass Drone)</h4>
            <p>The most dominant feature of this audio is a thick, continuous low-frequency drone primarily localized below 200 Hz. To a human listener, this manifests as a "room-filling" vibration that feels physical and steady. This pattern provides a consistent floor for the audio, with very little deviation in fundamental frequency. It is the defining spectral anchor of the piece.</p>
            <p><strong>Location:</strong> Exists continuously from 0:00.00 to 0:44.29.</p>
            <p><strong>Breaks:</strong> The pattern is never truly "broken" in terms of disappearance, but it undergoes "intensity thinning" at the following timecodes: 6.46s, 11.20s, 14.15s, 17.58s, 22.18s, 27.69s, 31.81s, and 42.42s.</p>
        </div>

        <div class="pattern-box">
            <h4>2. Harmonic Spectral Shimmer</h4>
            <p>Above the bass drone, there is a secondary pattern of higher-order harmonics that fluctuate in amplitude. This creates a "shimmering" or "breathing" effect within the sustained sound. Unlike the bass which is rigid, these upper frequencies exhibit a slight undulating motion, giving the sound a more organic, less synthetic feel.</p>
            <p><strong>Location:</strong> Most prominent between 200 Hz and 1000 Hz, active throughout the duration.</p>
            <p><strong>Breaks:</strong> These fluctuations align with the intensity breaks noted above, where the "shimmer" momentarily dims or resets.</p>
        </div>
    </div>

    <!-- File 02 -->
    <div class="file-section">
        <h2>02 sustained treble [2025-12-29-22-19-46].wav</h2>
        <div class="stats">
            <strong>Stats:</strong> Mean RMS: 0.2234 | Max RMS: 0.3204
        </div>
        <img src="data:image/png;base64,{img02}" alt="Analysis for 02 sustained treble">

        <h3>Prominent Patterns</h3>

        <div class="pattern-box">
            <h4>1. Static High-Frequency Cloud</h4>
            <p>This file exhibits an extremely stable, high-pitched texture that lacks any discernible rhythmic or melodic movement. It resembles a "white noise" or "hiss" but with more specific tonal resonance in the 4kHz+ range. For a listener, it creates a sense of spatial suspension or "air," remaining almost perfectly flat in its energy delivery. It is the most "stable" pattern across all four files.</p>
            <p><strong>Location:</strong> Present and unchanging from 0:00.00 to 0:44.29.</p>
            <p><strong>Breaks:</strong> No significant intensity or spectral breaks were detected. The pattern is unbroken for the entire duration.</p>
        </div>
    </div>

    <!-- File 03 -->
    <div class="file-section">
        <h2>03 transient bass [2025-12-29-22-19-46].wav</h2>
        <div class="stats">
            <strong>Stats:</strong> Mean RMS: 0.2268 | Max RMS: 0.6300
        </div>
        <img src="data:image/png;base64,{img03}" alt="Analysis for 03 transient bass">

        <h3>Prominent Patterns</h3>

        <div class="pattern-box">
            <h4>1. Rhythmic Pulsing (Groove Structure)</h4>
            <p>Unlike the sustained files, this audio contains a clear temporal pattern characterized by rhythmic pulses in the lower registers. To a listener, this feels like a "beat" or a "thump" that repeats at semi-regular intervals. The Self-Similarity Matrix shows distinct diagonal lines, confirming that short sequences of sound are repeating. This is the most "musical" pattern found, suggesting a primitive rhythm or cadence.</p>
            <p><strong>Location:</strong> Recurring throughout the file, specifically clustered in 4-5 second phrases.</p>
            <p><strong>Breaks:</strong> The rhythmic pulse is explicitly broken or "muted" at the following points: 1.83s, 5.09s, 9.20s, 12.38s, 15.35s, 18.25s, 21.04s, 24.31s, 26.24s, 31.78s, 34.34s, 38.83s, and 43.15s.</p>
        </div>

        <div class="pattern-box">
            <h4>2. Pitch Class Cycling</h4>
            <p>The Chroma analysis reveals a shifting pitch pattern, predominantly cycling between notes resembling G, A, and B. This creates a "harmonic narrative" that isn't present in the sustained files. A listener would perceive this as a very slow, three-note melody or bassline that repeats alongside the rhythmic pulses.</p>
            <p><strong>Location:</strong> Visible across the whole duration, but most defined during the high-energy peaks of the pulses.</p>
            <p><strong>Breaks:</strong> The pitch cycle is interrupted during the low-energy silences (the break points listed above) where the tonal content drops below the threshold of clear perception.</p>
        </div>
    </div>

    <!-- File 04 -->
    <div class="file-section">
        <h2>04 transient treble [2025-12-29-22-19-46].wav</h2>
        <div class="stats">
            <strong>Stats:</strong> Mean RMS: 0.1293 | Max RMS: 0.7731
        </div>
        <img src="data:image/png;base64,{img04}" alt="Analysis for 04 transient treble">

        <h3>Prominent Patterns</h3>

        <div class="pattern-box">
            <h4>1. Sparse Transient "Stabs"</h4>
            <p>This file is defined by its erratic and sharp bursts of sound followed by periods of near-silence. These "stabs" are rich in high-frequency content and have very fast attack times, sounding like "clicks," "pops," or "shards" of audio. This is a purely temporal pattern where the presence of sound itself is the pattern, rather than any internal rhythm.</p>
            <p><strong>Location:</strong> Occurs intermittently. Major spikes are seen at ~4s, ~12s, ~18s, ~24s, ~36s, and ~42s.</p>
            <p><strong>Breaks:</strong> The pattern of silence is broken by these transients. Conversely, the "stabs" are broken by the extended silences between them. Notable silences (breaks in sound) occur at: 0-2s, 7-10s, 13-16s, 29-31s, and 39-41s.</p>
        </div>

        <div class="pattern-box">
            <h4>2. Mid-Range Resonance Bursts</h4>
            <p>Within the transient stabs, there is a recurring spectral pattern where energy is concentrated in the 512Hz to 2048Hz range. Even though the sounds are short, they share a consistent "timbral DNA"—they sound like they are coming from the same resonant source (e.g., a struck metallic object). This gives the sparse events a sense of belonging to the same sound family.</p>
            <p><strong>Location:</strong> Coincident with each transient stab listed in the primary pattern.</p>
            <p><strong>Breaks:</strong> Broken completely during the silent intervals between stabs.</p>
        </div>
    </div>

</body>
</html>
"""

# Embed images
img01 = get_image_base64("analysis_output/01 sustained bass [2025-12-29-22-19-46].wav_analysis.png")
img02 = get_image_base64("analysis_output/02 sustained treble [2025-12-29-22-19-46].wav_analysis.png")
img03 = get_image_base64("analysis_output/03 transient bass [2025-12-29-22-19-46].wav_analysis.png")
img04 = get_image_base64("analysis_output/04 transient treble [2025-12-29-22-19-46].wav_analysis.png")

with open("audio_analysis_report.html", "w") as f:
    f.write(html_template.format(img01=img01, img02=img02, img03=img03, img04=img04))

print("Report generated: audio_analysis_report.html")
