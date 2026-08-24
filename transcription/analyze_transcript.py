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

# --- EARLY CRASH HANDLING ---
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    logging.critical("Uncaught exception", exc_info=(exc_type, exc_value, exc_traceback))
    input("\nFATAL ERROR: Script halted. Press Enter to close window...")
    sys.exit(1)

# --- LOGGING ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
logger = logging.getLogger("analyze_transcript")

# --- LINGUISTIC DEPENDENCIES ---
try:
    import nltk
    from nltk.corpus import cmudict
    import syllables
    from SoundsLike.SoundsLike import Word_Functions, Pronunciation_Functions
    import pronouncing
    NLTK_AVAILABLE = True
except ImportError as e:
    NLTK_AVAILABLE = False
    logger.warning(f"Linguistic libraries (nltk, syllables, SoundsLike, pronouncing) are not available: {e}")

try:
    import eng_to_ipa
    import eng_to_ipa.stress as stress_lib
    from eng_to_ipa.transcribe import get_cmu
    ENG_TO_IPA_AVAILABLE = True
except ImportError as e:
    ENG_TO_IPA_AVAILABLE = False
    logger.warning(f"eng-to-ipa library is not available: {e}")

# --- IPA OVERRIDES ---
try:
    _script_dir = os.path.dirname(os.path.abspath(__file__))
except NameError:
    _script_dir = os.getcwd()

IPA_OVERRIDES_PATH = os.path.join(_script_dir, "ipa_overrides.json")
try:
    with open(IPA_OVERRIDES_PATH, "r", encoding="utf-8") as f:
        IPA_OVERRIDES = json.load(f)
    logger.info(f"Loaded {len(IPA_OVERRIDES)} custom IPA overrides.")
except Exception as e:
    logger.warning(f"Failed to load IPA overrides from {IPA_OVERRIDES_PATH}: {e}")
    IPA_OVERRIDES = {}

IPA_SYMBOLS_MAP = {
    "a": "ə", "ey": "eɪ", "aa": "ɑ", "ae": "æ", "ah": "ə", "ao": "ɔ",
    "aw": "aʊ", "ay": "aɪ", "ch": "ʧ", "dh": "ð", "eh": "ɛ", "er": "ər",
    "hh": "h", "ih": "ɪ", "jh": "ʤ", "ng": "ŋ", "ow": "oʊ", "oy": "ɔɪ",
    "sh": "ʃ", "th": "θ", "uh": "ʊ", "uw": "u", "zh": "ʒ", "iy": "i", "y": "j"
}

VALID_DOUBLE_ONSETS = {
    ("P", "R"), ("P", "L"), ("P", "Y"),
    ("T", "R"), ("T", "W"), ("T", "Y"),
    ("K", "R"), ("K", "L"), ("K", "W"), ("K", "Y"),
    ("B", "R"), ("B", "L"), ("B", "Y"),
    ("D", "R"), ("D", "W"), ("D", "Y"),
    ("G", "R"), ("G", "L"), ("G", "W"),
    ("F", "R"), ("F", "L"), ("F", "Y"),
    ("V", "R"), ("V", "L"), ("V", "Y"),
    ("TH", "R"), ("TH", "W"), ("TH", "Y"),
    ("SH", "R"), ("S", "L"), ("S", "R"), ("S", "W"), ("S", "Y"),
    ("S", "P"), ("S", "T"), ("S", "K"), ("S", "M"), ("S", "N"), ("S", "F")
}

VALID_TRIPLE_ONSETS = {
    ("S", "P", "L"), ("S", "P", "R"), ("S", "P", "Y"),
    ("S", "T", "R"), ("S", "T", "Y"),
    ("S", "K", "L"), ("S", "K", "R"), ("S", "K", "W"), ("S", "K", "Y")
}

IPA_VOWELS_SET = {"aa", "ae", "ah", "ao", "aw", "ay", "eh", "er", "ey", "ih", "iy", "ow", "oy", "uh", "uw"}

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

def is_valid_onset(phones):
    t = tuple(re.sub(r"[ˈˌ]", "", p).upper() for p in phones)
    if len(t) == 1:
        return t[0] != "NG"
    elif len(t) == 2:
        return t in VALID_DOUBLE_ONSETS
    elif len(t) == 3:
        return t in VALID_TRIPLE_ONSETS
    return False

def phonemes_to_ipa(phonemes_list):
    ipa_form = ""
    for piece in phonemes_list:
        piece_clean = re.sub(r"\d", "", piece).lower()
        marked = False
        unmarked = piece_clean
        if piece_clean and piece_clean[0] in ["ˈ", "ˌ"]:
            marked = True
            mark_char = piece_clean[0]
            unmarked = piece_clean[1:]
        if unmarked in IPA_SYMBOLS_MAP:
            if marked:
                ipa_form += mark_char + IPA_SYMBOLS_MAP[unmarked]
            else:
                ipa_form += IPA_SYMBOLS_MAP[unmarked]
        else:
            ipa_form += piece_clean

    swap_list = [["ˈər", "əˈr"], ["ˈie", "iˈe"]]
    for sym in swap_list:
        if not ipa_form.startswith(sym[0]):
            ipa_form = ipa_form.replace(sym[0], sym[1])
    return ipa_form

def get_ipa_syllables(line):
    if not ENG_TO_IPA_AVAILABLE:
        return ""
    words = re.findall(r"[a-zA-Z0-9']+", line)
    if not words:
        return ""

    results = []
    for w in words:
        w_lower = w.lower()
        if w_lower in IPA_OVERRIDES:
            results.append(IPA_OVERRIDES[w_lower])
            continue

        cmu_res = get_cmu([w_lower])
        if not cmu_res or not cmu_res[0]:
            results.append(f"/{w_lower}*/")
            continue

        cmu_str = cmu_res[0][0]
        if cmu_str.startswith("__IGNORE__"):
            word_ignore = cmu_str.replace("__IGNORE__", "")
            results.append(f"/{word_ignore}*/")
            continue

        stressed_cmu = stress_lib.find_stress(cmu_str, type="all")
        phones = stressed_cmu.split(" ")

        vowel_indices = []
        for idx, p in enumerate(phones):
            clean_p = re.sub(r"[ˈˌ\d]", "", p).lower()
            if clean_p in IPA_VOWELS_SET:
                vowel_indices.append(idx)

        if not vowel_indices:
            ipa_val = phonemes_to_ipa(phones)
            results.append(f"/{ipa_val}/")
            continue

        syllables_list = []
        start = 0
        for idx in range(len(vowel_indices) - 1):
            v1 = vowel_indices[idx]
            v2 = vowel_indices[idx + 1]

            num_consonants = v2 - v1 - 1
            if num_consonants == 0:
                split_point = v2
            elif num_consonants == 1:
                split_point = v1 + 1
            else:
                consonant_sublist = phones[v1 + 1 : v2]
                split_point = v2 - 1
                for onset_len in [3, 2, 1]:
                    if onset_len <= num_consonants:
                        candidate = consonant_sublist[-onset_len:]
                        if is_valid_onset(candidate):
                            split_point = v2 - onset_len
                            break

            syllables_list.append(phones[start:split_point])
            start = split_point

        syllables_list.append(phones[start:])

        word_ipa_syls = []
        for syl in syllables_list:
            ipa_val = phonemes_to_ipa(syl)
            if ipa_val:
                word_ipa_syls.append(ipa_val)
        if word_ipa_syls:
            word_ipa_str = "/".join(word_ipa_syls)
            results.append(f"/{word_ipa_str}/")

    return " ".join(results)

def extract_ipa_vowels_from_line(line):
    ipa_str = get_ipa_syllables(line)
    if not ipa_str:
        return []
    words = ipa_str.split()
    syls = []
    for word in words:
        stripped_word = word.strip("/")
        syls.extend(stripped_word.split("/"))

    vowels = []
    ipa_vowels_sorted = ["ər", "eɪ", "aʊ", "aɪ", "oʊ", "ɔɪ", "ə", "ɑ", "æ", "ɔ", "ɛ", "ɪ", "ʊ", "u", "i"]
    for syl in syls:
        if "*" in syl:
            continue
        clean_syl = re.sub(r"[ˈˌ/*]", "", syl)
        found_vowel = None
        for v in ipa_vowels_sorted:
            if v in clean_syl:
                found_vowel = v
                break
        if found_vowel:
            vowels.append(found_vowel)
    return vowels

def split_word_into_spelling_syllables(word, num_syllables):
    if num_syllables <= 1:
        return [word]
    length = len(word)
    if length <= num_syllables:
        return [word[i] if i < length else "" for i in range(num_syllables)]
    parts = [[] for _ in range(num_syllables)]
    for i, char in enumerate(word):
        part_idx = int(i * num_syllables / length)
        if part_idx >= num_syllables:
            part_idx = num_syllables - 1
        parts[part_idx].append(char)
    return ["".join(p) for p in parts]

def get_line_syllables_and_vowels(line):
    words = re.findall(r"[a-zA-Z0-9']+", line)
    line_syls = []
    for word_idx, w in enumerate(words):
        ipa_str = get_ipa_syllables(w)
        vowels = []
        if ipa_str and "*" not in ipa_str:
            w_syls = [s.strip() for s in ipa_str.strip("/").split("/") if s.strip()]
            ipa_vowels_sorted = ["ər", "eɪ", "aʊ", "aɪ", "oʊ", "ɔɪ", "ə", "ɑ", "æ", "ɔ", "ɛ", "ɪ", "ʊ", "u", "i"]
            for s in w_syls:
                clean_syl = re.sub(r"[ˈˌ/*]", "", s)
                found_vowel = None
                for v in ipa_vowels_sorted:
                    if v in clean_syl:
                        found_vowel = v
                        break
                if found_vowel:
                    vowels.append(found_vowel)

        num_syls = len(vowels) if vowels else count_syllables_word(w)
        if num_syls == 0:
            num_syls = 1

        spelling_syls = split_word_into_spelling_syllables(w, num_syls)
        if len(vowels) < num_syls:
            vowels.extend([None] * (num_syls - len(vowels)))
        elif len(vowels) > num_syls:
            vowels = vowels[:num_syls]

        for i in range(num_syls):
            line_syls.append({
                'syllable': spelling_syls[i],
                'vowel': vowels[i],
                'word': w,
                'word_idx': word_idx
            })
    return line_syls

def to_unicode_bold(text):
    bold_chars = []
    for char in text:
        if 'a' <= char <= 'z':
            bold_chars.append(chr(0x1D5EE + ord(char) - ord('a')))
        elif 'A' <= char <= 'Z':
            bold_chars.append(chr(0x1D5D4 + ord(char) - ord('A')))
        elif '0' <= char <= '9':
            bold_chars.append(chr(0x1D7EC + ord(char) - ord('0')))
        else:
            bold_chars.append(char)
    return ''.join(bold_chars)

def strip_unicode_bold(text):
    plain_chars = []
    for char in text:
        cp = ord(char)
        if 0x1D5EE <= cp <= 0x1D607:
            plain_chars.append(chr(ord('a') + cp - 0x1D5EE))
        elif 0x1D5D4 <= cp <= 0x1D5ED:
            plain_chars.append(chr(ord('A') + cp - 0x1D5D4))
        elif 0x1D7EC <= cp <= 0x1D7F5:
            plain_chars.append(chr(ord('0') + cp - 0x1D7EC))
        else:
            plain_chars.append(char)
    return ''.join(plain_chars)

def reconstruct_line_from_syllables(syllables_list, bold_indices=None):
    if not syllables_list:
        return ""
    if bold_indices is None:
        bold_indices = set()
    parts = []
    for idx, item in enumerate(syllables_list):
        syl_text = item['syllable']
        if idx in bold_indices:
            syl_text = to_unicode_bold(syl_text)
        if idx > 0:
            prev_item = syllables_list[idx - 1]
            if prev_item['word'] != item['word']:
                parts.append(' ' + syl_text)
            else:
                parts.append(syl_text)
        else:
            parts.append(syl_text)
    return "".join(parts).strip()

def is_chain_contained(c_target, c_other):
    target_full = re.sub(r'\s+', ' ', ' '.join(c_target)).strip().lower()
    other_full = re.sub(r'\s+', ' ', ' '.join(c_other)).strip().lower()

    if target_full in other_full and len(other_full) > len(target_full):
        return True

    if len(c_target) == len(c_other):
        all_lines_contained = True
        strictly_smaller = False
        for l_t, l_o in zip(c_target, c_other):
            lt_norm = re.sub(r'\s+', ' ', l_t).strip().lower()
            lo_norm = re.sub(r'\s+', ' ', l_o).strip().lower()
            if lt_norm not in lo_norm:
                all_lines_contained = False
                break
            if len(lt_norm) < len(lo_norm):
                strictly_smaller = True
        if all_lines_contained and strictly_smaller:
            return True

    if len(c_target) < len(c_other):
        n_t = len(c_target)
        n_o = len(c_other)
        for start in range(n_o - n_t + 1):
            match = True
            for k in range(n_t):
                lt_norm = re.sub(r'\s+', ' ', c_target[k]).strip().lower()
                lo_norm = re.sub(r'\s+', ' ', c_other[start + k]).strip().lower()
                if lt_norm not in lo_norm:
                    match = False
                    break
            if match:
                return True

    return False

def cuts_word_in_half(selected_syls, full_line_syls):
    selected_counts = collections.Counter()
    for s in selected_syls:
        if 'word_idx' in s:
            selected_counts[s['word_idx']] += 1

    full_counts = collections.Counter()
    for s in full_line_syls:
        if 'word_idx' in s:
            full_counts[s['word_idx']] += 1

    for w_idx, count in selected_counts.items():
        if count < full_counts[w_idx]:
            return True

    return False

def count_syllables_word(word):
    if not NLTK_AVAILABLE:
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

def get_phrase_vowels(phrase):
    words = phrase.split()
    vowels = []
    for word in words:
        clean_word = word.lower().strip(".,!?:;()\"'")
        if not clean_word:
            continue
        pron = None
        if CMU_DICT and clean_word in CMU_DICT:
            pron = CMU_DICT[clean_word][0]
        elif NLTK_AVAILABLE:
            try:
                pron = Word_Functions.pronunciation(clean_word, generate=True)
            except Exception:
                pass

        if pron:
            for phone in pron:
                if phone[-1].isdigit():
                    vowel_sound = phone[:-1]
                    vowels.append(vowel_sound)
    return vowels

def truncate_line_beginning(line_text, target_syls):
    words = line_text.split()
    if not words:
        return None, 0
    accumulated_words = []
    current_syllables = 0
    for word in reversed(words):
        word_syls = count_syllables_word(word)
        if current_syllables + word_syls <= target_syls:
            accumulated_words.append(word)
            current_syllables += word_syls
        else:
            break
    if current_syllables != target_syls:
        return None, current_syllables
    truncated_line = " ".join(reversed(accumulated_words))
    return truncated_line, current_syllables

def _get_cleaned_content(line):
    prefix = ""
    content = line
    bold_match = re.match(r'^(\*\*([^*]+)\*\*\s*:\s*)(.*)', line)
    if bold_match:
        prefix = bold_match.group(1)
        content = bold_match.group(3)
    else:
        if not line.startswith("http://") and not line.startswith("https://"):
            plain_match = re.match(r'^([A-Za-z0-9_\-\s#@\[\]\(\)]+):\s*(.*)', line)
            if plain_match:
                name_part = plain_match.group(1)
                if len(name_part) < 40 and len(name_part.split()) <= 5:
                    prefix = name_part + ": "
                    content = plain_match.group(2)

    cleaned_content = re.sub(r'\s*\(\d+\)$', '', content).strip()
    return prefix, cleaned_content

def poetic_parse(text):
    text = re.sub(r'(?<=[A-Z])\.', '', text)
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

def transcribe_audio(audio_16k, aqua_key, script_dir=None):
    if script_dir is None:
        script_dir = _script_dir
    try:
        if audio_16k.dtype == np.float32 or audio_16k.dtype == np.float64:
            audio_int16 = (audio_16k * 32767).astype(np.int16)
        else:
            audio_int16 = audio_16k.astype(np.int16)

        try:
            from pydub import AudioSegment
            data_dir = os.path.join(script_dir, "data")
            os.makedirs(data_dir, exist_ok=True)
            mp3_filename = f"audio_{int(time.time() * 1000)}.mp3"
            mp3_path = os.path.join(data_dir, mp3_filename)
            segment = AudioSegment(
                data=audio_int16.tobytes(),
                sample_width=2,
                frame_rate=16000,
                channels=1
            )
            segment.export(mp3_path, format="mp3")
            logger.info(f"Saved MP3 copy of transcription audio to {mp3_path}")
        except Exception as ex:
            logger.error(f"Failed to save MP3 copy: {ex}")

        buffer = io.BytesIO()
        buffer.name = "audio.wav"
        with wave.open(buffer, 'wb') as wav_file:
            wav_file.setnchannels(1)
            wav_file.setsampwidth(2)
            wav_file.setframerate(16000)
            wav_file.writeframes(audio_int16.tobytes())

        buffer.seek(0)

        url = "https://api.aquavoice.com/api/v1/audio/transcriptions"
        headers = {"Authorization": f"Bearer {aqua_key}"}
        files = {"file": (buffer.name, buffer, "audio/wav")}
        data = {"model": "avalon-v1.5"}

        response = requests.post(url, headers=headers, files=files, data=data)
        response.raise_for_status()
        return response.json().get("text", "")
    except Exception as e:
        logger.error(f"Aqua transcription error: {e}")
        return ""

def cleanup_old_mp3s(target_dir=None):
    if target_dir is None:
        target_dir = _script_dir
    data_dir = os.path.join(target_dir, "data")
    if os.path.exists(data_dir):
        try:
            import send2trash
            for f in os.listdir(data_dir):
                if f.endswith(".mp3"):
                    fp = os.path.join(data_dir, f)
                    try:
                        send2trash.send2trash(fp)
                        logger.info(f"Moved old MP3 to trash: {fp}")
                    except Exception:
                        try:
                            os.remove(fp)
                            logger.info(f"Permanently deleted old MP3 (fallback): {fp}")
                        except Exception:
                            pass
        except Exception as e:
            logger.warning(f"Error cleaning up old MP3s in {data_dir}: {e}")

def load_credentials(search_dir=None):
    dirs_to_check = []
    if search_dir:
        dirs_to_check.append(search_dir)
    dirs_to_check.extend([_script_dir, os.getcwd(), os.path.dirname(_script_dir)])

    for d in dirs_to_check:
        cp = os.path.join(d, "credentials.json")
        if os.path.exists(cp):
            logger.info(f"Loading credentials from {cp}")
            with open(cp, "r", encoding="utf-8") as f:
                return json.load(f)
    logger.error("credentials.json not found in search paths.")
    sys.exit(1)

class BaseTranscriptionBot(discord.Client):
    def __init__(self, aqua_key=None, token=None, **kwargs):
        intents = kwargs.pop('intents', discord.Intents.all())
        super().__init__(intents=intents, **kwargs)
        self.tree = app_commands.CommandTree(self)
        self.aqua_key = aqua_key
        self.token = token
        self.last_world_message_time = time.time()
        self.text_channel_id = None
        self._executor = ThreadPoolExecutor(max_workers=1)

    async def setup_hook(self):
        @self.tree.command(name="purge", description="Purge all messages in the world channel")
        async def purge(interaction: discord.Interaction):
            await self.purge_logic(interaction)

        @self.tree.command(name="analyze", description="Count syllables in each line of the world channel and edit messages to display them")
        async def analyze(interaction: discord.Interaction):
            await self.analyze_logic(interaction)

        await self.tree.sync()
        logger.info("Base transcription bot slash commands synced.")

    def find_world_channel(self):
        for guild in self.guilds:
            world_channel = discord.utils.get(guild.text_channels, name="world")
            if world_channel:
                self.text_channel_id = world_channel.id
                logger.info(f"Target '#world' channel found in guild '{guild.name}' (ID: {self.text_channel_id})")
                return world_channel
        logger.warning("Could not find any text channel named 'world' in connected guilds.")
        return None

    async def on_message(self, message):
        if message.channel and hasattr(message.channel, "name") and message.channel.name == "world":
            self.last_world_message_time = time.time()

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

            collected_lines_syls = []
            all_repetitions = []
            histogram = collections.Counter()

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
                    if line.strip().startswith('/'):
                        new_lines.append(line)
                        continue

                    prefix, cleaned_content = _get_cleaned_content(line)
                    if not cleaned_content:
                        new_lines.append(line)
                        continue

                    if cleaned_content.startswith('/'):
                        new_lines.append(line)
                        continue

                    line_syls = get_line_syllables_and_vowels(cleaned_content)
                    total_syls = len(line_syls)

                    new_line = f"{prefix}{cleaned_content} ({total_syls})"

                    if new_line != line:
                        changed = True
                    new_lines.append(new_line)

                    if line_syls:
                        line_idx = len(collected_lines_syls)
                        collected_lines_syls.append(line_syls)

                        L = len(line_syls)
                        for i in range(L - 1, -1, -1):
                            v = line_syls[i]['vowel']
                            if not v:
                                continue
                            for j in range(i - 1, -1, -1):
                                if line_syls[j]['vowel'] == v:
                                    if line_syls[i]['word'].strip().lower() == line_syls[j]['word'].strip().lower():
                                        continue
                                    distance = i - j

                                    l2_syls = line_syls[j + 1 : i + 1]

                                    available = line_syls[0 : i + 1]
                                    if len(available) < 2 * distance:
                                        continue
                                    l1_indices = [j - k for k in range(distance - 1, -1, -1)]
                                    l1_syls = [available[idx] for idx in l1_indices]

                                    if cuts_word_in_half(l1_syls, line_syls) or cuts_word_in_half(l2_syls, line_syls):
                                        continue

                                    histogram[distance] += 1
                                    all_repetitions.append({
                                        'line_index': line_idx,
                                        'start_idx': j,
                                        'end_idx': i,
                                        'distance': distance,
                                        'vowel': v
                                    })

                if changed:
                    edited_content = "\n".join(new_lines)
                    if len(edited_content) > 2000:
                        edited_content = edited_content[:1997] + "..."
                    await msg.edit(content=edited_content)
                    await asyncio.sleep(0.5)

            serializable_histogram = {str(k): v for k, v in histogram.items()}
            vowel_histogram_path = os.path.join(_script_dir, "vowel_histogram.json")
            try:
                with open(vowel_histogram_path, "w") as f:
                    json.dump(serializable_histogram, f, indent=4)
                logger.info(f"Vowel histogram saved to {vowel_histogram_path}")
            except Exception as e:
                logger.error(f"Failed to save vowel histogram: {e}")

            if collected_lines_syls:
                await interaction.followup.send("Generating poem from rhyming lines using backward syllable histogram analysis...")

                best_distance = None
                if histogram:
                    best_distance = max(histogram.keys(), key=lambda d: histogram[d] * d)

                if not best_distance:
                    await interaction.followup.send("Could not identify any repeating vowel sounds in the syllable slots.")
                    return

                syl_pairs = []
                for rep in all_repetitions:
                    if rep['distance'] == best_distance:
                        line_idx = rep['line_index']
                        start_idx = rep['start_idx']
                        end_idx = rep['end_idx']
                        line_syls = collected_lines_syls[line_idx]

                        l2_syls = line_syls[start_idx + 1 : end_idx + 1]

                        available = line_syls[0 : end_idx + 1]
                        if len(available) < 2 * best_distance:
                            continue
                        l1_indices = [start_idx - k for k in range(best_distance - 1, -1, -1)]
                        l1_syls = [available[idx] for idx in l1_indices]

                        l1_plain = reconstruct_line_from_syllables(l1_syls).lower()
                        l2_plain = reconstruct_line_from_syllables(l2_syls).lower()

                        if not any(reconstruct_line_from_syllables(p[0]).lower() == l1_plain and reconstruct_line_from_syllables(p[1]).lower() == l2_plain for p in syl_pairs):
                            syl_pairs.append((l1_syls, l2_syls))

                def merge_syl_chains(chains_list):
                    changed = True
                    while changed:
                        changed = False
                        for i in range(len(chains_list)):
                            for j in range(len(chains_list)):
                                if i == j:
                                    continue
                                end_text = reconstruct_line_from_syllables(chains_list[i][-1]).strip().lower()
                                start_text = reconstruct_line_from_syllables(chains_list[j][0]).strip().lower()
                                if end_text == start_text:
                                    new_chain = chains_list[i] + chains_list[j][1:]
                                    chains_list.pop(max(i, j))
                                    chains_list.pop(min(i, j))
                                    chains_list.append(new_chain)
                                    changed = True
                                    break
                            if changed:
                                break
                    return chains_list

                initial_syl_chains = [[p[0], p[1]] for p in syl_pairs]
                merged_syl_chains = merge_syl_chains(initial_syl_chains)

                original_order = {reconstruct_line_from_syllables(p[0]).strip().lower(): idx for idx, p in enumerate(syl_pairs)}
                merged_syl_chains.sort(key=lambda c: original_order.get(reconstruct_line_from_syllables(c[0]).strip().lower(), 999999))

                filtered_chains = []
                for i, c1 in enumerate(merged_syl_chains):
                    c1_plain = [reconstruct_line_from_syllables(l) for l in c1]
                    contained = False
                    for j, c2 in enumerate(merged_syl_chains):
                        if i == j:
                            continue
                        c2_plain = [reconstruct_line_from_syllables(l) for l in c2]
                        text1 = re.sub(r'\s+', ' ', ' '.join(c1_plain)).strip().lower()
                        text2 = re.sub(r'\s+', ' ', ' '.join(c2_plain)).strip().lower()
                        if text1 == text2:
                            if j < i:
                                contained = True
                                break
                        elif is_chain_contained(c1_plain, c2_plain):
                            contained = True
                            break
                    if not contained:
                        filtered_chains.append(c1)

                poem_lines = []
                seen_line_keys = set()

                for chain in filtered_chains:
                    for line_idx, l_syls in enumerate(chain):
                        bold_indices = set()
                        for k in range(len(l_syls)):
                            v1 = l_syls[k]['vowel']
                            if not v1:
                                continue
                            w1 = l_syls[k]['word'].strip().lower()
                            for other_idx, other_syls in enumerate(chain):
                                if other_idx == line_idx:
                                    continue
                                if k < len(other_syls):
                                    v2 = other_syls[k]['vowel']
                                    w2 = other_syls[k]['word'].strip().lower()
                                    if v2 == v1 and w1 != w2:
                                        bold_indices.add(k)
                                        break

                        formatted_line = reconstruct_line_from_syllables(l_syls, bold_indices=bold_indices)
                        plain_key = strip_unicode_bold(formatted_line)
                        plain_key = re.sub(r'\s+', ' ', plain_key).strip().lower()

                        if plain_key and plain_key not in seen_line_keys:
                            seen_line_keys.add(plain_key)
                            poem_lines.append(formatted_line)

                def get_last_word(line_text):
                    words = re.findall(r"[a-zA-Z0-9']+", line_text)
                    if words:
                        return words[-1].lower()
                    return ""

                filtered_poem_lines = []
                i = 0
                while i < len(poem_lines):
                    if i < len(poem_lines) - 1:
                        w1 = get_last_word(poem_lines[i])
                        w2 = get_last_word(poem_lines[i+1])
                        if w1 and w2 and w1 == w2:
                            logger.info(f"Omit line due to identical last word '{w1}': {poem_lines[i]}")
                            i += 1
                            continue
                    filtered_poem_lines.append(poem_lines[i])
                    i += 1

                poem_lines = filtered_poem_lines

                if poem_lines:
                    response = "\n".join(poem_lines).strip()
                    if len(response) > 1800:
                        response = response[:1800] + "\n\n... (truncated due to length)"

                    await interaction.channel.send(f"**Poem Generated from Analysis (Best Distance: {best_distance}):**\n\n{response}")
                else:
                    await interaction.followup.send(f"Could not find enough repeating lines with distance {best_distance}.")

            await interaction.followup.send("Syllable analysis and poem generation complete.")

        except Exception as e:
            logger.exception(f"Error during analyze (syllables/poem) in {interaction.channel.name}: {e}")
            await interaction.followup.send(f"Error during analyze: {e}")

    async def post_transcription_to_channel(self, text, speaker=None):
        if not self.text_channel_id:
            channel = self.find_world_channel()
            if not channel:
                logger.warning("No '#world' text channel target. Cannot post transcription.")
                return
        else:
            channel = self.get_channel(self.text_channel_id)

        if channel:
            try:
                processed_lines = []
                for l in text.splitlines():
                    processed_lines.append(l)
                    ipa_line = get_ipa_syllables(l)
                    if ipa_line:
                        processed_lines.append(ipa_line)

                speaker_prefix = f"**{speaker}**: " if speaker else ""
                prefix_len = len(speaker_prefix)

                current_chunk = []
                current_len = 0

                for line in processed_lines:
                    line_len = len(line) + 1
                    if current_len + line_len + prefix_len > 1950:
                        if current_chunk:
                            chunk_text = "\n".join(current_chunk)
                            await channel.send(f"{speaker_prefix}{chunk_text}")
                        current_chunk = [line]
                        current_len = line_len
                    else:
                        current_chunk.append(line)
                        current_len += line_len

                if current_chunk:
                    chunk_text = "\n".join(current_chunk)
                    await channel.send(f"{speaker_prefix}{chunk_text}")
                logger.info(f"Transcription posted to #world channel (speaker={speaker}).")
            except Exception as e:
                logger.error(f"Failed to send transcription message: {e}")
        else:
            logger.error(f"Could not retrieve channel for ID {self.text_channel_id}")

    def process_and_post_audio_sync(self, audio_data, speaker=None):
        try:
            raw_text = transcribe_audio(audio_data, self.aqua_key, script_dir=_script_dir)
            if raw_text:
                clean_text = poetic_parse(raw_text)
                logger.info(f"TRANSCRIPTION SUCCESS [{speaker}]: {clean_text}")
                asyncio.run_coroutine_threadsafe(
                    self.post_transcription_to_channel(clean_text, speaker=speaker),
                    self.loop
                )
        except Exception as e:
            logger.error(f"Error in process_and_post_audio_sync: {e}")

    def transcribe_and_post_threadsafe(self, audio_data, speaker=None):
        self._executor.submit(self.process_and_post_audio_sync, audio_data, speaker)

    async def transcribe_and_post_async(self, audio_data, speaker=None):
        loop = asyncio.get_running_loop()
        raw_text = await loop.run_in_executor(self._executor, transcribe_audio, audio_data, self.aqua_key, _script_dir)
        if raw_text:
            clean_text = poetic_parse(raw_text)
            logger.info(f"TRANSCRIPTION SUCCESS [{speaker}]: {clean_text}")
            await self.post_transcription_to_channel(clean_text, speaker=speaker)
