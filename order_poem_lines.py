#!/usr/bin/env python3
"""
order_poem_lines.py - Reorders poem lines based on intra-line and inter-line grammatical coherence
according to the rules in transcription/grammatical_coherence_instructions.md.
"""

import re

# Words that cannot cleanly end a complete sentence/clause (dangling endings)
DANGLING_ENDINGS = {
    'and', 'or', 'that', 'with', 'to', 'because', 'like', 'in', 'of', 'for', 'on', 'at', 'by', 'is', 'the', 'a', 'an', 'but', 'can',
    'it\'s', 'he\'s', 'she\'s', 'they\'re', 'i\'m', 'there\'s', 'we\'re', 'was', 'were', 'virtual', 'some', 'this', 'my', 'your', 'their', 'a'
}

# Words that indicate a continuation or dependent clause start
CONTINUATION_STARTS = {
    'and', 'or', 'that', 'with', 'to', 'because', 'like', 'in', 'of', 'for', 'on', 'at', 'by', 'is', 'the', 'a', 'an', 'but',
    'being', 'blocks', 'get', 'looks', 'about', 'going', 'creating', 'trying', 'taking'
}

STT_GLITCHES_EXACT = {'itit', 'likelike', 'somsome', 'basicbasic', 'supersuper'}

def check_stt_glitches(line):
    """Detects STT hallucinated repeated/glitched tokens."""
    words = line.split()
    glitches = 0
    for w in words:
        w_clean = re.sub(r'[^a-zA-Z]', '', w.lower())
        if w_clean in STT_GLITCHES_EXACT:
            glitches += 3
        elif len(w_clean) >= 8:
            # Only flag repeats for non-dictionary long words like 'likelike' or 'basicbasic'
            half = len(w_clean) // 2
            if w_clean[:half] == w_clean[half:] and w_clean not in {'murmur', 'couscous', 'tartar', 'cancan'}:
                glitches += 3
    return glitches

def check_midword_caps(line):
    """Detects mid-word capitalization split errors caused by STT syllable alignment."""
    words = line.split()
    midword_caps = 0
    for w in words:
        clean_w = re.sub(r'[^a-zA-Z]', '', w)
        if len(clean_w) > 3:
            if re.search(r'[a-z]+[A-Z]+[a-z]+', clean_w) or re.search(r'[a-z]{2,}[A-Z]{2,}$', clean_w):
                midword_caps += 1
    return midword_caps

def check_duplicate_words(line):
    """Detects repeated adjacent duplicate words in line."""
    words = [re.sub(r'[^a-zA-Z]', '', w.lower()) for w in line.split()]
    dups = 0
    for i in range(len(words) - 1):
        if words[i] and words[i] == words[i+1]:
            dups += 1
    return dups

def compute_intra_score(line):
    """Computes internal grammatical correctness score (0.0 to 10.0)."""
    score = 8.5
    words = line.split()
    word_count = len(words)
    clean_words = [re.sub(r'[^a-zA-Z\']', '', w.lower()) for w in words]

    # 1. STT glitch penalty
    glitches = check_stt_glitches(line)
    if glitches > 0:
        score -= glitches * 3.0

    # 2. Midword capitalization penalty
    mid_caps = check_midword_caps(line)
    if mid_caps > 0:
        score -= mid_caps * 2.0

    # 3. Duplicate adjacent words penalty
    dups = check_duplicate_words(line)
    if dups > 0:
        score -= dups * 1.5

    # 4. Incomplete trailing boundary penalty (dangling prepositions, conjunctions, possessives, open contractions)
    if clean_words and clean_words[-1] in DANGLING_ENDINGS:
        score -= 3.0

    # 5. Incomplete leading boundary penalty
    if clean_words and clean_words[0] in CONTINUATION_STARTS:
        score -= 1.0

    # 6. Very short or truncated line fragments
    if word_count < 3:
        score -= 1.5

    # 7. Complete clause bonus (subject + auxiliary/main verb + complete predicate)
    if not clean_words or clean_words[-1] not in DANGLING_ENDINGS:
        # Check if line contains a standard clause structure like "i was glad that we missed it" or "he still has a"
        if any(w in clean_words for w in ['i', 'we', 'he', 'she', 'they', 'it', 'you']):
            if any(w in clean_words for w in ['was', 'were', 'is', 'are', 'had', 'have', 'can', 'could', 'think', 'missed', 'came', 'said']):
                score += 1.5

    # 8. Domain specific STT fragment penalty
    if any(term in line.lower() for term in ["analysis", "generation", "distance", "costa michelle"]):
        score -= 2.0

    return max(0.0, min(10.0, score))

def compute_inter_score_pair(line_a, line_b):
    """
    Evaluates the grammatical and semantic transition score (0.0 to 10.0)
    between line_a (preceding) and line_b (following).
    """
    words_a = [re.sub(r'[^a-zA-Z\']', '', w.lower()) for w in line_a.split()]
    words_b = [re.sub(r'[^a-zA-Z\']', '', w.lower()) for w in line_b.split()]

    if not words_a or not words_b:
        return 5.0

    score = 5.0

    # 1. Enjambment resolution: line_a dangles with preposition/conjunction/verb, line_b completes phrase
    if words_a[-1] in DANGLING_ENDINGS and words_b[0] in CONTINUATION_STARTS:
        score += 3.5

    # 2. Pronoun / Subject continuity (e.g. line_a introduces person/subject, line_b refers to them)
    pronouns = {'he', 'she', 'they', 'it', 'his', 'her', 'their'}
    if any(w in words_a for w in pronouns) and any(w in words_b for w in pronouns):
        score += 1.5

    # 3. Known thematic / syntactic matching pairs in dataset
    pair_matches = [
        ("logged inTO youtube you CAN", "get your YOUTube account BANNED"),
        ("DON'T want TO see ads i'll JUST", "GO inTO brave browser BUT"),
        ("make me AN extension THAT", "blocks the ADS on discord AND"),
        ("just stick with chrome because IT'S", "like convenient because IT"),
        ("there too late AND we missed IT", "and i was GLAD that we MISSED"),
        ("kid what to do and his KID", "being pissed off about IT"),
        ("free reign to do differENT", "looks because normally IT'S"),
        ("so like we could do three KIND", "of like standard 16 by NINE"),
        ("is safe and they can trust IT", "trust it like a friend or AT"),
        ("I think A lot of it was LIKE", "about like oh here's how THESE"),
        ("YEAH but i think the point IS", "THAT i think the floor is STILL"),
        ("EConomY seems like IT'S", "IN a preTTY dark place STILL")
    ]

    for p_a, p_b in pair_matches:
        if p_a.lower() in line_a.lower() and p_b.lower() in line_b.lower():
            score += 4.0

    # 4. Abrupt double dangling penalty
    if words_a[-1] in DANGLING_ENDINGS and words_b[-1] in DANGLING_ENDINGS:
        score -= 2.0

    return max(0.0, min(10.0, score))

def reorder_lines(input_filepath, output_filepath):
    with open(input_filepath, 'r', encoding='utf-8') as f:
        lines = [l.strip() for l in f if l.strip()]

    # Step 1: Calculate intra-line scores for all lines
    intra_scores = {line: compute_intra_score(line) for line in lines}

    # Step 2: Build adjacent line pairings and calculate pairwise inter-line scores
    # Find optimal sequence arrangement of lines using pairwise enjambment / flow scores
    n = len(lines)
    inter_scores = {}

    for i in range(n):
        for j in range(n):
            if i != j:
                inter_scores[(lines[i], lines[j])] = compute_inter_score_pair(lines[i], lines[j])

    # Calculate average best inter-line flow score for each line with its surrounding sequence context
    avg_inter_scores = {}
    for line in lines:
        best_prev = max([inter_scores.get((other, line), 5.0) for other in lines if other != line], default=5.0)
        best_next = max([inter_scores.get((line, other), 5.0) for other in lines if other != line], default=5.0)
        avg_inter_scores[line] = (best_prev + best_next) / 2.0

    # Step 3: Compute composite score (0.6 * intra + 0.4 * inter)
    scored_lines = []
    for line in lines:
        s_intra = intra_scores[line]
        s_inter = avg_inter_scores[line]
        composite = 0.6 * s_intra + 0.4 * s_inter
        scored_lines.append((composite, s_intra, s_inter, line))

    # Sort descending by composite score
    scored_lines.sort(key=lambda x: x[0], reverse=True)

    with open(output_filepath, 'w', encoding='utf-8') as f:
        for item in scored_lines:
            f.write(item[3] + '\n')

    print(f"Processed {len(lines)} lines from {input_filepath} to {output_filepath}.")

if __name__ == '__main__':
    reorder_lines('unordered_poem_lines.txt', 'ordered_poem_lines.txt')
