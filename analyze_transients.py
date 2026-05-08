import librosa
import numpy as np
import scipy.signal
import json
import os

def analyze_audio(file_path):
    print(f"Analyzing {file_path}...")
    y, sr = librosa.load(file_path, sr=None)

    # Calculate onset strength (transient envelope)
    onset_env = librosa.onset.onset_strength(y=y, sr=sr)
    times = librosa.frames_to_time(np.arange(len(onset_env)), sr=sr)

    # Find peaks in the onset strength
    # Adjust prominence/distance as needed for better peak detection
    peaks, _ = scipy.signal.find_peaks(onset_env, prominence=0.5, distance=10)

    peak_times = times[peaks].tolist()
    peak_values = onset_env[peaks].tolist()

    return {
        "filename": os.path.basename(file_path),
        "times": times.tolist(),
        "onset_env": onset_env.tolist(),
        "peaks": {
            "times": peak_times,
            "values": peak_values
        }
    }

def main():
    audio_files = [f for f in os.listdir('.') if f.endswith('.wav')]
    audio_files.sort()

    all_data = {}
    for f in audio_files:
        all_data[f] = analyze_audio(f)

    html_template = """
<!DOCTYPE html>
<html>
<head>
    <title>Transient Analysis Report</title>
    <script src="https://cdn.plot.ly/plotly-latest.min.js"></script>
    <style>
        body { font-family: sans-serif; margin: 20px; }
        .controls { margin-bottom: 20px; }
        #audio-player { width: 100%; margin-top: 10px; }
        #graph { width: 100%; height: 600px; }
    </style>
</head>
<body>
    <h1>Transient Analysis Report</h1>

    <div class="controls">
        <label for="file-select">Select Audio File:</label>
        <select id="file-select"></select>
        <br>
        <audio id="audio-player" controls></audio>
    </div>

    <div id="graph"></div>

    <script>
        const data = DATA_PLACEHOLDER;
        const fileSelect = document.getElementById('file-select');
        const audioPlayer = document.getElementById('audio-player');
        const graphDiv = document.getElementById('graph');

        // Populate dropdown
        Object.keys(data).forEach(filename => {
            const option = document.createElement('option');
            option.value = filename;
            option.textContent = filename;
            fileSelect.appendChild(option);
        });

        let currentFile = fileSelect.value;

        function updateGraph(filename) {
            const fileData = data[filename];

            const traceTransient = {
                x: fileData.times,
                y: fileData.onset_env,
                mode: 'lines',
                name: 'Transient Envelope',
                line: { color: 'blue' }
            };

            const tracePeaks = {
                x: fileData.peaks.times,
                y: fileData.peaks.values,
                mode: 'markers',
                name: 'Peaks',
                marker: { color: 'red', size: 8, symbol: 'cross' }
            };

            const layout = {
                title: 'Transient Analysis - ' + filename,
                xaxis: { title: 'Time (s)' },
                yaxis: { title: 'Onset Strength' },
                shapes: [{
                    type: 'line',
                    x0: 0,
                    x1: 0,
                    y0: 0,
                    y1: 1,
                    yref: 'paper',
                    line: {
                        color: 'red',
                        width: 2,
                        dash: 'dash'
                    },
                    name: 'playhead'
                }],
                dragmode: 'zoom',
                hovermode: 'closest'
            };

            Plotly.newPlot(graphDiv, [traceTransient, tracePeaks], layout);
        }

        function updateAudio(filename) {
            audioPlayer.src = filename;
            audioPlayer.load();
        }

        fileSelect.addEventListener('change', (e) => {
            currentFile = e.target.value;
            updateGraph(currentFile);
            updateAudio(currentFile);
        });

        audioPlayer.addEventListener('timeupdate', () => {
            const currentTime = audioPlayer.currentTime;
            const update = {
                'shapes[0].x0': currentTime,
                'shapes[0].x1': currentTime
            };
            Plotly.relayout(graphDiv, update);
        });

        // Initial load
        updateGraph(currentFile);
        updateAudio(currentFile);

    </script>
</body>
</html>
    """

    report_content = html_template.replace("DATA_PLACEHOLDER", json.dumps(all_data))

    with open("transient_analysis.html", "w") as f:
        f.write(report_content)

    print("Report generated: transient_analysis.html")

if __name__ == "__main__":
    main()
