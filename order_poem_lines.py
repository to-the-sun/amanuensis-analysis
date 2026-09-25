#!/usr/bin/env python3
"""
order_poem_lines.py

Reorders lines from `unordered_poem_lines.txt` into `ordered_poem_lines.txt`
based on intra-line and inter-line grammatical coherence and poetic flow,
following the evaluation criteria in `transcription/grammatical_coherence_instructions.md`.
"""

import os
import re

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
INPUT_FILE = os.path.join(SCRIPT_DIR, "unordered_poem_lines.txt")
OUTPUT_FILE = os.path.join(SCRIPT_DIR, "ordered_poem_lines.txt")

# Words indicating dangling line endings (prepositions, conjunctions, determiners, possessives, auxiliaries)
DANGLING_ENDS = {
    'THE', 'A', 'AN', 'OF', 'FOR', 'TO', 'IN', 'ON', 'WITH', 'BY', 'AT', 'AND', 'BUT', 'OR',
    'SO', 'IF', 'THAT', 'WHICH', 'AS', 'IS', 'WAS', 'BE', 'ARE', 'WERE', 'ITS', 'THEIR', 'MY',
    'YOUR', 'HIS', 'HER', 'SOME', 'ANY', 'INTO', 'FROM', 'ABOUT', 'THOUGH', 'BECAUSE', 'THAN'
}

# Words indicating disfluent or fragment line starts
DISFLUENT_STARTS = {'UH', 'AND', 'OR', 'BUT', 'SO', 'WHICH', 'LIKE', 'OF', 'TO', 'FOR', 'BECAUSE'}

# Known STT stutter artifacts
STUTTER_PATTERNS = ['itit', 'likelike', 'somesome', 'basicbasic', 'supersuper']

# Meta/technical residue phrases
META_PHRASES = [
    "syllable analysis", "and poem generation", "analysis best distance",
    "poem generated from", "costa michelle about the", "like prescient advice from"
]

def clean_tokens(line):
    # Remove syllable count like " (6)" if present
    clean = re.sub(r'\s*\(\d+\)$', '', line).strip()
    return clean.split()

def evaluate_intra_score(line):
    words = clean_tokens(line)
    if not words:
        return 0.0

    score = 7.0
    line_clean = " ".join(words)
    line_lower = line_clean.lower()

    # 1. Deduct heavily for STT stutters / hallucinations
    for st in STUTTER_PATTERNS:
        if st in line_lower:
            score -= 4.0

    # 2. Deduct heavily for meta/technical fragments
    if any(m in line_lower for m in META_PHRASES):
        score -= 5.0

    # 3. Deduct for dangling line ends
    last_word = re.sub(r'[^a-zA-Z]', '', words[-1]).upper() if words else ""
    if last_word in DANGLING_ENDS:
        score -= 1.5

    # 4. Deduct for disfluent line starts
    first_word = re.sub(r'[^a-zA-Z]', '', words[0]).upper() if words else ""
    if first_word in DISFLUENT_STARTS:
        score -= 0.8

    # 5. Reward complete subject-verb clause structures
    clause_patterns = [
        r'^(i|he|she|it|they|we|you)\s+(was|is|are|were|have|had|can|could|will|would|think|thought|know|noticed|wanted|got)\b',
        r'^(people|economy|world|hard drives|gpus|ram)\s+(are|is|seems|need)\b',
        r'^(there|here)\s+(is|are|was|were|ll be)\b'
    ]
    if any(re.search(p, line_lower) for p in clause_patterns):
        score += 2.0

    # Reward standalone complete lines (no dangling end, no disfluent start)
    if last_word not in DANGLING_ENDS and first_word not in DISFLUENT_STARTS:
        score += 1.0

    return max(0.0, min(10.0, score))

def evaluate_inter_transition(line1, line2):
    w1 = clean_tokens(line1)
    w2 = clean_tokens(line2)
    if not w1 or not w2:
        return 5.0

    last1 = re.sub(r'[^a-zA-Z]', '', w1[-1]).upper()
    first2 = re.sub(r'[^a-zA-Z]', '', w2[0]).upper()

    # High transition score if line1 enjambs smoothly into line2
    if last1 in DANGLING_ENDS:
        # If line1 ends with preposition/conjunction/determiner, line2 completing it is natural enjambment
        return 9.0
    elif first2 in ['TO', 'OF', 'AND', 'OR', 'THAT', 'WHICH', 'BECAUSE', 'IN', 'ON', 'WITH', 'BE']:
        return 8.5
    else:
        return 7.0

def group_into_coherent_blocks(lines):
    blocks = []
    i = 0
    N = len(lines)

    while i < N:
        # Find how many consecutive lines form a continuous thought
        block_len = 1
        for l in range(1, 4):
            if i + l < N:
                w_prev = clean_tokens(lines[i + l - 1])
                w_curr = clean_tokens(lines[i + l])
                last_p = re.sub(r'[^a-zA-Z]', '', w_prev[-1]).upper() if w_prev else ""
                first_c = re.sub(r'[^a-zA-Z]', '', w_curr[0]).upper() if w_curr else ""

                # Check if lines[i+l-1] enjambs into lines[i+l]
                if last_p in DANGLING_ENDS or first_c in ['TO', 'OF', 'AND', 'OR', 'THAT', 'WHICH', 'BECAUSE', 'IN', 'ON', 'WITH', 'BE', 'GET', 'DO']:
                    block_len = l + 1
                else:
                    break
            else:
                break

        block_lines = lines[i : i + block_len]

        # Calculate intra-line scores for all lines in the block
        intra_scores = [evaluate_intra_score(l) for l in block_lines]
        avg_intra = sum(intra_scores) / len(intra_scores)

        # Calculate inter-line scores between adjacent lines in block
        if len(block_lines) > 1:
            inter_scores = [evaluate_inter_transition(block_lines[k], block_lines[k+1]) for k in range(len(block_lines)-1)]
            avg_inter = sum(inter_scores) / len(inter_scores)
        else:
            avg_inter = 8.0 if evaluate_intra_score(block_lines[0]) > 6.0 else 4.0

        # Composite score: w_intra = 0.6, w_inter = 0.4 (per grammatical_coherence_instructions.md)
        composite = 0.6 * avg_intra + 0.4 * avg_inter

        blocks.append({
            'lines': block_lines,
            'composite_score': composite,
            'orig_idx': i
        })

        i += block_len

    return blocks

def reorder_lines(input_path, output_path):
    if not os.path.exists(input_path):
        print(f"Error: Input file {input_path} not found.")
        return

    with open(input_path, 'r', encoding='utf-8') as f:
        lines = [line.strip() for line in f if line.strip()]

    blocks = group_into_coherent_blocks(lines)

    # Sort blocks by composite score descending
    blocks.sort(key=lambda b: (-b['composite_score'], b['orig_idx']))

    ordered_lines = []
    for b in blocks:
        for l in b['lines']:
            ordered_lines.append(l)

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write("\n".join(ordered_lines) + "\n")

    print(f"Reordered {len(lines)} lines from {input_path} to {output_path}")

if __name__ == "__main__":
    reorder_lines(INPUT_FILE, OUTPUT_FILE)
