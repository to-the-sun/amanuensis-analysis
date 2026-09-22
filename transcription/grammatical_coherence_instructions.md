# Instructions for Grammatical Reordering of Poem Lines

## Overview & Context
This document serves as an operational instruction manual for AI agents (including future sessions of Jules or local LLMs like TinyLlama/Llama models) tasked with post-processing raw poem lines produced by the transcription bot (`transcription/analyze_transcript.py`).

The transcription bot accumulates spoken speech fragments, transcribes them using speech-to-text (STT), extracts IPA vowels, and groups rhyming syllables. However, because spoken language and transcription outputs often contain fragments, noise, or disjointed clauses, the raw list of generated poem lines (`unordered_poem_lines.txt`) requires reordering based on **grammatical coherence and poetic flow**.

---

## Goal & Target Output
Given a raw list of candidate poem lines, rank and reorder the entire list into a single ordered sequence where:
1. **Top of the List (Rank 1)**: The line / sequence of lines with the highest intra-line grammatical correctness, logical sense, clear syntax, and best poetic flow.
2. **Bottom of the List**: The line / sequence of lines that is the least grammatically coherent, contains broken syntax, hallucinated STT artifacts, or works the least.

---

## Evaluation Criteria

Grammatical consistency must be evaluated along two primary dimensions: **Intra-Line Analysis** and **Inter-Line Analysis**.

### 1. Intra-Line Grammatical Consistency (Internal Line Syntax)
Examine each line independently for grammatical precision and internal integrity:
- **Syntax & Structure**: Does the line form a syntactically valid clause, phrase, or complete sentence? Does it follow standard English constituent order (e.g., Subject-Verb-Object, prepositional phrases)?
- **Subject-Verb Agreement**: Do verbs agree in number and person with their respective subjects?
- **Tense Consistency**: Are tense markers within the line logical and consistent?
- **Word Integrity & STT Noise**: Does the line contain truncated words, accidental homophones, or transcription glitches (e.g., repeated hallucinated words like "the the" or non-sensical token drops)?
- **Semantic Sense**: Does the line express a coherent concept or mental picture, or is it a random scramble of dictionary words?

### 2. Inter-Line Grammatical Consistency & Flow (Contextual Continuity)
Examine how lines connect to and interact with neighboring lines in sequence:
- **Syntactic Continuity**: If a line is an incomplete clause (enjambment), does the preceding or following line logically complete the phrase or clause structure?
- **Antecedent & Pronoun Resolution**: Do pronouns (`he`, `she`, `it`, `they`, `this`) in a line clearly refer to proper nouns or entities in adjacent lines?
- **Semantic & Thematic Flow**: Is there a logical, narrative, or emotional connection between adjacent lines, rather than abrupt contextual jumps?
- **Rhythmic & Cadential Harmony**: Do sentence boundaries, natural pauses, and syllabic stresses create a natural poetic rhythm across line transitions?

---

## Step-by-Step Ranking Methodology

When processing a raw list of lines, execute the following procedure:

### Step 1: Input Parsing & Normalization
- Read all raw lines from `unordered_poem_lines.txt` or the source array.
- Clean minor whitespace artifacts while preserving exact word tokens and punctuation.

### Step 2: Intra-Line Scoring (0.0 to 10.0)
Assign an internal score $S_{\text{intra}}$ to each line based on:
- **10.0**: Flawless grammar, perfectly natural phrasing, clear meaning.
- **7.0–9.0**: Minor poetic license or fragment, but fully grammatical and natural.
- **4.0–6.0**: Awkward phrasing, slight STT transcription flaw, or partial sentence clause.
- **1.0–3.0**: Major grammatical violation, disjointed words, heavy transcription error.
- **0.0**: Complete gibberish or ungrammatical word salad.

### Step 3: Inter-Line Flow & Pairwise Alignment Scoring (0.0 to 10.0)
Evaluate potential ordering combinations of lines. For adjacent lines $(L_i, L_{i+1})$, calculate an inter-line score $S_{\text{inter}}$:
- High score if $L_i$ and $L_{i+1}$ form a smooth compound sentence, enjambed thought, or thematic pair.
- Low score if $L_{i+1}$ sharply contradicts the syntax or context of $L_i$.

### Step 4: Composite Ranking & Global Sorting
Calculate a composite score for each candidate position in the sequence:
$$\text{Score}(L) = w_{\text{intra}} \cdot S_{\text{intra}}(L) + w_{\text{inter}} \cdot S_{\text{inter}}(L_{i-1}, L_i, L_{i+1})$$
*(where $w_{\text{intra}} = 0.6$ and $w_{\text{inter}} = 0.4$)*.

Sort the entire set of lines in **descending order** of their total composite scores.

### Step 5: Final Output Formatting
Write the final ordered set of lines to `ordered_poem_lines.txt` (or present in output):
- Line 1 (Highest score: Most grammatically coherent, flows best).
- Line 2..N-1.
- Line N (Lowest score: Least grammatically coherent, works the least).

---

## Prompt Execution Reference for Future AI Sessions

When invoking an LLM or Jules API within Python scripts (e.g., in `transcription/analyze_transcript.py`), use the following system prompt template to ensure exact adherence to these guidelines:

```
You are an expert linguist, grammarian, and poetry editor.

TASK:
Reorder the following raw list of poem lines based strictly on grammatical coherence and flow.

RULES:
1. Evaluate INTRA-LINE grammar: Check subject-verb agreement, syntactic validity, word order, and absence of STT noise.
2. Evaluate INTER-LINE grammar and flow: Ensure transitions between lines are syntactically and semantically smooth.
3. Place the MOST grammatically coherent line—the one that makes the most sense and flows best—at the VERY TOP (Line 1).
4. Order remaining lines in decreasing order of grammatical consistency and flow.
5. Place the LEAST grammatically coherent line—the one that works the least—at the VERY BOTTOM.
6. Output ONLY the reordered lines, one per line. Do not add line numbers, bullet points, or explanatory text.
```
