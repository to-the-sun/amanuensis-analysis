import os
import json
import asyncio
import numpy as np
import collections
import threading
import time
import logging
from typing import Optional

import discord
from discord.ext import voice_recv
from faster_whisper import WhisperModel

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Load credentials
try:
    with open('credentials.json', 'r') as f:
        config = json.load(f)
except FileNotFoundError:
    logger.error("credentials.json not found!")
    config = {}

TOKEN = config.get('token', 'YOUR_DISCORD_BOT_TOKEN')
VOICE_CHANNEL_ID = config.get('world_voice', 0)
TEXT_CHANNEL_ID = config.get('world_text', 0)

# Whisper Configuration
MODEL_SIZE = "small"
# Use CPU for broader compatibility, can be changed to 'cuda' if available
try:
    logger.info(f"Loading Whisper model '{MODEL_SIZE}'...")
    MODEL = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
    logger.info("Whisper model loaded successfully.")
except Exception as e:
    logger.error(f"Failed to load Whisper model: {e}")
    MODEL = None

class WhisperTranscriptionSink(voice_recv.AudioSink):
    def __init__(self, bot, text_channel_id):
        super().__init__()
        self.bot = bot
        self.text_channel_id = text_channel_id
        # Buffers for each user: user -> bytearray of PCM data
        self.user_buffers = collections.defaultdict(bytearray)
        # Last time audio was received from a user
        self.last_audio_time = collections.defaultdict(float)
        # Custom decoders per user to bypass library's fragile internal decoding
        self.decoders = {}
        # Lock for thread-safe buffer access
        self.lock = threading.Lock()

        # Audio parameters
        self.sample_rate = 48000  # Discord's default
        self.channels = 2          # Discord's default (stereo)
        self.target_sample_rate = 16000 # Whisper's requirement

        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def wants_opus(self) -> bool:
        # We request Opus data so we can decode it ourselves and handle errors gracefully
        return True

    def write(self, user: Optional[discord.User], data: voice_recv.VoiceData):
        if user is None or not data.opus:
            return

        # Ensure we have a decoder for this user
        if user.id not in self.decoders:
            try:
                self.decoders[user.id] = discord.opus.Decoder()
            except Exception as e:
                logger.error(f"Could not create Opus decoder for {user.display_name}: {e}")
                return

        try:
            # Decode the Opus packet to PCM
            pcm = self.decoders[user.id].decode(data.opus)

            with self.lock:
                self.user_buffers[user].extend(pcm)
                self.last_audio_time[user] = time.time()
        except discord.opus.OpusError as e:
            # Catch the "corrupted stream" error here so it doesn't crash the router
            pass
        except Exception as e:
            logger.debug(f"Error decoding packet from {user.display_name}: {e}")

    async def _process_buffers(self):
        while True:
            try:
                await asyncio.sleep(1.0) # Check every second

                users_to_process = []
                now = time.time()
                with self.lock:
                    for user, buffer in list(self.user_buffers.items()):
                        if not buffer:
                            continue

                        buffer_duration = len(buffer) / (self.sample_rate * self.channels * 2)
                        time_since_last = now - self.last_audio_time[user]

                        # Process if we have > 1 sec of audio and user paused, or if buffer is long
                        if buffer_duration > 1.0 and (time_since_last > 0.6 or buffer_duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    text, is_complete = await self.bot.loop.run_in_executor(None, self._transcribe, audio_bytes)

                    if text and text.strip():
                        # We post if it's a complete sentence OR if we've reached a timeout (15s)
                        if is_complete or len(audio_bytes) > (self.sample_rate * self.channels * 2 * 14):
                            channel = self.bot.get_channel(self.text_channel_id)
                            if channel:
                                await channel.send(f"**{user.display_name}**: {text.strip()}")

                            # Clear processed audio from buffer
                            with self.lock:
                                processed_len = len(audio_bytes)
                                self.user_buffers[user] = self.user_buffers[user][processed_len:]
            except Exception as e:
                logger.error(f"Error in processing loop: {e}")

    def _transcribe(self, audio_bytes):
        if MODEL is None:
             return "Model not loaded", True

        try:
            # Convert bytes to numpy array (16-bit PCM)
            audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, self.channels)
            # Convert to float32 and mono
            audio_float32 = audio_np.astype(np.float32) / 32768.0
            audio_mono = audio_float32.mean(axis=1)

            # Resample to 16kHz (decimation 3:1)
            audio_16k = audio_mono[::3]

            segments, info = MODEL.transcribe(audio_16k, beam_size=5, language="en")

            segments = list(segments)
            full_text = "".join([s.text for s in segments])

            is_complete = False
            if segments:
                last_text = segments[-1].text.strip()
                if last_text and last_text[-1] in ('.', '!', '?'):
                    is_complete = True

            return full_text, is_complete
        except Exception as e:
            logger.error(f"Transcription error: {e}")
            return "", False

    def cleanup(self):
        self.processing_task.cancel()
        self.decoders.clear()

class TranscriptionBot(discord.Client):
    def __init__(self):
        intents = discord.Intents.default()
        intents.message_content = True
        intents.voice_states = True
        intents.guilds = True
        super().__init__(intents=intents)

    async def on_ready(self):
        logger.info(f'Logged in as {self.user} (ID: {self.user.id})')

        voice_channel = self.get_channel(VOICE_CHANNEL_ID)
        if voice_channel and isinstance(voice_channel, discord.VoiceChannel):
            logger.info(f"Connecting to {voice_channel.name}...")
            try:
                # Use VoiceRecvClient for receiving audio
                vc = await voice_channel.connect(cls=voice_recv.VoiceRecvClient)
                logger.info(f"Connected to voice. Encryption mode: {vc.mode}")

                sink = WhisperTranscriptionSink(self, TEXT_CHANNEL_ID)
                vc.listen(sink)
                logger.info("Listening for audio...")
            except Exception as e:
                logger.error(f"Failed to connect to voice channel: {e}")
        else:
            logger.error(f"Could not find voice channel with ID {VOICE_CHANNEL_ID}")

if __name__ == "__main__":
    # Ensure opus is loaded
    if not discord.opus.is_loaded():
        # On Windows, discord.py usually finds it if it's in the same folder or system PATH
        # On Linux, it's usually libopus.so.0
        try:
            discord.opus.load_opus('libopus-0.dll' if os.name == 'nt' else 'libopus.so.0')
        except Exception:
            pass

    bot = TranscriptionBot()
    bot.run(TOKEN)
