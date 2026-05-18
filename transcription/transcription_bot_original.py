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

# --- MAXIMUM LOGGING ---
logging.basicConfig(level=logging.DEBUG, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)

# Keep some libraries at INFO to avoid absolute flooding, but keep voice-recv at DEBUG
logging.getLogger('discord').setLevel(logging.INFO)
logging.getLogger('discord.ext.voice_recv').setLevel(logging.DEBUG)
logging.getLogger('faster_whisper').setLevel(logging.INFO)

# --- TOTAL TRANSPARENCY PATCHES ---

# 1. Broaden supported modes to see everything
all_known_modes = [
    'xsalsa20_poly1305_lite',
    'xsalsa20_poly1305_suffix',
    'xsalsa20_poly1305',
    'aead_xchacha20_poly1305_rtpsize',
    'aead_aes256_gcm_rtpsize'
]
voice_recv.VoiceRecvClient.supported_modes = tuple(all_known_modes)
from discord.ext.voice_recv.reader import PacketDecryptor
PacketDecryptor.supported_modes = all_known_modes

# 2. Log full websocket handshake
from discord.gateway import DiscordVoiceWebSocket
_original_initial_connection = DiscordVoiceWebSocket.initial_connection

async def _patched_initial_connection(self, data):
    # Log the ENTIRE payload from Discord
    logger.info(f"VOICE HANDSHAKE PAYLOAD: {json.dumps(data, indent=2)}")
    return await _original_initial_connection(self, data)

DiscordVoiceWebSocket.initial_connection = _patched_initial_connection

# 3. Patch the Decoder to log successes too
_original_decode = discord.opus.Decoder.decode
_decode_success = 0

def _transparent_decode(self, data, fec=False):
    global _decode_success
    try:
        res = _original_decode(self, data, fec)
        _decode_success += 1
        if _decode_success % 100 == 0:
            logger.debug(f"Decoded 100 packets successfully (total: {_decode_success})")
        return res
    except discord.opus.OpusError:
        logger.warning(f"OpusError on packet of length {len(data)}")
        return b'\x00' * 3840
    except Exception as e:
        logger.error(f"Generic decode error: {e}")
        return b'\x00' * 3840

discord.opus.Decoder.decode = _transparent_decode
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
        # LOG EVERY PACKET ARRIVAL
        user_name = user.display_name if user else f"UnknownSSRC:{data.packet.ssrc}"
        # logger.debug(f"PACKET IN: User={user_name}, SSRC={data.packet.ssrc}, PCMLen={len(data.pcm)}")

        # If user is None, it means the SSRC hasn't been mapped to a User ID yet.
        # This is very common during the first few seconds of speech.
        if user is None:
             # Create a placeholder user object to at least capture the audio
             user = collections.namedtuple('PlaceholderUser', ['id', 'display_name'])(-1, f"Guest-{data.packet.ssrc}")

        if data.pcm:
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

                        duration = len(buffer) / (48000 * 4)
                        time_since = now - self.last_audio_time[user]

                        if duration > 0.5:
                            logger.debug(f"Buffer Check: {user.display_name} has {duration:.2f}s audio, last seen {time_since:.2f}s ago")

                        if duration > 1.0 and (time_since > 0.6 or duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    if not self.debug_saved and len(audio_bytes) > (48000 * 4 * 1):
                        self._save_debug_wav(audio_bytes)
                        self.debug_saved = True

                    audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                    rms = np.sqrt(np.mean(audio_np.astype(np.float64)**2))

                    logger.info(f"Volume check for {user.display_name}: RMS={rms:.2f}")

                    if rms < 100: # Super low threshold for total transparency
                        logger.debug(f"Skipping silent buffer (RMS={rms:.2f})")
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

                    # Always clear buffer after processing in total transparency mode
                    with self.lock:
                        processed_len = len(audio_bytes)
                        self.user_buffers[user] = self.user_buffers[user][processed_len:]
            except Exception as e:
                logger.error(f"Processing loop error: {e}")
                traceback.print_exc()

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
                logger.info(f"Connected. Mode: {vc.mode}")
                sink = WhisperTranscriptionSink(self, TEXT_CHANNEL_ID)
                vc.listen(sink)
            except Exception as e:
                logger.error(f"Connect error: {e}")
                traceback.print_exc()

if __name__ == "__main__":
    if not discord.opus.is_loaded():
        try:
            discord.opus.load_opus('libopus-0.dll' if os.name == 'nt' else 'libopus.so.0')
        except Exception: pass
    bot = TranscriptionBot()
    bot.run(TOKEN)
