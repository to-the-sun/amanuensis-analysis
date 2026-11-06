# Atonal routing in the flux separator

`analysis/flux_separator.c` only. No fixed limits: lists realloc like `flux_stems`,
and the thresholds are settled in Phase 5. `cumulative_transience.c` and
`analyze.exe` are untouched.

## Assignment

Per frame:

1. Power `|X_k|²`, `k ≤ 4096` (5.38 Hz per bin at 44.1 kHz).
2. Harmonic summation for candidate f0s, greedy: take the best comb, subtract it,
   repeat while the added comb explains more than ε of what remains. No chord size
   cap — a twelve-note cluster yields twelve combs if twelve pass.
3. Claim each accepted f0's predicted harmonics, tolerance a few bins at low
   order, widening with order so a sharp or flat note still captures its series.
4. Claimed bin → the pitch stem of `round(12·log2(f0/440) + 69)`. Unclaimed bin
   → residual.
5. Residual → mode regions: each local maximum on the log-frequency axis owns the
   territory out to the troughs either side, and two regions whose dividing trough
   is shallower than the threshold stay joined. A drum's modes sit far enough
   apart, with troughs deep enough, to stand as separate regions.
6. Regions are tracked into ridges: a region continues the ridge whose territory it
   overlapped in the previous frame, and a ridge with no overlapping region ends.
   Territories come from the frame's own troughs, so tracking chooses no distance
   and needs no gap length.
7. Ridges are grouped into sources by shared decay: ridges rising in the same
   attack and damping in the same band form one group, and a second group exists
   only when the data rejects one. One anchor per group, one slot per anchor, and
   every frame of the group routes there. The slot is the group's mass-weighted
   centroid at its peak-flux frame — it names the address, and does not decide
   atonality or count sources.
8. A destination carrying hardly any energy is dropped here; survivors take the
   existing path unchanged: masked ISTFT, COLA, `compute_stem_transience_flux`,
   salience scaling, `write_wav_16bit`.

Routing runs on STFT power before per-stem normalisation, so stems sum to the
input. Code: `route_frame`, `track_ridges`, `group_ridges` in
`flux_separator_atonal.c`.

## Conservation

One destination per bin per frame, so `Σ stems = original` exactly. Assert it in a
debug build; a leak is silent and reads as bad tuning.

## Output

Atonal stems are exported as more stems: `pitch_stems/`, `[flux]`, the same
`_modified_flux.wav` convention, the same single recombined file. Their slot
numbers occupy a range of their own, so a slot can never read as one of the 128
MIDI pitches.

Output stays 16-bit PCM. One divisor is computed across all stems in place of the
per-stem peak divide, so relative levels survive; a stem quiet enough to round to
silence is meant to be silent.

## Timing

Two passes over a STFT that is already fully materialised, so the lookahead costs a
second pass and a realloc'd ridge and group table, not latency. The window still
smears a transient across 186 ms.

## Drift and identical sources

A ridge keeps its group's slot and the address is fixed at peak flux rather than
per frame, so drift can no longer split a hit or leave a late flux peak in a
neighbouring slot: the double trigger is gone by construction.

Two sources agreeing in tuning and damping stay one group — one anchor, one slot.
That is the right answer for a retrigger of the same drum, and nothing in this
family separates near-identical drums struck together.

## Numbers

| Fact | Value | Consequence |
|---|---|---|
| FFT bin | 5.38 Hz | harmonics resolve while f0 ≫ 11 Hz |
| Semitone at 1 kHz | 59 Hz ≈ 11 bins | addressing limits, not the FFT |
| Hann mainlobe | 4 bins | fundamentals closer than ~4 bins share one region |
| Label map | 2 bytes/bin/frame/channel | ~64 MB for 3 min stereo; 16-bit because destinations are unbounded |
| 0.009 | −41 dBFS | provisional floor for a destination to exist |

## Code

Everything lands in a copy, `analysis/flux_separator_atonal.c`; `flux_separator.c`
stays untouched until the copy is proven.

- **Analysis.** Keep `fft`, `init_fft_tables`, the 8192 / hop-2048 STFT loop, the
  `stft_data` store and `cola_norm`. Delete the `bin_pitch` / `pitch_bins` tables
  and replace them with `uint16_t *labels`, one entry per bin per frame, shared by
  both channels: `1..128` is pitch slot + 1, `512 + ridge` is a residual ridge,
  `0` is nothing.
- **Combs.** New `route_frame()` over the channel-summed power
  `|X_L|² + |X_R|²`. Candidate f0s are the local maxima of the residual; each is
  scored by harmonic summation; the best is accepted, its harmonic bins claimed
  and zeroed in a residual copy, and the search repeats while the best score still
  exceeds ε of the frame's residual energy.
- **Regions and ridges.** `route_frame()` also emits `Region {lo, hi, mass,
  centroid}` for what is left. `track_ridges()` gives each region the ridge whose
  previous span it overlaps most and ends any ridge left without one. `Ridge`
  carries `start_frame`, `end_frame`, and the sums for a least-squares slope of
  `ln(mass)` against frame, which is the decay.
- **Groups.** `group_ridges()` adds a ridge to an existing group when its start is
  within `ATTACK_FRAMES` of the group's start and its decay slope agrees within
  `DECAY_RATIO`; otherwise the ridge opens a group. A group takes the frame of its
  greatest summed mass, the mass-weighted centroid there, and slot `128 + group`.
- **Per-destination synthesis.** The existing mask / inverse FFT / overlap-add
  block becomes a loop over destinations derived from `labels`. Each destination
  keeps a bitmap of the bins it ever uses, so the inner loop walks those bins
  rather than all 4097. The `cola_norm` divide is unchanged.
- **Qualification.** Delete `noise_floor_threshold = overall_peak *
  g_noise_floor_ratio` (line 1906) and the `active_ms >= 99.0 && stem_rms > 1e-6`
  test (line 2123). Count samples where any channel reaches `g_silence_threshold`
  (0.009) and require 99 ms. `g_noise_floor_ratio` and the unused
  `apply_flux_noise_gate` go with them.
- **Flux and level.** Call `compute_stem_transience_flux(..., 0)` — no per-stem
  divide. Hold every destination's flux until all are measured, take one divisor
  from the largest peak, and apply it when writing. `modify_flux_envelope_channel`
  is unchanged and still runs per channel.
- **Output.** Pitch files keep
  `<basename>_pitch_%03d_%s_modified_flux.wav`; atonal files are
  `<basename>_atonal_%03d_modified_flux.wav` in the same folder. The recombined
  file gains the atonal stems and is peak-normalised as it is now (line 2256).
- **Skip check.** `dir_contains_pitch_stems_for` matches `"<basename>_pitch_"`;
  change it to `"<basename>_"` and keep the `.wav` suffix test.

The test build passes `TROUGH_SPLIT 0.5`, `ROUTE_EPS 0.01`, `DECAY_RATIO 3.0`,
and accepts a comb only when at least three harmonics clear 5 % of its
fundamental; all four are provisional.

## Phases

0. **Energy qualification.** A destination is dropped when hardly any of it
   rises above an absolute level for a minimum duration; 99 ms above 0.009 is a
   starting point, not a calibrated value. Absolute rather than a fraction of the
   file peak, so the rule does not move with the mix. No RMS floor, and no
   envelope is zeroed: the test decides existence only. `modified_flux` keeps its
   present derivation from raw flux: peaks, percentile salience, trough
   interpolation.
   *Accept:* near-empty destinations disappear and audible ones survive; the
   values are tuned in Phase 5; for a stem the current build also writes,
   `_modified_flux.wav` is identical.
1. **Claim and label.** Per-bin destination map; pitch stems routed from claimed
   bins; conservation assert. Pitch WAVs keep their names and per-bin masking.
   *Accept:* a three-note chord puts every note's harmonics in the root's stem and
   leaves the other stems silent; per-frame sums match exactly.
2. **Mode regions and ridges.** Per-frame residual regions, territory-overlap
   tracking, ridges in a realloc'd list.
   *Accept:* one sustained noise band is one ridge from start to end; two bursts
   with silence between are two ridges.
3. **Groups and anchors.** Decay-band grouping, one anchor per group at its
   peak-flux frame, atonal stems through the existing ISTFT/flux/salience/WAV path.
   *Accept:* one tom yields one anchor however many modes it shows; kick plus
   cymbal in one frame yield two anchors in separated slots; no slot of a group
   carries a flux peak later than the group's first.
4. **Recombined flux.** One file, atonal stems included.
   *Accept:* peak and shape unchanged on material without percussion; different
   only where percussion exists.
5. **Calibration.** Chords, notes of varying richness, single toms, cymbals and
   noise bursts alone and over chords, and two toms struck together. Fix ε, the
   trough threshold, the decay-band width, and the qualification's level and
   duration on measured leakage; record anchor jitter across repeated hits and
   count any second flux peaks.

## Settled

- Qualifying: a minimum level held for a minimum duration, absolute, for tonal
  and atonal alike; the values are provisional.
- Skip check: a WAV in `pitch_stems/` whose name begins with the source basename
  means that source is done, not the `_pitch_` infix.
- Slot granularity: the 128 semitones, matching the pitch stems.
- Placement: atonal stems go where pitch stems go, including the one recombined
  file.

## Delivery

Branch off `main`, one PR, and hand over the PR link plus
`artifacts/<branch-name>-changes.zip`. Rebuild `flux_separator.exe` with
`make flux_separator.exe` on any source change; the sandbox can compile it but
not run it.
