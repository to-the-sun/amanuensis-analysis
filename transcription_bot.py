import os
import json
import asyncio
import numpy as np
import collections
import threading
import time
import logging
import wave
import traceback
from typing import Optional

import discord
import soundcard as sc
from faster_whisper import WhisperModel

# --- LOGGING CONFIGURATION ---
# Reduce noise by setting levels to INFO
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)

# Keep some libraries quiet
logging.getLogger('discord').setLevel(logging.WARNING)
logging.getLogger('faster_whisper').setLevel(logging.WARNING)

# Load credentials
try:
    with open('credentials.json', 'r') as f:
        config = json.load(f)
except FileNotFoundError:
    logger.error("credentials.json not found!")
    config = {}

TOKEN = config.get('token', 'YOUR_DISCORD_BOT_TOKEN')
TEXT_CHANNEL_ID = config.get('world_text', 0)

# Whisper Configuration
MODEL_SIZE = "small"
try:
    logger.info(f"Loading Whisper model '{MODEL_SIZE}'...")
    MODEL = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
except Exception as e:
    logger.error(f"Failed to load Whisper model: {e}")
    MODEL = None

class LocalUser:
    def __init__(self, display_name):
        self.display_name = display_name
    def __hash__(self):
        return hash(self.display_name)
    def __eq__(self, other):
        if isinstance(other, LocalUser):
            return self.display_name == other.display_name
        return False

class WhisperTranscriptionSink:
    def __init__(self, bot, text_channel_id):
        self.bot = bot
        self.text_channel_id = text_channel_id
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.sample_rate = 48000
        self.channels = 2
        self.debug_saved = False
        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def write_numpy(self, user, data_np: np.ndarray):
        """
        Accepts float32 numpy data (from soundcard), converts to 16-bit PCM.
        Handles mono or stereo input.
        """
        # Ensure we have a 2D array (frames, channels)
        if data_np.ndim == 1:
            # Mono to Stereo
            data_np = np.column_stack((data_np, data_np))
        elif data_np.shape[1] == 1:
            # Mono to Stereo
            data_np = np.concatenate((data_np, data_np), axis=1)

        # Soundcard usually returns float32 in range [-1, 1]
        pcm_data = (data_np * 32767).astype(np.int16).tobytes()
        with self.lock:
            self.user_buffers[user].extend(pcm_data)
            self.last_audio_time[user] = time.time()

    async def _process_buffers(self):
        while True:
            try:
                await asyncio.sleep(1.0)
                users_to_process = []
                now = time.time()
                with self.lock:
                    for user, buffer in list(self.user_buffers.items()):
                        if not buffer: continue

                        duration = len(buffer) / (48000 * 4)
                        time_since = now - self.last_audio_time[user]

                        if duration > 1.0 and (time_since > 0.6 or duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                    rms = np.sqrt(np.mean(audio_np.astype(np.float64)**2))

                    if rms < 100: # Threshold for silence
                        with self.lock:
                             self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
                        continue

                    logger.info(f"Transcribing {len(audio_bytes)/(48000*4):.1f}s from {user.display_name}...")
                    text, is_complete = await self.bot.loop.run_in_executor(None, self._transcribe, audio_bytes)

                    if text:
                        logger.info(f"WHISPER RESULT [{user.display_name}]: \"{text.strip()}\"")
                        channel = self.bot.get_channel(self.text_channel_id)
                        if channel:
                            await channel.send(f"**{user.display_name}**: {text.strip()}")

                    with self.lock:
                        processed_len = len(audio_bytes)
                        self.user_buffers[user] = self.user_buffers[user][processed_len:]
            except Exception as e:
                logger.error(f"Processing loop error: {e}")
                traceback.print_exc()

    def _transcribe(self, audio_bytes):
        if MODEL is None: return "Model not loaded", True
        try:
            audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, self.channels)
            audio_float32 = audio_np.astype(np.float32) / 32768.0
            audio_mono = audio_float32.mean(axis=1)
            audio_16k = audio_mono[::3]
            segments, info = MODEL.transcribe(audio_16k, beam_size=5, language="en")
            segments = list(segments)
            full_text = "".join([s.text for s in segments])
            is_complete = any(s.text.strip().endswith(('.', '!', '?')) for s in segments) if segments else False
            return full_text, is_complete
        except Exception as e:
            logger.error(f"Transcription error: {e}")
            return "", False

    def cleanup(self):
        self.processing_task.cancel()

async def capture_loop(mic, user, sink, sample_rate=48000, chunk_duration=1.0):
    """
    Continuous capture loop for a given microphone/loopback device.
    """
    logger.info(f"Starting capture loop for {user.display_name} on {mic.name}")
    num_frames = int(sample_rate * chunk_duration)

    try:
        with mic.recorder(samplerate=sample_rate) as recorder:
            while True:
                # record is a blocking call, so we run it in an executor to keep the event loop free
                data = await asyncio.get_event_loop().run_in_executor(None, recorder.record, num_frames)
                sink.write_numpy(user, data)
    except Exception as e:
        logger.error(f"Capture loop error for {user.display_name}: {e}")
        traceback.print_exc()

class TranscriptionBot(discord.Client):
    def __init__(self):
        intents = discord.Intents.default()
        intents.message_content = True
        intents.guilds = True
        super().__init__(intents=intents)

    async def on_ready(self):
        logger.info(f'Logged in as {self.user} (ID: {self.user.id})')

        # Initialize the sink
        self.sink = WhisperTranscriptionSink(self, TEXT_CHANNEL_ID)

        # 1. Setup "To the Sun" (Microphone)
        try:
            mic = sc.default_microphone()
            user_tts = LocalUser("To the Sun")
            print(f"Microphone input detected: {mic.name}")
            asyncio.create_task(capture_loop(mic, user_tts, self.sink))
            logger.info(f"Started microphone capture for 'To the Sun' using {mic.name}")
        except Exception as e:
            logger.error(f"Failed to setup microphone capture: {e}")

        # 2. Setup "mind_over_moss" (Desktop Audio)
        try:
            default_speaker = sc.default_speaker()
            loopback = sc.get_microphone(id=str(default_speaker.name), include_loopback=True)
            user_mom = LocalUser("mind_over_moss")
            print(f"Desktop audio output detected: {loopback.name}")
            asyncio.create_task(capture_loop(loopback, user_mom, self.sink))
            logger.info(f"Started desktop capture for 'mind_over_moss' using {loopback.name}")
        except Exception as e:
            logger.error(f"Failed to setup desktop loopback capture: {e}")

if __name__ == "__main__":
    bot = TranscriptionBot()
    bot.run(TOKEN)
