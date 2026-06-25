import librosa
import numpy as np

def debug():
    sr = 44100
    y = np.zeros(sr)
    y[1024] = 1.0 # Peak at sample 1024

    n_fft = 2048
    hop_length = 44

    D = librosa.stft(y, n_fft=n_fft, hop_length=hop_length)
    # Find frame with max energy
    mags = np.sum(np.abs(D)**2, axis=0)
    max_frame = np.argmax(mags)
    print(f"Max energy at frame {max_frame}")

    S = librosa.feature.melspectrogram(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    max_frame_mel = np.argmax(np.sum(S, axis=0))
    print(f"Max mel energy at frame {max_frame_mel}")

    onset_env = librosa.onset.onset_strength(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    max_frame_onset = np.argmax(onset_env)
    print(f"Max onset strength at frame {max_frame_onset}")
    print(f"Difference: {max_frame_onset - max_frame_mel}")

if __name__ == "__main__":
    debug()
