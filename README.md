# Audio Analysis and Transcription Suite

This repository contains a suite of tools for advanced audio transient analysis and real-time Discord voice transcription. The project is divided into two primary components: `analysis/` for structural audio visualization and `transcription/` for AI-powered voice-to-text.

---

## 1. Analyze (`analysis/`)

The analysis suite focuses on identifying rhythmic energy, structural patterns, and transient density within audio files.

### Interactive Reports (`analyze_files.py` & `cumulative_transience.py`)
Generates high-resolution HTML reports and MP4 videos featuring real-time transient tracking.

**Usage:**
```bash
python3 analysis/analyze_files.py "path/to/audio.wav"
```

### Real-Time Playback (`play_files.py`)
Audibly plays audio files while simultaneously running the transient analysis engine and printing results to the console.

**Usage:**
```bash
python3 analysis/play_files.py "path/to/audio.wav"
```

**Diagnostic Options:**
*   `--list-devices`: Displays all available audio output devices and their indices.
*   `--device INDEX_OR_NAME`: Manually specify a playback device if the default is incorrect.
*   `--mock`: Runs the analysis without attempting audio output (useful for headless environments).

*   **Spectral Division:** Uses a Mel Spectrogram (128 bands) to split audio into four distinct bands for perceptual granularity:
    *   **Sub-Bass (Bins 0-31):** 0 Hz – 1,024 Hz (at 44.1 kHz)
    *   **Bass/Low-Mid (Bins 32-63):** 992 Hz – 2,849 Hz (at 44.1 kHz)
    *   **High-Mid (Bins 64-95):** 2,759 Hz – 7,926 Hz (at 44.1 kHz)
    *   **Treble (Bins 96-127):** 7,676 Hz – 22,050 Hz (at 44.1 kHz)
*   **Temporal Resolution:** Onset strength is calculated with a **1ms resolution**, ensuring even the fastest attacks are captured.
*   **Cumulative Buffer:** A 5001-sample (5-second) historical buffer tracks accumulated transient energy.
    *   **Cleanup Sweep:** To prevent perpetual accumulation, transient contributions are subtracted from the buffer exactly 15 seconds after they are added.
    *   **Peak Identification:** Identifies and labels the top three peaks in the buffer in real-time:
        *   **Gold (#f1c40f):** 1st largest peak.
        *   **Silver (#ecf0f1):** 2nd largest peak.
        *   **Bronze (#bdc3c7):** 3rd largest peak.
    *   **Scaling:** Dynamic Y-axis scaling excludes the last 100ms of the buffer to prevent visual distortion from the alignment peak.
*   **Video Generation:** Produces MP4s with synchronized transient graphs and mono audio (forced via FFmpeg for size efficiency).

### Automation: Amanuensis (`Amanuensis.py`)
A Discord bot that automates the deployment of the analysis pipeline.

*   **Monitoring:** Watches a local directory for new `.wav` files.
*   **Conversion:** Automatically converts high-res WAVs to mono MP3s, dynamically adjusting bitrate to stay under Discord's 10MB limit.
*   **Discord Integration:**
    *   Uploads the MP3 to a designated `#works-in-progress` channel.
    *   Posts a notification in `#general` and cleans up previous bot notifications.
    *   Uses alphanumeric segment-based matching to delete older versions of the same file.
*   **Background Processing:** Transient analysis and video generation are offloaded to background threads (using `asyncio.to_thread`) to prevent blocking the bot's heartbeat.
*   **Recency Logic:** Skips files by comparing their Modified/Created UTC timestamps against the timestamp of the bot's most recent post in the history.

---

## 2. Transcription (`transcription/`)

The transcription suite provides real-time voice-to-text capabilities for Discord voice channels, with a focus on poetic formatting and linguistic analysis.

### Aqua Transcription Bot (`transcription_bot_aqua.py`)
A sophisticated Discord bot utilizing the AquaVoice API.

**Usage:**
```bash
python3 transcription/transcription_bot_aqua.py
```

*   **DAVE Decryption:** Implements custom patches for `discord.ext.voice_recv` to handle Discord's End-to-End Encryption (DAVE). It manages AES-GCM decryption and tracks Sequence Numbers/Roll-Over Counters (ROC) for stable audio streams.
*   **Real-time Transcription:**
    *   Connects to the `avalon-v1.5` model via the AquaVoice API.
    *   Uses an in-memory buffer system to process audio chunks only when sufficient activity (RMS threshold) is detected.
*   **Poetic Parsing:** A deterministic engine that formats raw transcripts into verse:
    *   Splits lines based on punctuation (`. ! ? , ; : ( ) -`).
    *   Lowercases the start of every line.
    *   **Question Marks:** Specifically preserved and re-attached to the end of lines.
    *   Word-internal punctuation (like apostrophes) is maintained.
*   **Slash Commands:**
    *   `/analyze`: Performs a deep dive into the channel's history.
        *   Counts syllables for every line using NLTK (CMUdict) and the `syllables` library.
        *   Detects rhymes using the `SoundsLike` library (vowel-class homophones).
        *   Generates a poem by grouping the most common syllable-count lines by rhyme sound.
    *   `/purge`: Clears the transcription channel history.
*   **Stability:** Includes a health-check loop that monitors decryption failures and automatically reconnects the voice client if the stream stalls.

---

## Installation & Dependencies

### System Dependencies
The suite requires standard audio and DSP libraries:
```bash
sudo apt-get update && sudo apt-get install -y libsndfile1-dev libaubio-dev libjson-c-dev libfftw3-dev ffmpeg
```

### Python Environment
Install the required Python modules:
```bash
pip install librosa numpy scipy matplotlib soundcard soundfile discord.py[voice] discord-ext-voice-recv davey cryptography faster-whisper google-genai torch transformers mido plotly playwright openai requests tqdm nltk syllables SoundsLike pydub
```

### Linguistic Data
Required for the `/analyze` command in the transcription bot:
```bash
python3 -m nltk.downloader cmudict averaged_perceptron_tagger
```

## Configuration
Both `Amanuensis.py` and the transcription bots require a `credentials.json` file in the root directory:
```json
{
  "token": "YOUR_DISCORD_BOT_TOKEN",
  "aqua_key": "YOUR_AQUAVOICE_API_KEY"
}
```
