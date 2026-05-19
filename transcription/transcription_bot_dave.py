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

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)
logging.getLogger('discord.ext.voice_recv').setLevel(logging.WARNING)

# --- DAVE INTEGRATION PATCHES ---
from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader

# Patch AudioReader to pass voice_client to the decryptor
_orig_reader_init = AudioReader.__init__
def patched_reader_init(self, sink, voice_client, **kwargs):
    _orig_reader_init(self, sink, voice_client, **kwargs)
    self.decryptor.voice_client = voice_client
AudioReader.__init__ = patched_reader_init

_orig_decryptor_init = PacketDecryptor.__init__
def _patched_decryptor_init(self, mode, secret_key):
    self._secret_key = bytes(secret_key)
    self._aesgcm = AESGCM(self._secret_key)
    return _orig_decryptor_init(self, mode, secret_key)
PacketDecryptor.__init__ = _patched_decryptor_init

def _decrypt_rtp_aead_aes256_gcm_rtpsize(self, packet):
    header = bytes(packet.header)
    packet.adjust_rtpsize()
    nonce = bytearray(12)
    nonce[:4] = packet.nonce
    
    # Layer 1: Outer RTP Decryption
    try:
        res = self._aesgcm.decrypt(bytes(nonce), bytes(packet.data), header)
    except Exception as e:
        return b'\x00' * 3
        
    if packet.extended:
        offset = packet.update_ext_headers(res)
        res = res[offset:]

    # Layer 2: DAVE Decryption
    vc = getattr(self, 'voice_client', None)
    if vc:
        dave = getattr(vc._connection, 'dave_session', None)
        if dave and dave.ready:
            uid = vc._get_id_from_ssrc(packet.ssrc)
            if uid:
                try:
                    # media_type 0 = audio
                    res = dave.decrypt(uid, 0, res)
                    packet._dave_success = True
                except Exception as e:
                    packet._dave_success = False
        else:
            packet._dave_success = False # Still encrypted
    return res

PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize
PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = lambda self, data: data

# --- BOT ---
with open('credentials.json', 'r') as f:
    config = json.load(f)
TOKEN, VOICE_ID, TEXT_ID = config['token'], int(config['world_voice']), int(config['world_text'])

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
        # IMPORTANT: Only capture audio that was successfully DAVE-decrypted
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
                    duration = len(buffer)/(48000*4)
                    if duration > 1.5 and (now - self.last_audio_time[user] > 0.8 or duration > 10.0):
                        users_to_process.append((user, bytes(buffer)))
            
            for user, audio_bytes in users_to_process:
                audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, 2).astype(np.float32)/32768.0
                mono_16k = audio_np.mean(axis=1)[::3]
                rms = np.sqrt(np.mean(mono_16k**2)) * 32768
                
                if rms > 200:
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
        except: return ""

class TranscriptionBot(discord.Client):
    def __init__(self):
        super().__init__(intents=discord.Intents.all())
    async def on_ready(self):
        logger.info(f'Logged in as {self.user}')
        vc = self.get_channel(VOICE_ID)
        client = await vc.connect(cls=voice_recv.VoiceRecvClient)
        client.listen(WhisperTranscriptionSink(self))
        logger.info("Listening (v39 DAVE Enforcement)...")

if __name__ == '__main__':
    if not discord.opus.is_loaded():
        try: discord.opus.load_opus('libopus.so.0')
        except: pass
    TranscriptionBot().run(TOKEN)
