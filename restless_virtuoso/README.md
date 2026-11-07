# restless_virtuoso

Plays selected ASIO input channels on selected ASIO output channels, for monitoring
an audio interface at ASIO latency. MIDI input can be played through a built-in sine
synth and/or forwarded to a MIDI output.

The driver's own ASIO interface is used directly, so no ASIO SDK download, PortAudio
or WASAPI is involved and no non-ASIO device can be selected.

## Build

```bash
make -C restless_virtuoso   # x86_64-w64-mingw32-gcc on PATH
```

Or build with the same source in Visual Studio / MSVC as a console project linked
against `winmm.lib`.

## Usage

```bash
restless_virtuoso.exe --list
restless_virtuoso.exe --driver "Focusrite USB ASIO" --in 1,2 --out 1,2 --gain -6
restless_virtuoso.exe --in 1 --out 1,2 --buffer 128 --rate 48000
restless_virtuoso.exe --in none --midi-in "Keystation" --midi-out 0
```

- Without `--driver`, `Focusrite USB ASIO` is loaded when installed; otherwise the first installed driver is used.
- `--in` / `--out` take 1-based channel numbers (`1,2`, `1-4`, `3,5-6`), default `1,2`.
  Output channel `j` plays input channel `j`, wrapping if there are more outputs than inputs.
- `--buffer` picks the ASIO buffer size; omitted, the driver's preferred size is used.
- `--rate` is validated with the driver; omitted, the driver's current rate is used.
- `--midi-in` accepts a device index from `--list` or an exact device name.
- `--midi-out` also accepts an index or name and forwards incoming MIDI unchanged.

On startup the program prints every input and output channel of the selected ASIO
driver and every MIDI input device, so the indices used by `--in`, `--out` and
`--midi-in` are listed before monitoring starts.

While running: `m` mute, `+`/`-` gain, `p` open the ASIO control panel, `q` quit.

## Notes

- 64-bit Windows only; ASIO drivers are loaded in-process, so the driver must be 64-bit.
- Supported ASIO sample formats: `Int16LSB`, `Int24LSB`, `Int32LSB`, the 16/18/20/24-bit
  aligned `Int32LSB` variants, and `Float32LSB`. Big-endian formats are rejected.
- ASIO is exclusive: close your DAW before starting the monitor.
- The synth is a 16-voice sine with a short attack/release, mixed onto the first two
  selected outputs; `--no-synth` leaves MIDI input to the MIDI thru path only.
- Changing the sample rate or buffer size in the control panel reopens the buffers
  automatically.
- `restless_virtuoso.exe.log` next to the exe is created at startup; errors are appended.
