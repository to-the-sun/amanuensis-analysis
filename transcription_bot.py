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

# Load credentials
with open('credentials.json', 'r') as f:
    config = json.load(f)

TOKEN = config['token']
VOICE_CHANNEL_ID = 0  # REPLACE WITH YOUR VOICE CHANNEL ID
TEXT_CHANNEL_ID = 0   # REPLACE WITH YOUR TEXT CHANNEL ID

# Whisper Configuration
MODEL_SIZE = "small"
# Use CPU for broader compatibility in this environment, can be changed to 'cuda' if available
try:
    MODEL = WhisperModel(MODEL_SIZE, device="cpu", compute_type="int8")
except Exception:
    MODEL = None

class WhisperTranscriptionSink(voice_recv.AudioSink):
    def __init__(self, bot, text_channel_id):
        super().__init__()
        self._logger = logging.getLogger(__name__)
        self.bot = bot
        self.text_channel_id = text_channel_id
        # Buffers for each user: user_id -> bytearray of PCM data
        self.user_buffers = collections.defaultdict(bytearray)
        # Last time audio was received from a user to detect silence/breaks if needed
        self.last_audio_time = collections.defaultdict(float)
        # Lock for thread-safe buffer access
        self.lock = threading.Lock()

        # Audio parameters
        self.sample_rate = 48000  # Discord's default
        self.channels = 2          # Discord's default (stereo)
        self.target_sample_rate = 16000 # Whisper's requirement

        # We'll use a simple approach: collect audio and process it when it seems like a "sentence"
        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def wants_opus(self) -> bool:
        return False

    def write(self, user: Optional[discord.User], data: voice_recv.VoiceData):
        if user is None:
            return

        # If we didn't get PCM but we got Opus, we might have had a decoder failure
        # in the library. But we are in PCM mode (wants_opus=False), so we expect data.pcm.
        if not data.pcm:
            return

        with self.lock:
            # data.pcm is the raw PCM data (48kHz, stereo, 16-bit)
            self.user_buffers[user].extend(data.pcm)
            self.last_audio_time[user] = time.time()

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
                        # Process if we have significant audio (e.g. > 1 sec)
                        # and enough time has passed since last audio to check for sentence end,
                        # OR if the buffer is getting too long (e.g. 15 seconds)
                        buffer_duration = len(buffer) / (self.sample_rate * self.channels * 2)
                        time_since_last = now - self.last_audio_time[user]

                        if buffer_duration > 1.0 and (time_since_last > 0.5 or buffer_duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    text, is_complete = await self.bot.loop.run_in_executor(None, self._transcribe, audio_bytes)

                    if text.strip():
                        if is_complete or len(audio_bytes) > (self.sample_rate * self.channels * 2 * 15):
                            channel = self.bot.get_channel(self.text_channel_id)
                            if channel:
                                await channel.send(f"**{user.display_name}**: {text.strip()}")

                            # Clear buffer only after posting
                            with self.lock:
                                processed_len = len(audio_bytes)
                                new_buffer = self.user_buffers[user][processed_len:]
                                self.user_buffers[user] = new_buffer
            except Exception as e:
                print(f"Error in processing loop: {e}")

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

            # Check if the last segment ends with a sentence-ending punctuation
            is_complete = False
            if segments:
                last_text = segments[-1].text.strip()
                if last_text and last_text[-1] in ('.', '!', '?'):
                    is_complete = True

            return full_text, is_complete
        except Exception as e:
            return f"Error during transcription: {e}", True

    def cleanup(self):
        self.processing_task.cancel()

class TranscriptionBot(discord.Client):
    def __init__(self):
        intents = discord.Intents.default()
        intents.message_content = True
        intents.voice_states = True
        intents.guilds = True
        super().__init__(intents=intents)

    async def on_ready(self):
        print(f'Logged in as {self.user} (ID: {self.user.id})')
        print('------')

        voice_channel = self.get_channel(VOICE_CHANNEL_ID)
        if voice_channel and isinstance(voice_channel, discord.VoiceChannel):
            print(f"Connecting to {voice_channel.name}...")
            # Use VoiceRecvClient for receiving audio
            vc = await voice_channel.connect(cls=voice_recv.VoiceRecvClient)
            print(f"Connected to voice. Encryption mode: {vc.mode}")

            sink = WhisperTranscriptionSink(self, TEXT_CHANNEL_ID)
            vc.listen(sink)
            print("Listening...")
        else:
            print(f"Could not find voice channel with ID {VOICE_CHANNEL_ID}")

if __name__ == "__main__":
    bot = TranscriptionBot()
    bot.run(TOKEN)
