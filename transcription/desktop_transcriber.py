import os
import sys
import logging
import time
import threading
import collections
import asyncio
import numpy as np

import analyze_transcript

sys.excepthook = analyze_transcript.handle_exception

logger = logging.getLogger("desktop_transcriber")

# --- SOUNDCARD INITIALIZATION WITH GRACEFUL FALLBACK ---
if len(sys.argv) < 2:
    sys.argv.append("desktop_transcriber.py")

SOUNDCARD_AVAILABLE = False
sc = None
try:
    import soundcard as sc
    _test_spk = sc.default_speaker()
    SOUNDCARD_AVAILABLE = True
    logger.info(f"soundcard initialized successfully. Default speaker: {_test_spk.name if _test_spk else 'None'}")
except Exception as e:
    logger.warning(f"soundcard could not be initialized (falling back to mock mode): {e}")

class DesktopTranscriberBot(analyze_transcript.BaseTranscriptionBot):
    def __init__(self, speaker_name=None, **kwargs):
        super().__init__(**kwargs)
        self.speaker_name = speaker_name
        self.recording_thread = None
        self.recording_active = False
        self.accumulated_utterances_list = []
        self.accumulated_start_time = None

    async def on_ready(self):
        logger.info(f"Logged in as {self.user} (ID: {self.user.id})")
        self.find_world_channel()

        self.recording_active = True
        self.recording_thread = threading.Thread(target=self.run_desktop_audio_capture, daemon=True)
        self.recording_thread.start()
        logger.info("Desktop audio capture background thread started.")

    def run_desktop_audio_capture(self):
        sample_rate = 48000
        chunk_duration = 0.1 # 100ms
        chunk_frames = int(sample_rate * chunk_duration) # 4800 frames

        pre_roll_duration = 0.3 # 300ms look ahead
        pre_roll_max_len = int(pre_roll_duration / chunk_duration) # 3 chunks
        pre_roll_buffer = collections.deque(maxlen=pre_roll_max_len)

        rms_threshold = 0.0015
        silence_timeout = 1.0
        max_utterance_duration = 45.0

        is_active = False
        active_buffer = []
        silence_duration = 0.0

        logger.info("Setting up loopback microphone...")
        mic = None

        if SOUNDCARD_AVAILABLE:
            try:
                default_speaker = sc.default_speaker()
                logger.info(f"Using default speaker: {default_speaker.name}")
                mic = sc.get_microphone(id=str(default_speaker.name), include_loopback=True)
                logger.info(f"Loopback microphone successfully resolved: {mic.name}")
            except Exception as e:
                logger.error(f"Failed to resolve default speaker/loopback microphone: {e}. Falling back to mock mode.")
                mic = None

        if mic is None:
            logger.info("Starting in Mock Mode. Simulating periodic transcription activity.")
            mock_counter = 1
            while self.recording_active:
                time.sleep(25)
                if self.text_channel_id:
                    mock_text = f"This is mock transcription {mock_counter} of desktop audio."
                    mock_counter += 1
                    logger.info(f"MOCK TRANSCRIPTION EVENT: {mock_text}")
                    asyncio.run_coroutine_threadsafe(
                        self.post_transcription_to_channel(mock_text, speaker=self.speaker_name),
                        self.loop
                    )
            return

        try:
            with mic.recorder(samplerate=sample_rate) as recorder:
                logger.info("Loopback recorder stream opened. Listening continuously...")
                while self.recording_active:
                    if self.accumulated_utterances_list and self.accumulated_start_time is not None:
                        if time.time() - self.last_world_message_time > 49.0:
                            logger.info("Flush timeout reached (49 seconds of no world messages). Sending accumulated speech anyway...")
                            combined_audio = np.concatenate(self.accumulated_utterances_list)
                            self.accumulated_utterances_list = []
                            self.accumulated_start_time = None
                            mono_16k = combined_audio[::3]
                            self.transcribe_and_post_threadsafe(mono_16k, speaker=self.speaker_name)

                    chunk = recorder.record(numframes=chunk_frames)

                    if chunk.ndim > 1:
                        chunk_mono = chunk.mean(axis=1)
                    else:
                        chunk_mono = chunk

                    rms = np.sqrt(np.mean(chunk_mono ** 2))

                    if not is_active:
                        pre_roll_buffer.append(chunk_mono)
                        if rms > rms_threshold:
                            logger.info(f"Voice activity detected! (RMS: {rms:.5f}) Triggering active utterance.")
                            is_active = True
                            active_buffer = list(pre_roll_buffer)
                            pre_roll_buffer.clear()
                            silence_duration = 0.0
                    else:
                        active_buffer.append(chunk_mono)
                        current_duration = len(active_buffer) * chunk_duration

                        if rms < rms_threshold:
                            silence_duration += chunk_duration
                        else:
                            silence_duration = 0.0

                        if silence_duration >= silence_timeout or current_duration >= max_utterance_duration:
                            reason = "silence timeout" if silence_duration >= silence_timeout else "max duration limit"
                            logger.info(f"Utterance finished ({reason}, total duration: {current_duration:.2f}s). Processing...")

                            utterance_audio = np.concatenate(active_buffer)

                            self.accumulated_utterances_list.append(utterance_audio)
                            if self.accumulated_start_time is None:
                                self.accumulated_start_time = time.time()

                            total_accum_len = sum(len(arr) for arr in self.accumulated_utterances_list)
                            total_accum_duration = total_accum_len / sample_rate

                            if total_accum_duration >= 10.0:
                                logger.info(f"Accumulated speech duration is {total_accum_duration:.2f}s (>= 10s). Sending to transcription...")
                                combined_audio = np.concatenate(self.accumulated_utterances_list)
                                self.accumulated_utterances_list = []
                                self.accumulated_start_time = None
                                mono_16k = combined_audio[::3]
                                self.transcribe_and_post_threadsafe(mono_16k, speaker=self.speaker_name)
                            else:
                                logger.info(f"Accumulated speech duration is {total_accum_duration:.2f}s (< 10s). Waiting for more speech to fill the time...")

                            is_active = False
                            active_buffer = []
                            silence_duration = 0.0

        except Exception as e:
            logger.error(f"Error in run_desktop_audio_capture: {e}")
            self.recording_active = False

    async def close(self):
        self.recording_active = False
        await super().close()

if __name__ == "__main__":
    script_dir = os.path.dirname(os.path.abspath(__file__))
    analyze_transcript.cleanup_old_mp3s(script_dir)

    config = analyze_transcript.load_credentials(script_dir)
    TOKEN = config['token']
    AQUA_KEY = config['aqua_key']

    bot = DesktopTranscriberBot(aqua_key=AQUA_KEY, token=TOKEN)
    bot.run(TOKEN)
