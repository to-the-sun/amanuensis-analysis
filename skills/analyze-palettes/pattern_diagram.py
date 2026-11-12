import os
import numpy as np
import scipy.io.wavfile as wavfile
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'pattern_8.wav')
OUT = os.path.join(HERE, 'waveform_patterns.png')

BG, WAVE, SEP, MUTED, AXIS = '#07080c', '#5e6773', '#1b1c26', '#9aa2ae', '#4a505c'
W, H, DPI = 4600, 1600, 200
LEFT, RIGHT, TOP, BOT = 330, 4570, 232, 1440
RED, GOLD, GREEN, LIME = '#f12a1e', '#f1ad03', '#3ad07d', '#87fa96'

# One grid for every pattern: 2 cells = the 2.15 s bar, 1 cell = the bass's 1.07 s
# segment, so no two patterns' segment boundaries can land anywhere but together.
GRID = 1.075
# name, colour, segment length in cells, [(first cell, segment count), ...]
PATTERNS = [
    ('bass',   RED,   1, [(0, 73)]),
    ('cymbal', GOLD,  2, [(2, 35)]),
    ('kick',   GREEN, 2, [(2, 35)]),
    ('kick',   LIME,  2, [(2, 6), (36, 5), (62, 1)]),
]

sr, data = wavfile.read(SRC)
x = (data.astype(np.float64) / 32768.0) if data.dtype == np.int16 else data.astype(np.float64)
dur = len(x) / sr
ncols = RIGHT - LEFT
edges = np.linspace(0, len(x), ncols + 1).astype(int)
lo, hi = np.minimum.reduceat(x, edges[:-1]), np.maximum.reduceat(x, edges[:-1])
centres = (edges[:-1] + edges[1:]) / (2 * sr)

fig = plt.figure(figsize=(W / DPI, H / DPI), dpi=DPI, facecolor=BG)
ax = fig.add_axes([LEFT / W, 1 - BOT / H, (RIGHT - LEFT) / W, (BOT - TOP) / H])
ax.set_xlim(0, dur)
ax.set_ylim(0, 4)
ax.axis('off')

for i, (_, colour, _, _) in enumerate(PATTERNS):
    ax.axhspan(3 - i, 4 - i, color=colour, alpha=0.045, lw=0)
for y in (1, 2, 3):
    ax.axhline(y, color=SEP, lw=0.6)

ax.fill_between(centres, 2 + lo * 1.9, 2 + hi * 1.9, color=WAVE, lw=0)

inset = 0.055
for i, (_, colour, cells, groups) in enumerate(PATTERNS):
    y0, y1 = 3 - i + inset, 4 - i - inset
    seglen = cells * GRID
    for cell, count in groups:
        start = cell * GRID
        ax.add_patch(Rectangle((start, y0), count * seglen, y1 - y0, fill=False,
                               edgecolor=colour, lw=1.4, joinstyle='miter', clip_on=False))
        for k in range(1, count):
            ax.plot([start + k * seglen] * 2, [y0, y1], color=colour, lw=0.7,
                    ls=(0, (3, 3)), alpha=0.85, solid_capstyle='butt', clip_on=False)

ax.plot([0, dur], [0, 0], color=AXIS, lw=0.5)
for t in range(0, int(dur), 5):
    ax.plot([t, t], [0, -0.035], color=AXIS, lw=0.5)
    ax.text(t, -0.07, str(t), color=MUTED, fontsize=7.5, ha='center', va='top', clip_on=False)
ax.text(dur / 2, -0.24, 'time (s)', color=MUTED, fontsize=7.5, ha='center', va='top', clip_on=False)

lane_h = (BOT - TOP) / 4
for i, (name, colour, cells, groups) in enumerate(PATTERNS):
    yc = 1 - (TOP + (i + 0.5) * lane_h) / H
    fig.patches.append(Rectangle((48 / W, yc - 7 / H), 15 / W, 15 / H,
                                 transform=fig.transFigure, facecolor=colour, lw=0))
    fig.text(74 / W, yc + 0.006, name, color=colour, fontsize=11, fontweight='bold', va='center')
    fig.text(74 / W, yc - 0.013, f'{cells * GRID:.2f} s \u00d7 {sum(n for _, n in groups)}',
             color=MUTED, fontsize=8, va='center')

fig.text(48 / W, 1 - 78 / H, os.path.basename(SRC), color='#ffffff',
         fontsize=15, fontweight='bold', va='center')
fig.text(48 / W, 1 - 128 / H, f'{dur:.1f} s \u00b7 {sr / 1000:g} kHz \u00b7 mono \u00b7 four repeating patterns',
         color=MUTED, fontsize=9, va='center')
fig.text(48 / W, 1 - 174 / H,
         'thick outer rectangle = the whole pattern \u00b7 '
         'thin dashed verticals = the division between each segment',
         color=MUTED, fontsize=8.5, va='center')

fig.savefig(OUT, dpi=DPI, facecolor=BG)
print('wrote', OUT)
