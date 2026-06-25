import librosa
import numpy as np

def find_coeffs():
    sr = 44100
    n_fft = 2048
    n_mels = 128

    # HTK
    mel_f_htk = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=n_mels, htk=True, norm=None)
    # Slaney unnorm
    mel_f_slaney_unnorm = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=n_mels, htk=False, norm=None)
    # Slaney area norm
    mel_f_slaney = librosa.filters.mel(sr=sr, n_fft=n_fft, n_mels=n_mels, htk=False, norm='slaney')

    print("HTK [0, :5]:", mel_f_htk[0, :5])
    print("Slaney Unnorm [0, :5]:", mel_f_slaney_unnorm[0, :5])
    print("Slaney Area Norm [0, :5]:", mel_f_slaney[0, :5])

    # Calculate ratio
    ratio = mel_f_slaney[0, 1] / mel_f_slaney_unnorm[0, 1]
    print("Slaney Area Norm Ratio:", ratio)

    f_min = 0
    f_max = sr / 2.0
    mel_min = librosa.hz_to_mel(f_min, htk=False)
    mel_max = librosa.hz_to_mel(f_max, htk=False)
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = librosa.mel_to_hz(mel_points, htk=False)

    expected_enorm = 2.0 / (hz_points[2] - hz_points[0])
    print("Expected enorm for mel 0:", expected_enorm)

if __name__ == "__main__":
    find_coeffs()
