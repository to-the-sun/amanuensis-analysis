"""Pass-file discovery, parsing and pass-relative placement for palettes.

A passes file holds one line per recorded pass, ``index, loop_point_ms pad;``.
Pass i spans [loop_point_i, loop_point_{i+1}); the last pass runs to the end of
the audio. Pattern starts are anchored to these loop points.
"""
import os

TEXT_EXTS = ('.txt', '.text')
GRID_TOL = 0.05


def find_passes_file(audio_path):
    folder = os.path.dirname(os.path.abspath(audio_path))
    stem = os.path.splitext(os.path.basename(audio_path))[0]
    prefix = 'passes' + stem[7:] if stem.lower().startswith('palette') else 'passes'
    found = []
    for name in os.listdir(folder):
        base, ext = os.path.splitext(name)
        if ext.lower() not in TEXT_EXTS or not base.lower().startswith(prefix.lower()):
            continue
        rest = base[len(prefix):]
        if rest and rest[0].isalnum():
            continue
        found.append(os.path.join(folder, name))
    if not found:
        raise FileNotFoundError(
            'No passes text file for %s (expected %s*.txt beside the audio)'
            % (os.path.basename(audio_path), prefix))
    return sorted(found, key=lambda p: (len(os.path.basename(p)), os.path.basename(p)))[0]


def parse_passes(path):
    rows = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            text = line.strip()
            if not text or text[0] in '#/':
                continue
            parts = text.replace(';', ' ').replace(',', ' ').split()
            if len(parts) < 2:
                continue
            try:
                rows.append({
                    'index': int(parts[0]),
                    'loop': float(parts[1]) / 1000.0,
                    'pad': float(parts[2]) / 1000.0 if len(parts) > 2 else 0.0,
                })
            except ValueError:
                continue
    if not rows:
        raise ValueError('No pass lines in %s' % path)
    rows.sort(key=lambda r: (r['index'], r['loop']))
    return rows


def pass_spans(rows, duration):
    spans = []
    for i, row in enumerate(rows):
        end = rows[i + 1]['loop'] if i + 1 < len(rows) else duration
        start = max(0.0, row['loop'])
        end = min(duration, end)
        if end > start:
            spans.append({
                'index': row['index'],
                'loop': row['loop'],
                'pad': row['pad'],
                'start': start,
                'end': end,
            })
    return spans


def pass_at(t, spans):
    near = [s for s in spans if abs(t - s['loop']) <= GRID_TOL]
    if near:
        return min(near, key=lambda s: abs(t - s['loop']))
    for span in spans:
        if span['loop'] <= t < span['end'] - 1e-9:
            return span
    return min(spans, key=lambda s: abs(s['start'] - t))


def offset_in_pass(t, span):
    return t - span['loop'] + span['pad']


def entry_bar(t, span, bar_length):
    offset = offset_in_pass(t, span)
    bar = int(round(offset / bar_length))
    if abs(offset - bar * bar_length) > GRID_TOL:
        raise ValueError(
            'start %.3f s is %.3f s off the pass %d bar grid (bar %.6f s)'
            % (t, offset - bar * bar_length, span['index'], bar_length))
    if bar < 0:
        raise ValueError('start %.3f s falls %d bars before pass %d'
                         % (t, -bar, span['index']))
    return bar
