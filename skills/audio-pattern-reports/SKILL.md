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

- **Bar**: not an essential unit of this strategy, but equivalent to a measure in most standard depictions of music, generally smaller or the same size as the following measurements, though not necessarily.
- **Segment**: a single iteration of a repeating pattern. This is not necessarily
  the same as a bar. 
- **Pattern**: one contiguous repeated section of segments. A shorter tail is
  excluded rather than reported, so every segment has a uniform length.

All segments in a pattern have the same nominal length because the report
imposes a uniform grid. The report does not independently detect every segment
boundary.

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

## 1. Find sections and loop candidates

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

The helper prints candidates; it does not choose patterns for you. Interpret
them with the following checks:

1. **Section boundaries.** Prefer actual silence gaps. If the file is
   continuous, use novelty peaks and clear changes in timbre/level. Leave a
   little margin around fades; do not start a pattern in the middle of a tail.
2. **Fundamental vs harmonics.** A strong peak at 2L, 3L or 4L is often a
   harmonic of the loop, not the loop itself. Compare the onset ACF, mel lag
   similarity and band ACF. Prefer the shortest prominent peak that explains
   the whole section and is consistent with the musical structure.
3. **Nested periods.** A low band may repeat at half the full loop while a
   high layer repeats only at the full loop. Choose the full loop when the
   section's high-level content and SSM block require it. In the worked
   example, Pattern 4's low band has a ~4.1 s half-period but the full pattern
   is ~8.24 s.
4. **Cycle count.** Check that `(end - start) / loop_length` is close to an
   integer. A remainder is normal at the end and is excluded: the pattern ends
   at the last full loop. If the remainder is almost a whole loop, revisit the
   start/end.
5. **Bar multiple.** Estimate the base bar and express the loop as an integer
   number of bars. Typical bar lengths here are about 2.0–2.14 s, but measure
   rather than assuming. Across patterns the base can drift slightly.
6. **Confidence.** Record whether the loop is clear, plausible, or ambiguous.
   Do not claim sample-accurate boundaries from an ACF peak. A useful tolerance
   is a few tens of milliseconds for the loop estimate.

Write down a table before building anything:

| File | Pattern | Start (s) | End (s) | Loop (s) | Bars | Segments (all full) |
|---|---|---:|---:|---:|---:|---:|
| example.wav | 1 | 4.10 | 92.60 | 4.271 | 2 | 20 |

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

A song is layered from the same patterns the report shows. Each pattern
contributes one stem built from its full measured region and tiled whole; it is
never chopped into bars. Add a `song` block to the report object in the spec:

```json
"song": {
  "bars": 96,
  "targets": [-21, -18, -18, -20, -20],
  "sections": [
    [0, 8, [0, 2]],
    [8, 16, [0, 1, 2]],
    [16, 48, [0, 1, 2, 3, 4]],
    [48, 56, [0, 1, 2]],
    [56, 80, [0, 1, 2, 3, 4]],
    [80, 88, [2, 4]],
    [88, 96, [0, 1, 2, 3, 4]]
  ]
}
```

- `sections`: `[startBar, endBar, [patternIndexes]]`, 0-based, in bars. A stem
  is audible only inside its sections. All patterns must share a bar length
  within 20 ms or the builder refuses.
- `bars`: song length. With no `sections`, a 96-bar plan is generated (patterns
  enter progressively, one mid-song break and two breakdowns). Any other length
  requires explicit sections.
- `targets`: pre-master RMS target in dBFS per pattern, default -20.
- `fadeOutBars`: ending fade length, default 2.

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

- each stem is the whole measured region tiled end to end, with a 4 ms fade at
  every repeat; the last partial repeat is cut only under the ending fade;
- gates ramp over 30 ms at section boundaries;
- stems are trimmed to their targets, then share one master scale and the same
  fades; their sum equals the master to within 2 LSB, which the builder asserts;
- the index gets a Song card;
- peak levels load as script tags, so `song.html` works from `file://` as well
  as over HTTP.

The page plays the master or all stems in sync, seeks, mutes individual stems,
and draws one arrangement row per stem. Drag horizontally on a row to zoom,
right-click to zoom out, Reset Zoom to return, and click to jump and play.

Song-level checks:

- [ ] Builder exits 0; printed stem delta is at most 2 LSB; clipped is 0.
- [ ] All stem rows draw in `song.html`, with gaps where the arrangement mutes
      them.
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

## 8. Worked example from the original run

Three mono 44.1 kHz palette WAVs, analyzed independently:

| File | Duration | Pattern | Start–End (s) | Loop (s) | Bars | Segments |
|---|---:|---|---:|---:|---:|---:|
| palette_2026-11-05 | 580.648 s | 1 | 4.10–89.52 | 4.271 | 2 | 20 |
| palette_2026-11-05 | 580.648 s | 2 | 101.20–171.62 | 2.134 | 1 | 33 |
| palette_2026-11-05 | 580.648 s | 3 | 217.30–365.94 | 4.129 | 2 | 36 |
| palette_2026-11-05 | 580.648 s | 4 | 372.90–578.88 | 8.239 | 4 | 25 |
| palette_2026-11-07 | 228.158 s | 1 | 0.50–48.62 | 4.010 | 2 | 12 |
| palette_2026-11-07 | 228.158 s | 2 | 55.00–223.46 | 8.022 | 4 | 21 |
| palette_2026-11-08 | 371.367 s | 1 | 5.00–358.59 | 8.223 | 4 | 43 |

Observed structure:

- The base bar is roughly 2.0–2.14 s; loops are 1, 2 or 4 bars.
- Pattern 4 of the first file has a strong ~4.1 s low-band half-period but the
  full high-level loop is ~8.24 s. Do not mistake the half-period for the
  pattern length.
- Pattern regions end at the last full loop; shorter tails are excluded, so
  every reported segment is full length.

This run produced `artifacts/palette_pattern_reports/` and
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
- The reports use a uniform grid. Do not describe the grid as measured
  per-segment timing. 
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
- Keep the zip out of itself. Re-run the zip after adding a download link if
  the link target must be inside the served folder.
