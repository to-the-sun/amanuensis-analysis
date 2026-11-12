# Audio Pattern Reports

Use this skill when the user gives one or more audio files and wants:

- repeated sections/patterns found independently of project code;
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
  depictions of music, generally smaller than or the same size as the following
  measurements, though not necessarily. A file can change tempo, so measure the
  bar in each region rather than once for the file.
- **Segment**: a single iteration of a repeating pattern. This is not necessarily
  the same as a bar. All segments within the same pattern have the same length.
- **Pattern**: one contiguous repeated region of at least three segments. A
  shorter tail is excluded rather than reported, so every segment of that
  pattern has a uniform length.
- **Pattern region**: the span of audio the loop was measured over, from its
  start to the end of its last complete segment. This is the unit the song uses,
  whole.

All segments in a pattern have the same nominal length because the report
imposes a uniform grid. The report does not independently detect every segment
boundary.

Patterns are independent of each other. A file may hold one loop length in every
region, or a different length in each; both are normal, and neither is imposed
by the tool. In one file a 2-bar loop can run for thirty segments in one section,
a 4-bar loop for fourteen in the next and a 3-bar loop for nineteen after that.
Uniform segment length is required only *within* a pattern, never across patterns
or across the file. Measure each region on its own terms.

## 0. Inputs and preparation

1. Extract archives and list audio files. Preserve the originals.
2. Probe duration, sample rate and channel count.
3. Convert non-WAV input to mono 16-bit PCM WAV for the report builder:

   ```bash
   ffmpeg -v error -y -i input.mp3 -ac 1 -ar 44100 -c:a pcm_s16le output.wav
   ```

   The builder can average stereo WAVs itself, but mono 16-bit WAV is the most
   predictable input. Do not upgrade dependencies just for this workflow.
4. Create a workspace output directory, for example
   `artifacts/<batch>_pattern_reports`. Keep generated data with the reports.
5. Dependencies: Python 3, NumPy, SciPy. `ffmpeg` only for non-WAV input.
   Do not use repository-specific audio analysis code unless the user asks.

## 1. Find pattern regions and loop candidates

Run the bundled helper:

```bash
python3 analyze_audio.py input.wav [more.wav ...] --json analysis.json
```

It computes, independently of the project:

- a 64-band log-mel spectrogram (STFT 2048, hop 1024, about 43 fps);
- a positive spectral-flux onset envelope;
- band envelopes for sub, bass, lowmid, mid, high and air;
- silence-delimited sections using 0.1 s RMS, −50 dB, 0.5 s minimum silence;
- a low-rate self-similarity novelty curve;
- autocorrelation and lag-similarity peaks per section;
- band-wise autocorrelation peaks.

The helper prints candidates; it does not choose patterns for you.

### 1a. Regions come from sections

Start from the silence-delimited sections: they are the composer's own
divisions, and a section is normally one pattern. Report one pattern per
section, with the region covering the section from just after the gap to the end
of its last complete segment. Do not carve a section down to the few bars that
repeat most exactly: a pattern is what a listener hears looping through the
whole section, and a region of ten, twenty or thirty segments is a normal
result.

Split a long section only where the loop itself changes — a new bar length, a
new harmonic rhythm, an obvious change of density. Two or three sub-regions of
sixty to a hundred seconds each are typical for a five-minute section. Do not
leave a stretch of a section unrepresented when it loops at its own length.

### 1b. The loop is an integer number of bars

For each region:

1. Estimate the base bar from onset spacing or the ACF, typically 1.8–2.2 s in
   this material but measure it per region.
2. Test 1, 2, 3, 4 and 8 bars as candidate loop lengths.
3. Score each candidate by **mean lag similarity over the whole region**: the
   average, across every frame of the region, of the similarity between a frame
   and the frame one loop later. The candidate region must be at least three
   segments long.
4. Prefer the shortest candidate that explains the whole region. A stronger
   score at 2× or 4× a shorter loop is usually the same loop with a
   two- or four-bar layer on top — but take the longer one when the region's
   high-level content genuinely needs it (see the nested-period note below).
5. Confirm it is a **peak**: the chosen lag must score clearly above lags that
   are not near-integer bar multiples of the region. A held pad or a fade tail
   matches at every lag, so a high score alone is not evidence — the score has
   to fall away at neighbouring non-multiple lags.

Do **not** gate candidates on the fraction of frames that exceed a strict
similarity. Real loop regions here score about 0.55–0.81 on the mean, and a
frame-fraction gate at 0.8 rejects most of them while ranking the fade tail and
long held textures — which match at every lag, frame after frame — above the
actual music. The mean over the whole region plus the peak test is what
distinguishes a loop from a steady texture; a frame-wise threshold does not.

### 1c. Checks

1. **Section boundaries.** Prefer actual silence gaps. If the file is
   continuous, use novelty peaks and clear changes in timbre/level. Leave a
   little margin around fades; do not start a pattern in the middle of a tail.
2. **Fundamental vs harmonics.** A strong peak at 2L, 3L or 4L is often a
   harmonic of the loop, not the loop itself. Compare the onset ACF, mel lag
   similarity and band ACF. Prefer the shortest plausible peak that explains
   the whole section and is consistent with the musical structure.
3. **Nested periods.** A low band may repeat at half the full loop while a
   high layer repeats only at the full loop. Choose the full loop when the
   section's high-level content and SSM block require it. In the worked
   example, Pattern 4's low band has a ~4.1 s half-period but the full pattern
   is ~8.24 s.
4. **Cycle count.** Check that `(end - start) / loop_length` is close to an
   integer. A remainder is normal at the end and is excluded: the pattern ends
   at the last full loop. If the remainder is almost a whole loop, revisit the
   start/end. A region of fewer than three full segments is not a pattern.
5. **Bar multiple.** Express the loop as an integer number of bars. Typical bar
   lengths here are about 1.8–2.2 s, but measure rather than assuming. The bar
   can differ between regions of the same file.
6. **Confidence.** Record whether the loop is clear, plausible, or ambiguous,
   and record the mean similarity. Do not claim sample-accurate boundaries from
   an ACF peak. A useful tolerance is a few tens of milliseconds for the loop
   estimate.

Write down a table before building anything:

| File | Pattern | Start (s) | End (s) | Loop (s) | Bars | Mean sim | Segments (all full) |
|---|---|---:|---:|---:|---:|---:|---:|
| example.wav | 1 | 4.10 | 92.60 | 4.271 | 2 | 0.65 | 20 |

## 2. Build the report specification

Create a JSON spec. Paths in `file` are relative to `--src` unless absolute.

```json
{
  "title": "Audio Pattern Reports",
  "description": "Repeated sections with loop segments, zooming and playback.",
  "reports": [
    {
      "file": "example.wav",
      "patterns": [
        {
          "start": 4.10,
          "end": 92.60,
          "length": 4.271,
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
`name` and `color` are optional. Treat `end` as a search bound: the builder
clamps it to the file duration and truncates the pattern to the last complete
`length`-sized segment, dropping any shorter tail.

Build:

```bash
python3 pattern_report_builder.py \
  --spec report-spec.json \
  --src /path/to/audio \
  --out artifacts/pattern_reports
```

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
fill more time. The arrangement decides only the bar at which each pattern
enters; the pattern's own length decides when it stops. Add a `song` block to
the report object in the spec:

```json
"song": {
  "starts": [0, 16, 6, 10, 12, 8],
  "targets": [-20, -20, -20, -20, -20, -20]
}
```

- `starts`: the bar at which each pattern enters, one value per pattern,
  0-based, counted in that pattern's own bars. Pattern `i` then plays its
  entire region from that bar and stops at its natural end.
- Omitting `starts` gives a staggered plan in which each pattern enters half
  the previous pattern's region length after it.
- Song length is fixed by the patterns, not chosen: it is the furthest entry
  time plus that pattern's whole region.
- `targets`: pre-master RMS target in dBFS per pattern, default -20.
- Patterns may differ in segment length and bar length; the builder places each
  stem at its own entry time and needs no common grid. Choose entry bars for
  each pattern in its own bar count.

```bash
python3 song_builder.py --spec report-spec.json \
  --report-dir artifacts/pattern_reports
```

The builder reads the report's `summary.js` and copied WAV, so it uses exactly
the patterns and segment grid the report shows.

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
  reconstruct it sample for sample and the builder's sum check is 0 LSB for any
  number of patterns;
- there is no master fade in or out, so no pattern's tail is faded early;
- the index gets a Song card;
- peak levels load as script tags, so `song.html` works from `file://` as well
  as over HTTP.

The page plays the master or all stems in sync, seeks, mutes individual stems,
and draws one arrangement row per stem. Drag horizontally on a row to zoom,
right-click to zoom out, Reset Zoom to return, and click to jump and play.

Song-level checks:

- [ ] Builder exits 0; printed stem delta is 0 LSB; clipped is 0.
- [ ] Each stem's audio is exactly its pattern's measured region
      (`segments × length`) once, beginning at its entry bar, with nothing
      after the region and no repeated copy of it.
- [ ] All stem rows draw in `song.html`, silent before their entry bar.
- [ ] A deep zoom loads a finer `a*.js` level and Reset Zoom restores the view.
- [ ] Click-to-seek starts playback; mute toggles one stem.

## 4. Memory-safe waveform design

Do **not** inline the whole PCM or a dense waveform in the HTML. The report
uses a coarse resident tier plus on-demand min/max chunks, mirroring the
flux-report strategy:

- `summary.js` contains `window.PATTERN_META` and a base64 int16 coarse
  min/max array at step 4096. This is the only waveform data loaded initially.
- Fine tiers are min/max pairs at steps 8, 64 and 512 samples, written in
  chunks of 65,536 bins. Each chunk is a small JS file that calls
  `window.__patternChunkArrive("level_chunk", ["base64..."])`.
- On render, the client computes the visible sample range and chooses the
  finest tier satisfying `visibleSamples / step <= width * 4`.
- It requests only the chunks covering the visible range. Chunks are cached
  in a Map with a 192 MB byte budget; oldest entries are evicted first.
- If the desired tier is not resident, rendering falls back to the coarser
  level or the resident coarse tier. This prevents a blank waveform while
  chunks load.
- The full view therefore loads zero `l*.js` chunks. Zooming progressively
  loads `l512`, then `l64`, then `l8` as the view narrows.

Keep this invariant when modifying the report. A full view must remain
usable with only `summary.js`.

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
- [ ] Coarse min/max matches the WAV at sampled bins (the builder asserts a
      few bins).
- [ ] `l8_0.js` exists and its first bins match the PCM.
- [ ] Every pattern's segments are exactly `length` long; no partial tail
      segment exists.
- [ ] When a song is built, the stem sum check passes and the song page loads
      its arrangement data.
- [ ] The segment counts in the report match `(end - start) / length` rounded
      down, per pattern, and each region is covered whole.
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
zip -r pattern-reports.zip pattern_reports \
  -x 'pattern_reports/pattern-reports.zip'
unzip -t pattern-reports.zip
```

Include the index, all HTML reports, the WAVs, the `data/` chunks, and when a
song was built, `song.html` plus `song/` (master, stems and `song/data/`). Do not
include the zip inside itself. If the index should offer a download, create the
zip first, copy it into the report folder, and add a link; the zip must still
not contain itself. Report the final size and entry count.

## 8. Worked examples

**Original run, three mono 44.1 kHz palette WAVs**, analyzed independently:

| File | Duration | Pattern | Start–End (s) | Loop (s) | Bars | Segments |
|---|---:|---|---:|---:|---:|---:|
| palette_2026-11-05 | 580.648 s | 1 | 4.10–89.52 | 4.271 | 2 | 20 |
| palette_2026-11-05 | 580.648 s | 2 | 101.20–171.62 | 2.134 | 1 | 33 |
| palette_2026-11-05 | 580.648 s | 3 | 217.30–365.94 | 4.129 | 2 | 36 |
| palette_2026-11-05 | 580.648 s | 4 | 372.90–578.88 | 8.239 | 4 | 25 |
| palette_2026-11-07 | 228.158 s | 1 | 0.50–48.62 | 4.010 | 2 | 12 |
| palette_2026-11-07 | 228.158 s | 2 | 55.00–223.46 | 8.022 | 4 | 21 |
| palette_2026-11-08 | 371.367 s | 1 | 5.00–358.59 | 8.223 | 4 | 43 |

**Sections run, one 609.10 s mono 44.1 kHz palette.** Four silence-delimited
sections, with the long final section split into three where its loop changed:

| Pattern | Start–End (s) | Loop (s) | Bars | Mean sim | Segments |
|---|---:|---:|---:|---:|---:|
| 1 | 1.00–60.10 | 3.652 | 2 | 0.65 | 16 |
| 2 | 69.10–182.10 | 3.688 | 2 | 0.72 | 30 |
| 3 | 184.00–289.70 | 7.300 | 4 | 0.81 | 14 |
| 4 | 304.80–380.00 | 7.300 | 4 | 0.67 | 10 |
| 5 | 380.00–470.00 | 7.288 | 4 | 0.55 | 12 |
| 6 | 470.00–574.80 | 5.451 | 3 | 0.60 | 19 |

Six patterns, three loop lengths, 101 segments. Each region is a whole section
or sub-region, not a hand-picked run of the most identical bars. The song used
all six regions once — 140.14 s, 77 bars, peak −0.5 dBFS, 0 clipped, stem sum
0 LSB, and every stem verified as its full region once at its entry bar.

Observed structure:

- The base bar is roughly 1.8–2.2 s; loops are 1, 2, 3, 4 or 8 bars.
- A section can hold one loop length for its whole length, and the next section
  a different one. Both are reported on their own terms.
- Pattern 4 of the first file has a strong ~4.1 s low-band half-period but the
  full high-level loop is ~8.24 s. Do not mistake the half-period for the
  pattern length.
- Pattern regions end at the last full loop; shorter tails are excluded, so
  every reported segment is full length.

The first run produced `artifacts/palette_pattern_reports/` and
`artifacts/palette-pattern-reports.zip` (about 99.4 MiB, 128 entries).

## 9. Pitfalls and lessons

- Do not name a helper `inspect.py`; it shadows the standard library and can
  break imports. Use `analyze_audio.py` or another distinct name.
- `scipy.io.wavfile` handles WAV; convert other formats first. 24-bit WAV
  scaling is easy to get wrong; convert to 16-bit PCM for predictable reports.
- The analysis helper's STFT memory grows with duration. For very long files,
  analyze sections separately or downsample the analysis rate; the report data
  itself is already chunked and memory-safe.
- ACF peaks are not segment boundaries. A strong peak can be a harmonic or a
  half-time layer. Cross-check with the other bands and the novelty/SSM.
- Do not gate pattern candidates on the fraction of frames above a strict
  similarity. Steady textures and fade tails score near 1.00 at every lag and
  dominate such a gate, while real quasi-periodic loops score 0.55–0.81 and
  drop out. Use the region mean plus the peak test instead.
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
- The report server must support Range; otherwise WAV seeking and playhead
  jumps can fail even though the page loads.
- A chunk file holds `CHUNK_BINS` bins, which is `2 * CHUNK_BINS` interleaved
  int16 values. Slice the interleaved min/max array by `2 * CHUNK_BINS`, not
  `CHUNK_BINS`; otherwise chunk indices point at the wrong audio, the second
  half of every chunk is dropped, and tail chunks are never written.
- If a preview host rejects plain curl with 403, that may be the preview's
  authentication layer, not a broken app. Check the app on loopback and open
  the preview through the provided browser panel.
- A song may not repeat a pattern to fill time or gate one out of part of the
  song: that stops it being heard whole. Give patterns later entry bars instead.
- Keep the zip out of itself. Re-run the zip after adding a download link if
  the link target must be inside the served folder.
