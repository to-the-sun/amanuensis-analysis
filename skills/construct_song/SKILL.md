# Construct Song

Use this skill when the user supplies a palette recording (mono mix plus its
`passes` text file) and wants a finished song and stems, without using the
repository's C tools to build it.

The palette is one continuous performance, not multitracks. The song is built by
finding repeated 4-bar units, selecting units that agree harmonically and differ
rhythmically, and layering them as separate tracks. Each stem is one of those
units played as its own part; it is not a frequency band of one recording.

## Vocabulary

- **Bar**: four beats at the palette tempo, about 1.83 s at 131.4 BPM.
- **Unit**: one 4-bar phrase, the loop length used for layering.
- **Pattern**: a unit taken from a specific pass and bar offset.
- **Layer**: one pattern assigned a role by EQ and level.
- **Stem**: the rendered layer across the whole song, silent where the
  arrangement removes it.

## 0. Inputs

- `palette_<stamp>.wav`: mono PCM; its duration is the whole performance.
- `passes_<stamp>.txt`: one line per pass, `index, loop_point_ms pad;`.
  Pass `i` spans `[loop_point_ms_i, loop_point_ms_{i+1})`. The boundaries are
  loop points, so every pass starts on the musical grid; the last pass runs to
  the end of the file.

Read the file, convert to float, and keep the pass table:

```python
P = []
for line in open(passes):
    q = line.replace(';', ' ').replace(',', ' ').split()
    if q:
        P.append((int(q[0]), float(q[1])))
P.sort()
START = {i: ms / 1000 for i, ms in P}
```

Confirm that pass durations are integer multiples of half a bar before relying
on the grid.

## 1. Grid and tempo

Pass durations are the most reliable tempo evidence: each is an exact multiple
of two beats. Divide a long pass by its plausible bar count and average across
passes. For the reference palette, `47484.8 ms / 26 bars = 1826.34 ms`, giving
131.4 BPM and `BAR = 1.82640 s`; a 4-bar unit is 7.3056 s.

Cross-check with a high-band onset detector (STFT 512, hop 64, flux above
3.5 kHz). Real music here clusters at ~210–260 ms (eighth), ~450 ms (beat) and
~1.8 s (bar). A flat histogram means the material has no usable pulse.

Cross-check repetition with a coarse self-similarity matrix (0.25 s features,
band energies plus 12 log bands): similarity peaks at the 4-bar lag confirm the
unit length. Do not trust a 2-beat "loop point" as the pattern length; the
palette repeats at 4 bars even though boundaries fall every 2 beats.

## 2. Candidate units

Slice every 4-bar unit that fits inside a pass, stepping 4 bars:

```python
unit_len = 4 * BAR
for pidx, ms in P:
    a0 = ms / 1000
    for b in range(0, int((end - a0) / BAR) - 3, 4):
        seg = x[round((a0 + b * BAR) * SR):round((a0 + (b + 4) * BAR) * SR)]
```

Skip units that are silent or shorter than the unit. Record `(pass, bar, time)`
for every candidate; those three values identify it for the rest of the run.

## 3. Per-unit features

Compute four things for each unit.

1. **Band balance** from a 2048-point STFT: energy share below 80 Hz, 80–250,
   250–1200, 1200–4000 and above 4 kHz. This decides role, not instrument:
   low-dominant units become bass, high-dominant units become percussion.
2. **Rhythm signature**: one value per sixteenth (64 slots over 4 bars), from
   the percussive component of the spectrogram (magnitude minus a time-median
   harmonic estimate). Normalize to peak 1. Two units whose signatures are
   uncorrelated occupy different slots and layer cleanly.
3. **Pitch-class content** from f0 tracking, not from raw spectral binning.
   Raw chroma is dominated by low-frequency energy and returns the same answer
   for everything. Autocorrelation per 46 ms frame gives usable f0s:

   ```python
   ac = np.fft.irfft(np.abs(np.fft.rfft(pre * hann)) ** 2, n, axis=1)
   ac /= ac[:, :1]
   k = np.argmax(ac[:, int(SR/700):int(SR/60)], axis=1) + int(SR/700)
   f0 = SR / k
   ```

   Keep frames with periodicity strength above ~0.25, fold to pitch class with
   `round(69 + 12*log2(f0/440)) % 12`, and weight by strength. The reference run
   produced a stable F#/G#/C#/E/A distribution, while the first 185 s of the
   palette is F/C based. Those eras must not be layered together: F against F#
   is a semitone clash.
4. **Phase**: the slot index of maximum percussive energy, modulo 16. Most units
   of one performance share it; units that differ by whole bars can still be
   aligned, but check the shift before trusting it.

## 4. Choose the layer set

Score combinations of four units, all from the mutually compatible tonality:

- **Harmony**: mean pairwise correlation of the f0 pitch-class vectors, target
  above +0.85.
- **Rhythm**: mean pairwise correlation of rhythm signatures; near 0 is ideal,
  strongly positive means doubling the same part.
- **Band separation**: mean `1 - corr(band vectors)`, so the four cover
  different registers.
- **Phase**: prefer pairs whose best circular shift is small, or apply the shift
  to the audio.

Assign roles from the chosen units' own balance rather than from taste:
low-dominant → bass, high-dominant/noisy → percussion, mid-dominant tonal →
chords, mid-high with clear attacks → lead. In the reference run:

| Layer | Source | Role EQ | Level |
|---|---|---|---|
| bass | pass 26, bar 8 (524.7 s) | LP 300 Hz | −20 dBFS |
| perc | pass 18, bar 0 (228.9 s) | HP 1.5 kHz | −25 dBFS |
| chords | pass 22, bar 8 (356.7 s) | 200 Hz–5 kHz | −18 dBFS |
| lead | pass 24, bar 12 (440.7 s) | HP 900 Hz, +3 dB @ 3 kHz | −21 dBFS |

Correlation between the four rendered layers should be near zero; if two layers
correlate strongly they are the same part and one should be replaced.

## 5. Arrangement

Tile each unit to the song length with a ~10 ms equal-power crossfade at every
seam, gate it per section, and shape section dynamics:

```python
SECTIONS = [
    ('intro',   8, {'chords'},                         -2.5),
    ('verse1', 16, {'chords', 'perc', 'bass'},          0.0),
    ('chorus1', 16, {'chords', 'perc', 'bass', 'lead'}, 2.0),
    ('verse2',  8, {'chords', 'bass'},                 -1.0),
    ('build',   8, {'chords', 'perc', 'bass'},          0.5),
    ('break',   1, set(),                             -60.0),
    ('chorus2', 16, {'chords', 'perc', 'bass', 'lead'}, 2.0),
    ('bridge',  8, {'chords', 'lead'},                 -3.0),
    ('final',  16, {'chords', 'perc', 'bass', 'lead'}, 2.5),
    ('outro',   8, {'chords', 'bass'},                 -3.0),
]
```

Use 0.25–0.30 s equal-power fades on every layer entrance and exit, plus a 4 s
fade-in and 7 s fade-out on the shared section gain. One bar of full silence
before a chorus is an effective drop and costs nothing. The reference form is
105 bars, 191.77 s, with section RMS from −23.2 (intro) to −13.9 dBFS (final).

## 6. Master chain

Apply to the summed mix, and to every stem through the same linear stages so the
stems still sum (section 7):

1. Peaking −3 dB at 400 Hz (Q 1.0), peaking +2.5 dB at 3 kHz (Q 0.8), peaking
   +1.5 dB at 10 kHz (Q 0.7), 28 Hz high-pass.
2. Normalize peak to 1.
3. Makeup gain to target RMS, then a lookahead limiter:

   ```python
   g = np.minimum(1.0, thr / (np.abs(sig) + 1e-9))
   g = minimum_filter1d(g, size=2*k_look + 1)       # 3 ms lookahead
   g = uniform_filter1d(g, size=k_rel)              # 35 ms
   g = minimum_filter1d(g, size=k_rel)              # guarantee no overshoot
   sig *= g
   ```

4. Final peak −0.45 dBFS. The reference master measured −15.9 dBFS RMS with
   15.5 dB crest and 0 clipped samples.

## 7. Exact-sum stems

Render each stem with the same common factor:

```python
mstems = {n: master_eq(s) for n, s in stems.items()}
mix = np.zeros(n, np.float64)
for s in mstems.values():
    mix += s                       # float64 accumulation, not sum()
peak = np.abs(mix).max()
common = 10 ** (makeup_db / 20) * limiter_gain(mix / peak * makeup_lin) * final_scalar
song  = (mix / peak * common_lin).astype(np.float32)
stems = {n: (s / peak * common).astype(np.float32) for n, s in mstems.items()}
```

A single common gain keeps the property that the stems add up to the song. In
16-bit output the sum is within 2 LSB (about −95 dBFS); that is rounding, not an
error.

## 8. Verification

- All WAVs mono, 44.1 kHz, 16-bit, identical length; 0 clipped samples.
- Stems vs song: max 2 LSB; report the dB figure.
- Peak, RMS and crest of the song; octave-band shares (this palette lands around
  23% at 160–320 Hz, 39% at 320–640, 14% at 640–2500, 8% at 2.5–5 kHz).
- Section RMS arc shows a real lift into the chorus.
- Layer pairwise correlation near zero.
- Block RMS audit: the only long quiet run should be the intentional break.
- Zip listing and `unzip -t`.

## 9. Pitfalls

- Do not filter loud sections with a 25–40 Hz low-pass through `sosfiltfilt`.
  Edge transients inflated the crest from 12 to 25 dB and forced a −26 dBFS
  master. Leave those sections unfiltered; keep low-pass sweeps for intros,
  bridges and outros only.
- A zero-phase FFT EQ can overshoot on dense material. A minimum-phase biquad
  chain behaves predictably; if a linear-phase chain is used, measure crest
  before deciding on makeup gain.
- HPSS stems are not useful on this material: the percussive mask captures
  almost everything and the harmonic stems come out 20 dB down. Pattern layers
  are the meaningful decomposition here.
- Chroma computed by folding raw FFT bins is unreliable; the low end dominates
  and every unit looks like the same key. Use f0 tracking.
- Do not layer units across a tonal shift. Find the split (here, F/C before
  ~185 s versus F#/G#/C#/E after) and keep layers inside one side.
- Equal-power crossfades at seams are fine, but a lone loud sample at a join is
  usually source material, not an edit click; compare jump density with the
  source before chasing it.
- Never claim the mix was auditioned. Verify structure, levels and sums, and say
  plainly that listening is still required.
