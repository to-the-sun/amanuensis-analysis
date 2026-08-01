import os
import sys
import json
import logging
import asyncio
import re
import collections
import wave
import io
import time
import threading
import numpy as np
import requests
from concurrent.futures import ThreadPoolExecutor

import discord
from discord import app_commands

# --- EARLY CRASH HANDLING FOR CLI/WINDOWS DOUBLE-CLICK ---
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    logging.critical("Uncaught exception", exc_info=(exc_type, exc_value, exc_traceback))
    input("\nFATAL ERROR: Script halted. Press Enter to close window...")
    sys.exit(1)

sys.excepthook = handle_exception

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger("desktop_transcriber")

# --- SOUNDCARD INITIALIZATION WITH GRACEFUL FALLBACK ---
if len(sys.argv) < 2:
    sys.argv.append("desktop_transcriber.py")

SOUNDCARD_AVAILABLE = False
sc = None
try:
    import soundcard as sc
    # Try a quick test of default speaker to check if soundcard is fully functional
    _test_spk = sc.default_speaker()
    SOUNDCARD_AVAILABLE = True
    logger.info(f"soundcard initialized successfully. Default speaker: {_test_spk.name if _test_spk else 'None'}")
except Exception as e:
    logger.warning(f"soundcard could not be initialized (falling back to mock mode): {e}")

try:
    import soundfile as sf
except ImportError:
    sf = None
    logger.warning("soundfile module not found. Save to WAV functions may fail if used.")

# --- SYLLABLE AND LINGUISTIC IMPORTS FOR /ANALYZE ---
try:
    import nltk
    from nltk.corpus import cmudict
    import syllables
    from SoundsLike.SoundsLike import Word_Functions, Pronunciation_Functions
    import pronouncing
    NLTK_AVAILABLE = True
except ImportError as e:
    NLTK_AVAILABLE = False
    logger.warning(f"Linguistic libraries (nltk, syllables, SoundsLike, pronouncing) are not available. /analyze command will use basic fallbacks: {e}")

# Load CMUdict if available
CMU_DICT = None
if NLTK_AVAILABLE:
    try:
        CMU_DICT = cmudict.dict()
    except LookupError:
        try:
            nltk.download('cmudict')
            CMU_DICT = cmudict.dict()
        except Exception as e:
            logger.warning(f"Failed to download NLTK cmudict: {e}")
    try:
        nltk.data.find('taggers/averaged_perceptron_tagger')
    except LookupError:
        try:
            nltk.download('averaged_perceptron_tagger')
        except Exception as e:
            logger.warning(f"Failed to download NLTK taggers: {e}")

# --- SYLLABLE AND RHYME UTILITIES ---
def count_syllables_word(word):
    if not NLTK_AVAILABLE:
        # Fallback syllable estimation using vowels
        clean_word = word.lower().strip(".,!?:;()\"'")
        if not clean_word:
            return 0
        vowels = "aeiouy"
        count = 0
        if clean_word[0] in vowels:
            count += 1
        for index in range(1, len(clean_word)):
            if clean_word[index] in vowels and clean_word[index - 1] not in vowels:
                count += 1
        if clean_word.endswith("e"):
            count -= 1
        if count == 0:
            count = 1
        return count

    clean_word = word.lower().strip(".,!?:;()\"'")
    if not clean_word:
        return 0
    if CMU_DICT and clean_word in CMU_DICT:
        return max([len([y for y in x if y[-1].isdigit()]) for x in CMU_DICT[clean_word]])
    return syllables.estimate(clean_word)

def get_last_stressed_vowel_sound(word):
    if not NLTK_AVAILABLE:
        return None
    clean_word = word.lower().strip(".,!?:;()\"'")
    if not clean_word:
        return None
    try:
        pron = Word_Functions.pronunciation(clean_word, generate=True)
        if not pron:
            return None
        idx = Pronunciation_Functions.index_last_stressed_vowel(pron)
        if idx is None:
            # Fallback to last vowel if no stress
            for i in range(len(pron)-1, -1, -1):
                if pron[i][-1].isdigit():
                    return pron[i][:-1]
            return None
        return pron[idx][:-1]
    except Exception:
        return None

def do_words_rhyme(w1, w2):
    if not NLTK_AVAILABLE:
        return False
    cw1 = w1.lower().strip(".,!?:;()\"'")
    cw2 = w2.lower().strip(".,!?:;()\"'")
    if not cw1 or not cw2:
        return False
    if cw1 == cw2:
        return True
    try:
        return cw2 in pronouncing.rhymes(cw1) or cw1 in pronouncing.rhymes(cw2)
    except Exception:
        return False

# --- CREDENTIALS LOADING ---
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CREDENTIALS_PATH = os.path.join(SCRIPT_DIR, "credentials.json")

def load_credentials():
    logger.info(f"Loading credentials from: {CREDENTIALS_PATH}")
    if not os.path.exists(CREDENTIALS_PATH):
        logger.error(f"credentials.json not found at {CREDENTIALS_PATH}")
        sys.exit(1)
    with open(CREDENTIALS_PATH, "r") as f:
        return json.load(f)

_executor = ThreadPoolExecutor(max_workers=1)

# --- BOT CLIENT ---
class DesktopTranscriberBot(discord.Client):
    def __init__(self):
        super().__init__(intents=discord.Intents.all())
        self.tree = app_commands.CommandTree(self)
        self.text_channel_id = None
        self.config = {}
        self.recording_thread = None
        self.recording_active = False

    async def setup_hook(self):
        @self.tree.command(name="purge", description="Purge all messages in the world channel")
        async def purge(interaction: discord.Interaction):
            await self.purge_logic(interaction)

        @self.tree.command(name="analyze", description="Count syllables in each line of the world channel and edit messages to display them")
        async def analyze(interaction: discord.Interaction):
            await self.analyze_logic(interaction)

        await self.tree.sync()
        logger.info("Slash commands synced successfully.")

    def _get_cleaned_content(self, line):
        prefix = ""
        content = line
        if line.startswith("**"):
            parts = line.split("**: ", 1)
            if len(parts) > 1:
                prefix = parts[0] + "**: "
                content = parts[1]

        cleaned_content = re.sub(r'\s*\(\d+\)$', '', content).strip()
        return prefix, cleaned_content

    async def purge_logic(self, interaction: discord.Interaction):
        if not (isinstance(interaction.channel, discord.TextChannel) and interaction.channel.name == "world"):
            await interaction.response.send_message("This command can only be used in the 'world' channel.", ephemeral=True)
            return

        await interaction.response.defer(ephemeral=True)
        try:
            logger.info(f"Purge command received in {interaction.channel.name} from {interaction.user}")
            total_deleted = 0
            while True:
                deleted = await interaction.channel.purge(limit=100)
                num_deleted = len(deleted)
                total_deleted += num_deleted
                logger.info(f"Purged {num_deleted} messages in this chunk. Total deleted: {total_deleted}")
                if num_deleted < 100:
                    break
                await asyncio.sleep(1.5)
            logger.info(f"Successfully completed purge of {interaction.channel.name}. Total deleted: {total_deleted}")
            await interaction.followup.send(f"Successfully purged {total_deleted} messages.")
        except discord.Forbidden:
            logger.error(f"Failed to purge {interaction.channel.name}: Missing permissions.")
            await interaction.followup.send("Failed to purge: Missing permissions.")
        except Exception as e:
            logger.error(f"Error during purge in {interaction.channel.name}: {e}")
            await interaction.followup.send(f"Error during purge: {e}")

    async def analyze_logic(self, interaction: discord.Interaction):
        if not (isinstance(interaction.channel, discord.TextChannel) and interaction.channel.name == "world"):
            await interaction.response.send_message("This command can only be used in the 'world' channel.", ephemeral=True)
            return

        await interaction.response.defer()
        try:
            logger.info(f"Analyze command (syllables + poem) received in {interaction.channel.name} from {interaction.user}")
            await interaction.followup.send("Starting syllable analysis and poem generation...")

            collected_data = [] # List of (text, syllable_count)

            # Fetch all bot messages first to process them chronologically
            bot_messages = []
            async for msg in interaction.channel.history(limit=None):
                if msg.author == self.user:
                    bot_messages.append(msg)
            bot_messages.reverse()

            for msg in bot_messages:
                lines = msg.content.splitlines()
                new_lines = []
                changed = False

                for line in lines:
                    prefix, cleaned_content = self._get_cleaned_content(line)
                    if not cleaned_content:
                        new_lines.append(line)
                        continue

                    # Count syllables
                    words = cleaned_content.split()
                    total_syls = sum(count_syllables_word(w) for w in words)

                    collected_data.append((cleaned_content, total_syls))
                    new_line = f"{prefix}{cleaned_content} ({total_syls})"

                    if new_line != line:
                        changed = True
                    new_lines.append(new_line)

                if changed:
                    await msg.edit(content="\n".join(new_lines))
                    await asyncio.sleep(0.5) # Rate limit safety

            if collected_data:
                await interaction.followup.send("Generating poem from rhyming lines...")

                # Find most common syllable count (excluding 0)
                counts = [d[1] for d in collected_data if d[1] > 0]
                if not counts:
                    await interaction.followup.send("No lines with syllables found.")
                    return

                # Calculate weighted scores: syllables * frequency
                counter = collections.Counter(counts)
                best_syllable_count = 0
                max_score = -1
                for syl_count, freq in counter.items():
                    score = syl_count * freq
                    if score > max_score:
                        max_score = score
                        best_syllable_count = syl_count

                # Group by rhyme sound for the highest scoring syllable count
                rhyme_groups = collections.defaultdict(list)
                for text, count in collected_data:
                    if count == best_syllable_count:
                        words = text.split()
                        if not words: continue
                        last_word = words[-1]
                        rhyme_sound = get_last_stressed_vowel_sound(last_word)
                        if rhyme_sound:
                            rhyme_groups[rhyme_sound].append(text)

                # Assemble poem
                poem_lines = []
                for sound, lines in rhyme_groups.items():
                    subgroups = []
                    for line in lines:
                        words = line.split()
                        if not words:
                            continue
                        last_word = words[-1]
                        placed = False
                        for sg in subgroups:
                            if all(do_words_rhyme(last_word, sg_line.split()[-1]) for sg_line in sg if sg_line.split()):
                                sg.append(line)
                                placed = True
                                break
                        if not placed:
                            subgroups.append([line])

                    for sg in subgroups:
                        if len(sg) >= 2:
                            poem_lines.extend(sg)
                            poem_lines.append("") # Stanza break

                if poem_lines:
                    response = "\n".join(poem_lines).strip()
                    # Discord message limit is 2000 chars.
                    # We truncate if it's too long, taking roughly 1800 chars to be safe with the prefix.
                    if len(response) > 1800:
                        response = response[:1800] + "\n\n... (truncated due to length)"

                    await interaction.channel.send(f"**Poem Generated from Analysis (Syllables: {best_syllable_count}):**\n\n{response}")
                else:
                    await interaction.followup.send(f"Could not find enough rhyming lines with {best_syllable_count} syllables.")

            await interaction.followup.send("Syllable analysis and poem generation complete.")

        except Exception as e:
            logger.exception(f"Error during analyze (syllables/poem) in {interaction.channel.name}: {e}")
            await interaction.followup.send(f"Error during analyze: {e}")

    async def on_ready(self):
        logger.info(f"Logged in as {self.user} (ID: {self.user.id})")

        # Locate target "#world" text channel
        for guild in self.guilds:
            world_channel = discord.utils.get(guild.text_channels, name="world")
            if world_channel:
                self.text_channel_id = world_channel.id
                logger.info(f"Target '#world' channel found in guild '{guild.name}' (ID: {self.text_channel_id})")
                break

        if not self.text_channel_id:
            logger.warning("Could not find any text channel named 'world' in connected guilds.")

        # Start desktop audio capture thread
        self.recording_active = True
        self.recording_thread = threading.Thread(target=self.run_desktop_audio_capture, daemon=True)
        self.recording_thread.start()
        logger.info("Desktop audio capture background thread started.")

    def run_desktop_audio_capture(self):
        # We define loop parameters
        sample_rate = 48000
        chunk_duration = 0.1 # 100ms
        chunk_frames = int(sample_rate * chunk_duration) # 4800 frames

        # VAD Parameters
        pre_roll_duration = 0.5 # 500ms
        pre_roll_max_len = int(pre_roll_duration / chunk_duration) # 5 chunks
        pre_roll_buffer = collections.deque(maxlen=pre_roll_max_len)

        rms_threshold = 0.0015 # equivalent to int16 RMS of 50
        silence_timeout = 0.8 # 800ms
        max_utterance_duration = 15.0 # 15 seconds

        # State variables
        is_active = False
        active_buffer = []
        silence_duration = 0.0

        logger.info("Setting up loopback microphone...")
        mic = None

        if SOUNDCARD_AVAILABLE:
            try:
                default_speaker = sc.default_speaker()
                logger.info(f"Using default speaker: {default_speaker.name}")
                mic = sc.get_microphone(id=str(default_speaker.name), include_loopback=True)
                logger.info(f"Loopback microphone successfully resolved: {mic.name}")
            except Exception as e:
                logger.error(f"Failed to resolve default speaker/loopback microphone: {e}. Falling back to mock mode.")
                mic = None

        if mic is None:
            # Mock mode loop
            logger.info("Starting in Mock Mode. Simulating periodic transcription activity.")
            mock_counter = 1
            while self.recording_active:
                time.sleep(25) # Mock speech every 25 seconds
                if self.text_channel_id:
                    mock_text = f"This is mock transcription {mock_counter} of desktop audio."
                    mock_counter += 1
                    logger.info(f"MOCK TRANSCRIPTION EVENT: {mock_text}")
                    asyncio.run_coroutine_threadsafe(
                        self.post_transcription_to_channel(mock_text),
                        self.loop
                    )
            return

        # Continuous live recording loop
        try:
            with mic.recorder(samplerate=sample_rate) as recorder:
                logger.info("Loopback recorder stream opened. Listening continuously...")
                while self.recording_active:
                    # Record 100ms of audio
                    chunk = recorder.record(numframes=chunk_frames)

                    # Compute mono and RMS
                    if chunk.ndim > 1:
                        chunk_mono = chunk.mean(axis=1)
                    else:
                        chunk_mono = chunk

                    rms = np.sqrt(np.mean(chunk_mono ** 2))

                    if not is_active:
                        # Idle state: update pre-roll buffer
                        pre_roll_buffer.append(chunk_mono)
                        if rms > rms_threshold:
                            logger.info(f"Voice activity detected! (RMS: {rms:.5f}) Triggering active utterance.")
                            is_active = True
                            # Assemble the initial buffer using pre-roll data
                            active_buffer = list(pre_roll_buffer)
                            pre_roll_buffer.clear()
                            silence_duration = 0.0
                    else:
                        # Active state: accumulate audio
                        active_buffer.append(chunk_mono)
                        current_duration = len(active_buffer) * chunk_duration

                        if rms < rms_threshold:
                            silence_duration += chunk_duration
                        else:
                            silence_duration = 0.0

                        # Check endpoint triggers
                        if silence_duration >= silence_timeout or current_duration >= max_utterance_duration:
                            reason = "silence timeout" if silence_duration >= silence_timeout else "max duration limit"
                            logger.info(f"Utterance finished ({reason}, total duration: {current_duration:.2f}s). Processing...")

                            # Join all chunks in active_buffer
                            utterance_audio = np.concatenate(active_buffer)

                            # Transcribe in a separate executor thread to avoid blocking the audio thread
                            self.transcribe_and_post_threadsafe(utterance_audio)

                            # Reset state
                            is_active = False
                            active_buffer = []
                            silence_duration = 0.0

        except Exception as e:
            logger.error(f"Error in run_desktop_audio_capture: {e}")
            self.recording_active = False

    def transcribe_and_post_threadsafe(self, audio_data):
        # We submit the task to our ThreadPoolExecutor
        _executor.submit(self._run_transcribe_task, audio_data)

    def _run_transcribe_task(self, audio_data):
        try:
            # 1. Downsample from 48kHz to 16kHz via [::3]
            mono_16k = audio_data[::3]

            # 2. Transcribe using the Avalon API
            text = self._transcribe(mono_16k)
            if text:
                clean_text = self._poetic_parse(text)
                logger.info(f"AVALON TRANSCRIPTION SUCCESS: {clean_text}")
                # Post to discord text channel
                asyncio.run_coroutine_threadsafe(
                    self.post_transcription_to_channel(clean_text),
                    self.loop
                )
        except Exception as e:
            logger.error(f"Error in _run_transcribe_task: {e}")

    def _poetic_parse(self, text):
        # Remove any periods that come directly after a capital letter.
        text = re.sub(r'(?<=[A-Z])\.', '', text)
        # Split sentences only on periods, exclamation points, and question marks.
        parts = re.split(r'([.!?])', text)
        lines = []
        for part in parts:
            if part in ".!?":
                if lines:
                    lines[-1] += part
                continue
            clean = part.strip()
            if clean:
                lines.append(clean)
        return '\n'.join(lines)

    def _transcribe(self, audio_16k):
        try:
            # Convert float32 mono_16k back to int16 PCM
            audio_int16 = (audio_16k * 32767).astype(np.int16)

            # Write to in-memory WAV file
            buffer = io.BytesIO()
            buffer.name = "audio.wav"
            with wave.open(buffer, 'wb') as wav_file:
                wav_file.setnchannels(1)
                wav_file.setsampwidth(2)
                wav_file.setframerate(16000)
                wav_file.writeframes(audio_int16.tobytes())

            buffer.seek(0)

            url = "https://api.aquavoice.com/api/v1/audio/transcriptions"
            headers = {"Authorization": f"Bearer {self.config['aqua_key']}"}
            files = {"file": (buffer.name, buffer, "audio/wav")}
            data = {"model": "avalon-v1.5"}

            response = requests.post(url, headers=headers, files=files, data=data)
            response.raise_for_status()
            return response.json().get("text", "")
        except Exception as e:
            logger.error(f"Aqua transcription error: {e}")
            return ""

    async def post_transcription_to_channel(self, text):
        if not self.text_channel_id:
            logger.warning("No '#world' text channel target. Cannot post transcription.")
            return

        channel = self.get_channel(self.text_channel_id)
        if channel:
            try:
                # Post in the expected format (without user tag since it is desktop-wide capture)
                await channel.send(text)
                logger.info("Transcription posted to #world channel.")
            except Exception as e:
                logger.error(f"Failed to send transcription message: {e}")
        else:
            logger.error(f"Could not retrieve channel for ID {self.text_channel_id}")

    async def close(self):
        self.recording_active = False
        await super().close()

if __name__ == "__main__":
    config = load_credentials()
    bot = DesktopTranscriberBot()
    bot.config = config

    # Run the bot
    bot.run(config['token'])
