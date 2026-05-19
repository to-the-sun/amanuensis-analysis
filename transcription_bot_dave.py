import os
import collections
import time
import threading
import logging
import json
import asyncio
import numpy as np
import wave
import struct
from typing import Optional
from concurrent.futures import ThreadPoolExecutor

import discord
from discord.ext import voice_recv
from faster_whisper import WhisperModel
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from discord.gateway import DiscordVoiceWebSocket
import davey
from davey import MediaType, ProposalsOperationType

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)
logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)

# --- DAVE GATEWAY PATCH ---
# We supplement the gateway hook to ensure DAVE/MLS OpCodes are logged and
# we can monitor the state of the dave_session.
import discord.ext.voice_recv.gateway as gateway

_orig_hook = gateway.hook

async def dave_hook(self, msg):
    op = msg.get('op')
    data = msg.get('d')
    vc = getattr(self._connection, 'voice_client', None)
    if not vc: return await _orig_hook(self, msg)

    # Modern Discord DAVE/MLS OpCodes (21-31)
    if op >= 21:
        logger.info(f"DAVE OP {op} received: {data}")

    # The native discord.py 2.7+ handles MLS OpCodes (25-31) internally
    # in DiscordVoiceWebSocket.received_message and received_binary_message.
    # We let it do its job, but we can intercept or log if needed.

    return await _orig_hook(self, msg)

gateway.hook = dave_hook

# --- DAVE DECRYPTION PATCHES ---
from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader

# Ensure the modern encryption mode is recognized
if 'aead_aes256_gcm_rtpsize' not in PacketDecryptor.supported_modes:
    PacketDecryptor.supported_modes.append('aead_aes256_gcm_rtpsize')

# Patch AudioReader to pass voice_client to the decryptor for access to dave_session
_orig_reader_init = AudioReader.__init__
def patched_reader_init(self, sink, voice_client, **kwargs):
    _orig_reader_init(self, sink, voice_client, **kwargs)
    self.decryptor.voice_client = voice_client
AudioReader.__init__ = patched_reader_init

_orig_decryptor_init = PacketDecryptor.__init__
def _patched_decryptor_init(self, mode, secret_key):
    self._secret_key = bytes(secret_key)
    self._aesgcm = AESGCM(self._secret_key)
    # RollOver Counter (ROC) and sequence tracking for 64-bit frame index reconstruction
    self._dave_roc = collections.defaultdict(int)
    self._dave_last_seq = {}
    return _orig_decryptor_init(self, mode, secret_key)
PacketDecryptor.__init__ = _patched_decryptor_init

def _decrypt_rtp_aead_aes256_gcm_rtpsize(self, packet):
    header = bytes(packet.header)
    packet.adjust_rtpsize()
    # 12-byte nonce (4-byte suffix from packet + 8 zero bytes)
    nonce = bytearray(12)
    nonce[:4] = packet.nonce

    # Layer 1: Outer RTP Decryption (AES-256-GCM)
    try:
        res = self._aesgcm.decrypt(bytes(nonce), bytes(packet.data), header)
    except Exception as e:
        # If RTP decryption fails, return a safe minimal payload
        return b'\x00' * 3

    if packet.extended:
        offset = packet.update_ext_headers(res)
        res = res[offset:]

    # Layer 2: DAVE/E2EE Decryption
    vc = getattr(self, 'voice_client', None)
    if vc:
        # discord.py 2.7+ stores the session in the connection state
        dave = getattr(vc._connection, 'dave_session', None)
        if dave and dave.ready:
            uid = vc._get_id_from_ssrc(packet.ssrc)
            if uid:
                # Update ROC to track sequence number wraps
                seq = packet.sequence
                last_seq = self._dave_last_seq.get(packet.ssrc, seq)
                if seq < last_seq and (last_seq - seq) > 32768:
                    self._dave_roc[packet.ssrc] += 1
                elif seq > last_seq and (seq - last_seq) > 32768:
                    self._dave_roc[packet.ssrc] -= 1
                self._dave_last_seq[packet.ssrc] = seq

                # Full 64-bit frame index
                frame_index = (self._dave_roc[packet.ssrc] << 16) | seq

                try:
                    # Media type 0 is audio. The davey library expects the payload
                    # which includes the truncated sync nonce at the end.
                    res = dave.decrypt(uid, MediaType.audio, res)
                    packet._dave_success = True
                except Exception as e:
                    # If DAVE decryption fails, it's likely noise/encrypted.
                    # We flag it so the sink can ignore it, and return empty to avoid OpusError.
                    packet._dave_success = False
                    return b''
        else:
            packet._dave_success = False # Still E2EE encrypted
            return b'' # Do not pass encrypted noise to Opus decoder
    return res

PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize
PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = lambda self, data: data

# --- BOT ---
try:
    with open('credentials.json', 'r') as f:
        config = json.load(f)
    TOKEN = config['token']
    VOICE_ID = int(config['world_voice'])
    TEXT_ID = int(config['world_text'])
except FileNotFoundError:
    logger.error("credentials.json not found!")
    # Use dummy values if missing for code review purposes, but IRL this would exit
    TOKEN, VOICE_ID, TEXT_ID = "MISSING", 0, 0

MODEL = WhisperModel('small', device='cpu', compute_type='int8')
_executor = ThreadPoolExecutor(max_workers=1)

class WhisperTranscriptionSink(voice_recv.AudioSink):
    def __init__(self, bot):
        super().__init__()
        self.bot = bot
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def cleanup(self):
        self.processing_task.cancel()

    def wants_opus(self): return False
    def write(self, user, data):
        # IMPORTANT: Only capture audio that was successfully DAVE-decrypted.
        # This prevents "hallucinations" caused by processing encrypted noise.
        if data.pcm and getattr(data.packet, '_dave_success', False):
            with self.lock:
                self.user_buffers[user].extend(data.pcm)
                self.last_audio_time[user] = time.time()

    async def _process_buffers(self):
        while True:
            await asyncio.sleep(1.0)
            users_to_process = []
            now = time.time()
            with self.lock:
                for user, buffer in list(self.user_buffers.items()):
                    if not buffer: continue
                    duration = len(buffer)/(48000*4) # 48kHz, 16-bit stereo = 4 bytes per frame
                    if duration > 1.5 and (now - self.last_audio_time[user] > 0.8 or duration > 10.0):
                        users_to_process.append((user, bytes(buffer)))

            for user, audio_bytes in users_to_process:
                # Convert PCM to Mono 16kHz for Whisper
                audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, 2).astype(np.float32)/32768.0
                mono_16k = audio_np.mean(axis=1)[::3]
                rms = np.sqrt(np.mean(mono_16k**2)) * 32768

                if rms > 200: # Simple VAD
                    logger.info(f"Transcribing {len(mono_16k)/16000:.1f}s from {user} (RMS={rms:.0f})...")
                    text = await self.bot.loop.run_in_executor(_executor, self._transcribe, mono_16k)
                    if text and len(text.strip()) > 3:
                        logger.info(f"WHISPER [{user}]: {text.strip()}")
                        channel = self.bot.get_channel(TEXT_ID)
                        if channel: await channel.send(f"**{user}**: {text.strip()}")

                with self.lock: self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]

    def _transcribe(self, audio_16k):
        try:
            segments, info = MODEL.transcribe(audio_16k, beam_size=5, language='en')
            return "".join([s.text for s in segments])
        except Exception:
            return ""

class TranscriptionBot(discord.Client):
    def __init__(self):
        super().__init__(intents=discord.Intents.all())
    async def on_ready(self):
        logger.info(f'Logged in as {self.user}')
        vc_channel = self.get_channel(VOICE_ID)
        if vc_channel:
            client = await vc_channel.connect(cls=voice_recv.VoiceRecvClient)
            client.listen(WhisperTranscriptionSink(self))
            logger.info("Listening with DAVE Decryption active...")
        else:
            logger.error(f"Voice channel {VOICE_ID} not found.")

if __name__ == '__main__':
    if not discord.opus.is_loaded():
        try: discord.opus.load_opus('libopus.so.0')
        except Exception: pass

    if TOKEN != "MISSING":
        TranscriptionBot().run(TOKEN)
    else:
        logger.error("Token missing. Bot not started.")
