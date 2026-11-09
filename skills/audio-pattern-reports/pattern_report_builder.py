import os, re, json, base64, shutil, argparse, html
import numpy as np
import scipy.io.wavfile as wavfile

CHUNK_BINS = 65536
COARSE_STEP = 4096
LEVELS = [(8, True), (64, True), (512, True)]
COLORS = ['#4dabf7', '#ffa94d', '#51cf66', '#cc5de8']


def fmt_precise(t):
    t = max(0.0, float(t))
    m = int(t // 60)
    s = t - m * 60
    return f"{m}:{s:05.2f}"


def build_segments(start, end, length):
    segs = []
    t = start
    while t < end - 0.01:
        e = min(end, t + length)
        segs.append({'start': round(t, 3), 'end': round(e, 3), 'partial': (e - t) < length - 0.001})
        t = e
    return segs


def minmax_bins(pcm, step):
    n = len(pcm)
    nb = (n + step - 1) // step
    pad = nb * step - n
    xp = np.pad(pcm, (0, pad), constant_values=0) if pad else pcm
    xb = xp.reshape(nb, step)
    out = np.empty(nb * 2, dtype='<i2')
    out[0::2] = xb.min(axis=1)
    out[1::2] = xb.max(axis=1)
    return out, nb


def b64_i16(arr):
    return base64.b64encode(arr.astype('<i2', copy=False).tobytes()).decode('ascii')


def write_level(pcm, step, data_dir, level_idx):
    inter, nb = minmax_bins(pcm, step)
    nchunks = max(1, (nb + CHUNK_BINS - 1) // CHUNK_BINS)
    for c in range(nchunks):
        chunk = inter[c * CHUNK_BINS * 2:(c + 1) * CHUNK_BINS * 2]
        with open(os.path.join(data_dir, f'l{step}_{c}.js'), 'w') as f:
            f.write('window.__patternChunkArrive("%d_%d",["%s"]);\n' % (level_idx, c, b64_i16(chunk)))
    return nb, nchunks


def pattern_cards(patterns):
    out = []
    for i, p in enumerate(patterns):
        color = p.get('color', COLORS[i % len(COLORS)])
        segs = p['segments']
        meta = (f"{fmt_precise(p['start'])} &ndash; {fmt_precise(p['end'])} &middot; "
                f"{p['length']:.3f} s &middot; {p['bars']} bar{'s' if p['bars'] != 1 else ''} &middot; "
                f"{len(segs)} segments")
        chips = []
        for j, s in enumerate(segs):
            dur = s['end'] - s['start']
            title = (f"Segment {j+1} &middot; {fmt_precise(s['start'])} &ndash; {fmt_precise(s['end'])} "
                     f"&middot; {dur:.3f} s" + (" &middot; partial" if s['partial'] else ""))
            label = str(j + 1) + ('*' if s['partial'] else '')
            cls = 'seg partial' if s['partial'] else 'seg'
            chips.append(f'<button class="{cls}" onclick="playSegment({i},{j})" title="{title}">{label}</button>')
        out.append(
            f'<div class="pcard" style="--c:{color}">'
            f'<div class="phead"><span class="pdot"></span><span class="pname">{html.escape(p["name"])}</span>'
            f'<span class="pmeta">{meta}</span>'
            f'<span class="pactions"><button class="mini" onclick="zoomPattern({i})">Zoom</button>'
            f'<button class="mini" onclick="playPattern({i})">Play</button></span></div>'
            f'<div class="segrow">{"".join(chips)}</div></div>'
        )
    return '\n'.join(out)


JS = r"""
const META = window.PATTERN_META;
const PATTERNS = META.patterns;
const LEVELS = META.levels;
const COARSE = { step: META.coarseStep, bins: META.coarseBins, arr: null };
const CACHE_BUDGET = 192 * 1024 * 1024;
const CHUNK_BINS = META.chunkBins;
const cache = new Map();
const pending = new Set();
let cacheBytes = 0;
let viewStart = 0;
let viewEnd = META.duration;
let zoomHistory = [];
let drag = null;
let hover = null;
let renderQueued = false;
let playheadRaf = 0;
const rangeOut = { mn: 0, mx: 0 };
const canvas = document.getElementById('wave');
const ctx = canvas.getContext('2d');
const tooltip = document.getElementById('tooltip');
const playheadEl = document.getElementById('playhead');
const audio = document.getElementById('audio');
const viewInfo = document.getElementById('viewInfo');
const LAYOUT = { axisH: 22, rowH: 30, rowGap: 6, waveTop: 10, waveGap: 12 };

function base64ToI16(b64) {
  if (!b64) return null;
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new Int16Array(bytes.buffer, 0, bin.length >> 1);
}

function hexA(hex, a) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
}

function formatMSSPrecise(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const m = Math.floor(sec / 60);
  const s = sec - m * 60;
  return `${m}:${s < 10 ? '0' + s.toFixed(2) : s.toFixed(2)}`;
}

function formatMSS(sec) {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec - m * 60);
  return `${m}:${s < 10 ? '0' + s : '' + s}`;
}

function evictCache() {
  for (const [key, arr] of cache) {
    if (cacheBytes <= CACHE_BUDGET) break;
    cacheBytes -= arr.byteLength;
    cache.delete(key);
  }
}

window.__patternChunkArrive = function (key, streams) {
  pending.delete(key);
  if (!cache.has(key)) {
    const arr = base64ToI16(streams[0]);
    if (arr) {
      cacheBytes += arr.byteLength;
      cache.set(key, arr);
      evictCache();
    }
  }
  scheduleRender();
};

function requestChunk(li, c) {
  const key = li + '_' + c;
  if (cache.has(key) || pending.has(key)) return;
  pending.add(key);
  const el = document.createElement('script');
  el.src = META.dataDir + '/l' + LEVELS[li].step + '_' + c + '.js';
  el.onload = () => el.remove();
  el.onerror = () => { pending.delete(key); };
  document.head.appendChild(el);
}

function chunksForRange(li, s1, s2) {
  const L = LEVELS[li];
  const last = Math.max(1, Math.ceil(L.bins / CHUNK_BINS)) - 1;
  let c1 = Math.floor(Math.floor(s1 / L.step) / CHUNK_BINS);
  let c2 = Math.floor(Math.floor(s2 / L.step) / CHUNK_BINS);
  c1 = Math.max(0, Math.min(last, c1));
  c2 = Math.max(0, Math.min(last, c2));
  const out = [];
  for (let c = c1; c <= c2; c++) out.push(c);
  return out;
}

function resident(li, s1, s2) {
  for (const c of chunksForRange(li, s1, s2)) {
    if (!cache.has(li + '_' + c)) return false;
  }
  return true;
}

function pickLevel(visibleSamples, width) {
  const maxBins = Math.max(1, width * 4);
  for (let i = 0; i < LEVELS.length; i++) {
    if (visibleSamples / LEVELS[i].step <= maxBins) return i;
  }
  return -1;
}

function usableLevel(s1, s2, width) {
  const want = pickLevel(s2 - s1 + 1, width);
  if (want < 0) return -1;
  for (let i = want; i < LEVELS.length; i++) {
    if (resident(i, s1, s2)) return i;
  }
  return -1;
}

function requestViewLevel(s1, s2, width) {
  const want = pickLevel(s2 - s1 + 1, width);
  if (want < 0) return;
  for (const c of chunksForRange(want, s1, s2)) requestChunk(want, c);
}

function streamRange(li, a, b) {
  let ok = false;
  if (li < 0) {
    const arr = COARSE.arr;
    if (!arr) return false;
    let b1 = Math.floor(a / COARSE.step);
    let b2 = Math.floor(b / COARSE.step);
    b1 = Math.max(0, b1);
    b2 = Math.min(COARSE.bins - 1, b2);
    if (b2 < b1) return false;
    for (let i = b1; i <= b2; i++) {
      const lo = arr[i * 2] / 32767;
      const hi = arr[i * 2 + 1] / 32767;
      if (!ok) { rangeOut.mn = lo; rangeOut.mx = hi; ok = true; }
      else { if (lo < rangeOut.mn) rangeOut.mn = lo; if (hi > rangeOut.mx) rangeOut.mx = hi; }
    }
    return ok;
  }
  const L = LEVELS[li];
  let b1 = Math.floor(a / L.step);
  let b2 = Math.floor(b / L.step);
  b1 = Math.max(0, b1);
  b2 = Math.min(L.bins - 1, b2);
  if (b2 < b1) return false;
  for (let i = b1; i <= b2; i++) {
    const c = Math.floor(i / CHUNK_BINS);
    const arr = cache.get(li + '_' + c);
    if (!arr) return false;
    const loc = (i - c * CHUNK_BINS) * 2;
    if (loc + 1 >= arr.length) continue;
    const lo = arr[loc] / 32767;
    const hi = arr[loc + 1] / 32767;
    if (!ok) { rangeOut.mn = lo; rangeOut.mx = hi; ok = true; }
    else { if (lo < rangeOut.mn) rangeOut.mn = lo; if (hi > rangeOut.mx) rangeOut.mx = hi; }
  }
  return ok;
}

function timeToX(t, w) {
  return ((t - viewStart) / (viewEnd - viewStart)) * w;
}

function xToTime(x, w) {
  return viewStart + (x / w) * (viewEnd - viewStart);
}

function layout(H) {
  const n = PATTERNS.length;
  const rowsH = n ? n * LAYOUT.rowH + (n - 1) * LAYOUT.rowGap : 0;
  const waveBottom = H - LAYOUT.axisH - rowsH - LAYOUT.waveGap;
  const waveH = Math.max(1, waveBottom - LAYOUT.waveTop);
  return {
    waveTop: LAYOUT.waveTop,
    waveBottom: waveBottom,
    waveH: waveH,
    midY: LAYOUT.waveTop + waveH / 2,
    halfH: waveH / 2,
    rowTop: waveBottom + LAYOUT.waveGap
  };
}

function sampleRange() {
  const s1 = Math.max(0, Math.floor((viewStart / META.duration) * META.totalSamples));
  const s2 = Math.min(META.totalSamples - 1, Math.ceil((viewEnd / META.duration) * META.totalSamples));
  return [s1, s2];
}

function updateViewInfo() {
  viewInfo.innerHTML = `Visible: ${formatMSSPrecise(viewStart)} &ndash; ${formatMSSPrecise(viewEnd)} (Duration: ${formatMSSPrecise(viewEnd - viewStart)})`;
}

function updatePlayhead() {
  const w = canvas.clientWidth;
  const t = isFinite(audio.currentTime) ? audio.currentTime : 0;
  if (!w || (t <= 0 && audio.paused) || t < viewStart || t > viewEnd) {
    playheadEl.style.display = 'none';
    return;
  }
  playheadEl.style.display = 'block';
  playheadEl.style.left = timeToX(t, w) + 'px';
}

function startPlayheadLoop() {
  if (playheadRaf) return;
  const loop = () => {
    updatePlayhead();
    playheadRaf = audio.paused ? 0 : requestAnimationFrame(loop);
  };
  playheadRaf = requestAnimationFrame(loop);
}

function draw() {
  const rect = canvas.getBoundingClientRect();
  const W = rect.width;
  const H = rect.height;
  if (!W || !H) return;
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(W * dpr) || canvas.height !== Math.round(H * dpr)) {
    canvas.width = Math.round(W * dpr);
    canvas.height = Math.round(H * dpr);
  }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = '#0d0d11';
  ctx.fillRect(0, 0, W, H);

  const LY = layout(H);

  ctx.strokeStyle = '#1e1e2c';
  ctx.lineWidth = 1;
  [-1, -0.5, 0, 0.5, 1].forEach(v => {
    const y = LY.midY - v * LY.halfH;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(W, y);
    ctx.stroke();
  });

  for (const p of PATTERNS) {
    const x1 = timeToX(p.start, W);
    const x2 = timeToX(p.end, W);
    if (x2 < 0 || x1 > W) continue;
    ctx.fillStyle = hexA(p.color, 0.10);
    ctx.fillRect(Math.max(0, x1), LY.waveTop, Math.min(W, x2) - Math.max(0, x1), LY.waveH);
  }

  const [s1, s2] = sampleRange();
  const visible = Math.max(1, s2 - s1 + 1);
  const level = usableLevel(s1, s2, W);
  ctx.fillStyle = 'rgba(180,190,210,0.55)';
  for (let px = 0; px < W; px++) {
    const a = s1 + Math.floor((px / W) * visible);
    const b = Math.min(s2, s1 + Math.ceil(((px + 1) / W) * visible) - 1);
    if (!streamRange(level, a, b)) continue;
    const y1 = LY.midY - rangeOut.mx * LY.halfH;
    const y2 = LY.midY - rangeOut.mn * LY.halfH;
    ctx.fillRect(px, Math.min(y1, y2), 1, Math.max(1, Math.abs(y2 - y1)));
  }

  for (const p of PATTERNS) {
    const x1 = timeToX(p.start, W);
    const x2 = timeToX(p.end, W);
    if (x2 < 0 || x1 > W) continue;
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, LY.waveTop, W, LY.waveH);
    ctx.clip();
    ctx.strokeStyle = 'rgba(255,255,255,0.22)';
    ctx.lineWidth = 1;
    let lastSegX = x1;
    for (let k = 1; k < p.segments.length; k++) {
      const xs = timeToX(p.segments[k].start, W);
      if (xs > x1 + 0.5 && xs < x2 - 0.5 && xs - lastSegX >= 2.5) {
        ctx.beginPath();
        ctx.moveTo(xs, LY.waveTop);
        ctx.lineTo(xs, LY.waveBottom);
        ctx.stroke();
        lastSegX = xs;
      }
    }
    ctx.strokeStyle = hexA(p.color, 0.9);
    ctx.lineWidth = 1.5;
    for (const x of [x1, x2]) {
      ctx.beginPath();
      ctx.moveTo(x, LY.waveTop);
      ctx.lineTo(x, LY.waveBottom);
      ctx.stroke();
    }
    ctx.restore();

    const label = p.name + ' \u00b7 ' + p.length.toFixed(2) + 's';
    ctx.font = '600 10px monospace';
    const tw = ctx.measureText(label).width;
    const lx = Math.max(3, x1 + 4);
    if (x2 - lx > tw + 6) {
      ctx.fillStyle = 'rgba(13,13,17,0.78)';
      ctx.fillRect(lx - 3, LY.waveTop + 2, tw + 6, 14);
      ctx.fillStyle = hexA(p.color, 1);
      ctx.textAlign = 'left';
      ctx.fillText(label, lx, LY.waveTop + 12);
    }
  }

  for (let i = 0; i < PATTERNS.length; i++) {
    const p = PATTERNS[i];
    const y = LY.rowTop + i * (LAYOUT.rowH + LAYOUT.rowGap);
    ctx.fillStyle = '#15151d';
    ctx.fillRect(0, y, W, LAYOUT.rowH);
    const x1 = timeToX(p.start, W);
    const x2 = timeToX(p.end, W);
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, y, W, LAYOUT.rowH);
    ctx.clip();
    for (let k = 0; k < p.segments.length; k++) {
      const seg = p.segments[k];
      const ax = Math.max(0, timeToX(seg.start, W));
      const bx = Math.min(W, timeToX(seg.end, W));
      if (bx <= ax) continue;
      const cellW = bx - ax;
      ctx.fillStyle = hexA(p.color, cellW >= 2 ? (k % 2 ? 0.20 : 0.34) : 0.28);
      ctx.fillRect(ax, y + 4, cellW, LAYOUT.rowH - 8);
      if (k > 0 && cellW >= 2) {
        ctx.strokeStyle = 'rgba(13,13,17,0.85)';
        ctx.beginPath();
        ctx.moveTo(ax, y + 4);
        ctx.lineTo(ax, y + LAYOUT.rowH - 4);
        ctx.stroke();
      }
      if (cellW > 16) {
        ctx.fillStyle = 'rgba(255,255,255,0.87)';
        ctx.font = '9px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(String(k + 1) + (seg.partial ? '*' : ''), (ax + bx) / 2, y + 23);
      }
    }
    const bx1 = Math.max(0, x1);
    const bx2 = Math.min(W, x2);
    if (bx2 > bx1) {
      ctx.strokeStyle = hexA(p.color, 0.85);
      ctx.lineWidth = 1;
      ctx.strokeRect(bx1 + 0.5, y + 4.5, Math.max(1, bx2 - bx1 - 1), LAYOUT.rowH - 9);
    }
    if (bx2 > bx1 + 20) {
      ctx.fillStyle = '#fff';
      ctx.font = '600 10px monospace';
      ctx.textAlign = 'left';
      ctx.fillText(p.name, bx1 + 5, y + 13);
    }
    ctx.restore();
  }

  const axisY = H - 18;
  ctx.strokeStyle = 'rgba(255,255,255,0.28)';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, axisY);
  ctx.lineTo(W, axisY);
  ctx.stroke();
  ctx.fillStyle = '#a0a0b0';
  ctx.font = '10px monospace';
  for (let i = 0; i <= 8; i++) {
    const frac = i / 8;
    const x = frac * W;
    ctx.beginPath();
    ctx.moveTo(x, axisY);
    ctx.lineTo(x, axisY + 4);
    ctx.stroke();
    ctx.textAlign = i === 0 ? 'left' : i === 8 ? 'right' : 'center';
    ctx.fillText(formatMSS(viewStart + frac * (viewEnd - viewStart)), x, H - 3);
  }

  if (drag) {
    const a = Math.max(0, Math.min(drag.x0, drag.x1));
    const b = Math.min(W, Math.max(drag.x0, drag.x1));
    ctx.fillStyle = 'rgba(255,235,59,0.16)';
    ctx.fillRect(a, LY.waveTop, b - a, H - LAYOUT.axisH - LY.waveTop);
    ctx.strokeStyle = '#ffeb3b';
    ctx.lineWidth = 1;
    ctx.strokeRect(a + 0.5, LY.waveTop + 0.5, Math.max(0, b - a - 1), H - LAYOUT.axisH - LY.waveTop - 1);
  }

  if (hover) {
    ctx.strokeStyle = 'rgba(255,255,255,0.55)';
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(hover.x, LY.waveTop);
    ctx.lineTo(hover.x, H - LAYOUT.axisH);
    ctx.stroke();
    ctx.setLineDash([]);
  }
}

function renderAll() {
  const W = canvas.clientWidth;
  if (!W) return;
  const [s1, s2] = sampleRange();
  requestViewLevel(s1, s2, W);
  draw();
  updatePlayhead();
}

function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => { renderQueued = false; renderAll(); });
}

function resetZoom() {
  zoomHistory = [];
  viewStart = 0;
  viewEnd = META.duration;
  hover = null;
  tooltip.style.opacity = '0';
  updateViewInfo();
  renderAll();
}

function zoomOut() {
  if (!zoomHistory.length) return;
  const prev = zoomHistory.pop();
  viewStart = prev.start;
  viewEnd = prev.end;
  hover = null;
  tooltip.style.opacity = '0';
  updateViewInfo();
  renderAll();
}

function zoomPattern(i) {
  const p = PATTERNS[i];
  zoomHistory.push({ start: viewStart, end: viewEnd });
  viewStart = p.start;
  viewEnd = p.end;
  hover = null;
  tooltip.style.opacity = '0';
  updateViewInfo();
  renderAll();
}

function playSegment(i, j) {
  seekAndPlay(PATTERNS[i].segments[j].start);
}

function playPattern(i) {
  seekAndPlay(PATTERNS[i].start);
}

function seekAndPlay(t) {
  t = Math.max(0, Math.min(META.duration, t));
  const go = () => {
    try { audio.currentTime = t; } catch (err) {}
    updatePlayhead();
    const p = audio.play();
    if (p && p.catch) p.catch(() => {});
  };
  if (audio.readyState >= 1) go();
  else audio.addEventListener('loadedmetadata', go, { once: true });
}

function hitSegment(x, y) {
  const W = canvas.clientWidth;
  const H = canvas.clientHeight;
  const LY = layout(H);
  if (y < LY.rowTop) return null;
  const idx = Math.floor((y - LY.rowTop) / (LAYOUT.rowH + LAYOUT.rowGap));
  if (idx < 0 || idx >= PATTERNS.length) return null;
  const ry = LY.rowTop + idx * (LAYOUT.rowH + LAYOUT.rowGap);
  if (y > ry + LAYOUT.rowH) return null;
  const t = xToTime(x, W);
  const p = PATTERNS[idx];
  for (let j = 0; j < p.segments.length; j++) {
    const s = p.segments[j];
    if (t >= s.start && t <= s.end) return { i: idx, j: j };
  }
  return null;
}

function attach() {
  canvas.addEventListener('mousedown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const rect = canvas.getBoundingClientRect();
    const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));
    const y = Math.max(0, Math.min(rect.height, e.clientY - rect.top));
    drag = { x0: x, y0: y, x1: x, y1: y };
    draw();
  });

  canvas.addEventListener('mousemove', (e) => {
    const rect = canvas.getBoundingClientRect();
    const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));
    const y = Math.max(0, Math.min(rect.height, e.clientY - rect.top));
    hover = { x: x, y: y };
    if (drag) { drag.x1 = x; drag.y1 = y; }
    const t = xToTime(x, rect.width);
    const hit = hitSegment(x, y);
    let txt = `Time ${formatMSSPrecise(t)} \u00b7 sample ${Math.round(t * META.sampleRate)}`;
    if (hit) {
      const p = PATTERNS[hit.i];
      const s = p.segments[hit.j];
      txt += ` \u00b7 ${p.name} \u00b7 segment ${hit.j + 1}/${p.segments.length} \u00b7 ${formatMSSPrecise(s.start)}\u2013${formatMSSPrecise(s.end)} (${(s.end - s.start).toFixed(3)}s)`;
    } else {
      const p = PATTERNS.find(p => t >= p.start && t <= p.end);
      if (p) txt += ` \u00b7 ${p.name} \u00b7 ${p.length.toFixed(3)}s`;
    }
    tooltip.innerHTML = txt;
    tooltip.style.opacity = '1';
    draw();
  });

  canvas.addEventListener('mouseup', (e) => {
    if (!drag) return;
    const d = drag;
    drag = null;
    const W = canvas.clientWidth;
    const dx = Math.abs(d.x1 - d.x0);
    if (dx > 4) {
      const t1 = xToTime(Math.min(d.x0, d.x1), W);
      const t2 = xToTime(Math.max(d.x0, d.x1), W);
      if (t2 - t1 > 0.005) {
        zoomHistory.push({ start: viewStart, end: viewEnd });
        viewStart = Math.max(0, t1);
        viewEnd = Math.min(META.duration, t2);
        hover = null;
        tooltip.style.opacity = '0';
        updateViewInfo();
        renderAll();
      }
    } else {
      const hit = hitSegment(d.x0, d.y0);
      if (hit) seekAndPlay(PATTERNS[hit.i].segments[hit.j].start);
      else seekAndPlay(xToTime(d.x0, W));
    }
    draw();
  });

  canvas.addEventListener('mouseleave', () => {
    drag = null;
    hover = null;
    tooltip.style.opacity = '0';
    draw();
  });

  canvas.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    zoomOut();
  });

  document.getElementById('resetZoomBtn').addEventListener('click', resetZoom);
  audio.addEventListener('play', startPlayheadLoop);
  audio.addEventListener('pause', updatePlayhead);
  audio.addEventListener('ended', updatePlayhead);
  audio.addEventListener('seeked', updatePlayhead);
  audio.addEventListener('timeupdate', updatePlayhead);
  window.addEventListener('resize', scheduleRender);
}

function init() {
  COARSE.arr = base64ToI16(window.PATTERN_COARSE);
  attach();
  updateViewInfo();
  renderAll();
}

if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
else init();
"""

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>__TITLE__</title>
<style>
  :root { --bg:#121216; --card:#1a1a22; --border:#2a2a36; --text:#e1e1e6; --muted:#a0a0b0; --accent:#2196f3; }
  * { box-sizing:border-box; margin:0; padding:0; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif; }
  body { background:var(--bg); color:var(--text); padding:20px; line-height:1.5; }
  header { background:var(--card); padding:18px 20px; border-radius:10px; border:1px solid var(--border); margin-bottom:16px; }
  h1 { font-size:1.45rem; color:#fff; margin-bottom:4px; }
  h2 { font-size:1.05rem; color:#fff; margin:20px 0 8px; }
  .meta, .pmeta { font-size:0.85rem; color:var(--muted); }
  .meta { display:flex; gap:18px; flex-wrap:wrap; margin-bottom:12px; }
  .toolbar { display:flex; align-items:center; gap:12px; flex-wrap:wrap; padding-top:10px; border-top:1px solid var(--border); }
  audio { height:34px; outline:none; border-radius:4px; }
  .btn, .mini { background:var(--accent); color:#fff; border:none; border-radius:6px; font-weight:600; cursor:pointer; }
  .btn { padding:7px 14px; font-size:0.85rem; }
  .mini { padding:4px 10px; font-size:0.78rem; }
  .btn:hover, .mini:hover { opacity:0.88; }
  .view-info { font-size:0.8rem; color:#4caf50; font-family:monospace; font-weight:bold; }
  .hint { font-size:0.78rem; color:var(--muted); margin-top:8px; font-style:italic; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:10px; padding:12px; margin-bottom:16px; }
  .canvas-wrap { position:relative; width:100%; height:380px; background:#0d0d11; border:1px solid #22222e; border-radius:6px; overflow:hidden; user-select:none; }
  canvas { width:100%; height:100%; display:block; cursor:crosshair; }
  .playhead { position:absolute; top:0; bottom:0; width:2px; margin-left:-1px; background:#ff5252; box-shadow:0 0 4px rgba(255,82,82,0.8); pointer-events:none; display:none; z-index:2; }
  .tooltip { position:absolute; top:8px; right:12px; background:rgba(10,10,15,0.88); border:1px solid #333344; padding:5px 8px; border-radius:4px; font-size:0.74rem; font-family:monospace; color:#fff; pointer-events:none; opacity:0; transition:opacity 0.12s; z-index:3; max-width:calc(100% - 24px); }
  .pcard { background:var(--card); border:1px solid var(--border); border-left:4px solid var(--c); border-radius:8px; padding:10px 12px; margin-bottom:10px; }
  .phead { display:flex; align-items:center; gap:10px; flex-wrap:wrap; }
  .pdot { width:10px; height:10px; border-radius:50%; background:var(--c); flex:0 0 auto; }
  .pname { font-weight:700; color:#fff; font-size:0.92rem; }
  .pmeta { flex:1 1 320px; }
  .pactions { display:flex; gap:6px; margin-left:auto; }
  .segrow { display:flex; flex-wrap:wrap; gap:4px; margin-top:8px; }
  .seg { min-width:27px; height:24px; padding:0 5px; border-radius:4px; border:1px solid var(--c); background:rgba(255,255,255,0.04); color:var(--text); font-family:monospace; font-size:0.72rem; cursor:pointer; }
  .seg:hover { background:var(--c); color:#0d0d11; }
  .seg.partial { border-style:dashed; opacity:0.75; }
  .legend { display:flex; gap:16px; flex-wrap:wrap; font-size:0.78rem; color:var(--muted); margin-top:8px; }
  .legend span { display:flex; align-items:center; gap:5px; }
  .swatch { width:20px; height:10px; border-radius:2px; }
</style>
</head>
<body>
<header>
  <h1>Pattern Analysis Report</h1>
  <div class="meta">__META_LINE__</div>
  <div class="toolbar">
    <audio id="audio" controls preload="metadata" src="__AUDIO_SRC__"></audio>
    <button class="btn" id="resetZoomBtn">Reset Zoom</button>
    <span class="view-info" id="viewInfo"></span>
  </div>
  <div class="hint">Drag horizontally on the graph to zoom. Right-click to zoom out one level. Click the waveform or a segment to jump the playhead and play.</div>
  <div class="legend">
    <span><span class="swatch" style="background:#b4bed2"></span>Full waveform</span>
    <span><span class="swatch" style="background:rgba(255,255,255,0.35)"></span>Segment boundaries</span>
    <span><span class="swatch" style="background:#ff5252"></span>Playhead</span>
  </div>
</header>
<div class="card">
  <div class="canvas-wrap" id="canvasWrap">
    <canvas id="wave"></canvas>
    <div class="playhead" id="playhead"></div>
    <div class="tooltip" id="tooltip"></div>
  </div>
</div>
<h2>Patterns &amp; segments</h2>
__PATTERN_CARDS__
<script src="__DATA_SCRIPT__"></script>
<script>
__REPORT_JS__
</script>
</body>
</html>
"""

INDEX_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="icon" href="data:,">
<title>__INDEX_TITLE__</title>
<style>
  :root { --bg:#121216; --card:#1a1a22; --border:#2a2a36; --text:#e1e1e6; --muted:#a0a0b0; --accent:#2196f3; }
  * { box-sizing:border-box; margin:0; padding:0; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif; }
  body { background:var(--bg); color:var(--text); padding:24px; line-height:1.5; }
  h1 { font-size:1.5rem; color:#fff; margin-bottom:6px; }
  p { color:var(--muted); font-size:0.9rem; margin-bottom:20px; }
  .list { display:grid; grid-template-columns:repeat(auto-fit,minmax(300px,1fr)); gap:14px; max-width:1100px; }
  .card { display:block; background:var(--card); border:1px solid var(--border); border-radius:10px; padding:18px; text-decoration:none; color:var(--text); transition:border-color 0.15s; }
  .card:hover { border-color:var(--accent); }
  .card strong { display:block; color:#fff; font-size:1rem; margin-bottom:6px; }
  .card span { display:block; color:var(--muted); font-size:0.85rem; }
  .btn { display:inline-block; margin-top:10px; background:var(--accent); color:#fff; padding:6px 12px; border-radius:6px; font-size:0.8rem; font-weight:600; }
</style>
</head>
<body>
<h1>__INDEX_TITLE__</h1>
<p>__INDEX_DESC__</p>
<div class="list">
__CARDS__
</div>
</body>
</html>
"""


def to_pcm16(data):
    """Convert an AudioFile-like numpy array to mono signed 16-bit PCM."""
    if data.dtype == np.int16:
        return data.astype('<i2', copy=False)
    if data.dtype == np.uint8:
        return ((data.astype(np.int32) - 128) * 256).astype('<i2')
    if data.dtype == np.int32:
        x = data.astype(np.float64)
        return np.clip(np.round(x / 2147483648.0 * 32767.0), -32768, 32767).astype('<i2')
    if data.dtype == np.float32 or data.dtype == np.float64:
        x = data.astype(np.float64)
        return np.clip(np.round(x * 32767.0), -32768, 32767).astype('<i2')
    raise ValueError('Unsupported WAV sample dtype: %s' % data.dtype)


def build_report(spec, out_dir, src_dir):
    source = spec['file']
    src_path = source if os.path.isabs(source) else os.path.join(src_dir, source)
    filename = os.path.basename(source)
    basename = os.path.splitext(filename)[0]
    if not os.path.isfile(src_path):
        raise FileNotFoundError(src_path)
    sr, data = wavfile.read(src_path)
    channels = 1 if data.ndim == 1 else data.shape[1]
    if data.ndim == 1:
        pcm = to_pcm16(data)
    else:
        mono = data.astype(np.float64).mean(axis=1)
        if data.dtype == np.int16:
            pcm = np.clip(np.round(mono), -32768, 32767).astype('<i2')
        elif data.dtype == np.uint8:
            pcm = np.clip(np.round((mono - 128.0) * 256.0), -32768, 32767).astype('<i2')
        elif data.dtype == np.int32:
            pcm = np.clip(np.round(mono / 2147483648.0 * 32767.0), -32768, 32767).astype('<i2')
        else:
            pcm = np.clip(np.round(mono * 32767.0), -32768, 32767).astype('<i2')
    total = len(pcm)
    duration = total / sr

    out_wav = os.path.join(out_dir, filename)
    if not os.path.exists(out_wav) or os.path.getsize(out_wav) != os.path.getsize(src_path):
        shutil.copy2(src_path, out_wav)

    data_dir_name = os.path.join('data', basename)
    data_dir = os.path.join(out_dir, data_dir_name)
    os.makedirs(data_dir, exist_ok=True)

    patterns = []
    for i, p in enumerate(spec.get('patterns', [])):
        start = float(p['start'])
        end = min(duration, float(p['end']))
        length = float(p['length'])
        if length <= 0:
            raise ValueError('Pattern %d has non-positive length' % (i + 1))
        if start < 0 or start >= duration or end <= start:
            raise ValueError('Pattern %d has invalid range %.3f-%.3f' % (i + 1, start, end))
        name = str(p.get('name', 'Pattern %d' % (i + 1)))
        color = p.get('color', COLORS[i % len(COLORS)])
        bars = int(p.get('bars', 1))
        patterns.append({
            'id': i,
            'name': name,
            'color': color,
            'start': round(start, 3),
            'end': round(end, 3),
            'length': length,
            'bars': bars,
            'segments': build_segments(start, end, length),
        })

    if not patterns:
        raise ValueError('No patterns supplied for %s' % filename)

    inter_coarse, coarse_bins = minmax_bins(pcm, COARSE_STEP)
    levels_meta = []
    for li, (step, minmax) in enumerate(LEVELS):
        nb, nchunks = write_level(pcm, step, data_dir, li)
        levels_meta.append({'step': step, 'minmax': bool(minmax), 'bins': nb})
        print('  level %d: %d bins, %d chunks' % (step, nb, nchunks))

    meta = {
        'file': filename,
        'sampleRate': int(sr),
        'channels': channels,
        'totalSamples': int(total),
        'duration': round(duration, 6),
        'dataDir': data_dir_name.replace(os.sep, '/'),
        'chunkBins': CHUNK_BINS,
        'coarseStep': COARSE_STEP,
        'coarseBins': int(coarse_bins),
        'levels': levels_meta,
        'patterns': patterns,
    }
    with open(os.path.join(data_dir, 'summary.js'), 'w') as f:
        f.write('window.PATTERN_META = ' + json.dumps(meta, separators=(',', ':')) + ';\n')
        f.write('window.PATTERN_COARSE = "' + b64_i16(inter_coarse) + '";\n')

    meta_line = ('<span><strong>Source:</strong> %s</span>'
                 '<span><strong>Duration:</strong> %.2fs</span>'
                 '<span><strong>Sample Rate:</strong> %s Hz</span>'
                 '<span><strong>Channels:</strong> %s (%s)</span>'
                 '<span><strong>Patterns:</strong> %d</span>') % (
                     html.escape(filename), duration, sr, channels,
                     'Mono' if channels == 1 else 'Stereo', len(patterns))
    html_text = (HTML_TEMPLATE
                 .replace('__TITLE__', html.escape('Pattern Analysis - ' + filename))
                 .replace('__META_LINE__', meta_line)
                 .replace('__AUDIO_SRC__', filename)
                 .replace('__PATTERN_CARDS__', pattern_cards(patterns))
                 .replace('__DATA_SCRIPT__', data_dir_name.replace(os.sep, '/') + '/summary.js')
                 .replace('__REPORT_JS__', JS))
    with open(os.path.join(out_dir, basename + '.html'), 'w') as f:
        f.write(html_text)

    validate(pcm, inter_coarse, coarse_bins, data_dir)
    return {'filename': filename, 'basename': basename, 'duration': duration, 'patterns': len(patterns)}


def validate(pcm, inter_coarse, coarse_bins, data_dir):
    arr = np.frombuffer(base64.b64decode(b64_i16(inter_coarse)), dtype='<i2')
    assert len(arr) == coarse_bins * 2, (len(arr), coarse_bins)
    for b in [0, 1, 17, coarse_bins // 2, coarse_bins - 1]:
        a = b * COARSE_STEP
        e = min(len(pcm), a + COARSE_STEP)
        if e <= a:
            continue
        seg = pcm[a:e]
        assert arr[b * 2] == seg.min(), (b, arr[b * 2], seg.min())
        assert arr[b * 2 + 1] == seg.max(), (b, arr[b * 2 + 1], seg.max())
    with open(os.path.join(data_dir, 'l8_0.js')) as f:
        m = re.search(r'\["([^"]+)"\]', f.read())
    assert m, 'chunk base64 not found'
    chunk = np.frombuffer(base64.b64decode(m.group(1)), dtype='<i2')
    for b in [0, 1, 7, 100]:
        a = b * 8
        e = min(len(pcm), a + 8)
        seg = pcm[a:e]
        assert chunk[b * 2] == seg.min(), (b, chunk[b * 2], seg.min())
        assert chunk[b * 2 + 1] == seg.max(), (b, chunk[b * 2 + 1], seg.max())


def main():
    ap = argparse.ArgumentParser(description='Build interactive audio pattern reports.')
    ap.add_argument('--spec', required=True, help='JSON report specification')
    ap.add_argument('--out', required=True, help='Output report directory')
    ap.add_argument('--src', default=None, help='Base directory for relative audio paths')
    args = ap.parse_args()

    with open(args.spec) as f:
        spec = json.load(f)
    os.makedirs(args.out, exist_ok=True)
    src_dir = args.src or spec.get('srcDir') or os.getcwd()
    reports = spec.get('reports')
    if reports is None:
        if 'file' in spec:
            reports = [spec]
        else:
            raise SystemExit('Spec must contain a "reports" list or a top-level "file".')

    results = []
    seen = set()
    for r in reports:
        source = r['file']
        basename = os.path.splitext(os.path.basename(source))[0]
        if basename in seen:
            raise SystemExit('Duplicate report basename: %s' % basename)
        seen.add(basename)
        print('Building', source)
        results.append(build_report(r, args.out, src_dir))

    cards = []
    for r in results:
        cards.append(
            '<a class="card" href="%s.html"><strong>%s</strong>'
            '<span>%s &middot; %d pattern%s</span>'
            '<span class="btn">Open report</span></a>' % (
                html.escape(r['basename']), html.escape(r['filename']),
                fmt_precise(r['duration']), r['patterns'],
                '' if r['patterns'] == 1 else 's'))
    title = spec.get('title', 'Pattern Analysis Reports')
    desc = spec.get('description', 'Full waveforms with detected pattern regions, loop segments, zooming, playback and playhead jumping.')
    index = (INDEX_TEMPLATE
             .replace('__INDEX_TITLE__', html.escape(title))
             .replace('__INDEX_DESC__', html.escape(desc))
             .replace('__CARDS__', '\n'.join(cards)))
    with open(os.path.join(args.out, 'index.html'), 'w') as f:
        f.write(index)
    print('Done:', args.out)


if __name__ == '__main__':
    main()
