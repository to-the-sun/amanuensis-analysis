import sys
import traceback

def exit_on_error(type, value, tb):
    traceback.print_exception(type, value, tb)
    print("\nCRITICAL ERROR encountered. Press Enter to close...")
    input()
    sys.exit(1)

sys.excepthook = exit_on_error

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
import re
import io
import random
from typing import Optional

import google.generativeai as genai
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
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
GEMINI_API_KEY = config.get('gemini_api_key')

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

class SpeakerStats:
    def __init__(self):
        self.word_lengths = []
        self.sentence_word_counts = []
        self.current_sentence_words = 0

    def update(self, text):
        # Basic word tokenization (removing non-alphanumeric except spaces)
        clean_text = re.sub(r'[^\w\s\.\!\?]', '', text)

        # We need to track sentences. Punctuation . ! ? end sentences.
        # Let's split by punctuation but keep it to identify sentence ends.
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
        self.stats = collections.defaultdict(SpeakerStats)
        self.all_sentences = []
        self.processing_task = self.bot.loop.create_task(self._process_buffers())
        self.reporting_task = self.bot.loop.create_task(self._reporting_loop())
        self.gemini_task = self.bot.loop.create_task(self._gemini_loop())

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
                        with self.lock:
                            self.stats[user].update(text)
                            self.all_sentences.append(text.strip())
                        channel = self.bot.get_channel(self.text_channel_id)
                        if channel:
                            await channel.send(f"**{user.display_name}**: {text.strip()}")

                    with self.lock:
                        processed_len = len(audio_bytes)
                        self.user_buffers[user] = self.user_buffers[user][processed_len:]
            except Exception as e:
                logger.error(f"Processing loop error: {e}")
                traceback.print_exc()

    async def _reporting_loop(self):
        while True:
            await asyncio.sleep(60)
            try:
                await self._send_report()
            except Exception as e:
                logger.error(f"Reporting loop error: {e}")
                traceback.print_exc()

    async def _send_report(self):
        with self.lock:
            if not self.stats:
                return
            users = list(self.stats.keys())
            all_metrics = {u: self.stats[u].get_metrics() for u in users}

        # Check if we have any stats to report
        if all(m['word_count'] == 0 for m in all_metrics.values()):
            return

        user_names = [u.display_name for u in users]
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

        # Build message
        msg = "**Objective Speaker Stats (Running Analysis)**\n\n"
        for u in users:
            m = all_metrics[u]
            msg += f"**{u.display_name}**:\n"
            msg += f"- Word Count: {m['word_count']}\n"
            msg += f"- Word Length (Letters): Avg: {m['avg_word_len']:.1f}, Min: {m['min_word_len']}, Max: {m['max_word_len']}\n"
            msg += f"- Words per Sentence: Avg: {m['avg_words_per_sentence']:.1f}, Min: {m['min_words_per_sentence']}, Max: {m['max_words_per_sentence']}\n\n"

        channel = self.bot.get_channel(self.text_channel_id)
        if channel:
            await channel.send(msg, file=discord.File(buf, filename="stats.png"))

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

    async def _gemini_loop(self):
        if not GEMINI_API_KEY:
            logger.warning("No Gemini API key found in config. Gemini loop disabled.")
            return

        try:
            genai.configure(api_key=GEMINI_API_KEY)
            model = genai.GenerativeModel('gemini-2.0-flash')
        except Exception as e:
            logger.error(f"Failed to initialize Gemini: {e}")
            return

        while True:
            await asyncio.sleep(20)
            try:
                target_sentence = None
                with self.lock:
                    if self.all_sentences:
                        target_sentence = random.choice(self.all_sentences)

                if target_sentence:
                    logger.info(f"Sending sentence to Gemini: \"{target_sentence}\"")
                    # Run generate_content in executor because it might be blocking
                    response = await self.bot.loop.run_in_executor(
                        None,
                        lambda: model.generate_content(target_sentence)
                    )

                    try:
                        if response and response.text:
                            channel = self.bot.get_channel(self.text_channel_id)
                            if channel:
                                await channel.send(f"**Gemini**: {response.text.strip()}")
                        else:
                            logger.warning("Gemini returned empty response.")
                    except ValueError:
                        # This often happens if the response was blocked by safety filters
                        logger.warning("Gemini response was blocked or contains no text.")

            except Exception as e:
                logger.error(f"Gemini loop error: {e}")
                traceback.print_exc()

    def cleanup(self):
        self.processing_task.cancel()
        self.reporting_task.cancel()
        self.gemini_task.cancel()

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
            target_mic_name = "Microphone (NVIDIA Broadcast)"
            mics = sc.all_microphones()
            mic = next((m for m in mics if target_mic_name in m.name), None)

            if mic:
                print(f"Preferred microphone found: {mic.name}")
            else:
                mic = sc.default_microphone()
                print(f"Preferred microphone not found. Falling back to default: {mic.name}")

            user_tts = LocalUser("To the Sun")
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
    try:
        bot = TranscriptionBot()
        bot.run(TOKEN)
    except Exception as e:
        print(f"\nCRITICAL STARTUP ERROR: {e}")
        traceback.print_exc()
    finally:
        print("\nScript has stopped. Press Enter to close...")
        input()
