# Analyze Palettes

Use this skill when the user gives one or more palette recordings and wants:

- repeated sections/patterns measured independently of project code;
- loop/bar/segment lengths measured;
- an interactive HTML report per file with the full waveform, pattern regions,
  segment rows, zooming, playback and playhead jumping;
- a song assembled from the patterns, with a mastered WAV and one WAV per
  pattern stem, plus a player page with per-stem arrangement waveforms;
- the reports, song and audio packaged for download.

The goal is a reproducible analysis, not a black-box detector. Report what is
measured, what is estimated, and where the uncertainty is.

## Core vocabulary

- **Bar**: the base pulse of a region. Equivalent to a measure in most standard
  depictions of music. Estimate it from the pass durations (every pass is an
  integer number of bars) and confirm with onset spacing.
- **Pass**: one recorded loop cycle. Its **loop point** is the millisecond
  position at which it starts. Pass `i` spans
  `[loop_point_i, loop_point_{i+1})`; the last pass runs to the end of the file.
- **Segment**: a single iteration of a repeating pattern. All segments within
  the same pattern have the same length.
- **Pattern**: one contiguous repeated region of at least three segments. A
  pattern always begins on a pass loop point plus a whole number of bars.
- **Pattern region**: the span of audio the loop was measured over, from its
  start to the end of its last complete segment. This is the unit the song uses,
  whole.

Patterns are independent of each other. A file may hold one loop length in every
region, or a different length in each; both are normal, and neither is imposed
by the tool. Uniform segment length is required only *within* a pattern, never
across patterns or across the file. Measure each region on its own terms.

## 0. Inputs and preparation

1. Extract archives and list audio files. Preserve the originals.
2. Probe duration, sample rate and channel count.
3. Convert non-WAV input to mono 16-bit PCM WAV for the report builder:

   ```bash
   ffmpeg -v error -y -i input.mp3 -ac 1 -ar 44100 -c:a pcm_s16le output.wav
   ```

4. **Find the passes file. It is required.** Keep it beside the audio:
   - `palette_<stamp>.wav` pairs with `passes_<stamp>.txt`;
   - otherwise `passes*.txt` in the same folder.

   `passes.py` does this discovery for the scripts. If no passes file is found,
   stop and ask for it; do not fall back to silence-delimited starts. Do not use
   repository-specific audio analysis code unless the user asks.
5. Create a workspace output directory, for example
   `artifacts/<batch>_pattern_reports`. Keep generated data with the reports.
6. Dependencies: Python 3, NumPy, SciPy. `ffmpeg` only for non-WAV input.

### 0a. The passes format

One line per pass, `index, loop_point_ms pad;`. The pad is optional and is the
arbitrary offset used by the C analyzer to align a stem to its pass:

```text
0, 977.324 1.;
1, 8531.882244 1.;
2, 9594.195189 1.;
```

Read the file, keep the pass table sorted by index, and convert milliseconds to
seconds:

```python
rows = passes.parse_passes(passes_path)
spans = passes.pass_spans(rows, duration)
```

Each pass duration must be very close to an integer number of bars. A first
pass may be a partial pickup and a last pass may be a short tail; both are
normal and are excluded from patterns, not stretched onto the grid.

## 1. Passes determine where patterns begin

Run the bundled helper:

```bash
python3 analyze_audio.py input.wav [more.wav ...] --json analysis.json
```

It computes, independently of the project:

- the pass table, an estimated bar, and runs of consecutive equal-length passes;
- a 64-band log-mel spectrogram (STFT 2048, hop 1024, about 43 fps);
- a positive spectral-flux onset envelope;
- band envelopes for sub, bass, lowmid, mid, high and air;
- silence-delimited sections, for reference only;
- a low-rate self-similarity novelty curve;
- autocorrelation and lag-similarity peaks per pass run;
- a suggested loop length per pass run, with mean similarity and confidence.

The helper prints candidates; verify them rather than accepting them blindly.

### 1a. Regions come from passes

Passes are the composer's own loop boundaries, so they are the start grid:

1. Group consecutive passes with the same duration (within a few tens of
   milliseconds). Each run is a section; a run's first pass loop point is the
   pattern start.
2. One pattern per run, unless the loop itself changes inside the run.
3. Do not start a pattern from a silence gap or an arbitrary ACF peak. If a run
   begins with a fade, leave the fade out of the region by starting at the pass
   loop point, not before it.
4. Skip the intro/first partial pass and any final tail shorter than three
   segments. A pass whose duration is not an integer number of bars is not a
   pattern start; report it as a pickup or tail.
5. Do not carve a run down to the bars that repeat most exactly. A pattern is
   what a listener hears looping through the whole pass run.

### 1b. The loop is an integer number of bars

For each run:

1. Estimate the base bar from the pass durations: each pass is an integer
   number of bars. Confirm with onset spacing. A file can change tempo, so
   measure per run rather than assuming one bar for the file.
2. Test integer bar lengths that leave at least three segments in the run.
   Include the pass length itself as a candidate.
3. Score each candidate by **mean lag similarity over the whole run**: the
   average, across every frame of the run, of the similarity between a frame
   and the frame one loop later.
4. Prefer the shortest candidate that explains the whole run. A stronger score
   at 2× a shorter loop is usually the same loop with a two-bar layer on top.
5. Confirm it is a **peak**: the chosen lag must score clearly above lags that
   are not near-integer bar multiples. A held pad or a fade tail matches at
   every lag, so a high score alone is not evidence.

Do **not** gate candidates on the fraction of frames that exceed a strict
similarity. Real loop regions score about 0.55–0.81 on the mean, and a
frame-fraction gate rejects most of them while ranking steady textures above
the actual music.

### 1c. Checks

1. **Pass grid.** The chosen start must equal the run's first loop point. The
   report builder rejects a start that is more than 50 ms off the pass bar grid.
2. **Fundamental vs harmonics.** A strong peak at 2L, 3L or 4L is often a
   harmonic of the loop, not the loop itself. Compare onset ACF, mel lag
   similarity and band ACF. Prefer the shortest plausible peak consistent with
   the musical structure.
3. **Nested periods.** A low band may repeat at half the full loop while a
   high layer repeats only at the full loop. Choose the full loop when the
   run's high-level content requires it.
4. **Cycle count.** Check that `(end - start) / loop_length` is close to an
   integer. A remainder is normal at the end and is excluded: the pattern ends
   at the last full loop. A region of fewer than three full segments is not a
   pattern.
5. **Bar multiple.** Express the loop as an integer number of bars.
6. **Confidence.** Record whether the loop is clear, plausible, or ambiguous,
   and record the mean similarity. A useful tolerance is a few tens of
   milliseconds for the loop estimate.

Write down a table before building anything:

| File | Pattern | Pass | Start (s) | End (s) | Loop (s) | Bars | Mean sim | Segments |
|---|---|---|---:|---:|---:|---:|---:|---:|
| palette_2026-11-10 | 1 | 2–5 | 9.594 | 46.195 | 3.652825 | 2 | 0.58 | 10 |

## 2. Build the report specification

Create a JSON spec. Paths in `file` are relative to `--src` unless absolute.
`start` must be a pass loop point; `end` is a search bound that the builder
truncates to the last complete segment.

```json
{
  "title": "Analyze Palettes",
  "description": "Patterns anchored to pass loop points.",
  "reports": [
    {
      "file": "palette_2026-11-10.wav",
      "patterns": [
        {
          "start": 9.594,
          "end": 46.195,
          "length": 3.6524,
          "bars": 2,
          "name": "Pattern 1",
          "color": "#4dabf7"
        }
      ]
    }
  ]
}
```

`start`, `end` and `length` are seconds. `bars` is informational for the card.
`name` and `color` are optional. The builder finds the passes file beside the
audio, assigns each pattern to the pass containing its start, and records the
pass index and bar offset in `summary.js`.

Build:

```bash
python3 pattern_report_builder.py \
  --spec report-spec.json \
  --src /path/to/audio \
  --out artifacts/pattern_reports
```

The builder fails if the passes file is missing or a pattern start is off the
pass bar grid. That failure is intentional: pattern starts come from the passes
file, not from a guessed boundary.

Output layout:

```
pattern_reports/
  index.html
  <audio-basename>.html
  <audio-basename>.wav
  data/<audio-basename>/
    summary.js
    l8_*.js
    l64_*.js
    l512_*.js
```

The builder validates the resident coarse min/max data and the first fine
chunk against the PCM before finishing.

## 3. Build the song from the patterns

A song is layered from the same patterns the report shows, and every pattern
used in it is used in its entirety. A stem is the pattern's whole measured
region, first sample to last, placed once: no segment is dropped, no portion is
silenced, gated or faded out early, and the region is never tiled or looped to
fill more time.

The passes file decides where each pattern begins. A pattern that starts on its
pass loop point enters at bar 0; a pattern that starts `k` bars into its pass
enters at bar `k`, after shifting the earliest pattern to bar 0. There is no
manual `starts` list: the report's `summary.js` stores the pass-derived
`entryBar` for each pattern, and `song_builder.py` uses it. Adding `song.starts`
to the spec is an error.

Add a `song` block to the report object for levels only:

```json
"song": {
  "targets": [-20, -20, -20, -20]
}
```

- `targets`: pre-master RMS target in dBFS per pattern, default -20.
- Patterns may differ in segment length and bar length; the builder places each
  stem at its own entry bar and needs no common grid.
- Song length is fixed by the patterns, not chosen: it is the furthest entry
  time plus that pattern's whole region.

```bash
python3 song_builder.py --spec report-spec.json \
  --report-dir artifacts/pattern_reports
```

The builder reads the report's `summary.js` and copied WAV, so it uses exactly
the patterns, pass starts and segment grid the report shows.

Output:

```
pattern_reports/
  song.html
  song/
    finished_song.wav
    stems/pattern_1.wav ...
    data/arr.js  a4096.js  a512.js  a64.js
```

Guarantees:

- each stem is the whole measured region, once, from first sample to last,
  with no truncation, muting, gating or tiling; the only attenuation is a 4 ms
  anti-click ramp at each end of the region;
- stems are trimmed to their targets, then share one master scale;
- the master is written as the exact sum of the quantized stems, so the stems
  reconstruct it sample for sample and the builder's sum check is 0 LSB;
- there is no master fade in or out, so no pattern's tail is faded early;
- the index gets a Song card;
- peak levels load as script tags, so `song.html` works from `file://`.

The page plays the master or all stems in sync, seeks, mutes individual stems,
and draws one arrangement row per stem. Drag horizontally on a row to zoom,
right-click to zoom out, Reset Zoom to return, and click to jump and play.

Song-level checks:

- [ ] Builder exits 0; printed stem delta is 0 LSB; clipped is 0.
- [ ] Each stem's audio is exactly its pattern's measured region
      (`segments × length`) once, beginning at its pass-derived entry bar.
- [ ] All stem rows draw in `song.html`, silent before their entry bar.
- [ ] A deep zoom loads a finer `a*.js` level and Reset Zoom restores the view.
- [ ] Click-to-seek starts playback; mute toggles one stem.

## 4. Memory-safe waveform design

Do **not** inline the whole PCM or a dense waveform in the HTML. The report
uses a coarse resident tier plus on-demand min/max chunks:

- `summary.js` contains `window.PATTERN_META` and a base64 int16 coarse
  min/max array at step 4096. This is the only waveform data loaded initially.
- Fine tiers are min/max pairs at steps 8, 64 and 512 samples, written in
  chunks of 65,536 bins.
- On render, the client chooses the finest tier satisfying
  `visibleSamples / step <= width * 4`, and requests only the visible chunks.
- Chunks are cached with a 192 MB byte budget; oldest entries are evicted first.
- If the desired tier is not resident, rendering falls back to a coarser level.
- The full view therefore loads zero `l*.js` chunks. Zooming progressively
  loads `l512`, then `l64`, then `l8`.

Keep this invariant when modifying the report. A full view must remain usable
with only `summary.js`.

## 5. Interaction requirements

The report must provide:

- full-file waveform and a colored band for each pattern;
- a row per pattern with one numbered cell per full segment;
- clickable segment chips that jump to the segment start and play;
- drag horizontally to zoom into a time range;
- right-click to zoom out one level; a Reset Zoom button;
- click the waveform to seek and play;
- a red playhead that follows `audio.currentTime` via `requestAnimationFrame`;
- a native `<audio controls>` element using the copied WAV;
- segment grid lines thinned when spacing is under ~2.5 px;
- an index page linking all reports.

Audio seeking requires a server that supports HTTP byte ranges. Use the
bundled server, not a plain static server:

```bash
python3 pattern_report_server.py --root artifacts/pattern_reports --port 8000
```

It binds `0.0.0.0`, serves the report directory, and answers `Range` requests
with `206 Partial Content`. Verify:

```bash
curl -s -D - -o /dev/null -H 'Range: bytes=0-99' \
  http://127.0.0.1:8000/example.wav
```

Expect `206` and `Content-Range`.

## 6. Validation checklist

Builder-level:

- [ ] Command exits 0.
- [ ] `summary.js` parses; duration, sample rate, pattern count and segment
      count match the spec.
- [ ] Every pattern's `start` equals a pass loop point; `pass` and `startBar`
      are present and `entryBar` is the shifted start bar.
- [ ] Every pattern's segments are exactly `length` long; no partial tail
      segment exists.
- [ ] Coarse min/max matches the WAV at sampled bins (the builder asserts a
      few bins).
- [ ] `l8_0.js` exists and its first bins match the PCM.
- [ ] When a song is built, the stem sum check passes and the song page loads
      its arrangement data.
- [ ] No pattern region was reduced to a handful of bars to raise its mean
      similarity.

Browser-level, when a browser is available:

- [ ] Initial full view loads `summary.js` only; no `l*.js` requests.
- [ ] Drag zoom narrows the view and eventually loads a finer chunk.
- [ ] Right-click zoom-out and Reset Zoom return to previous/full views.
- [ ] A segment chip sets `audio.currentTime` to that segment's start and
      starts playback; the playhead is visible at the correct position.
- [ ] Pattern Play starts at the pattern start.
- [ ] No console errors and no failed chunk/audio requests.
- [ ] The index page opens each report.

If the browser automation wrapper errors, use the available DOM/evaluate and
click tools instead of claiming the interaction was tested. A screenshot is
not proof of playback or seeking.

## 7. Packaging

```bash
cd artifacts
zip -r analyze-palettes.zip pattern_reports \
  -x 'pattern_reports/analyze-palettes.zip'
unzip -t analyze-palettes.zip
```

Include the index, all HTML reports, the WAVs, the `data/` chunks, and when a
song was built, `song.html` plus `song/` (master, stems and `song/data/`). Do not
include the zip inside itself. Report the final size and entry count.

## 8. Worked example

**`palette_2026-11-10-18-16-29.wav`**, 609.10 s mono 44.1 kHz, with 29 passes.
Every pass is an integer number of 1.826 s bars; the pass lengths grow from 5 to
26 bars. Runs of equal-length passes are the patterns:

| Pattern | Passes | Start–End (s) | Loop (s) | Bars | Segments | Mean sim |
|---|---|---:|---:|---:|---:|---:|
| 1 | 2–5 | 9.59–46.12 | 3.652400 | 2 | 10 | 0.62 |
| 2 | 6–9 | 46.20–90.02 | 1.826200 | 1 | 24 | 0.66 |
| 3 | 10–13 | 90.04–148.48 | 3.652400 | 2 | 16 | 0.64 |
| 4 | 14–15 | 148.50–185.02 | 3.652400 | 2 | 10 | 0.63 |
| 5 | 16–17 | 185.03–228.86 | 7.304800 | 4 | 6 | 0.69 |
| 6 | 18–19 | 228.87–280.00 | 7.304800 | 4 | 7 | 0.67 |
| 7 | 20 | 280.00–309.22 | 3.652400 | 2 | 8 | 0.67 |
| 8 | 21 | 309.23–338.45 | 7.304800 | 4 | 4 | 0.30 |
| 9 | 22 | 342.12–378.64 | 7.304800 | 4 | 5 | 0.48 |
| 10 | 23 | 378.64–418.82 | 3.652400 | 2 | 11 | 0.40 |
| 11 | 24 | 418.82–462.65 | 3.652400 | 2 | 12 | 0.43 |
| 12 | 25–27 | 462.66–605.10 | 5.478600 | 3 | 26 | 0.52 |

The 8.53 s first pass (4.67 bars), the 1.06 s false-start pass, and the 4.0 s
final tail are not patterns. All twelve patterns begin on their run's first
pass loop point (`startBar` 0), so the song enters them together on bar 0 and
its length is the longest region, 142.44 s (78 bars). Every pattern is used
once and whole; the stem sum is 0 LSB and 0 samples clip.

## 9. Pitfalls and lessons

- A passes file is required. Do not silently fall back to silence-delimited
  starts or a staggered song plan; without the passes file the pattern starts
  are guesses.
- The pass loop point is a start, not necessarily the loop length. Measure the
  loop inside the run; a 26-bar pass can hold a 3-bar loop.
- Pattern starts must sit on the pass bar grid. The report builder rejects a
  start more than 50 ms off it, so fix the spec rather than loosening the check.
- `song.starts` is gone. Entries come from the passes file; do not reintroduce a
  manual start list or a staggered default.
- Do not name a helper `inspect.py`; it shadows the standard library. Use
  `analyze_audio.py` or another distinct name.
- `scipy.io.wavfile` handles WAV; convert other formats first.
- The analysis helper's STFT memory grows with duration. For very long files,
  analyze sections separately or downsample; the report data is chunked.
- ACF peaks are not segment boundaries. A strong peak can be a harmonic or a
  half-time layer. Cross-check with other bands and the novelty curve.
- Do not gate pattern candidates on the fraction of frames above a strict
  similarity. Steady textures and fade tails score near 1.00 at every lag.
- Do not shrink a region to the bars with the highest similarity. A region of
  ten to thirty segments that loops approximately is the pattern a listener
  hears; three bars of it are not.
- The reports use a uniform grid within each pattern. Do not describe the grid
  as measured per-segment timing, and do not assume two patterns share a grid.
- With several stems, independently rounded int16 stems can differ from a
  separately rounded master by a few LSB. Write the master as the clipped sum
  of the quantized stems so the reconstruction is exact.
- Do not change repository source, commit, or open a PR for artifact-only work
  unless the user explicitly asks. Keep generated files under `artifacts/`.
- The report server must support Range; otherwise WAV seeking can fail.
- A chunk file holds `CHUNK_BINS` bins, which is `2 * CHUNK_BINS` interleaved
  int16 values. Slice the interleaved min/max array by `2 * CHUNK_BINS`, not
  `CHUNK_BINS`; otherwise chunk indices point at the wrong audio.
- If a preview host rejects plain curl with 403, that may be the preview's
  authentication layer, not a broken app. Check loopback and use the preview
  panel.
- A song may not repeat a pattern to fill time or gate one out of part of the
  song: that stops it being heard whole.
- Keep the zip out of itself. Re-run the zip after adding a download link if
  the link target must be inside the served folder.
