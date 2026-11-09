#!/usr/bin/env python3
"""Independent candidate finder for repeated audio sections and loop lengths.

This does not use any project-specific analysis code. It prints, for each
silence-delimited section, the strongest autocorrelation and lag-similarity
peaks, plus band-wise peaks. The operator still has to interpret the
candidates and choose pattern boundaries and loop lengths.

Usage:
    python3 analyze_audio.py file.wav [more.wav ...] [--json out.json]

For non-WAV input, ffmpeg must be on PATH; the file is converted to a
temporary 44.1 kHz mono WAV.
"""
import argparse
import json
import os
import subprocess
import tempfile

import numpy as np
import scipy.io.wavfile as wavfile
import scipy.signal as sig


def load_audio(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == '.wav':
        sr, data = wavfile.read(path)
    else:
        fd, tmp = tempfile.mkstemp(suffix='.wav')
        os.close(fd)
        try:
            subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', path,
                            '-ac', '1', '-ar', '44100', tmp], check=True)
            sr, data = wavfile.read(tmp)
        finally:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    if data.ndim > 1:
        x = data.astype(np.float64).mean(axis=1)
    else:
        x = data.astype(np.float64)
    if data.dtype == np.int16:
        x /= 32768.0
    elif data.dtype == np.uint8:
        x = (x - 128.0) / 128.0
    elif data.dtype == np.int32:
        x /= 2147483648.0
    elif data.dtype in (np.float32, np.float64):
        pass
    else:
        raise ValueError('Unsupported sample type: %s' % data.dtype)
    return int(sr), x.astype(np.float32)


def melbank(sr, n_fft, nbands=64, fmin=30, fmax=None):
    fmax = fmax or sr / 2.0

    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

    pts = mel2hz(np.linspace(hz2mel(fmin), hz2mel(fmax), nbands + 2))
    freqs = np.fft.rfftfreq(n_fft, 1.0 / sr)
    B = np.zeros((nbands, len(freqs)))
    for i in range(nbands):
        lo, ce, hi = pts[i], pts[i + 1], pts[i + 2]
        B[i] = np.clip(np.minimum((freqs - lo) / (ce - lo + 1e-9),
                                  (hi - freqs) / (hi - ce + 1e-9)), 0, None)
    return B


def acf1(v, maxlag):
    v = np.asarray(v, dtype=np.float64)
    v = v - v.mean()
    v = v / (np.linalg.norm(v) + 1e-12)
    n = len(v)
    if n < 2 or maxlag < 1:
        return np.zeros(maxlag + 1)
    nff = 1 << (2 * n - 1).bit_length()
    V = np.fft.rfft(v, nff)
    ac = np.fft.irfft(V * np.conj(V), nff)[:maxlag + 1]
    counts = np.arange(n, n - maxlag - 1, -1)
    ac = ac / np.maximum(counts, 1)
    return ac / (ac[0] + 1e-12)


def acf_bands(F, maxlag):
    nb, n = F.shape
    F = F - F.mean(axis=1, keepdims=True)
    F = F / (np.linalg.norm(F, axis=1, keepdims=True) + 1e-12)
    nff = 1 << (2 * n - 1).bit_length()
    Ff = np.fft.rfft(F, nff, axis=1)
    ac = np.fft.irfft(Ff * np.conj(Ff), nff, axis=1)[:, :maxlag + 1].sum(axis=0)
    counts = np.arange(n, n - maxlag - 1, -1)
    ac = ac / np.maximum(counts, 1)
    return ac / (ac[0] + 1e-12)


def peak_list(v, fps, max_lag, top=10, min_lag=0.5):
    v = np.nan_to_num(v, nan=-1.0)
    pk, props = sig.find_peaks(v, distance=max(1, int(0.5 * fps)), prominence=0.005)
    rows = []
    for p in pk:
        lag = p / fps
        if min_lag <= lag <= max_lag:
            rows.append((float(lag), float(v[p])))
    rows.sort(key=lambda z: z[1], reverse=True)
    return rows[:top]


def detect_sections(x, sr, threshold_db=-50.0, min_silence=0.5, min_section=1.0):
    win = max(1, int(round(0.1 * sr)))
    n = len(x) // win
    if n < 2:
        return [(0.0, len(x) / sr)]
    rms = np.sqrt(np.mean(x[:n * win].reshape(n, win) ** 2, axis=1))
    db = 20.0 * np.log10(rms + 1e-12)
    active = db > threshold_db
    runs = []
    start = None
    for i, a in enumerate(active):
        if a and start is None:
            start = i
        elif not a and start is not None:
            runs.append([start * 0.1, i * 0.1])
            start = None
    if start is not None:
        runs.append([start * 0.1, n * 0.1])
    if not runs:
        return [(0.0, len(x) / sr)]
    merged = [runs[0]]
    for s, e in runs[1:]:
        if s - merged[-1][1] < min_silence:
            merged[-1][1] = e
        else:
            merged.append([s, e])
    out = [(max(0.0, s), min(len(x) / sr, e)) for s, e in merged if e - s >= min_section]
    return out or [(0.0, len(x) / sr)]


def novelty_peaks(F, fps, max_lag=8.0):
    """Cheap novelty from a 4 fps downsample; avoids a full-resolution SSM."""
    target = 4.0
    step = max(1.0, fps / target)
    n = int(F.shape[1] / step)
    if n < 4:
        return []
    Md = np.zeros((F.shape[0], n), dtype=np.float32)
    for i in range(n):
        a = int(i * step)
        b = max(a + 1, int((i + 1) * step))
        Md[:, i] = F[:, a:b].mean(axis=1)
    Md = Md - Md.mean(axis=1, keepdims=True)
    Md = Md / (np.linalg.norm(Md, axis=1, keepdims=True) + 1e-12)
    fd = n / (F.shape[1] / fps)
    S = Md.T @ Md
    L = max(2, int(max_lag * fd))
    nov = np.zeros(n)
    for i in range(L, n - L):
        nov[i] = (S[i:i + L, i:i + L].mean() + S[i - L:i, i - L:i].mean()
                  - 2.0 * S[i - L:i, i:i + L].mean())
    pk, _ = sig.find_peaks(nov, distance=max(2, int(4.0 * fd)),
                           prominence=max(1e-6, float(np.std(nov)) * 0.7))
    return [round(float(p / fd), 2) for p in pk]


def analyze(path, args):
    sr, x = load_audio(path)
    dur = len(x) / sr
    nfft = 2048
    if len(x) < nfft:
        nfft = 1 << max(8, int(np.ceil(np.log2(max(16, len(x))))))
    hop = nfft // 2
    f, t, Z = sig.stft(x, fs=sr, nperseg=nfft, noverlap=nfft - hop,
                       boundary=None, padded=False)
    P = np.abs(Z) ** 2
    B = melbank(sr, nfft, 64)
    M = np.log1p(B @ P)
    fps = sr / hop
    F = M - M.mean(axis=1, keepdims=True)
    F = F / (F.std(axis=1, keepdims=True) + 1e-9)
    F = F / (np.linalg.norm(F, axis=0, keepdims=True) + 1e-9)
    dM = np.diff(M, axis=1, prepend=M[:, :1])
    onset = np.maximum(dM, 0).sum(axis=0)
    onset = onset - onset.mean()

    max_lag = min(args.max_lag, max(0.1, dur / 2.0))
    maxlag = int(max_lag * fps)
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    band_defs = [
        ('sub', 20, 60), ('bass', 60, 150), ('lowmid', 150, 400),
        ('mid', 400, 1000), ('high', 1000, 4000),
        ('air', 4000, min(12000, sr / 2.0)),
    ]
    band_env = {}
    for name, lo, hi in band_defs:
        mask = (freqs >= lo) & (freqs < hi)
        if not np.any(mask):
            continue
        env = np.sqrt(P[mask].mean(axis=0))
        env = env - env.mean()
        band_env[name] = env

    sections = detect_sections(x, sr, args.threshold_db,
                               args.min_silence, args.min_section)
    result = {
        'file': os.path.basename(path),
        'sampleRate': int(sr),
        'duration': round(dur, 3),
        'fps': round(float(fps), 3),
        'sections': [[round(s, 3), round(e, 3)] for s, e in sections],
        'novelty_peaks': novelty_peaks(F, fps),
        'candidates': [],
    }
    print('\n===== %s  %.2fs  %d Hz' % (os.path.basename(path), dur, sr))
    print('sections:', ' '.join('%.2f-%.2f' % se for se in sections))
    if result['novelty_peaks']:
        print('novelty peaks:', ' '.join('%.2f' % p for p in result['novelty_peaks']))
    for s, e in sections:
        i0 = max(0, int(round(s * fps)))
        i1 = min(F.shape[1], int(round(e * fps)))
        if i1 - i0 < 4:
            continue
        fs = F[:, i0:i1]
        od = onset[i0:i1]
        ml = min(maxlag, i1 - i0 - 2)
        if ml < 2:
            continue
        cand = {
            'section': [round(s, 3), round(e, 3)],
            'onset_peaks': peak_list(acf1(od, ml), fps, min(max_lag, (e - s) / 2), args.top),
            'lag_peaks': peak_list(acf_bands(fs, ml), fps, min(max_lag, (e - s) / 2), args.top),
            'band_peaks': {},
        }
        for name, env in band_env.items():
            cand['band_peaks'][name] = peak_list(
                acf1(env[i0:i1], ml), fps, min(max_lag, (e - s) / 2), 5)
        result['candidates'].append(cand)
        print('  %.2f-%.2f' % (s, e))
        print('    onset:', ' '.join('%.2f:%.3f' % z for z in cand['onset_peaks']))
        print('    lag  :', ' '.join('%.2f:%.3f' % z for z in cand['lag_peaks']))
        for name in ('sub', 'bass', 'mid', 'high', 'air'):
            if name in cand['band_peaks']:
                print('    %-5s:' % name, ' '.join('%.2f:%.3f' % z for z in cand['band_peaks'][name]))
    return result


def main():
    ap = argparse.ArgumentParser(description='Find repeated-section and loop-length candidates.')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--json', default=None, help='Write full results to this JSON file')
    ap.add_argument('--max-lag', type=float, default=30.0)
    ap.add_argument('--threshold-db', type=float, default=-50.0)
    ap.add_argument('--min-silence', type=float, default=0.5)
    ap.add_argument('--min-section', type=float, default=1.0)
    ap.add_argument('--top', type=int, default=10)
    args = ap.parse_args()
    results = [analyze(p, args) for p in args.files]
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(results, f, indent=1)
        print('\nWrote', args.json)


if __name__ == '__main__':
    main()
