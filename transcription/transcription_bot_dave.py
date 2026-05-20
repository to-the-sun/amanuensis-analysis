import os
import collections
import time
import threading
import logging
import json
import asyncio
import numpy as np
import random
import re
import io
from typing import Optional
from concurrent.futures import ThreadPoolExecutor

import discord
from discord.ext import voice_recv
from faster_whisper import WhisperModel
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from discord.gateway import DiscordVoiceWebSocket
import davey
from davey import MediaType
from google import genai
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger(__name__)
logging.getLogger('discord').setLevel(logging.WARNING)
logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)
logging.getLogger('faster_whisper').setLevel(logging.WARNING)

# --- DAVE DECRYPTION PATCHES ---
from discord.ext.voice_recv.reader import PacketDecryptor, AudioReader
import discord.ext.voice_recv.opus as opus_module

# 1. Broaden supported modes
if 'aead_aes256_gcm_rtpsize' not in voice_recv.VoiceRecvClient.supported_modes:
    voice_recv.VoiceRecvClient.supported_modes += ('aead_aes256_gcm_rtpsize',)

if 'aead_aes256_gcm_rtpsize' not in PacketDecryptor.supported_modes:
    PacketDecryptor.supported_modes.append('aead_aes256_gcm_rtpsize')

# 2. Patch AudioReader to pass voice_client to the decryptor
_orig_reader_init = AudioReader.__init__
def patched_reader_init(self, sink, voice_client, **kwargs):
    _orig_reader_init(self, sink, voice_client, **kwargs)
    self.decryptor.voice_client = voice_client
AudioReader.__init__ = patched_reader_init

# 3. Patch PacketDecryptor
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
    except Exception as e:
        if packet.sequence % 100 == 0:
            logger.error(f"Outer Decryption Failed: SSRC={packet.ssrc}, Seq={packet.sequence}, Err={e}")
        return None

    if packet.extended:
        offset = packet.update_ext_headers(res)
        res = res[offset:]

    # Layer 2: DAVE/E2EE Decryption
    vc = getattr(self, 'voice_client', None)
    if not vc:
        return res

    state = getattr(vc, '_connection', None)
    dave = getattr(state, 'dave_session', None)

    # Track sequence for ROC
    seq = packet.sequence
    ssrc = packet.ssrc
    last_seq = self._dave_last_seq.get(ssrc, seq)
    if seq < last_seq and (last_seq - seq) > 32768:
        self._dave_roc[ssrc] += 1
    elif seq > last_seq and (seq - last_seq) > 32768:
        self._dave_roc[ssrc] -= 1
    self._dave_last_seq[ssrc] = seq
    roc = self._dave_roc[ssrc]

    if dave and dave.ready:
        uid = vc._get_id_from_ssrc(ssrc)
        if uid:
            try:
                # Media type 0 = audio
                dec = dave.decrypt(uid, MediaType.audio, res)
                if seq % 500 == 0:
                    logger.info(f"DAVE OK: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}")
                return dec
            except Exception as e:
                if seq % 100 == 0:
                    logger.warning(f"DAVE FAIL: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}, Err={e}")
                return None # Return None to trigger PLC/silence and avoid hallucinations

    return res

PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize
PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = lambda self, data: data

# Patch PacketDecoder to handle None (RTP failure)
_orig_decode_packet = opus_module.PacketDecoder._decode_packet
def _patched_decode_packet(self, packet):
    if packet and packet.decrypted_data is None:
        return packet, b''
    
    try:
        return _orig_decode_packet(self, packet)
    except Exception:
        return packet, b''

opus_module.PacketDecoder._decode_packet = _patched_decode_packet

# --- SPEAKER STATS ---
class SpeakerStats:
    def __init__(self):
        self.word_lengths = []
        self.sentence_word_counts = []
        self.current_sentence_words = 0

    def update(self, text):
        clean_text = re.sub(r'[^\w\s\.\!\?]', '', text)
        parts = re.split(r'([\.\!\?])', clean_text)

        for part in parts:
            if part in ('.', '!', '?'):
                if self.current_sentence_words > 0:
                    self.sentence_word_counts.append(self.current_sentence_words)
                    self.current_sentence_words = 0
            else:
                words = part.split()
                for word in words:
                    self.word_lengths.append(len(word))
                    self.current_sentence_words += 1

    def get_metrics(self):
        word_count = len(self.word_lengths)
        avg_word_len = sum(self.word_lengths) / word_count if word_count > 0 else 0
        min_word_len = min(self.word_lengths) if word_count > 0 else 0
        max_word_len = max(self.word_lengths) if word_count > 0 else 0

        s_counts = self.sentence_word_counts
        avg_words_per_sentence = sum(s_counts) / len(s_counts) if s_counts else 0
        min_words_per_sentence = min(s_counts) if s_counts else 0
        max_words_per_sentence = max(s_counts) if s_counts else 0

        return {
            "word_count": word_count,
            "avg_word_len": avg_word_len,
            "min_word_len": min_word_len,
            "max_word_len": max_word_len,
            "avg_words_per_sentence": avg_words_per_sentence,
            "min_words_per_sentence": min_words_per_sentence,
            "max_words_per_sentence": max_words_per_sentence
        }

# --- TRANSCRIPTION SINK ---
class WhisperTranscriptionSink(voice_recv.AudioSink):
    def __init__(self, bot, text_id):
        super().__init__()
        self.bot = bot
        self.text_id = text_id
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.all_sentences = []
        self.stats = collections.defaultdict(SpeakerStats)
        self.processing_task = self.bot.loop.create_task(self._process_buffers())
        self.gemini_task = self.bot.loop.create_task(self._gemini_loop())
        self.reporting_task = self.bot.loop.create_task(self._reporting_loop())

    def cleanup(self):
        self.processing_task.cancel()
        self.gemini_task.cancel()
        self.reporting_task.cancel()

    def wants_opus(self): return False

    def write(self, user, data):
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
                    duration = len(audio_bytes)/(48000*4)
                    
                    if rms > 50:
                        audio_float32 = audio_np.reshape(-1, 2).astype(np.float32) / 32768.0
                        mono_16k = audio_float32.mean(axis=1)[::3]
                        
                        logger.info(f"Transcribing {duration:.1f}s from {user}...")
                        text = await self.bot.loop.run_in_executor(_executor, self._transcribe, mono_16k)
                        
                        if text:
                            clean_text = text.strip()
                            logger.info(f"WHISPER RESULT [{user}]: {clean_text}")
                            if len(clean_text) > 1:
                                with self.lock:
                                    self.all_sentences.append(clean_text)
                                    self.stats[user].update(clean_text)
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
            segments, info = MODEL.transcribe(audio_16k, beam_size=5, language='en')
            return "".join([s.text for s in segments])
        except Exception:
            return ""

    async def _gemini_loop(self):
        if not GEMINI_API_KEY:
            logger.warning("No Gemini API key found. Gemini loop disabled.")
            return

        client = genai.Client(api_key=GEMINI_API_KEY)

        while True:
            await asyncio.sleep(20)
            if not self.all_sentences:
                continue

            sentence = random.choice(self.all_sentences)
            prompt = f"React to this sentence in a short, witty, and slightly cryptic way: \"{sentence}\""

            try:
                response = await self.bot.loop.run_in_executor(None, lambda: client.models.generate_content(model="gemini-2.0-flash", contents=prompt))
                if response and response.text:
                    channel = self.bot.get_channel(self.text_id)
                    if channel:
                        await channel.send(f"*Gemini:* {response.text.strip()}")
            except Exception as e:
                logger.error(f"Gemini error: {e}")

    async def _reporting_loop(self):
        while True:
            await asyncio.sleep(60)
            try:
                await self._send_report()
            except Exception as e:
                logger.error(f"Reporting loop error: {e}")

    async def _send_report(self):
        with self.lock:
            if not self.stats:
                return
            users = list(self.stats.keys())
            all_metrics = {u: self.stats[u].get_metrics() for u in users}

        if all(m['word_count'] == 0 for m in all_metrics.values()):
            return

        user_names = [getattr(u, 'display_name', str(u)) for u in users]
        metrics_to_plot = ["word_count", "avg_word_len", "avg_words_per_sentence"]
        metric_labels = ["Word Count", "Avg Word Length", "Avg Words / Sentence"]

        fig, axs = plt.subplots(1, 3, figsize=(15, 5))
        fig.suptitle("Speaker Statistics Comparison")

        colors = plt.colormaps.get_cmap('tab10').colors
        for i, metric in enumerate(metrics_to_plot):
            values = [all_metrics[u][metric] for u in users]
            axs[i].bar(user_names, values, color=colors[:len(users)])
            axs[i].set_title(metric_labels[i])

        plt.tight_layout(rect=[0, 0.03, 1, 0.95])

        buf = io.BytesIO()
        plt.savefig(buf, format='png')
        buf.seek(0)
        plt.close(fig)

        msg = "**Objective Speaker Stats**\n\n"
        for u in users:
            m = all_metrics[u]
            name = getattr(u, 'display_name', str(u))
            msg += f"**{name}**:\n"
            msg += f"- Word Count: {m['word_count']}\n"
            msg += f"- Word Length: Avg: {m['avg_word_len']:.1f}, Min: {m['min_word_len']}, Max: {m['max_word_len']}\n"
            msg += f"- Words/Sentence: Avg: {m['avg_words_per_sentence']:.1f}, Min: {m['min_words_per_sentence']}, Max: {m['max_words_per_sentence']}\n\n"

        channel = self.bot.get_channel(self.text_id)
        if channel:
            await channel.send(msg, file=discord.File(buf, filename="stats.png"))

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
    GEMINI_API_KEY = config.get('gemini_api_key')

    if not discord.opus.is_loaded():
        try: discord.opus.load_opus('libopus.so.0')
        except: pass

    logger.info("Loading Whisper model...")
    MODEL = WhisperModel('small', device='cpu', compute_type='int8')
    _executor = ThreadPoolExecutor(max_workers=1)

    bot = TranscriptionBot(VOICE_ID, TEXT_ID)
    bot.run(TOKEN)
