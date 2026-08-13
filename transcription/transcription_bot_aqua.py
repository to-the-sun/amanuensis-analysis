import sys
import logging

# --- EARLY CRASH HANDLING ---
def handle_exception(exc_type, exc_value, exc_traceback):
    if issubclass(exc_type, KeyboardInterrupt):
        sys.__excepthook__(exc_type, exc_value, exc_traceback)
        return
    logging.critical("Uncaught exception", exc_info=(exc_type, exc_value, exc_traceback))
    input("\nFATAL ERROR: Script halted. Press Enter to close window...")
    sys.exit(1)

sys.excepthook = handle_exception

try:
    import re
    import os
    import wave
    import io
    import collections
    import time
    import threading
    import json
    import asyncio
    import numpy as np
    import requests
    from typing import Optional
    from concurrent.futures import ThreadPoolExecutor

    # --- LOGGING ---
    logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(name)s - %(message)s')
    logger = logging.getLogger(__name__)
    logging.getLogger('discord').setLevel(logging.WARNING)
    logging.getLogger('discord.ext.voice_recv').setLevel(logging.INFO)

    import nltk
    from nltk.corpus import cmudict
    import syllables

    from SoundsLike.SoundsLike import Word_Functions, Pronunciation_Functions
    import pronouncing

    try:
        import eng_to_ipa
        import eng_to_ipa.stress as stress_lib
        from eng_to_ipa.transcribe import get_cmu
        ENG_TO_IPA_AVAILABLE = True
    except ImportError:
        ENG_TO_IPA_AVAILABLE = False

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

    import discord
    from discord import app_commands
    from discord.ext import voice_recv
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    from discord.gateway import DiscordVoiceWebSocket
    import davey
    from davey import MediaType

    _executor = ThreadPoolExecutor(max_workers=1)

    # --- DAVE DECRYPTION PATCHES ---
    from discord.ext.voice_recv import rtp
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
        self.consecutive_failures = 0
        self.last_success = time.time()
        return _orig_decryptor_init(self, mode, secret_key)
    PacketDecryptor.__init__ = _patched_decryptor_init

    def _decrypt_rtp_aead_aes256_gcm_rtpsize(self, packet):
        packet.adjust_rtpsize()
        header = bytes(packet.header)
        nonce = bytearray(12)
        nonce[:4] = packet.nonce

        # Layer 1: Outer RTP Decryption
        try:
            res = self._aesgcm.decrypt(bytes(nonce), bytes(packet.data), header)
        except Exception as e:
            self.consecutive_failures += 1
            if packet.sequence % 100 == 0:
                logger.error(f"Outer Decryption Failed: SSRC={packet.ssrc}, Seq={packet.sequence}, Err={type(e).__name__}: {e}")
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
                    dec = dave.decrypt(uid, MediaType.audio, res)
                    if seq % 500 == 0:
                        logger.info(f"DAVE OK: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}")
                    self.consecutive_failures = 0
                    self.last_success = time.time()
                    return dec
                except Exception as e:
                    self.consecutive_failures += 1
                    if seq % 100 == 0:
                        logger.warning(f"DAVE FAIL: SSRC={ssrc}, UID={uid}, Seq={seq}, ROC={roc}, Err={type(e).__name__}: {e}")
                    return None

        self.consecutive_failures = 0
        self.last_success = time.time()
        return res

    PacketDecryptor._decrypt_rtp_aead_aes256_gcm_rtpsize = _decrypt_rtp_aead_aes256_gcm_rtpsize

    def _decrypt_rtcp_aead_aes256_gcm_rtpsize(self, data):
        header = data[:8]
        nonce = bytearray(12)
        nonce[:4] = data[-4:]
        ciphertext = data[8:-4]
        try:
            dec = self._aesgcm.decrypt(bytes(nonce), ciphertext, header)
            self.consecutive_failures = 0
            self.last_success = time.time()
            return header + dec
        except Exception as e:
            self.consecutive_failures += 10
            logger.error(f"RTCP Decryption Failed (Failures={self.consecutive_failures}): Err={type(e).__name__}: {e}")
            return data

    PacketDecryptor._decrypt_rtcp_aead_aes256_gcm_rtpsize = _decrypt_rtcp_aead_aes256_gcm_rtpsize

    def _patched_callback(self, packet_data):
        packet = rtp_packet = rtcp_packet = None
        try:
            if not rtp.is_rtcp(packet_data):
                packet = rtp_packet = rtp.decode_rtp(packet_data)
                packet.decrypted_data = self.decryptor.decrypt_rtp(packet)
            else:
                packet = rtcp_packet = rtp.decode_rtcp(self.decryptor.decrypt_rtcp(packet_data))
                if not isinstance(packet, (rtp.ReceiverReportPacket, rtp.SenderReportPacket)):
                    logger.info("Received unexpected rtcp packet: type=%s, %s", packet.type, type(packet))
        except Exception as e:
            if self._is_ip_discovery_packet(packet_data): return
            logger.exception("Error unpacking packet")
        finally:
            if self.error: self.stop(); return
            if not packet: return

        if rtcp_packet:
            self.packet_router.feed_rtcp(rtcp_packet)
        elif rtp_packet:
            ssrc = rtp_packet.ssrc
            if ssrc not in self.voice_client._ssrc_to_id:
                if rtp_packet.is_silence(): return
                else: logger.debug("Received packet for unknown ssrc %s", ssrc)

            self.speaking_timer.notify(ssrc)
            try:
                self.packet_router.feed_rtp(rtp_packet)
            except Exception as e:
                logger.exception('Error processing rtp packet')
                self.error = e; self.stop()

    AudioReader.callback = _patched_callback

    _orig_decode_packet = opus_module.PacketDecoder._decode_packet
    def _patched_decode_packet(self, packet):
        if packet and packet.decrypted_data is None: return packet, b''
        try: return _orig_decode_packet(self, packet)
        except Exception: return packet, b''
    opus_module.PacketDecoder._decode_packet = _patched_decode_packet

    # --- SYLLABLE UTILITIES ---
    try:
        CMU_DICT = cmudict.dict()
    except LookupError:
        nltk.download('cmudict')
        CMU_DICT = cmudict.dict()

    try:
        nltk.data.find('taggers/averaged_perceptron_tagger')
    except LookupError:
        nltk.download('averaged_perceptron_tagger')

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
        clean_word = word.lower().strip(".,!?:;()\"'")
        if not clean_word:
            return 0
        if clean_word in CMU_DICT:
            return max([len([y for y in x if y[-1].isdigit()]) for x in CMU_DICT[clean_word]])
        return syllables.estimate(clean_word)

    def get_last_stressed_vowel_sound(word):
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
            else:
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
            # Truncation would cut a word into two, so return None
            return None, current_syllables
        truncated_line = " ".join(reversed(accumulated_words))
        return truncated_line, current_syllables

    # --- TRANSCRIPTION SINK ---
    class AquaTranscriptionSink(voice_recv.AudioSink):
        def __init__(self, bot, text_id):
            super().__init__()
            self.bot = bot
            self.text_id = text_id
            self.lock = threading.Lock()

            # VAD State variables per user
            self.is_active = collections.defaultdict(bool)
            self.active_buffers = collections.defaultdict(bytearray)
            self.pre_roll_buffers = collections.defaultdict(lambda: collections.deque(maxlen=15)) # 15 chunks of 20ms = 300ms lookahead
            self.silence_durations = collections.defaultdict(float)
            self.utterance_durations = collections.defaultdict(float)
            self.last_audio_times = collections.defaultdict(float)
            self.completed_utterances = collections.defaultdict(list)
            self.to_be_sent_buffers = collections.defaultdict(bytearray)
            self.accumulated_start_times = collections.defaultdict(lambda: None)

            self.processing_task = self.bot.loop.create_task(self._process_buffers())

        def cleanup(self):
            self.processing_task.cancel()

        def wants_opus(self): return False

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

        def write(self, user, data):
            if not data.pcm:
                return

            with self.lock:
                self.last_audio_times[user] = time.time()

                # Compute RMS of the incoming chunk
                chunk_np = np.frombuffer(data.pcm, dtype=np.int16)
                if len(chunk_np) == 0:
                    return
                rms = np.sqrt(np.mean(chunk_np.astype(np.float64)**2))

                # data.pcm contains stereo 16-bit PCM (48kHz, 2 channels, 2 bytes per sample -> 4 bytes per frame)
                chunk_len_bytes = len(data.pcm)
                chunk_duration = chunk_len_bytes / (48000 * 4)

                is_active = self.is_active[user]
                pre_roll = self.pre_roll_buffers[user]

                if not is_active:
                    # Idle state: update pre-roll buffer
                    pre_roll.append(data.pcm)
                    if rms > 50.0:
                        logger.info(f"VAD: Voice started for {user} (RMS: {rms:.1f})")
                        self.is_active[user] = True
                        # Assemble the initial buffer using pre-roll data
                        self.active_buffers[user] = bytearray()
                        for c in pre_roll:
                            self.active_buffers[user].extend(c)
                        pre_roll.clear()
                        self.silence_durations[user] = 0.0
                        self.utterance_durations[user] = chunk_duration
                else:
                    # Active state: accumulate audio
                    self.active_buffers[user].extend(data.pcm)
                    self.utterance_durations[user] += chunk_duration

                    if rms < 50.0:
                        self.silence_durations[user] += chunk_duration
                    else:
                        self.silence_durations[user] = 0.0

                    # Silence timeout (greater than 1.0 seconds) or max duration limits (45 seconds)
                    if self.silence_durations[user] >= 1.0 or self.utterance_durations[user] >= 45.0:
                        reason = "silence" if self.silence_durations[user] >= 1.0 else "max duration"
                        logger.info(f"VAD: Voice ended for {user} due to {reason} (duration: {self.utterance_durations[user]:.2f}s)")

                        audio_data = bytes(self.active_buffers[user])

                        # Accumulate in the to-be-sent buffer
                        self.to_be_sent_buffers[user].extend(audio_data)
                        if self.accumulated_start_times[user] is None:
                            self.accumulated_start_times[user] = time.time()

                        accum_len_bytes = len(self.to_be_sent_buffers[user])
                        accum_duration = accum_len_bytes / (48000 * 4)

                        if accum_duration >= 10.0:
                            logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (>= 10s). Sending to transcription...")
                            self.completed_utterances[user].append(bytes(self.to_be_sent_buffers[user]))
                            self.to_be_sent_buffers[user] = bytearray()
                            self.accumulated_start_times[user] = None
                        else:
                            logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (< 10s). Waiting for more speech to fill the time...")

                        # Reset state for this user
                        self.is_active[user] = False
                        self.active_buffers[user] = bytearray()
                        self.silence_durations[user] = 0.0
                        self.utterance_durations[user] = 0.0

        async def _process_buffers(self):
            while True:
                try:
                    await asyncio.sleep(0.5)
                    users_to_process = []
                    now = time.time()

                    with self.lock:
                        # Check for 49-second flush timeout based on last message in the world channel
                        for user, buf in list(self.to_be_sent_buffers.items()):
                            if len(buf) > 0 and self.accumulated_start_times[user] is not None:
                                if now - self.bot.last_world_message_time > 49.0:
                                    logger.info(f"VAD: Flush timeout reached (49 seconds of no world messages) for {user}. Sending accumulated speech anyway...")
                                    self.completed_utterances[user].append(bytes(buf))
                                    self.to_be_sent_buffers[user] = bytearray()
                                    self.accumulated_start_times[user] = None

                        # Force endpointing for active users if we haven't received audio packets in over 1.0 seconds
                        for user in list(self.is_active.keys()):
                            if self.is_active[user]:
                                time_since = now - self.last_audio_times[user]
                                if time_since >= 1.0:
                                    logger.info(f"VAD: Voice ended for {user} due to packet timeout (duration: {self.utterance_durations[user]:.2f}s)")
                                    audio_data = bytes(self.active_buffers[user])

                                    # Accumulate in the to-be-sent buffer
                                    self.to_be_sent_buffers[user].extend(audio_data)
                                    if self.accumulated_start_times[user] is None:
                                        self.accumulated_start_times[user] = time.time()

                                    accum_len_bytes = len(self.to_be_sent_buffers[user])
                                    accum_duration = accum_len_bytes / (48000 * 4)

                                    if accum_duration >= 10.0:
                                        logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (>= 10s). Sending to transcription...")
                                        self.completed_utterances[user].append(bytes(self.to_be_sent_buffers[user]))
                                        self.to_be_sent_buffers[user] = bytearray()
                                        self.accumulated_start_times[user] = None
                                    else:
                                        logger.info(f"VAD: Accumulated speech duration for {user} is {accum_duration:.2f}s (< 10s). Waiting for more speech to fill the time...")

                                    self.is_active[user] = False
                                    self.active_buffers[user] = bytearray()
                                    self.silence_durations[user] = 0.0
                                    self.utterance_durations[user] = 0.0

                        # Collect completed utterances to process
                        for user in list(self.completed_utterances.keys()):
                            while self.completed_utterances[user]:
                                audio_bytes = self.completed_utterances[user].pop(0)
                                users_to_process.append((user, audio_bytes))

                    for user, audio_bytes in users_to_process:
                        audio_np = np.frombuffer(audio_bytes, dtype=np.int16)
                        if len(audio_np) == 0:
                            continue

                        # Handle potential odd frame length boundary issues
                        if len(audio_np) % 2 != 0:
                            audio_np = audio_np[:(len(audio_np) // 2) * 2]

                        audio_float32 = audio_np.reshape(-1, 2).astype(np.float32) / 32768.0
                        mono_16k = audio_float32.mean(axis=1)[::3]
                        text = await self.bot.loop.run_in_executor(_executor, self._transcribe, mono_16k)
                        if text:
                            clean_text = self._poetic_parse(text)
                            logger.info(f"AQUA RESULT [{user}]:\n{text}")
                            channel = self.bot.get_channel(self.text_id)
                            if channel:
                                processed_lines = []
                                for l in clean_text.splitlines():
                                    processed_lines.append(l)
                                    ipa_line = get_ipa_syllables(l)
                                    if ipa_line:
                                        processed_lines.append(ipa_line)

                                # Send in chunks of 1950 characters
                                current_chunk = []
                                current_len = 0
                                user_prefix = f"**{user}**: "
                                prefix_len = len(user_prefix)

                                for line in processed_lines:
                                    line_len = len(line) + 1  # include newline char
                                    if current_len + line_len + prefix_len > 1950:
                                        if current_chunk:
                                            chunk_text = "\n".join(current_chunk)
                                            await channel.send(f"{user_prefix}{chunk_text}")
                                        current_chunk = [line]
                                        current_len = line_len
                                    else:
                                        current_chunk.append(line)
                                        current_len += line_len

                                if current_chunk:
                                    chunk_text = "\n".join(current_chunk)
                                    await channel.send(f"{user_prefix}{chunk_text}")
                except Exception as e:
                    logger.error(f"Error in processing loop: {e}")

        def _transcribe(self, audio_16k):
            try:
                # Convert float32 mono_16k back to int16 PCM
                audio_int16 = (audio_16k * 32767).astype(np.int16)

                # Save MP3 copy in data/ folder
                try:
                    from pydub import AudioSegment
                    data_dir = os.path.join(_script_dir, "data")
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
                headers = {"Authorization": f"Bearer {AQUA_KEY}"}
                files = {"file": (buffer.name, buffer, "audio/wav")}
                data = {"model": "avalon-v1.5"}

                response = requests.post(url, headers=headers, files=files, data=data)
                response.raise_for_status()
                return response.json().get("text", "")
            except Exception as e:
                logger.error(f"Aqua transcription error: {e}")
                return ""

    # --- BOT CLIENT ---
    class TranscriptionBot(discord.Client):
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

        def __init__(self):
            super().__init__(intents=discord.Intents.all())
            self.tree = app_commands.CommandTree(self)
            self.last_world_message_time = time.time()

        async def setup_hook(self):
            @self.tree.command(name="purge", description="Purge all messages in the world channel")
            async def purge(interaction: discord.Interaction):
                await self.purge_logic(interaction)

            @self.tree.command(name="analyze", description="Count syllables in each line of the world channel and edit messages to display them")
            async def analyze(interaction: discord.Interaction):
                await self.analyze_logic(interaction)

            self.loop.create_task(self.health_check_loop())
            await self.tree.sync()
            logger.info("Application commands synced and health check loop started.")

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
                                        if line_syls[i]['word'].strip().lower() == line_syls[j]['word'].strip().lower():
                                            continue
                                        distance = i - j

                                        # Line 2 (repeated line)
                                        l2_syls = line_syls[j + 1 : i + 1]

                                        # Line 1 (duplicate line)
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
                            if len(available) < 2 * best_distance:
                                continue
                            l1_indices = [start_idx - k for k in range(best_distance - 1, -1, -1)]
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

        async def connect_to_world(self, guild):
            vc_channel = discord.utils.get(guild.voice_channels, name="world")
            text_channel = discord.utils.get(guild.text_channels, name="world")

            if vc_channel and text_channel:
                try:
                    if guild.voice_client:
                        if guild.voice_client.is_connected():
                            return
                        else:
                            await guild.voice_client.disconnect(force=True)

                    client = await vc_channel.connect(cls=voice_recv.VoiceRecvClient)
                    client.listen(AquaTranscriptionSink(self, text_channel.id))
                    logger.info(f"Connected to {vc_channel.name} in {guild.name}. Listening...")
                except Exception as e:
                    logger.error(f"Failed to connect to voice in {guild.name}: {e}")
            else:
                if not vc_channel:
                    logger.warning(f"Voice channel 'world' not found in {guild.name}")
                if not text_channel:
                    logger.warning(f"Text channel 'world' not found in {guild.name}")

        async def health_check_loop(self):
            await self.wait_until_ready()
            while not self.is_closed():
                try:
                    await asyncio.sleep(20)
                    for vc in self.voice_clients:
                        if not isinstance(vc, voice_recv.VoiceRecvClient):
                            continue

                        reader = getattr(vc, '_reader', None)
                        if reader:
                            decryptor = getattr(reader, 'decryptor', None)
                            if decryptor:
                                failures = getattr(decryptor, 'consecutive_failures', 0)
                                last_success = getattr(decryptor, 'last_success', 0)
                                if failures > 100 or (failures > 0 and time.time() - last_success > 30):
                                    reason = f"Excessive failures ({failures})" if failures > 100 else f"Decryption stalled for {time.time() - last_success:.1f}s"
                                    logger.warning(f"{reason} detected in {vc.guild.name}. Reconnecting...")
                                    await vc.disconnect(force=True)
                                    await self.connect_to_world(vc.guild)
                        else:
                            # If we are connected but not listening, maybe we should start listening?
                            # Or just wait for the next iteration to connect_to_world if it's completely disconnected
                            if not vc.is_connected():
                                await self.connect_to_world(vc.guild)

                except Exception as e:
                    logger.error(f"Error in health check loop: {e}")

        async def on_ready(self):
            logger.info(f'Logged in as {self.user} (ID: {self.user.id})')
            for guild in self.guilds:
                await self.connect_to_world(guild)

        async def on_message(self, message):
            if message.channel and hasattr(message.channel, "name") and message.channel.name == "world":
                self.last_world_message_time = time.time()

    # --- MAIN ---
    if __name__ == '__main__':
        # Cleanup old MP3 files in data/ directory using send2trash (with os.remove fallback)
        try:
            import send2trash
            data_dir = os.path.join(_script_dir, "data")
            os.makedirs(data_dir, exist_ok=True)
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
            logger.warning(f"Error cleaning up old MP3s: {e}")

        with open('credentials.json', 'r') as f:
            config = json.load(f)
        TOKEN = config['token']
        AQUA_KEY = config['aqua_key']

        if not discord.opus.is_loaded():
            try: discord.opus.load_opus('libopus.so.0')
            except: pass

        bot = TranscriptionBot()
        bot.run(TOKEN)

except Exception as e:
    logging.error(f"STARTUP ERROR: {type(e).__name__}: {e}", exc_info=True)
    input("\nFATAL ERROR: Script halted during startup. Press Enter to close window...")
    sys.exit(1)
