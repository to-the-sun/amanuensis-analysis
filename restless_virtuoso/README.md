# restless_virtuoso

Plays selected ASIO input channels, or the WAV files in a folder, on selected ASIO
output channels, for monitoring an audio interface at ASIO latency. MIDI input can
be played through a built-in sine synth, drum samples or a VST instrument and/or
forwarded to a MIDI output.

The driver's own ASIO interface is used directly, so no ASIO SDK download, PortAudio
or WASAPI is involved and no non-ASIO device can be selected.

## Build

```bash
make -C restless_virtuoso   # x86_64-w64-mingw32-gcc on PATH
```

Or build with the same source in Visual Studio / MSVC as a console project linked
against `winmm.lib` and `shell32.lib`.

## Usage

```bash
restless_virtuoso.exe --list
restless_virtuoso.exe --driver "Focusrite USB ASIO" --in 1,2 --out 1,2 --gain -6
restless_virtuoso.exe --in 1 --out 1,2 --buffer 128 --rate 48000
restless_virtuoso.exe --in none --midi-in "Keystation" --midi-out 0
restless_virtuoso.exe "Focusrite USB ASIO Input 1.json"
```

- Without `--driver`, `Focusrite USB ASIO` is loaded when installed; otherwise the first installed driver is used.
- `--in` / `--out` take 1-based channel numbers (`1,2`, `1-4`, `3,5-6`), default `1,2`.
  Output channel `j` plays input channel `j`, wrapping if there are more outputs than inputs.
- `--buffer` picks the ASIO buffer size; omitted, the driver's preferred size is used.
- `--rate` is validated with the driver; omitted, the driver's current rate is used.
- `--midi-in` accepts a device index from `--list` or an exact device name.
- `--midi-out` also accepts an index or name and forwards incoming MIDI unchanged.

## Settings file

Drop a JSON file onto `restless_virtuoso.exe` (or pass it as the first argument)
to start from it:

```json
{
  "instrument": "Focusrite USB ASIO",
  "input-channel": 1
}
```

A folder dropped onto `restless_virtuoso.exe` (or passed as the first argument) is
played directly, as if it were the `instrument` key, and may be longer than
`MAX_PATH`.

`instrument` is an ASIO driver or MIDI input name from `--list`, or the absolute path
of a folder. A folder's `.wav` files play together as stems and loop when the longest
one ends. `input-channel` is the 1-based ASIO input, or the MIDI channel for a MIDI
device, whose messages are then streamed to the console.
`Focusrite USB ASIO Input 1.json` is a ready template; settings files are named after
the input device, with its exact capitalization.

When `instrument` is a MIDI input, two more keys add instruments to it:

```json
{
  "instrument": "Keystation",
  "input-channel": 1,
  "samples_path": "C:\\samples",
  "vsts_path": "C:\\VSTs",
  "samples_ratio": 50
}
```

- `samples_path` loads every `.wav` under the folder and its subfolders as drum
  samples. Each of the 128 MIDI notes gets a different one at random, and a note-on
  plays it.
- `vsts_path` loads every 64-bit VST 2 `.dll` under the folder and its subfolders,
  then picks one at random and sets a random factory preset on it. Plugins without
  factory presets keep their default.
- `samples_ratio` is the percentage chance, 0 to 100 (default 50), that a new sound
  comes from `samples_path` instead of `vsts_path` when both are loaded. Only one of
  them plays.

The sine synth stays off in this mode; the loaded instrument is the sound source.
A key that is not an existing folder is logged and ignored.

A folder is mixed onto the selected outputs with no ASIO input monitoring unless
`--in` is given. Files of any channel count play on all outputs, wrapping when there
are more outputs than file channels, and each file's sample rate is converted to the
driver's rate.

On startup the program lists every installed ASIO driver, then every MIDI input
device.

In instrument mode, click `new sound` in the console for another random sound, drawn
from `samples_path` or `vsts_path` by `samples_ratio`.

## Notes

- 64-bit Windows only; ASIO drivers are loaded in-process, so the driver must be 64-bit.
- Supported ASIO sample formats: `Int16LSB`, `Int24LSB`, `Int32LSB`, the 16/18/20/24-bit
  aligned `Int32LSB` variants, and `Float32LSB`. Big-endian formats are rejected.
- ASIO is exclusive: close your DAW before starting the monitor.
- The synth is a 16-voice sine with a short attack/release, mixed onto the first two
  selected outputs; `--no-synth` leaves MIDI input to the MIDI thru path only.
- Changing the sample rate or buffer size in the control panel reopens the buffers
  automatically.
- The master output is scaled by the Windows master volume, re-read while running.
- A MIDI input that fails to open is retried, then reported with the Windows error code,
  its device name and driver version, and whether it opens without a callback.
- Every run writes a new `restless_virtuoso-YYYY-MM-DD_HH-MM-SS.log` next to the exe with startup stages and errors.
- At startup, the program's logs older than 7 days are moved to the Recycle Bin.
