# Construct Song

Use this skill when the user supplies a palette recording (one continuous mono
performance) and wants a finished song plus separate stems, without the
repository's C tools.

The palette is not a multitrack. The song is assembled from material already in
it: sequences of segments that repeat. You find every such sequence, report
them, and layer a chosen subset as separate parts.

## What is allowed

Construction is limited to chopping, looping, arranging and mixing.

- Allowed: trimming on the grid, equal-power crossfades, gain, fades, panning,
  EQ, compression, limiting.
- Not allowed: synthesis, sample replacement, reverb, delay, chorus,
  saturation, pitch shifting, time-stretching, or any other effect.

EQ is mixing: it changes the level of parts of the spectrum. Use it to give
each layer a role and to keep layers from masking each other.

## Guarantees

- No `passes` file, loop-point table, or project-specific analysis code.
- No tempo, meter, bar count or phrase length is assumed. The period is
  measured from the audio.
- A **pattern** is a maximal run of at least two consecutive segments that
  share a through-line: a beat and/or a pitch that occurs at the same place in
  every segment. All such patterns are reported, not just the ones used.
- Each stem is one pattern with its own gain/EQ role, never a frequency band of
  one mixdown.
- The stems sum to the song: every stem passes through the same master stages
  and the same gain envelopes.
- Delivery is one zip containing the song, every stem and the full report.

## Vocabulary

- **Segment**: one measured repetition period.
- **Period**: the repeating time unit found by autocorrelation, at least 1 s so
  subdivisions are not mistaken for segments.
- **Pattern**: a maximal run of at least two consecutive similar segments that
  passes the through-line check. Its loop length is `segments * period`.
- **Complexity**: how much sequence a pattern contains. Longer runs and more
  variation between their segments rank higher, but the through-line must
  still hold.
- **Layer**: a pattern with a gain and EQ role.
- **Stem**: a layer rendered across the whole song, silent where the
  arrangement removes it.
- **Unit**: one arrangement step, normally two periods.

## 0. Analyze the palette

Start with the audio-pattern-reports analyzer. It gives silence-delimited
sections, novelty and lag peaks, which show where the material changes.

```bash
python3 skills/audio-pattern-reports/analyze_audio.py palette.wav --json analysis.json
```

Use the sections to avoid cutting across silence or a tonal change. Treat its
lag peaks as candidates only; section 1 measures the period directly.

## 1. Measure the period

Compute a positive log-flux onset envelope, then its autocorrelation. Search
lags from 1.0 s to 8.0 s; anything shorter is usually a subdivision.

```python
NFFT, HOP = 2048, 512
S = np.abs(np.fft.rfft(frames * hann, axis=1))
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
is at least 85% of the strongest and whose 2x and 3x multiples are also at
least 40% of the strongest. Refine with parabolic interpolation. That is the
period; `duration / period` is the segment count.

If no peak exceeds ~0.1, the material has no usable repetition; say so instead
of inventing a grid.

## 2. Segment grid, runs and complexity

Try eight phase offsets inside one period and keep the phase that maximizes the
mean similarity of adjacent segments. For each segment compute:

1. **Spectrum**: mean log-band vector over the segment (64 values).
2. **Rhythm**: onset envelope resampled to 24 slots.
3. **Pitch content**: f0 chroma (section 3).

Similarity is `0.5 * cosine(spectrum) + 0.5 * corr(rhythm)`. A run is a maximal
sequence of adjacent segments above `max(0.45, percentile(similarity, 60))`.

A run is a pattern only if the through-line holds across **every** segment:

- **Common beat**: the strongest slot of the run's mean rhythm signature is
  also at least half of that segment's own maximum in at least 65% of segments.
- **Consistency**: each segment's rhythm signature correlates at least 0.30
  with the run mean.
- **Pitch**: mean pairwise chroma correlation inside the run is at least 0.45.

Rank patterns by complexity, `(segments - 1) * 2 + min(variation, 1.5)`, where
variation is the mean pairwise distance between the segments' band profiles.
Length dominates; variation breaks ties. Report every pattern that passes,
whether or not it is used in the song.

## 3. Choose the layer set

The song may use any number of patterns, including all of them. Select for
coherence, not for a fixed count. For each candidate compute:

- **Tonal compatibility**: pairwise correlation of f0 chroma with the chosen
  layers. Require at least 0.55. Never layer across a tonal shift; a semitone
  clash cannot be fixed by EQ.
- **Transient alignment**: correlation of the mean rhythm signatures. Positive
  values mean the patterns hit at the same moments, which is desirable. Weight
  it strongly and reject a set whose mean alignment is below −0.1.
- **Complexity**: prefer more complex patterns, all else equal.
- **Spectral contrast**: mean absolute difference of the band profiles, so the
  layers do not all occupy the same band.
- **Coverage**: each layer comes from a different part of the performance.

Reject patterns more than 9 dB below the loudest pattern before choosing
layers: they need so much makeup gain that they raise noise. Keep them in the
report; they are still patterns.

Score pairs and triples first, then start from the best-scoring coherent set
rather than the single most complex pattern: a complex outlier can fail to
align with everything else. Then add the best-scoring compatible pattern until
no candidate qualifies or the layer count reaches five. Fewer coherent layers
beat five that mask each other.

Assign roles from each pattern's band profile relative to the palette average,
`d = mean_log_band - global_mean_log_band`: the band where `d` is largest is
its role (low → bass, mid → chords, high → air/percussion).

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

All patterns sit on the same measured grid, so their segment boundaries align
by construction. If a pattern still fights the grid, cross-correlate rhythm
signatures and rotate its audio circularly by the best lag inside its own
length. Rotation is a chop, not a time-stretch. Do not resample.

## 5. Arrange

Tile each pattern's full run to the song length with a ~10 ms equal-power
crossfade at every seam, then gate it per section. Different patterns have
different loop lengths, which is fine: they all share the period grid.

```python
SCHEDULE = [
    ('intro', 2, [0, 1],      -7.0),
    ('A',     4, [0, 1, 2],    0.0),
    ('B',     4, [0, 1, 2, 3], 1.0),
    ('break', 1, [],         -60.0),
    ('A2',    2, [0, 1, 2],   -1.0),
    ('C',     4, list(range(N)), 1.5),
    ('D',     2, [1, 2, 3, 4], 0.5),
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

Do not smooth the gain in a way that raises it again after the minimum; that
lets peaks through and the final normalization then makes the song quiet.

## 7. Stems that sum to the song

Apply the master EQ, compressor gain and limiter gain to the sum, and the same
linear stages and gain arrays to each stem. Accumulate in float64.

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
final = 0.95 / max(np.abs(scaled * g).max(),
                   max(np.abs(s * comp * g).max() for s in proc))
song = scaled * g * final
stems = [(s / peak) * 10 ** (-6 / 20) * comp * g * final for s in proc]
```

Scale by the loudest stem, not just the song, or a stem can clip at 16-bit and
break the sum. The 16-bit sum should stay within 2 LSB (about −95 dBFS); that
is rounding, not a mismatch.

## 8. Report every pattern

The report is where the user sees what was found. Include **all** patterns that
pass the through-line check, not only the chosen layers, and mark the used
ones. Set `length` to one period so every segment is drawn.

```json
{
  "title": "All patterns found in the palette",
  "reports": [{
    "file": "palette.wav",
    "patterns": [
      {"start": 3.70, "end": 22.00, "length": 3.66, "bars": 1,
       "name": "P1 · 5 seg · used", "color": "#4dabf7"},
      {"start": 36.60, "end": 43.90, "length": 3.66, "bars": 1,
       "name": "P2 · 2 seg", "color": "#ffa94d"}
    ]
  }]
}
```

`bars` is informational. `end` is a search bound: the builder truncates to the
last complete `length` segment, so a five-segment run shows five segments.

```bash
python3 skills/audio-pattern-reports/pattern_report_builder.py \
  --spec report_spec.json --src /path/to/palette --out artifacts/pattern_reports
python3 skills/audio-pattern-reports/pattern_report_server.py \
  --root artifacts/pattern_reports --port 3007
curl -s -D - -o /dev/null -H 'Range: bytes=0-99' \
  http://127.0.0.1:3007/palette.wav        # expect 206
```

The report directory goes inside the single delivery zip (section 9). Show the
preview and report the builder's exit status; a page that loads is not proof
that seeking works.

## 9. Delivery

Deliver everything in **one** zip. Never hand over separate song and report
archives.

```
construct_song/
  palette_song.wav
  stem_01.wav
  stem_02.wav
  ...
  report/
    index.html
    <palette>.html
    <palette>.wav
    data/...
```

Copy the song and stems into the archive root and the report directory, with
its audio, under `report/`. Keep the zip outside itself and verify it with
`unzip -t`. State the period, the pattern table (source times, segment counts,
complexity, used or not), and the master numbers.

## 10. Verification checklist

- [ ] All WAVs mono, 44.1 kHz, 16-bit, identical length, 0 clipped samples.
- [ ] Peak, RMS and crest reported; no sample above −0.45 dBFS.
- [ ] Stems vs song: max residual ≤ 2 LSB; report the dB figure.
- [ ] Every reported pattern has at least two full segments in the report.
- [ ] Song layers are tonally compatible and non-negatively transient-aligned.
- [ ] Section RMS arc rises into the full sections and the break is silent.
- [ ] Report builder exits 0 and the Range request returns 206.
- [ ] Say plainly that the mix was measured, not auditioned.

## 11. Pitfalls

- An ACF peak at ~0.45 s is a subdivision, not a segment. Keep the 1 s floor.
- Band shares from mean magnitude per bin are wrong because wide high bands are
  averaged over more bins. Use band energy and compare with the palette average.
- Chroma from raw FFT bins is unreliable; use f0 autocorrelation. Do not layer
  patterns from different tonal eras.
- Do not assume four bars, 131 BPM, or a 7.3 s unit. The measured period is the
  only grid.
- Do not read or require a `passes` file. If one is present, ignore it.
- A run is not a pattern just because adjacent segments are similar. It needs
  the common beat and pitch through-line; check every segment.
- Ranking by two-segment spectral variance alone puts short, dissimilar pairs
  above long coherent phrases. Weight length first, variation second.
- A compressor's target gain is `-max(over, 0) * (1 - 1/ratio)`. Using
  `min(0, over)` mutes everything below the threshold and produces a 30 dB crest.
- A limiter that smooths gain after the sliding minimum lets peaks through.
- Do not level stems after the master chain; it breaks the exact sum.
- Scale the final master by the loudest stem as well as the song, or a stem can
  clip at 16-bit and break the sum.
