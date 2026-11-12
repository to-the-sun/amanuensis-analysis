#!/usr/bin/env python3
"""Build a song, stems and player page from a finished pattern report.

Each stem is the pattern's full measured region, placed once and never cut,
gated or tiled; patterns differ only in the bar at which they enter. The stems
are gain-trimmed and summed into the master.

Usage:
    python3 song_builder.py --spec report-spec.json --report-dir artifacts/pattern_reports
"""
import argparse
import base64
import json
import os
import re

import numpy as np
import scipy.io.wavfile as wavfile

STEPS = [4096, 512, 64]
SEAM_S = 0.004


def default_starts(region_s, bar_of):
    starts, acc = [0], 0.0
    for d, b in zip(region_s[:-1], bar_of[1:]):
        acc += d / 2
        starts.append(int(round(acc / b)))
    return starts


def load_meta(report_dir, basename):
    with open(os.path.join(report_dir, 'data', basename, 'summary.js')) as f:
        text = f.read()
    return json.loads(re.search(r'window\.PATTERN_META = (\{.*?\});', text).group(1))


def read_wav(path):
    sr, data = wavfile.read(path)
    if data.dtype != np.int16:
        raise ValueError('expected 16-bit PCM in %s' % path)
    if data.ndim == 2:
        data = data.mean(axis=1).astype(np.int16)
    return sr, data


def region(x, sr, pattern):
    s0 = int(round(pattern['start'] * sr))
    length = int(round(pattern['length'] * sr))
    return x[s0:s0 + len(pattern['segments']) * length].astype(np.float64).copy()


def fade_edges(reg, sr):
    seam = max(1, int(round(SEAM_S * sr)))
    reg = reg.copy()
    reg[:seam] *= np.linspace(0, 1, seam)
    reg[-seam:] *= np.linspace(1, 0, seam)
    return reg


def rms_db(v):
    return 20 * np.log10(np.sqrt(np.mean(v ** 2)) + 1e-12)


def peak_js(pcm16, step):
    n = len(pcm16)
    nb = (n + step - 1) // step
    buf = np.empty(nb * 2, dtype='<i2')
    full = n // step
    xb = pcm16[:full * step].reshape(full, step)
    buf[0:full * 2:2] = xb.min(axis=1)
    buf[1:full * 2:2] = xb.max(axis=1)
    if full < nb:
        buf[full * 2] = pcm16[full * step:].min()
        buf[full * 2 + 1] = pcm16[full * step:].max()
    return base64.b64encode(buf.tobytes()).decode('ascii')


def write_levels(stems_pcm, data_dir):
    totals = {len(s) for s in stems_pcm}
    if len(totals) != 1:
        raise ValueError('stem lengths differ')
    total = totals.pop()
    bins = {}
    for step in STEPS:
        bins[step] = (total + step - 1) // step
        parts = [peak_js(s, step) for s in stems_pcm]
        with open(os.path.join(data_dir, 'a%d.js' % step), 'w') as f:
            f.write('window.ARR_LEVEL=window.ARR_LEVEL||{};window.ARR_LEVEL["%d"]=[%s];\n'
                    % (step, ','.join('"%s"' % p for p in parts)))
    return total, bins


def page_html(title, summary, defs, arr_meta, duration):
    return PAGE_TEMPLATE \
        .replace('__TITLE__', title) \
        .replace('__HEAD__', summary) \
        .replace('__DEFS__', defs) \
        .replace('__ARR_META__', arr_meta) \
        .replace('__DUR__', '%d:%04.1f' % (int(duration // 60), duration % 60))


def build_song(report_dir, basename, spec_report):
    meta = load_meta(report_dir, basename)
    pats = meta['patterns']
    sr = meta['sampleRate']
    if len(pats) < 1:
        raise ValueError('report has no patterns')
    bar_of = [p['length'] / max(1, p['bars']) for p in pats]
    region_bars = [len(p['segments']) * p['bars'] for p in pats]
    region_s = [len(p['segments']) * p['length'] for p in pats]

    cfg = spec_report.get('song') or {}
    starts = cfg.get('starts') or default_starts(region_s, bar_of)
    starts = [int(s) for s in starts]
    if len(starts) != len(pats):
        raise ValueError('song.starts must have one value per pattern')
    if min(starts) < 0:
        raise ValueError('song.starts has a negative bar')
    start_s = [s * b for s, b in zip(starts, bar_of)]

    targets = cfg.get('targets') or [-20.0] * len(pats)
    if len(targets) != len(pats):
        raise ValueError('song.targets must have one value per pattern')

    sr2, x = read_wav(os.path.join(report_dir, basename + '.wav'))
    if sr2 != sr:
        raise ValueError('sample rate mismatch')
    x = x.astype(np.float64) / 32768.0
    regs = [fade_edges(region(x, sr, p), sr) for p in pats]
    offs = [int(round(t * sr)) for t in start_s]
    song = max(o + len(r) for o, r in zip(offs, regs))
    dur = song / sr
    stems, gains = [], []
    for i, reg in enumerate(regs):
        stem = np.zeros(song)
        stem[offs[i]:offs[i] + len(reg)] = reg
        stems.append(stem)
        gains.append(10 ** ((targets[i] - rms_db(reg)) / 20))

    mix = sum(s * g for s, g in zip(stems, gains))
    scale = 0.95 / (np.abs(mix).max() + 1e-12)

    out_dir = os.path.join(report_dir, 'song')
    os.makedirs(os.path.join(out_dir, 'stems'), exist_ok=True)
    os.makedirs(os.path.join(out_dir, 'data'), exist_ok=True)

    def w(path, v):
        wavfile.write(path, sr, np.clip(np.round(v * 32767), -32768, 32767).astype('<i2'))

    names = []
    for i, s in enumerate(stems):
        w(os.path.join(out_dir, 'stems', 'pattern_%d.wav' % (i + 1)), s * gains[i] * scale)
        names.append('pattern_%d' % (i + 1))

    stems_pcm = [wavfile.read(os.path.join(out_dir, 'stems', 'pattern_%d.wav' % (i + 1)))[1]
                 for i in range(len(pats))]
    summed = np.sum([s.astype(np.int32) for s in stems_pcm], axis=0)
    wavfile.write(os.path.join(out_dir, 'finished_song.wav'), sr,
                  np.clip(summed, -32768, 32767).astype('<i2'))
    total, bins = write_levels(stems_pcm, os.path.join(out_dir, 'data'))

    arr_meta = {
        'total': int(total),
        'steps': STEPS,
        'bins': {str(k): v for k, v in bins.items()},
        'bar': round(bar_of[0], 4),
        'bars': int(round(dur / bar_of[0])),
        'entries': [round(t, 6) for t in start_s],
    }
    with open(os.path.join(out_dir, 'data', 'arr.js'), 'w') as f:
        f.write('window.ARR_META = ' + json.dumps(arr_meta, separators=(',', ':')) + ';\n')

    defs = ',\n'.join(
        "  { file: 'song/stems/%s.wav', name: %s, meta: %s, color: %s }" % (
            names[i],
            json.dumps(p.get('name', 'Pattern %d' % (i + 1))),
            json.dumps('%d bars &middot; %.3f s' % (region_bars[i], p['length'] * len(p['segments']))),
            json.dumps(p.get('color', '#4dabf7')))
        for i, p in enumerate(pats))
    summary = ('%d patterns (%d:%04.1f), %d Hz mono. Each pattern plays its whole region once '
               'from its entry; the %d stems sum to the master.'
               % (len(pats), int(dur // 60), dur % 60, sr, len(pats)))
    html_text = page_html('Song &mdash; ' + basename, summary, defs,
                          json.dumps(arr_meta, separators=(',', ':')), dur)
    with open(os.path.join(report_dir, 'song.html'), 'w') as f:
        f.write(html_text)

    idx = os.path.join(report_dir, 'index.html')
    if os.path.exists(idx):
        with open(idx) as f:
            text = f.read()
        if 'href="song.html"' not in text:
            card = ('<a class="card" href="song.html"><strong>Song &mdash; %s</strong>'
                    '<span>Master plus %d stems &middot; %d:%04.1f &middot; arrangement view</span>'
                    '<span class="btn">Open song</span></a>\n'
                    % (basename, len(pats), int(dur // 60), dur % 60))
            text = text.replace('<div class="list">', '<div class="list">\n' + card, 1)
            with open(idx, 'w') as f:
                f.write(text)

    song_pcm = wavfile.read(os.path.join(out_dir, 'finished_song.wav'))[1].astype(np.int32)
    delta = int(np.abs(np.sum([s.astype(np.int32) for s in stems_pcm], axis=0) - song_pcm).max())
    if delta > 2:
        raise RuntimeError('stem sum differs from master by %d LSB' % delta)

    out = {
        'song': os.path.join(out_dir, 'finished_song.wav'),
        'stems': [os.path.join(out_dir, 'stems', n + '.wav') for n in names],
        'page': os.path.join(report_dir, 'song.html'),
        'bars': arr_meta['bars'], 'bar': round(bar_of[0], 4), 'duration': round(dur, 3),
        'peak_db': round(20 * np.log10(np.abs(song_pcm).max() / 32768.0), 2),
        'rms_db': round(rms_db(song_pcm / 32768.0), 1),
        'clipped': int(np.sum(np.abs(song_pcm) >= 32767)),
        'stem_delta_lsb': delta,
        'gains_db': [round(20 * np.log10(g), 1) for g in gains],
    }
    return out


def main():
    ap = argparse.ArgumentParser(description='Build a song, stems and player page from a pattern report.')
    ap.add_argument('--spec', required=True, help='report spec used to build the report')
    ap.add_argument('--report-dir', required=True, help='directory holding the built report')
    ap.add_argument('--report', default=None, help='report basename (default: the only report)')
    args = ap.parse_args()

    with open(args.spec) as f:
        spec = json.load(f)
    reports = spec.get('reports') or [spec]
    if args.report:
        reports = [r for r in reports
                   if os.path.splitext(os.path.basename(r['file']))[0] == args.report]
        if not reports:
            raise SystemExit('No report named %s in spec' % args.report)
    elif len(reports) != 1:
        raise SystemExit('Spec has several reports; pass --report')
    r = reports[0]
    basename = os.path.splitext(os.path.basename(r['file']))[0]
    res = build_song(args.report_dir, basename, r)
    print('song   %.2f s  %d bars  peak %.1f dBFS  rms %.1f dBFS  clipped %d'
          % (res['duration'], res['bars'], res['peak_db'], res['rms_db'], res['clipped']))
    print('stems  %d  sum delta %d LSB  gains %s'
          % (len(res['stems']), res['stem_delta_lsb'], res['gains_db']))
    print('page   %s' % res['page'])


PAGE_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>__TITLE__</title>
<style>
  :root { --bg:#121216; --card:#1a1a22; --border:#2a2a36; --text:#e1e1e6; --muted:#a0a0b0; --accent:#2196f3; }
  * { box-sizing:border-box; margin:0; padding:0; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif; }
  body { background:var(--bg); color:var(--text); padding:24px; line-height:1.5; max-width:1000px; }
  h1 { font-size:1.5rem; color:#fff; margin-bottom:6px; }
  h2 { font-size:1.05rem; color:#fff; margin:22px 0 10px; }
  p, .hint { color:var(--muted); font-size:0.88rem; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:10px; padding:16px; margin-top:14px; }
  .transport { display:flex; align-items:center; gap:14px; flex-wrap:wrap; }
  button { font:inherit; cursor:pointer; border-radius:6px; border:1px solid var(--border); background:#23232d; color:var(--text); padding:7px 14px; }
  button:hover:not(:disabled) { border-color:var(--accent); }
  button:disabled { opacity:0.45; cursor:default; }
  #play { background:var(--accent); border-color:var(--accent); color:#fff; font-weight:600; min-width:84px; }
  .time { font-family:monospace; font-size:0.9rem; color:var(--muted); }
  .modes { display:flex; gap:6px; margin-left:auto; }
  .mode.active { border-color:var(--accent); color:#fff; background:#1d2a3a; }
  #seek { width:100%; margin:14px 0 4px; accent-color:var(--accent); }
  .status { min-height:1.2em; font-size:0.8rem; color:var(--muted); margin-top:6px; }
  .dot { width:10px; height:10px; border-radius:50%; background:var(--c); flex:none; }
  .sname { font-weight:600; color:#fff; font-size:0.92rem; min-width:78px; }
  .smeta { color:var(--muted); font-size:0.78rem; font-family:monospace; flex:1; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .mute { min-width:74px; padding:4px 12px; font-size:0.82rem; }
  .mute[aria-pressed="true"] { border-color:#ff6b6b; color:#ff6b6b; }
  a.back { display:inline-block; margin-top:20px; color:var(--accent); text-decoration:none; font-size:0.88rem; }
  .arr-bar { display:flex; align-items:center; gap:12px; margin-bottom:10px; }
  .viewinfo { font-family:monospace; font-size:0.72rem; color:#4caf50; margin-left:auto; }
  .arow { border-top:1px solid var(--border); padding:8px 0 6px; }
  .arow:first-child { border-top:0; padding-top:0; }
  .ahead { display:flex; align-items:center; gap:10px; margin-bottom:6px; }
  .ahead .mute { margin-left:auto; }
  .awrap { position:relative; }
  .awave { width:100%; height:40px; display:block; background:#0d0d11; border:1px solid #22222e; border-radius:5px; cursor:crosshair; }
  .aaxis { width:100%; height:20px; display:block; margin-top:2px; }
  .arr-ph { position:absolute; top:0; bottom:0; width:2px; margin-left:-1px; background:#ff5252; box-shadow:0 0 4px rgba(255,82,82,0.8); pointer-events:none; display:none; z-index:2; }
</style>
</head>
<body>
__HEAD__
<div class="card">
  <div class="transport">
    <button id="play" disabled>Play</button>
    <span class="time"><span id="cur">0:00.0</span> / <span id="dur">__DUR__</span></span>
    <div class="modes">
      <button class="mode active" id="mMaster">Master</button>
      <button class="mode" id="mStems">Stems</button>
    </div>
  </div>
  <input id="seek" type="range" min="0" max="1000" value="0" step="1" aria-label="Seek">
  <div class="hint">Master plays the mixed master. Stems plays the stem files in sync; mute any stem to hear the rest.</div>
  <div class="status" id="status"></div>
</div>
<h2>Stems &amp; Arrangement</h2>
<div class="card">
  <div class="arr-bar">
    <button id="resetZoom">Reset Zoom</button>
    <span class="viewinfo" id="arrInfo"></span>
  </div>
  <div id="stems"></div>
  <canvas id="axis" class="aaxis"></canvas>
  <div class="hint">Drag horizontally on a stem to zoom. Right-click to zoom out. Click to jump and play.</div>
</div>
<a class="back" href="index.html">&larr; Back to reports</a>
<script src="song/data/arr.js"></script>
<script>
const DEFS = [
__DEFS__
];

const master = new Audio('song/finished_song.wav');
master.preload = 'auto';
const stems = DEFS.map(d => { const a = new Audio(d.file); a.preload = 'metadata'; return a; });
[master, ...stems].forEach(a => { a.hidden = true; document.body.appendChild(a); });
stems.forEach(s => s.addEventListener('playing', () => {
  if (playing && mode === 'stems' && Math.abs(s.currentTime - master.currentTime) > 0.03) {
    try { s.currentTime = master.currentTime; } catch (e) {}
  }
}));
const muted = DEFS.map(() => false);
const playBtn = document.getElementById('play');
const curEl = document.getElementById('cur');
const durEl = document.getElementById('dur');
const seek = document.getElementById('seek');
const statusEl = document.getElementById('status');
let mode = 'master';
let playing = false;
let ready = true;
let seeking = false;

const fmt = t => {
  if (!isFinite(t) || t < 0) t = 0;
  const m = Math.floor(t / 60);
  const s = t - m * 60;
  return m + ':' + (s < 10 ? '0' : '') + s.toFixed(1);
};

const aRows = [];
DEFS.forEach((d, i) => {
  const row = document.createElement('div');
  row.className = 'arow';
  row.innerHTML = `<div class="ahead"><span class="dot" style="--c:${d.color}"></span><span class="sname">${d.name}</span><span class="smeta">${d.meta}</span><button class="mute" aria-pressed="false">Mute</button></div>`
    + `<div class="awrap"><canvas class="awave"></canvas><div class="arr-ph"></div></div>`;
  const btn = row.querySelector('button');
  btn.addEventListener('click', () => {
    muted[i] = !muted[i];
    btn.setAttribute('aria-pressed', String(muted[i]));
    btn.textContent = muted[i] ? 'Muted' : 'Mute';
    if (mode === 'stems') stems[i].muted = muted[i];
  });
  document.getElementById('stems').appendChild(row);
  aRows.push({ canvas: row.querySelector('canvas'), ph: row.querySelector('.arr-ph'), color: d.color });
});

function setTime(t) {
  master.currentTime = t;
  stems.forEach(s => { try { s.currentTime = t; s.playbackRate = 1; } catch (e) {} });
  curEl.textContent = fmt(t);
}

function applyStemMutes() {
  stems.forEach((s, i) => { s.muted = muted[i]; });
}

function pauseAll() {
  [master, ...stems].forEach(a => a.pause());
  playing = false;
  playBtn.textContent = 'Play';
}

async function playAll() {
  if (!ready) return;
  const t = master.currentTime;
  master.muted = mode === 'stems';
  applyStemMutes();
  stems.forEach(s => { try { s.currentTime = t; s.playbackRate = 1; } catch (e) {} });
  const els = mode === 'master' ? [master] : [master, ...stems];
  playing = true;
  playBtn.textContent = 'Pause';
  try {
    await Promise.all(els.map(a => a.play()));
  } catch (e) {
    pauseAll();
    statusEl.textContent = 'Playback was blocked. Press Play again.';
  }
}

playBtn.addEventListener('click', () => (playing ? pauseAll() : playAll()));

function loadStems(resume) {
  let done = 0;
  ready = false;
  playBtn.disabled = true;
  statusEl.textContent = 'Loading stems 0/' + stems.length;
  const doneOne = () => {
    done += 1;
    statusEl.textContent = 'Loading stems ' + done + '/' + stems.length;
    if (done === stems.length) {
      ready = true;
      playBtn.disabled = false;
      statusEl.textContent = 'Stems ready.';
      if (resume) playAll();
    }
  };
  stems.forEach(s => {
    if (s.readyState >= 4) return doneOne();
    s.preload = 'auto';
    s.addEventListener('canplaythrough', doneOne, { once: true });
    s.addEventListener('error', doneOne, { once: true });
    s.load();
  });
}

function setMode(next) {
  if (next === mode) return;
  const wasPlaying = playing;
  pauseAll();
  mode = next;
  document.getElementById('mMaster').classList.toggle('active', mode === 'master');
  document.getElementById('mStems').classList.toggle('active', mode === 'stems');
  if (mode === 'master') {
    ready = true;
    playBtn.disabled = false;
    statusEl.textContent = '';
    if (wasPlaying) playAll();
  } else {
    loadStems(wasPlaying);
  }
}

document.getElementById('mMaster').addEventListener('click', () => setMode('master'));
document.getElementById('mStems').addEventListener('click', () => setMode('stems'));

seek.addEventListener('input', () => {
  seeking = true;
  setTime((master.duration || AM.total / AM.bar) * seek.value / 1000);
});
seek.addEventListener('change', () => { seeking = false; });

master.addEventListener('loadedmetadata', () => {
  durEl.textContent = fmt(master.duration);
  playBtn.disabled = false;
});
master.addEventListener('ended', () => {
  pauseAll();
  setTime(0);
});

const AM = window.ARR_META;
const SR = 44100;
const aData = {}, aPending = {};
const aStack = [];
let aStart = 0, aEnd = AM.total / SR, aDrag = null, aHover = null;
const axis = document.getElementById('axis');
const aInfo = document.getElementById('arrInfo');

const aFmt = t => {
  if (!isFinite(t) || t < 0) t = 0;
  const m = Math.floor(t / 60);
  const s = t - m * 60;
  return m + ':' + (s < 10 ? '0' : '') + s.toFixed(2);
};
const aRgba = (hex, a) => {
  const n = parseInt(hex.slice(1), 16);
  return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
};
const aX = (t, w) => (t - aStart) / (aEnd - aStart) * w;
const aT = (x, w) => aStart + x / w * (aEnd - aStart);

function b64i16(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Int16Array(bytes.buffer, 0, bin.length >> 1);
}

function aLoad(step) {
  if (aData[step] || aPending[step]) return;
  aPending[step] = 1;
  const s = document.createElement('script');
  s.src = 'song/data/a' + step + '.js';
  s.onload = () => {
    aPending[step] = 0;
    const raw = window.ARR_LEVEL && window.ARR_LEVEL[step];
    if (!raw || !raw.length) return;
    aData[step] = raw.map(b64i16);
    aDraw();
  };
  s.onerror = () => { aPending[step] = 0; };
  document.head.appendChild(s);
}

function aInfoText(t) {
  aInfo.textContent = 'Visible: ' + aFmt(aStart) + ' \u2013 ' + aFmt(aEnd)
    + ' (' + aFmt(aEnd - aStart) + ')' + (t == null ? '' : ' \u00b7 cursor ' + aFmt(t));
}

function aDraw() {
  const W = aRows[0].canvas.clientWidth, H = aRows[0].canvas.clientHeight;
  if (!W || !H) return;
  const spp = (aEnd - aStart) * SR / W;
  const all = AM.steps.filter(s => s <= spp);
  aLoad(all.length ? Math.max.apply(null, all) : Math.min.apply(null, AM.steps));
  const ok = AM.steps.filter(s => aData[s]);
  const fit = ok.filter(s => s <= spp);
  const step = ok.length ? (fit.length ? Math.max.apply(null, fit) : Math.min.apply(null, ok)) : null;
  const barPx = AM.bar / (aEnd - aStart) * W;
  const dpr = window.devicePixelRatio || 1;
  aRows.forEach((row, i) => {
    const c = row.canvas, ctx = c.getContext('2d');
    if (c.width !== Math.round(W * dpr) || c.height !== Math.round(H * dpr)) {
      c.width = Math.round(W * dpr);
      c.height = Math.round(H * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = '#0d0d11';
    ctx.fillRect(0, 0, W, H);
    for (const t of AM.entries) {
      const x = aX(t, W);
      ctx.strokeStyle = 'rgba(255,255,255,0.09)';
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
    }
    if (barPx >= 9) {
      ctx.strokeStyle = 'rgba(255,255,255,0.035)';
      for (let b = 0; b <= AM.bars; b++) {
        const x = aX(b * AM.bar, W);
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
      }
    }
    const mid = H / 2, half = H * 0.38;
    ctx.strokeStyle = 'rgba(255,255,255,0.10)';
    ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(W, mid); ctx.stroke();
    if (step) {
      const nb = AM.bins[step], d = aData[step][i];
      ctx.fillStyle = aRgba(row.color, 0.85);
      for (let px = 0; px < W; px++) {
        const a = Math.max(0, Math.min(nb - 1, Math.floor((aStart + px / W * (aEnd - aStart)) * SR / step)));
        const b = Math.max(0, Math.min(nb - 1, Math.floor((aStart + (px + 1) / W * (aEnd - aStart)) * SR / step)));
        let mn = 32767, mx = -32768;
        for (let k = a; k <= b; k++) {
          const lo = d[k * 2], hi = d[k * 2 + 1];
          if (lo < mn) mn = lo;
          if (hi > mx) mx = hi;
        }
        if (mx < mn) continue;
        const y1 = mid - mx / 32768 * half, y2 = mid - mn / 32768 * half;
        ctx.fillRect(px, y1, 1, Math.max(1, y2 - y1));
      }
    }
    if (aDrag) {
      const a = Math.max(0, Math.min(aDrag.x0, aDrag.x1)), b = Math.min(W, Math.max(aDrag.x0, aDrag.x1));
      ctx.fillStyle = 'rgba(255,235,59,0.16)';
      ctx.fillRect(a, 0, b - a, H);
      ctx.strokeStyle = '#ffeb3b';
      ctx.strokeRect(a + 0.5, 0.5, Math.max(0, b - a - 1), H - 1);
    }
    if (aHover && aHover.i === i) {
      ctx.strokeStyle = 'rgba(255,255,255,0.5)';
      ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(aHover.x, 0); ctx.lineTo(aHover.x, H); ctx.stroke();
      ctx.setLineDash([]);
    }
  });
  aAxisDraw();
  aHead();
}

function aAxisDraw() {
  const W = axis.clientWidth, H = axis.clientHeight;
  if (!W || !H) return;
  const dpr = window.devicePixelRatio || 1;
  if (axis.width !== Math.round(W * dpr) || axis.height !== Math.round(H * dpr)) {
    axis.width = Math.round(W * dpr);
    axis.height = Math.round(H * dpr);
  }
  const ctx = axis.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  const span = aEnd - aStart;
  const iv = span > 120 ? 30 : span > 60 ? 10 : span > 20 ? 5 : span > 8 ? 2 : span > 3 ? 1 : 0.5;
  ctx.strokeStyle = 'rgba(255,255,255,0.28)';
  ctx.beginPath(); ctx.moveTo(0, 0.5); ctx.lineTo(W, 0.5); ctx.stroke();
  ctx.font = '10px monospace';
  ctx.fillStyle = '#a0a0b0';
  ctx.textAlign = 'left';
  let lastRight = -1e9;
  for (let t = Math.ceil(aStart / iv) * iv; t <= aEnd + 1e-9; t += iv) {
    const x = aX(t, W);
    ctx.strokeStyle = 'rgba(255,255,255,0.28)';
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, 4); ctx.stroke();
    const lab = iv >= 2 ? aFmt(t).slice(0, -3) : aFmt(t);
    const tw = ctx.measureText(lab).width;
    const lx = Math.max(0, Math.min(W - tw, x - tw / 2));
    if (lx < lastRight + 6) continue;
    ctx.fillText(lab, lx, 14);
    lastRight = lx + tw;
  }
}

function aHead() {
  const W = aRows[0].canvas.clientWidth, t = master.currentTime;
  const show = W && isFinite(t) && t >= aStart && t <= aEnd;
  aRows.forEach(r => {
    r.ph.style.display = show ? 'block' : 'none';
    if (show) r.ph.style.left = aX(t, W) + 'px';
  });
}

function aSeek(t) {
  setTime(Math.max(0, Math.min(AM.total / SR, t)));
  playAll();
}

const aPoint = (e, c) => {
  const r = c.getBoundingClientRect();
  return Math.max(0, Math.min(r.width, e.clientX - r.left));
};

aRows.forEach((row, i) => {
  const c = row.canvas;
  c.addEventListener('mousedown', e => {
    if (e.button !== 0) return;
    e.preventDefault();
    const x = aPoint(e, c);
    aDrag = { x0: x, x1: x };
    aHover = { i: i, x: x };
    aDraw();
  });
  c.addEventListener('mousemove', e => {
    if (!aDrag) aHover = { i: i, x: aPoint(e, c) };
    else aDrag.x1 = aPoint(e, c);
    aInfoText(aT(aPoint(e, c), c.clientWidth));
    aDraw();
  });
  c.addEventListener('mouseleave', () => {
    if (aDrag) return;
    aHover = null;
    aInfoText();
    aDraw();
  });
  c.addEventListener('mouseup', () => {
    if (!aDrag) return;
    const d = aDrag;
    aDrag = null;
    const W = c.clientWidth;
    if (Math.abs(d.x1 - d.x0) > 4) {
      let t1 = aT(Math.min(d.x0, d.x1), W), t2 = aT(Math.max(d.x0, d.x1), W);
      if (t2 - t1 < 1.5) {
        const mid = (t1 + t2) / 2;
        t1 = Math.min(Math.max(0, mid - 0.75), aEnd - 1.5);
        t2 = t1 + 1.5;
      }
      aStack.push([aStart, aEnd]);
      aStart = Math.max(0, t1);
      aEnd = Math.min(AM.total / SR, t2);
      aHover = null;
      aInfoText();
    } else {
      aSeek(aT(d.x0, W));
    }
    aDraw();
  });
  c.addEventListener('contextmenu', e => {
    e.preventDefault();
    const prev = aStack.pop();
    if (!prev) return;
    aStart = prev[0];
    aEnd = prev[1];
    aHover = null;
    aInfoText();
    aDraw();
  });
});

document.getElementById('resetZoom').addEventListener('click', () => {
  aStack.length = 0;
  aStart = 0;
  aEnd = AM.total / SR;
  aHover = null;
  aInfoText();
  aDraw();
});
window.addEventListener('resize', aDraw);
aInfoText();
aLoad(4096);
aDraw();

function frame() {
  if (!seeking && master.duration) {
    seek.value = Math.round(master.currentTime / master.duration * 1000);
    curEl.textContent = fmt(master.currentTime);
  }
  if (playing && mode === 'stems') {
    for (const s of stems) {
      if (s.readyState < 3) continue;
      const d = s.currentTime - master.currentTime;
      const r = d < -0.02 ? 1.03 : d > 0.02 ? 0.97 : 1;
      if (s.playbackRate !== r) s.playbackRate = r;
    }
  }
  aHead();
  requestAnimationFrame(frame);
}
frame();
</script>
</body>
</html>
"""


if __name__ == '__main__':
    main()
