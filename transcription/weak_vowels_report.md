# PHONETIC ANALYSIS & WEAK VOWEL INTERPRETATION REPORT

## 1. Libraries Engaged in the Transcription Pipeline

The transcription bots (`transcription_bot_aqua.py` and `desktop_transcriber.py`) leverage a powerful chain of phonetic, lexical, and syllabic libraries:

1. **`eng_to_ipa`**: The primary engine used to perform English-to-IPA conversion.
2. **`nltk.corpus.cmudict`**: The Carnegie Mellon University (CMU) Pronouncing Dictionary, which provides phonetic transcriptions of over 134,000 words in ARPAbet format.
3. **`syllables`**: An estimation library used for counting word syllables as a fallback when words are missing from the primary dictionary.
4. **`pronouncing`**: An interface to the CMU dictionary that handles rhyming and stress patterns.
5. **`SoundsLike`**: A phonetic similarity utility used for advanced syllable-by-syllable vowel indexing and stressed vowel extraction.

---

## 2. Step-by-Step Word-to-IPA Syllable Pipeline

When audio is processed and transcribed into text, the following algorithmic sequence occurs to produce and map IPA syllables:

### Step 2.1: Transcription Capture
- Audio is captured (via `voice_recv` on Discord or a loopback soundcard on Desktop) and sent to the **Avalon (AquaVoice) transcription API**.
- The API returns raw English text (e.g., `"The diet is finest and simple."`).

### Step 2.2: Tokenization & Word Retrieval
- The line is tokenized into word lists using regular expressions: `re.findall(r"[a-zA-Z0-9']+", line)`.

### Step 2.3: CMU Dictionary Query
- For each word, the scripts query the CMU Pronouncing Dictionary database via `eng-to-ipa`'s `get_cmu(word)`.
- If a word is missing, it is marked with an asterisk (e.g., `"/diet*/"`) and skipped from vowel extraction.
- If present, CMU returns its **ARPAbet pronunciation** (a set of phonemes represented by uppercase ASCII letters and numerical stress markers, such as `0`, `1`, or `2`).
  - Example: `"diet"` $\rightarrow$ `D AY1 AH0 T`
  - Example: `"finest"` $\rightarrow$ `F AY1 N AH0 S T`
  - Example: `"and"` $\rightarrow$ `AH0 N D` (often returned as the weak connected form) or `AE1 N D`

### Step 2.4: Stress and Syllabification (Maximal Onset Principle)
- Stress rules are applied to the ARPAbet chain to ensure vowel indices are located.
- The syllable boundaries are determined using the **Maximal Onset Principle (MOP)**:
  - Consonants between vowels are shifted into the onset (the start) of the next syllable if they form a phonetically valid English consonant cluster (such as `ST`, `TR`, `SPL`).
  - Otherwise, they split across the coda of the previous syllable and the onset of the next.
- Example boundary calculation for `"diet"` (`D AY1 AH0 T`):
  - Vowels: `AY1` (Index 1) and `AH0` (Index 2).
  - No consonants sit between `AY1` and `AH0`, so the syllables divide cleanly: `['D', 'AY1']` and `['AH0', 'T']`.

### Step 2.5: Mapping to IPA Symbols (`IPA_SYMBOLS_MAP`)
- The segmented ARPAbet phoneme lists are translated to IPA using a static lookup map (`IPA_SYMBOLS_MAP`):
  ```python
  IPA_SYMBOLS_MAP = {
      "a": "ə", "ey": "eɪ", "aa": "ɑ", "ae": "æ", "ah": "ə", "ao": "ɔ",
      "aw": "aʊ", "ay": "aɪ", "ch": "ʧ", "dh": "ð", "eh": "ɛ", "er": "ər",
      "hh": "h", "ih": "ɪ", "jh": "ʤ", "ng": "ŋ", "ow": "oʊ", "oy": "ɔɪ",
      "sh": "ʃ", "th": "θ", "uh": "ʊ", "uw": "u", "zh": "ʒ", "iy": "i", "y": "j"
  }
  ```
- Any stress number is stripped from the phoneme.
- `"diet"` $\rightarrow$ `['D', 'AY1']` / `['AH0', 'T']` $\rightarrow$ `/daɪ/` + `/ət/` $\rightarrow$ `/dˈaɪət/` (using schwa `/ə/`).
- `"finest"` $\rightarrow$ `['F', 'AY1']` / `['N', 'AH0', 'S', 'T']` $\rightarrow$ `/faɪ/` + `/nəst/` $\rightarrow$ `/fˈaɪnəst/` (using schwa `/ə/`).
- `"and"` $\rightarrow$ `['AH0', 'N', 'D']` $\rightarrow$ `/ənd/` (using schwa `/ə/`).

---

## 3. The Root of the Weak Vowel Dilemma

The reason you are seeing a schwa (`ə`) in "and", "diet", and "finest" is due to two core linguistic and technical factors:

### A. The Weak Vowel Merger in General American (GenAm)
Under the hood, `eng_to_ipa` and CMU dict represent standard **General American English**.
- In GenAm, unstressed vowels in suffixes (such as `-est` in *finest*, `-et` in *diet*, or `-ed` in *hunted*) undergo **vowel reduction**.
- While some speakers preserve a distinct "weak-i" sound `/ɪ/` (pronouncing *finest* as `/fˈaɪnɪst/`), the CMU dictionary standardizes these unstressed syllables into the schwa sound **`AH0`**, which translates directly to **`/ə/`**.

### B. Connected-Speech Grammatical Weak Forms
Functional grammatical words (like *and*, *of*, *the*, *to*, *for*) have two pronunciations in English:
1. **Strong/Citation Form**: Pronounced in isolation or when stressed. For *"and"*, this is **`AE1 N D`** (with the short A sound `/æ/` as in *cat*).
2. **Weak/Reduced Form**: Pronounced in rapid, natural, connected speech. For *"and"*, this is **`AH0 N D`** (with a schwa `/ənd/` or even vocalic `/n/`).
- Because `eng-to-ipa` aims to emulate flowing, continuous speech, its internal database frequently defaults to the weak/reduced form **`/ənd/`** for *"and"*, which completely bypasses the strong short-A vowel.

---

## 4. Proposed Algorithmic Solutions

To fix this without changing the core libraries, we can implement four highly effective, elegant, and non-intrusive programmatic solutions within the transcription scripts:

### Solution 1: Citation-Form Override for High-Priority Function Words
We can force grammatical function words (like "and") to always prioritize their citation (strong) forms. This ensures that words containing clear, recognizable vowels are not collapsed into schwas.

* **Implementation Concept:**
  We add a dictionary of exact function-word overrides at the very start of `get_ipa_syllables`:
  ```python
  CIT_FORM_OVERRIDES = {
      "and": "ænd",
      "can": "kæn",
      "that": "ðæt"
  }
  ```
  If `w_lower` is found in `CIT_FORM_OVERRIDES`, we output `/ænd/` directly and skip the CMU lookup entirely. This perfectly solves the *"and"* problem.

---

### Solution 2: Spelling-Aware Weak Vowel Restoration (Suffix Rules)
We can design an intelligent post-processing filter that examines the original spelling of the word and "restores" the unstressed schwa `/ə/` back to its clear spelling-corresponding vowel sound (such as `/ɪ/` or `/ɛ/`).

* **Implementation Concept:**
  In `phonemes_to_ipa`, when converting a syllable, if we encounter an unstressed schwa (`AH0` / `/ə/`), we check the suffix of the original spelling:
  ```python
  def restore_weak_vowels(word, ipa_syllables):
      word_lower = word.lower()
      restored = []

      for syl in ipa_syllables:
          # If the syllable contains a schwa "ə"
          if "ə" in syl:
              # Rule A: -est suffixes (e.g., finest, greatest)
              if word_lower.endswith("est") and syl.endswith("əst"):
                  syl = syl.replace("əst", "ɪst")  # Restore to weak-i /ɪ/

              # Rule B: -et suffixes (e.g., diet, quiet, blanket)
              elif word_lower.endswith("et") and syl.endswith("ət"):
                  syl = syl.replace("ət", "ɪt")   # Restore to weak-i /ɪ/ or /ɛt/

              # Rule C: -less / -ness suffixes (e.g., finest, sadness)
              elif word_lower.endswith("ness") and "ənəs" in syl:
                  syl = syl.replace("ənəs", "ɪnəs")
          restored.append(syl)
      return restored
  ```
  This beautifully resolves *"diet"* $\rightarrow$ `/daɪ.ɪt/` and *"finest"* $\rightarrow$ `/faɪ.nɪst/` while preserving natural schwas in words where they belong (like *about*).

---

### Solution 3: Custom ARPAbet-to-IPA Phoneme Re-Mapping
Currently, both the `ah` and `a` ARPAbet keys map to the schwa `"ə"`.
```python
"a": "ə", "ah": "ə"
```
In many CMU transcriptions, `ah` corresponds to a mid-central unrounded vowel, while `a` is sometimes used for alternative reductions. If we want a clearer phonetic representation, we can re-map or refine how certain ARPAbet unstressed categories map to IPA. However, because CMU uses `AH0` extensively, **Solution 2 (Spelling-Aware Restoration)** is much more precise.

---

### Solution 4: Word-Specific Custom Dictionary (JSON Override)
We can maintain an external `ipa_overrides.json` file. This lets you manually customize the exact IPA string of any word you disagree with.

* **Implementation Concept:**
  ```python
  # Load at startup
  try:
      with open("ipa_overrides.json", "r") as f:
          IPA_OVERRIDES = json.load(f)
  except FileNotFoundError:
      IPA_OVERRIDES = {}
  ```
  Inside `get_ipa_syllables(line)`:
  - If a word is found in `IPA_OVERRIDES`, it instantly uses your preferred syllabified IPA (e.g., `"diet": "/daɪ/ɛt/"`, `"finest": "/faɪ/nɪst/"`). This gives you absolute, 100% fine-grained control over any and all edge cases.

---

## 5. Summary and Next Steps

1. **For "and":** Use **Solution 1** (citation-form override).
2. **For "diet" and "finest":** Use **Solution 2** (spelling-aware suffix restoration).
3. **For manual control over other edge cases:** Use **Solution 4** (custom JSON overrides).
