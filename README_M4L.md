# Max for Live Proof of Concept: Music Box

This repository contains a proof-of-concept Max for Live instrument patcher: `MusicBox.maxpat`.

## How to use in Ableton Live

1. **Open Ableton Live.**
2. **Create a new Max Instrument:**
   - Drag a blank "Max Instrument" from the "Max for Live" category in the browser into a MIDI track.
3. **Open the Max Editor:**
   - Click the "Edit" button (the small rectangle icon) on the Max Instrument device title bar.
4. **Load the Patcher:**
   - In the Max editor, go to `File > Open` and select `MusicBox.maxpat` from this repository.
5. **Save as AMXD:**
   - Go to `File > Save As...` and save the file as `MusicBox.amxd`.
   - Max will automatically bundle the dependencies.
6. **Play:**
   - Click the "Play" button in the device UI to start the "Twinkle Twinkle Little Star" sequencer.
   - Use the "Gain" slider to adjust the volume.
   - You can also play MIDI notes into the track to trigger the internal synthesizer (triangle oscillator).

## Patch Architecture

- **Sequencer:** A `metro` object triggers a `counter`, which reads MIDI pitch values from an embedded `coll` object containing the melody.
- **Synthesis:**
  - `mtof`: Converts MIDI pitch to frequency.
  - `tri~`: Triangle oscillator.
  - `adsr~`: Simple amplitude envelope with a 5ms attack and 100ms decay, creating a plucked/music-box sound.
  - `live.gain~`: Master volume control.
  - `plugout~`: Sends the stereo audio signal to Ableton Live's track.
- **External MIDI:** A `notein` object allows the device to be played like a standard synthesizer using external MIDI clips or controllers.
