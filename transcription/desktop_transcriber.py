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

try:
    import eng_to_ipa
    import eng_to_ipa.stress as stress_lib
    from eng_to_ipa.transcribe import get_cmu
    ENG_TO_IPA_AVAILABLE = True
except ImportError as e:
    ENG_TO_IPA_AVAILABLE = False
    logger.warning(f"eng-to-ipa library is not available. IPA syllables feature will be disabled: {e}")

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
    # Fricatives
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
    # Split on whitespace to get word-level IPA representations (e.g. "/syl1/syl2/")
    words = ipa_str.split()
    syls = []
    for word in words:
        # Strip outer slashes first (e.g. "syl1/syl2")
        stripped_word = word.strip("/")
        # Split on single slash to get syllables
        syls.extend(stripped_word.split("/"))

    vowels = []
    ipa_vowels_sorted = ["ər", "eɪ", "aʊ", "aɪ", "oʊ", "ɔɪ", "ə", "ɑ", "æ", "ɔ", "ɛ", "ɪ", "ʊ", "u", "i"]
    for syl in syls:
        clean_syl = re.sub(r"[ˈˌ/*]", "", syl)
        found_vowel = None
        for v in ipa_vowels_sorted:
            if v in clean_syl:
                found_vowel = v
                break
        if found_vowel:
            vowels.append(found_vowel)
    return vowels

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
        if ipa_str:
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

def reconstruct_line_from_syllables(syllables_list):
    if not syllables_list:
        return ""
    parts = []
    for idx, item in enumerate(syllables_list):
        syl_text = item['syllable']
        if idx > 0:
            prev_item = syllables_list[idx - 1]
            if prev_item['word'] != item['word']:
                parts.append(' ' + syl_text)
            else:
                parts.append(syl_text)
        else:
            parts.append(syl_text)
    return "".join(parts).strip()

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
        # Truncation would cut a word into two, so return None
        return None, current_syllables
    truncated_line = " ".join(reversed(accumulated_words))
    return truncated_line, current_syllables

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
                    if line.strip().startswith('/'):
                        new_lines.append(line)
                        continue

                    prefix, cleaned_content = self._get_cleaned_content(line)
                    if not cleaned_content:
                        new_lines.append(line)
                        continue

                    if cleaned_content.startswith('/'):
                        new_lines.append(line)
                        continue

                    # Extract aligned syllables and vowels
                    line_syls = get_line_syllables_and_vowels(cleaned_content)
                    total_syls = len(line_syls)

                    new_line = f"{prefix}{cleaned_content} ({total_syls})"

                    if new_line != line:
                        changed = True
                    new_lines.append(new_line)

                    if line_syls:
                        line_idx = len(collected_lines_syls)
                        collected_lines_syls.append(line_syls)

                        # Run backward duplicate vowel sound search for each syllable in this line
                        L = len(line_syls)
                        for i in range(L - 1, -1, -1):
                            v = line_syls[i]['vowel']
                            if not v:
                                continue
                            for j in range(i - 1, -1, -1):
                                if line_syls[j]['vowel'] == v:
                                    distance = i - j

                                    # Line 2 (repeated line)
                                    l2_syls = line_syls[j + 1 : i + 1]

                                    # Line 1 (duplicate line)
                                    available = line_syls[0 : i + 1]
                                    if len(available) < 4 * distance:
                                        continue
                                    l1_indices = [(j - k) % (j + 1) for k in range(distance - 1, -1, -1)]
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
                    await asyncio.sleep(0.5) # Rate limit safety

            # Save the histogram as JSON in the same directory as the script
            serializable_histogram = {str(k): v for k, v in histogram.items()}
            vowel_histogram_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vowel_histogram.json")
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

                # 1. Collect unique pairs of (l1_text, l2_text) for best_distance
                pairs = []
                for rep in all_repetitions:
                    if rep['distance'] == best_distance:
                        line_idx = rep['line_index']
                        start_idx = rep['start_idx']
                        end_idx = rep['end_idx']
                        line_syls = collected_lines_syls[line_idx]

                        # Line 2 (repeated line)
                        l2_syls = line_syls[start_idx + 1 : end_idx + 1]

                        # Line 1 (duplicate line)
                        available = line_syls[0 : end_idx + 1]
                        if len(available) < 4 * best_distance:
                            continue
                        l1_indices = [(start_idx - k) % (start_idx + 1) for k in range(best_distance - 1, -1, -1)]
                        l1_syls = [available[idx] for idx in l1_indices]

                        # Reconstruct text
                        l1_text = reconstruct_line_from_syllables(l1_syls)
                        l2_text = reconstruct_line_from_syllables(l2_syls)

                        if (l1_text, l2_text) not in pairs:
                            pairs.append((l1_text, l2_text))

                # Helper to merge chains
                def merge_chains(chains_list):
                    changed = True
                    while changed:
                        changed = False
                        for i in range(len(chains_list)):
                            for j in range(len(chains_list)):
                                if i == j:
                                    continue
                                if chains_list[i][-1].strip().lower() == chains_list[j][0].strip().lower():
                                    new_chain = chains_list[i] + chains_list[j][1:]
                                    chains_list.pop(max(i, j))
                                    chains_list.pop(min(i, j))
                                    chains_list.append(new_chain)
                                    changed = True
                                    break
                            if changed:
                                break
                    return chains_list

                # Initialize chains
                initial_chains = [[p[0], p[1]] for p in pairs]
                merged_chains = merge_chains(initial_chains)

                # Sort chains by original appearance of their first element
                original_order = {p[0].strip().lower(): idx for idx, p in enumerate(pairs)}
                merged_chains.sort(key=lambda c: original_order.get(c[0].strip().lower(), 999999))

                # Flatten chains into poem_lines without blank lines
                poem_lines = []
                for chain in merged_chains:
                    for line in chain:
                        poem_lines.append(line)

                # Helper to get last word
                def get_last_word(line_text):
                    words = re.findall(r"[a-zA-Z0-9']+", line_text)
                    if words:
                        return words[-1].lower()
                    return ""

                # Filter consecutive lines with identical ending words
                filtered_poem_lines = []
                i = 0
                while i < len(poem_lines):
                    if i < len(poem_lines) - 1:
                        w1 = get_last_word(poem_lines[i])
                        w2 = get_last_word(poem_lines[i+1])
                        if w1 and w2 and w1 == w2:
                            # Omit the first of the two lines
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
                processed_lines = []
                for l in text.splitlines():
                    processed_lines.append(l)
                    ipa_line = get_ipa_syllables(l)
                    if ipa_line:
                        processed_lines.append(ipa_line)
                final_text = "\n".join(processed_lines)
                # Post in the expected format (without user tag since it is desktop-wide capture)
                await channel.send(final_text)
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
