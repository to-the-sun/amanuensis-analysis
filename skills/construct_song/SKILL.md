# Construct Song

Use this skill when the user supplies a palette recording (one continuous mono
performance) and wants a finished song plus separate stems, without the
repository's C tools.

The palette is not a multitrack. The song is assembled from material that is
already in it: sequences of segments that repeat. You find those sequences,
chop them on the measured grid, layer them as separate parts, and arrange them.
Nothing is generated.

## What is allowed

The construction is limited to chopping, looping, arranging and mixing.

- Allowed: trimming on the grid, equal-power crossfades, gain changes, fades,
  panning, EQ, compression, limiting.
- Not allowed: synthesis, sample replacement, reverb, delay, chorus,
  saturation, pitch shifting, time-stretching, or any other effect. Do not
  "repair" a layer with a filter that changes its rhythm or pitch.

EQ is mixing: it changes the level of parts of the spectrum. Use it to give
each layer a role and to keep layers from masking each other.

## Guarantees

- No `passes` file, loop-point table, or project-specific analysis code is
  needed or used.
- No tempo, meter, bar count or phrase length is assumed. The repeating period
  is measured from the audio.
- A **pattern** is a sequential repeating series of at least two **segments**.
  A segment is valid when its rhythm and/or spectrum recurs in the next segment
  (same onsets in the same places, same pitch content, same band shape).
- Each stem is one pattern with its own gain/EQ role. It is never a frequency
  band cut out of one mixdown.
- The stems sum to the song: every stem passes through the same linear master
  stages and the same gain envelopes.

## Vocabulary

- **Segment**: one measured repetition period of the palette.
- **Period**: the repeating time unit found by autocorrelation, at least 1 s so
  subdivisions are not mistaken for segments.
- **Pattern**: a run of at least two consecutive similar segments. Its audio is
  the loop; its length is `segments * period`.
- **Layer**: a pattern with a gain and EQ role.
- **Stem**: a layer rendered across the whole song, silent where the
  arrangement removes it.
- **Unit**: one pattern length, the arrangement step.

## 0. Analyze the palette

Start with the audio-pattern-reports analyzer. It gives silence-delimited
sections, novelty and lag peaks, which tell you where the material changes and
which lags are worth checking. It does not choose patterns.

```bash
python3 skills/audio-pattern-reports/analyze_audio.py palette.wav --json analysis.json
```

Use the sections to avoid cutting across silence or a tonal change. Treat the
lag peaks as candidates only; section 1 measures the period directly.

## 1. Measure the period

Compute a positive log-flux onset envelope, then its autocorrelation. Search
lags from 1.0 s to 8.0 s; anything shorter is usually a subdivision.

```python
NFFT, HOP = 2048, 512
S = np.abs(np.fft.rfft(frames * hann, axis=1))
# 64 log-spaced bands, band energy (sum of squared magnitudes), not mean magnitude
bands = np.stack([(S[:, a:b] ** 2).sum(axis=1) for a, b in band_edges], axis=1)
logE = np.log(bands + 1e-6)
flux = np.maximum(np.diff(logE, axis=0), 0).sum(axis=1)
env = np.maximum(flux - median_filter(flux, 63), 0); env /= env.max()

e = env - env.mean()
nfft = 1 << int(np.ceil(np.log2(2 * len(e))))
E = np.fft.rfft(e, nfft)
ac = np.fft.irfft(E * np.conj(E), nfft)[:len(e)]
ac /= np.maximum(len(e) - np.arange(len(e)), 1); ac /= ac[0]
```

Find local maxima in the 1–8 s range. Choose the **shortest** peak whose value
is at least 85% of the strongest peak and whose 2x and 3x multiples are also at
least 40% of the strongest. Refine it with parabolic interpolation. That is the
period; `duration / period` is the number of segments.

Record the period and the segment count. If the ACF has no peak above ~0.1,
the material has no usable repetition; say so instead of inventing a grid.

## 2. Segment grid, similarity and runs

Try eight phase offsets inside one period and keep the phase that maximizes the
mean similarity of adjacent segments. For each segment compute:

1. **Spectrum**: the mean log-band vector over the segment (64 values).
2. **Rhythm**: the onset envelope resampled to 24 slots over the segment.

Segment similarity is `0.5 * cosine(spectrum) + 0.5 * corr(rhythm)`. A run is a
maximal sequence of adjacent segments whose similarity exceeds
`max(0.45, percentile(similarity, 60))`. Keep runs of at least two segments.

Candidate patterns are windows of two or three consecutive segments inside a
run. Score each by the mean adjacent similarity of its segments. Record its
start/end time, segment count, score, RMS, band profile and chroma.

## 3. Choose the layer set

Reject candidates that are more than 9 dB below the loudest candidate: a quiet
window will need so much makeup gain that it raises noise instead of adding a
part. Then require:

- **Tonal compatibility**: pairwise correlation of the f0 chroma vectors at
  least 0.7 (fall back to 0.5 only if nothing qualifies). Never layer across a
  tonal shift; a semitone clash cannot be fixed by EQ.
- **No overlap in time**: each layer comes from a different part of the
  performance, so the stems are genuinely different material.
- **Spectral role coverage**: compare each candidate's band profile with the
  palette average, `d = mean_log_band - global_mean_log_band`. Classify by the
  band where `d` is largest (low / mid / high). Prefer a set that covers
  different roles.
- **Rhythm decorrelation**: mean pairwise rhythm correlation near zero. Strong
  positive correlation means two layers are doubling the same part.
- **Profile contrast**: mean absolute difference of the `d` vectors should be
  large enough that the layers occupy different spectral space.

Choose three or four layers. Fewer good layers sound better than four that
mask each other. Assign roles from measurement, not taste: relative low boost
becomes bass, relative mid boost becomes chords, relative high boost becomes
air/percussion.

Use f0 autocorrelation for chroma. Folding raw FFT bins is dominated by the low
end and reports the same key for everything:

```python
ac = np.fft.irfft(np.abs(np.fft.rfft(frame * hann)) ** 2, NFFT)
ac /= ac[:1]
k = np.argmax(ac[int(SR/700):int(SR/55)]) + int(SR/700)
f0 = SR / k                       # keep frames with periodicity > 0.25
pc = round(69 + 12 * np.log2(f0 / 440)) % 12
```

## 4. Align

Because every pattern sits on the same measured grid, layers align by
construction. If two patterns still fight, cross-correlate their rhythm
signatures and rotate one pattern's audio circularly by the best lag inside its
own length. Rotation is a chop, not a time-stretch. Do not resample.

## 5. Arrange

Tile each pattern to the song length with a ~10 ms equal-power crossfade at
every seam, then gate it per section. A pattern boundary is the natural
arrangement step. A simple form that works:

```python
SCHEDULE = [
    ('intro', 2, [0, 1],      -7.0),
    ('A',     4, [0, 1, 2],    0.0),
    ('B',     4, [0, 1, 2, 3], 1.0),
    ('break', 1, [],         -60.0),
    ('A2',    2, [0, 1, 2],   -1.0),
    ('C',     4, [0, 1, 2, 3], 1.5),
    ('D',     2, [1, 2, 3],    0.5),
    ('outro', 2, [0, 1],      -6.0),
]
```

Use 0.25–0.30 s fades on every entrance and exit, a 4 s fade-in and a 7 s
fade-out on the shared section gain. One unit of silence before a chorus is an
effective drop and needs no effect. Section gains are dB trims applied to all
layers alike.

## 6. Mix

Per layer: gain to a common active RMS (for example −17 dBFS), then EQ for its
role. Measured roles, not fixed frequencies:

| Role | Typical chain |
|---|---|
| bass | high-pass 26 Hz, low-pass 130–150 Hz, +2 dB at 90 Hz |
| chords | high-pass 120 Hz, −2 dB at 380 Hz, low-pass 1.5 kHz |
| air/perc | high-pass 700 Hz, +1.5 dB at 2.8 kHz, high shelf +1 dB at 9 kHz |

Master chain, in order:

1. EQ: high-pass 30 Hz, −2 dB at 380 Hz (Q 0.9), +2 dB at 3.2 kHz (Q 0.8),
   +1.5 dB high shelf at 10.5 kHz.
2. Bus compression on 2 ms block RMS: threshold −26 dB, ratio 4:1, 5 ms attack,
   120 ms release, makeup to the target RMS. Peak-normalized palettes are often
   clipped and have 20 dB+ crest; compress before limiting or the master will
   be quiet.
3. Limiter: required gain `min(1, threshold / |x|)`, then a sliding **minimum**
   over ~30 ms so no sample can exceed the ceiling. Threshold −6 dB, final peak
   −0.45 dBFS.

Do not smooth the gain with a filter that can raise it again after the minimum;
that lets peaks through and the final normalization then makes the whole song
quiet. A sliding minimum is the guarantee.

## 7. Stems that sum to the song

Apply the master EQ, compressor gain and limiter gain to the sum, and the same
linear stages and the same gain arrays to each stem. Accumulate in float64.

```python
proc = [master_eq(layer) for layer in layers]
mix = np.zeros(n, np.float64)
for s in proc:
    mix += s
peak = np.abs(mix).max()
pre = mix / peak * 10 ** (-6 / 20)
comp = compressor_gain(pre)          # common envelope
scaled = pre * comp
g = limiter_gain(scaled)             # common envelope
final = 0.95 / np.abs(scaled * g).max()
song = scaled * g * final
stems = [(s / peak) * 10 ** (-6 / 20) * comp * g * final for s in proc]
```

The 16-bit sum should be within 2 LSB of the song (about −95 dBFS); that is
rounding, not a mismatch. Do not level the stems independently afterwards.

## 8. Show the patterns with audio-pattern-reports

The reports are how the user sees what was identified. For each chosen pattern,
give the start and end of its segment window and set `length` to one period so
the report draws every segment:

```json
{
  "title": "Palette patterns used in the song",
  "reports": [{
    "file": "palette.wav",
    "patterns": [
      {"start": 54.9, "end": 62.2, "length": 3.66, "bars": 1,
       "name": "Layer 1", "color": "#4dabf7"}
    ]
  }]
}
```

`bars` is informational. `end` is a search bound: the builder truncates to the
last complete `length` segment, so a two-segment window with
`length = period` shows exactly two segments.

```bash
python3 skills/audio-pattern-reports/pattern_report_builder.py \
  --spec report_spec.json --src /path/to/palette --out artifacts/pattern_reports
python3 skills/audio-pattern-reports/pattern_report_server.py \
  --root artifacts/pattern_reports --port 8000
curl -s -D - -o /dev/null -H 'Range: bytes=0-99' \
  http://127.0.0.1:8000/palette.wav        # expect 206
```

Include the report directory in the delivery zip and show the preview. Report
the builder's exit status; a page that loads is not proof that seeking works.

## 9. Delivery

Package the song, one WAV per stem, and the report directory. Keep the zip
outside itself and verify it with `unzip -t`. State the pattern table (source
times, segments, role), the measured period, and the master numbers.

## 10. Verification checklist

- [ ] All WAVs mono, 44.1 kHz, 16-bit, identical length, 0 clipped samples.
- [ ] Peak, RMS and crest reported; no sample above −0.45 dBFS.
- [ ] Stems vs song: max residual ≤ 2 LSB; report the dB figure.
- [ ] Layer correlation near zero; each stem audible only in its sections.
- [ ] Section RMS arc rises into the full sections and the break is silent.
- [ ] Every pattern has at least two full segments; report builder exits 0.
- [ ] Range request returns 206 and the preview opens.
- [ ] Say plainly that the mix was measured, not auditioned.

## 11. Pitfalls

- An ACF peak at ~0.45 s is a subdivision, not a segment. Keep the 1 s floor.
- Band shares computed from mean magnitude per bin are wrong because wide high
  bands are averaged over more bins. Use band energy and compare the profile
  with the palette average.
- Chroma from raw FFT bins is unreliable; use f0 autocorrelation. Do not layer
  patterns from different tonal eras.
- Do not assume four bars, 131 BPM, or a 7.3 s unit. The measured period is the
  only grid.
- Do not read or require a `passes` file. If one is present, ignore it.
- A compressor's target gain is `-max(over, 0) * (1 - 1/ratio)`. Using
  `min(0, over)` mutes everything below the threshold and produces a 30 dB crest.
- A limiter that smooths gain after the sliding minimum lets peaks through; the
  final normalization then drops the whole mix by 6–10 dB.
- Do not level stems after the master chain; it breaks the exact sum.
