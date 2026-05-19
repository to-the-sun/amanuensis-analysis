import os
import collections
import time
import threading
import logging
import json
import asyncio
import numpy as np
from typing import Optional
from concurrent.futures import ThreadPoolExecutor

import discord
from discord.ext import voice_recv
from faster_whisper import WhisperModel
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from discord.gateway import DiscordVoiceWebSocket
import davey
from davey import MediaType

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)
logging.getLogger('discord').setLevel(logging.WARNING)
logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)

# --- DAVE DECRYPTION PATCHES ---
from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader
import discord.ext.voice_recv.opus as opus_module

# Ensure modern encryption mode is recognized
if 'aead_aes256_gcm_rtpsize' not in PacketDecryptor.supported_modes:
    PacketDecryptor.supported_modes.append('aead_aes256_gcm_rtpsize')

# Patch AudioReader to pass voice_client to the decryptor
_orig_reader_init = AudioReader.__init__
def patched_reader_init(self, sink, voice_client, **kwargs):
    _orig_reader_init(self, sink, voice_client, **kwargs)
    self.decryptor.voice_client = voice_client
AudioReader.__init__ = patched_reader_init

# Patch PacketDecryptor
_orig_decryptor_init = PacketDecryptor.__init__
def _patched_decryptor_init(self, mode, secret_key):
    self._secret_key = bytes(secret_key)
    self._aesgcm = AESGCM(self._secret_key)
    self._dave_roc = collections.defaultdict(int)
    self._dave_last_seq = {}
    return _orig_decryptor_init(self, mode, secret_key)
PacketDecryptor.__init__ = _patched_decryptor_init

def _decrypt_rtp_aead_aes256_gcm_rtpsize(self, packet):
    header = bytes(packet.header[:12])
    packet.adjust_rtpsize()
    nonce = bytearray(12)
    nonce[:4] = packet.nonce

    # Layer 1: Outer RTP Decryption
    try:
        res = self._aesgcm.decrypt(bytes(nonce), bytes(packet.data), header)
    except Exception:
        return None

    if packet.extended:
        offset = packet.update_ext_headers(res)
        res = res[offset:]

    # Layer 2: DAVE/E2EE Decryption
    vc = getattr(self, 'voice_client', None)
    if vc:
        state = getattr(vc, '_connection', None)
        dave = getattr(state, 'dave_session', None)
        
        if dave and dave.ready:
            uid = vc._get_id_from_ssrc(packet.ssrc)
            if uid:
                # Track sequence for ROC (diagnostic)
                seq = packet.sequence
                last_seq = self._dave_last_seq.get(packet.ssrc, seq)
                if seq < last_seq and (last_seq - seq) > 32768:
                    self._dave_roc[packet.ssrc] += 1
                elif seq > last_seq and (seq - last_seq) > 32768:
                    self._dave_roc[packet.ssrc] -= 1
                self._dave_last_seq[packet.ssrc] = seq
                
                try:
                    # Media type 0 = audio
                    dec = dave.decrypt(uid, MediaType.audio, res)
                    packet._dave_success = True
                    return dec
                except Exception:
                    packet._dave_success = False
                    # Return original outer-decrypted (but E2EE encrypted) frame 
                    # to allow Whisper to "see" the noise/hallucinate.
                    return res
        
        # If DAVE not ready, return the outer-decrypted frame
        packet._dave_success = False
        return res
            
    return res

PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize
PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = lambda self, data: data

# Patch PacketDecoder to handle None (RTP failure) but pass encrypted noise (DAVE failure)
_orig_decode_packet = opus_module.PacketDecoder._decode_packet
def _patched_decode_packet(self, packet):
    if packet and packet.decrypted_data is None:
        return packet, b''
    
    try:
        return _orig_decode_packet(self, packet)
    except Exception:
        # If Opus decoding fails on encrypted noise, return empty bytes
        # instead of crashing the router.
        return packet, b''

opus_module.PacketDecoder._decode_packet = _patched_decode_packet

# --- TRANSCRIPTION SINK ---
class WhisperTranscriptionSink(voice_recv.AudioSink):
    def __init__(self, bot, text_id):
        super().__init__()
        self.bot = bot
        self.text_id = text_id
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.processing_task = self.bot.loop.create_task(self._process_buffers())

    def cleanup(self):
        self.processing_task.cancel()

    def wants_opus(self): return False

    def write(self, user, data):
        # HALLUCINATION FILTER REMOVED: Process all PCM produced by the decoder
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
                        duration = len(buffer)/(48000*4)
                        time_since = now - self.last_audio_time[user]
                        if duration > 1.5 and (time_since > 0.8 or duration > 15.0):
                            users_to_process.append((user, bytes(buffer)))

                for user, audio_bytes in users_to_process:
                    audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                    rms = np.sqrt(np.mean(audio_np.astype(np.float64)**2))
                    
                    if rms > 150:
                        audio_float32 = audio_np.reshape(-1, 2).astype(np.float32) / 32768.0
                        mono_16k = audio_float32.mean(axis=1)[::3]
                        
                        logger.info(f"Transcribing {len(mono_16k)/16000:.1f}s from {user}...")
                        text = await self.bot.loop.run_in_executor(_executor, self._transcribe, mono_16k)
                        
                        if text and len(text.strip()) > 1:
                            clean_text = text.strip()
                            logger.info(f"WHISPER RESULT [{user}]: {clean_text}")
                            channel = self.bot.get_channel(self.text_id)
                            if channel:
                                await channel.send(f"**{user}**: {clean_text}")

                    with self.lock:
                        self.user_buffers[user] = self.user_buffers[user][len(audio_bytes):]
            except Exception as e:
                logger.error(f"Error in processing loop: {e}")

    def _transcribe(self, audio_16k):
        if not MODEL: return ""
        try:
            segments, info = MODEL.transcribe(audio_16k, beam_size=1, language='en')
            return "".join([s.text for s in segments])
        except Exception:
            return ""

# --- BOT CLIENT ---
class TranscriptionBot(discord.Client):
    def __init__(self, voice_id, text_id):
        self.voice_id = voice_id
        self.text_id = text_id
        super().__init__(intents=discord.Intents.all())

    async def on_ready(self):
        logger.info(f'Logged in as {self.user} (ID: {self.user.id})')
        vc_channel = self.get_channel(self.voice_id)
        if vc_channel:
            try:
                client = await vc_channel.connect(cls=voice_recv.VoiceRecvClient)
                client.listen(WhisperTranscriptionSink(self, self.text_id))
                logger.info(f"Connected to {vc_channel.name}. Listening...")
            except Exception as e:
                logger.error(f"Failed to connect to voice: {e}")
        else:
            logger.error(f"Voice channel {self.voice_id} not found.")

# --- MAIN ---
if __name__ == '__main__':
    with open('credentials.json', 'r') as f:
        config = json.load(f)
    TOKEN = config['token']
    VOICE_ID = int(config['world_voice'])
    TEXT_ID = int(config['world_text'])

    if not discord.opus.is_loaded():
        try: discord.opus.load_opus('libopus.so.0')
        except: pass

    logger.info("Loading Whisper model...")
    MODEL = WhisperModel('small', device='cpu', compute_type='int8')
    _executor = ThreadPoolExecutor(max_workers=1)

    bot = TranscriptionBot(VOICE_ID, TEXT_ID)
    bot.run(TOKEN)
