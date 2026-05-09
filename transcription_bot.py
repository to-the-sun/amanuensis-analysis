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
from discord.ext import voice_recv
from faster_whisper import WhisperModel

# Set up logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

# Suppress the RTCP packet spam and jitter buffer warnings
logging.getLogger('discord.ext.voice_recv.reader').setLevel(logging.WARNING)
logging.getLogger('discord.ext.voice_recv.opus').setLevel(logging.ERROR)

# --- DEEP DIAGNOSTIC PATCHES ---

# 1. Block the problematic AES mode definitively
stable_modes = [
    'xsalsa20_poly1305_lite',
    'xsalsa20_poly1305_suffix',
    'xsalsa20_poly1305',
]
aead_mode = 'aead_xchacha20_poly1305_rtpsize'

voice_recv.VoiceRecvClient.supported_modes = tuple(stable_modes + [aead_mode])
from discord.ext.voice_recv.reader import PacketDecryptor
PacketDecryptor.supported_modes = stable_modes + [aead_mode]

# 2. Advanced Decoder Patch with Hex Logging
_original_decode = discord.opus.Decoder.decode
_err_logged = 0

def _deep_diagnostic_decode(self, data, fec=False):
    global _err_logged
    if not data:
        return b'\x00' * 3840

    try:
        return _original_decode(self, data, fec)
    except discord.opus.OpusError:
        if _err_logged < 10:
            hex_data = data.hex()[:32]
            logger.warning(f"DECODE FAIL! First 16 bytes (hex): {hex_data} | Length: {len(data)}")
            _err_logged += 1
        return b'\x00' * 3840
    except Exception:
        return b'\x00' * 3840

discord.opus.Decoder.decode = _deep_diagnostic_decode

# 3. Connection Hook
from discord.gateway import DiscordVoiceWebSocket
_original_initial_connection = DiscordVoiceWebSocket.initial_connection

async def _patched_initial_connection(self, data):
    modes = data.get('modes', [])
    logger.info(f"Discord offered encryption modes: {modes}")
    # Force AEAD XChaCha over AES if both present
    preferred = [m for m in stable_modes + [aead_mode] if m in modes]
    if not preferred:
        data['modes'] = modes[:1]
    else:
        data['modes'] = preferred
    return await _original_initial_connection(self, data)

DiscordVoiceWebSocket.initial_connection = _patched_initial_connection
logger.info("Deep diagnostic mode activated.")
# -------------------------------

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
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.sample_rate = 48000
        self.channels = 2
        self.debug_saved = False
        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def wants_opus(self) -> bool:
        return False

    def write(self, user: Optional[discord.User], data: voice_recv.VoiceData):
        if user is None or not data.pcm:
            return

        # RTP Extension Check for DAVE (E2EE)
        if hasattr(data.packet, 'extension_data') and data.packet.extension_data:
             # DAVE often uses extension IDs like 11 or 12
             if any(eid > 10 for eid in data.packet.extension_data.keys()):
                 if not hasattr(self, '_dave_warned'):
                     logger.warning(f"Detected unusual RTP extensions: {list(data.packet.extension_data.keys())}. E2EE (DAVE) might be active!")
                     self._dave_warned = True

        with self.lock:
            self.user_buffers[user].extend(data.pcm)
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
                        buffer_duration = len(buffer) / (self.sample_rate * self.channels * 2)
                        time_since_last = now - self.last_audio_time[user]
                        if buffer_duration > 1.0 and (time_since_last > 0.6 or buffer_duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    if not self.debug_saved and len(audio_bytes) > (48000 * 4 * 1):
                        self._save_debug_wav(audio_bytes)
                        self.debug_saved = True

                    audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                    rms = np.sqrt(np.mean(audio_np.astype(np.float64)**2))

                    if rms < 400:
                        with self.lock:
                             self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
                        continue

                    logger.info(f"Transcribing {len(audio_bytes)/(48000*4):.1f}s from {user.display_name} (RMS: {rms:.1f})...")
                    text, is_complete = await self.bot.loop.run_in_executor(None, self._transcribe, audio_bytes)

                    if text:
                        logger.info(f"Whisper output: \"{text.strip()}\"")
                        if is_complete or len(audio_bytes) > (self.sample_rate * self.channels * 2 * 14):
                            channel = self.bot.get_channel(self.text_channel_id)
                            if channel:
                                await channel.send(f"**{user.display_name}**: {text.strip()}")
                            with self.lock:
                                self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
                    else:
                        with self.lock:
                            self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
            except Exception as e:
                logger.error(f"Error in processing loop: {e}")

    def _save_debug_wav(self, audio_bytes):
        try:
            with wave.open('debug_audio.wav', 'wb') as wf:
                wf.setnchannels(self.channels)
                wf.setsampwidth(2)
                wf.setframerate(self.sample_rate)
                wf.writeframes(audio_bytes)
            logger.info("Saved debug sample to 'debug_audio.wav'.")
        except Exception as e:
            logger.error(f"Failed to save debug wav: {e}")

    def _transcribe(self, audio_bytes):
        if MODEL is None: return "Model not loaded", True
        try:
            audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, self.channels)
            audio_float32 = audio_np.astype(np.float32) / 32768.0
            audio_mono = audio_float32.mean(axis=1)
            audio_16k = audio_mono[::3]
            segments, info = MODEL.transcribe(audio_16k, beam_size=5, language="en", initial_prompt="Hello.")
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
                vc = await voice_channel.connect(cls=voice_recv.VoiceRecvClient)
                logger.info(f"Connected to voice. Encryption mode: {vc.mode}")
                sink = WhisperTranscriptionSink(self, TEXT_CHANNEL_ID)
                vc.listen(sink)
                logger.info("Listening for audio...")
            except Exception as e:
                logger.error(f"Failed to connect to voice channel: {e}")
                traceback.print_exc()
        else:
            logger.error(f"Could not find voice channel with ID {VOICE_CHANNEL_ID}")

if __name__ == "__main__":
    if not discord.opus.is_loaded():
        try:
            discord.opus.load_opus('libopus-0.dll' if os.name == 'nt' else 'libopus.so.0')
        except Exception:
            pass
    bot = TranscriptionBot()
    bot.run(TOKEN)
