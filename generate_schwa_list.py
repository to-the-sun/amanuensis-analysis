#!/usr/bin/env python3
import re
from nltk.corpus import cmudict

# Map of ARPAbet (lowercase, stripped of stress) to IPA
IPA_SYMBOLS_MAP = {
    "a": "ə", "ey": "eɪ", "aa": "ɑ", "ae": "æ", "ah": "ə", "ao": "ɔ",
    "aw": "aʊ", "ay": "aɪ", "ch": "ʧ", "dh": "ð", "eh": "ɛ", "er": "ər",
    "hh": "h", "ih": "ɪ", "jh": "ʤ", "ng": "ŋ", "ow": "oʊ", "oy": "ɔɪ",
    "sh": "ʃ", "th": "θ", "uh": "ʊ", "uw": "u", "zh": "ʒ", "iy": "i", "y": "j"
}

def convert_cmu_to_ipa(phonemes):
    """Converts a list of CMU phonemes into a clean, standard IPA string."""
    ipa_pcs = []
    for p in phonemes:
        stress = ""
        if p[-1].isdigit():
            stress = p[-1]
            base = p[:-1].lower()
        else:
            base = p.lower()

        # Translate base using IPA_SYMBOLS_MAP
        ipa_v = IPA_SYMBOLS_MAP.get(base, base)

        # Append stress markers to the vowel
        if stress == "1":
            ipa_pcs.append("ˈ" + ipa_v)
        elif stress == "2":
            ipa_pcs.append("ˌ" + ipa_v)
        else:
            ipa_pcs.append(ipa_v)

    return "".join(ipa_pcs)

def main():
    print("Loading CMU Pronouncing Dictionary...")
    d = cmudict.dict()

    pure_schwa_list = []
    r_schwa_list = []

    # Sort keys alphabetically
    sorted_words = sorted(d.keys())

    for word in sorted_words:
        prons = d[word]

        # Track matches for each type
        matched_ah0_prons = []
        matched_er0_prons = []

        for pron in prons:
            # Find the last vowel phoneme in the pronunciation
            vowels = [ph for ph in pron if ph[-1].isdigit()]
            if vowels:
                last_vowel = vowels[-1]
                if last_vowel == "AH0":
                    matched_ah0_prons.append(pron)
                elif last_vowel == "ER0":
                    matched_er0_prons.append(pron)

        # If the word has any pronunciation ending in AH0, add it
        if matched_ah0_prons:
            pure_schwa_list.append((word, matched_ah0_prons))

        # If the word has any pronunciation ending in ER0, add it
        if matched_er0_prons:
            r_schwa_list.append((word, matched_er0_prons))

    # Write to the final output file
    output_filepath = "schwa_words.txt"
    print(f"Writing results to {output_filepath}...")

    with open(output_filepath, "w", encoding="utf-8") as f:
        f.write("========================================================================\n")
        f.write("COMPREHENSIVE PHONETIC DICTIONARY OF ENGLISH WORDS ENDING IN A SCHWA\n")
        f.write("========================================================================\n\n")

        f.write("This file lists every word in the CMU Pronouncing Dictionary whose final syllable\n")
        f.write("has a vowel sound of a schwa. In General American English and the CMU Pronouncing\n")
        f.write("Dictionary, the schwa sound has two primary representations:\n\n")

        f.write("1. PURE SCHWA (ARPAbet: 'AH0' -> IPA: '/ə/'):\n")
        f.write("   This is the standard, unstressed mid-central vowel. Examples include the final\n")
        f.write("   syllables of words like 'sofa' (/ˈsoʊfə/), 'diet' (/dˈaɪət/), 'finest' (/fˈaɪnəst/),\n")
        f.write("   'pencil' (/pˈɛnsəl/), and 'about' (/əbˈaʊt/).\n\n")

        f.write("2. R-COLORED SCHWA (ARPAbet: 'ER0' -> IPA: '/ər/' or '[ɚ]'):\n")
        f.write("   This is the r-colored or rhoticized central vowel (sometimes referred to as a 'schwer').\n")
        f.write("   Examples include the final syllables of words like 'butter' (/bˈʌtər/), 'actor' (/ˈæktər/),\n")
        f.write("   'teacher' (/tˈiʧər/), and 'doctor' (/dˈɑktər/).\n\n")

        f.write("Below, words are listed alphabetically under their respective phonetic categories.\n")
        f.write("For each word, we display its matching pronunciation(s) in IPA format (inside slashes)\n")
        f.write("and its corresponding raw CMU (ARPAbet) phonemes.\n\n")

        f.write("------------------------------------------------------------------------\n")
        f.write(f"SECTION 1: WORDS WITH A FINAL PURE SCHWA (AH0 / /ə/)\n")
        f.write(f"Total entries: {len(pure_schwa_list)}\n")
        f.write("------------------------------------------------------------------------\n\n")

        for word, prons in pure_schwa_list:
            for pron in prons:
                ipa_str = convert_cmu_to_ipa(pron)
                cmu_str = " ".join(pron)
                f.write(f"{word} -> IPA: /{ipa_str}/ | CMU: {cmu_str}\n")

        f.write("\n\n")
        f.write("------------------------------------------------------------------------\n")
        f.write(f"SECTION 2: WORDS WITH A FINAL R-COLORED SCHWA (ER0 / /ər/)\n")
        f.write(f"Total entries: {len(r_schwa_list)}\n")
        f.write("------------------------------------------------------------------------\n\n")

        for word, prons in r_schwa_list:
            for pron in prons:
                ipa_str = convert_cmu_to_ipa(pron)
                cmu_str = " ".join(pron)
                f.write(f"{word} -> IPA: /{ipa_str}/ | CMU: {cmu_str}\n")

    print(f"Successfully generated {output_filepath}!")
    print(f"Total Pure Schwa matching pronunciations: {sum(len(p) for _, p in pure_schwa_list)}")
    print(f"Total R-colored Schwa matching pronunciations: {sum(len(p) for _, p in r_schwa_list)}")

if __name__ == "__main__":
    main()
