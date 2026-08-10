#!/usr/bin/env python3
import re
import json
from nltk.corpus import cmudict

# Base starting overrides from the user's prompt
STARTING_OVERRIDES = {
    "and": "/ænd/",
    "diet": "/daɪ/ɪt/",
    "finest": "/faɪ/nɪst/",
    "incompetence": "/ɪn/kˈɑm/pə/tɪns/",
    "endless": "/ˈɛn/dlɪs/",
    "forests": "/ˈfɔr/ɪsts/",
    "forest": "/ˈfɔr/ɪst/",
    "forsaken": "/fɔr/ˈseɪ/kɪn/",
    "places": "/pleɪ/sɪz/",
    "isn't": "/ɪz/ɪnt/",
    "doesn't": "/dəz/ɪnt/"
}

# Mapping from ARPAbet to IPA symbols
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

def add_stress_marks(phones):
    stressed_phones = []
    for p in phones:
        if p[-1].isdigit():
            stress = p[-1]
            base = p[:-1]
            if stress == "1":
                stressed_phones.append("ˈ" + base)
            elif stress == "2":
                stressed_phones.append("ˌ" + base)
            else:
                stressed_phones.append(base)
        else:
            stressed_phones.append(p)
    return stressed_phones

def syllabify_phonemes(phones):
    """Syllabifies a stressed phone list into slash-separated IPA string."""
    vowel_indices = []
    for idx, p in enumerate(phones):
        clean_p = re.sub(r"[ˈˌ\d]", "", p).lower()
        if clean_p in IPA_VOWELS_SET:
            vowel_indices.append(idx)

    if not vowel_indices:
        ipa_val = phonemes_to_ipa(phones)
        return f"/{ipa_val}/"

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
        return f"/{word_ipa_str}/"
    return ""

def main():
    print("Loading CMU Pronouncing Dictionary...")
    d = cmudict.dict()
    
    overrides = dict(STARTING_OVERRIDES)
    
    word_pattern = re.compile(r"^[a-z'\-]+$")
    
    # Process each word in the dictionary
    for word, prons in d.items():
        if word in overrides:
            continue
        if not word_pattern.match(word):
            continue
            
        # We will use the first pronunciation
        pron = prons[0]
        
        # Check matching suffix rules for American English weak vowel restorations
        matched = False
        new_pron = list(pron)
        
        # Rule 1: Endings in -es, -s, -ses etc. after sibilants (pronunciation ends in AH0 Z)
        # e.g., places, presses, raises, matches, houses, bosses
        if word.endswith("es") or word.endswith("s"):
            if len(pron) >= 2 and pron[-2:] == ["AH0", "Z"]:
                # Ensure the preceding sound is a sibilant (S, Z, SH, ZH, CH, JH)
                preceding = pron[-3] if len(pron) >= 3 else ""
                if preceding in ["S", "Z", "SH", "ZH", "CH", "JH"]:
                    new_pron[-2] = "IH0"
                    matched = True
                    
        # Rule 2: Endings in -est, -ests (e.g. finest, forest, forests, greatest)
        if word.endswith("est") or word.endswith("ests"):
            if len(pron) >= 3 and pron[-3:] == ["AH0", "S", "T"]:
                new_pron[-3] = "IH0"
                matched = True
            elif len(pron) >= 4 and pron[-4:] == ["AH0", "S", "T", "Z"]:
                new_pron[-4] = "IH0"
                matched = True
                
        # Rule 3: Endings in -less (e.g. endless, useless)
        if word.endswith("less") or word.endswith("lessness"):
            if len(pron) >= 3 and pron[-3:] == ["AH0", "L", "S"]:
                # change to L IH0 S
                new_pron[-3:] = ["L", "IH0", "S"]
                matched = True
            elif len(pron) >= 4 and pron[-4:] == ["AH0", "L", "S", "T"]:
                new_pron[-4:] = ["L", "IH0", "S", "T"]
                matched = True
                
        # Rule 4: Endings in -ness, -nesses (e.g. sadness, goodness)
        if word.endswith("ness") or word.endswith("nesses"):
            if len(pron) >= 3 and pron[-3:] == ["N", "AH0", "S"]:
                new_pron[-2] = "IH0"
                matched = True
            elif len(pron) >= 5 and pron[-5:] == ["N", "AH0", "S", "AH0", "Z"]:
                new_pron[-4] = "IH0"
                new_pron[-2] = "IH0"
                matched = True

        # Rule 5: Endings in -et, -ets (e.g. diet, quiet, blanket, ticket, bullet)
        if word.endswith("et") or word.endswith("ets"):
            if len(pron) >= 2 and pron[-2:] == ["AH0", "T"]:
                new_pron[-2] = "IH0"
                matched = True
            elif len(pron) >= 3 and pron[-3:] == ["AH0", "T", "S"]:
                new_pron[-3] = "IH0"
                matched = True

        # Rule 6: Endings in -en, -on, -in, -ence, -nt, -em, -om (e.g., button, happen, open, problem, motion, isn't, doesn't, incompetence)
        # Suffixes typically representing a weak short-i /ɪn/ or /ɪt/ or /ɪs/ or /ɪm/ rather than schwa
        if not matched:
            ends_with_suffix = (
                word.endswith("en") or word.endswith("on") or word.endswith("in") or
                word.endswith("ent") or word.endswith("ence") or word.endswith("ency") or
                word.endswith("em") or word.endswith("om") or word.endswith("um") or
                word.endswith("n't") or word.endswith("nt")
            )
            if ends_with_suffix:
                if len(pron) >= 2 and pron[-2:] == ["AH0", "N"]:
                    new_pron[-2] = "IH0"
                    matched = True
                elif len(pron) >= 3 and pron[-3:] == ["AH0", "N", "D"]:
                    new_pron[-3] = "IH0"
                    matched = True
                elif len(pron) >= 3 and pron[-3:] == ["AH0", "N", "T"]:
                    new_pron[-3] = "IH0"
                    matched = True
                elif len(pron) >= 3 and pron[-3:] == ["AH0", "N", "S"]:
                    new_pron[-3] = "IH0"
                    matched = True
                elif len(pron) >= 2 and pron[-2:] == ["AH0", "M"]:
                    new_pron[-2] = "IH0"
                    matched = True
                    
        if matched:
            stressed_phones = add_stress_marks(new_pron)
            ipa_str = syllabify_phonemes(stressed_phones)
            if ipa_str:
                overrides[word] = ipa_str

    print(f"Generated {len(overrides)} total overrides.")
    
    # Write to ipa_overrides.json (using ensure_ascii=False to write actual UTF-8 characters directly)
    output_path = "transcription/ipa_overrides.json"
    print(f"Writing overrides to {output_path}...")
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(overrides, f, indent=4, ensure_ascii=False)
        
    print("Done!")

if __name__ == "__main__":
    main()
