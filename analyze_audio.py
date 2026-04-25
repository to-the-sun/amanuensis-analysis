import librosa
import librosa.display
import matplotlib.pyplot as plt
import numpy as np
import os

def analyze_file(filepath, output_dir):
    filename = os.path.basename(filepath)
    print(f"Analyzing {filename}...")

    y, sr = librosa.load(filepath, sr=None)
    duration = librosa.get_duration(y=y, sr=sr)

    # 1. RMS Energy
    rms = librosa.feature.rms(y=y)[0]
    times = librosa.times_like(rms, sr=sr)

    # 2. Mel Spectrogram
    S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128)
    S_dB = librosa.power_to_db(S, ref=np.max)

    # 3. Chroma Features
    chroma = librosa.feature.chroma_stft(y=y, sr=sr)

    # 4. Self-Similarity Matrix (Recurrence)
    # Use chroma for SSM to find tonal patterns, or Mel for timbral ones.
    # Let's use a combined feature or just MFCC/Mel for timbre.
    mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=13)
    R = librosa.segment.recurrence_matrix(mfcc, mode='affinity', sym=True)

    # Create plots
    fig, axes = plt.subplots(nrows=4, ncols=1, figsize=(12, 16))

    # Plot RMS
    axes[0].plot(times, rms, label='RMS Energy')
    axes[0].set_title(f'RMS Energy - {filename}')
    axes[0].set_ylabel('Amplitude')
    axes[0].set_xlabel('Time (s)')

    # Plot Mel Spectrogram
    img1 = librosa.display.specshow(S_dB, x_axis='time', y_axis='mel', sr=sr, ax=axes[1])
    axes[1].set_title(f'Mel Spectrogram - {filename}')
    fig.colorbar(img1, ax=axes[1], format='%+2.0f dB')

    # Plot Chroma
    img2 = librosa.display.specshow(chroma, x_axis='time', y_axis='chroma', sr=sr, ax=axes[2])
    axes[2].set_title(f'Chroma Features - {filename}')
    fig.colorbar(img2, ax=axes[2])

    # Plot SSM
    img3 = librosa.display.specshow(R, x_axis='time', y_axis='time', sr=sr, ax=axes[3])
    axes[3].set_title(f'Self-Similarity Matrix - {filename}')

    plt.tight_layout()
    plot_path = os.path.join(output_dir, f"{filename}_analysis.png")
    plt.savefig(plot_path)
    plt.close()

    # Print some stats for qualitative analysis
    print(f"Stats for {filename}:")
    print(f"  Duration: {duration:.2f}s")
    print(f"  Max RMS: {np.max(rms):.4f} at {times[np.argmax(rms)]:.2f}s")
    print(f"  Mean RMS: {np.mean(rms):.4f}")

    # Simple break detection (sharp changes in RMS)
    diff_rms = np.diff(rms)
    breaks = times[1:][np.abs(diff_rms) > (np.std(diff_rms) * 5)]
    if len(breaks) > 0:
        print(f"  Potential intensity breaks detected at: {breaks}")

if __name__ == "__main__":
    output_dir = "analysis_output"
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    wav_files = [f for f in os.listdir('.') if f.endswith('.wav')]
    for wav_file in sorted(wav_files):
        analyze_file(wav_file, output_dir)
