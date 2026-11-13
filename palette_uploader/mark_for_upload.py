"""Set the modified time of this folder's palette and passes pair to now, so the watcher uploads it."""

import os
from pathlib import Path

PAIR = (("palette_", ".wav"), ("passes_", ".txt"))

found = {}
for entry in Path.cwd().iterdir():
    name = entry.name.lower()
    for prefix, extension in PAIR:
        if name.startswith(prefix) and name.endswith(extension):
            found.setdefault(name[len(prefix):-len(extension)], {})[prefix] = entry

marked = [match for match in found.values() if len(match) == len(PAIR)]
for match in marked:
    for entry in match.values():
        os.utime(entry)

if marked:
    for match in marked:
        print("marked " + ", ".join(sorted(entry.name for entry in match.values())))
else:
    print(f"no palette_*.wav with a matching passes_*.txt in {Path.cwd()}")

input("Press Enter to close")
