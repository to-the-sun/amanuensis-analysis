#!/usr/bin/env python3
import re

def clean_word(w):
    return re.sub(r'[^a-zA-Z0-9\']', '', w).lower()

def get_glitch_penalty(line):
    penalty = 0.0
    lower = line.lower()

    # Meta / system pipeline outputs
    if any(m in lower for m in ["analysis best distance", "syllable analysis", "poem generated from", "and poem generation"]):
        return 100.0

    # Severe stutters / glued words: 'itit', 'likelike', 'basicbasic', 'somesome', 'supersuper'
    if re.search(r'\b(itit|likelike|basicbasic|somesome|supersuper)\b', lower) or "itit" in lower or "likelike" in lower or "basicbasic" in lower or "somesome" in lower or "supersuper" in lower:
        return 50.0

    # Repeated adjacent words
    words = [clean_word(w) for w in line.split() if clean_word(w)]
    for i in range(len(words) - 1):
        if words[i] == words[i+1] and len(words[i]) > 1:
            penalty += 15.0

    return penalty

def eval_intraline(line):
    raw = line.strip()
    words = [w for w in raw.split() if w]
    if not words:
        return 0.0

    penalty = get_glitch_penalty(raw)
    if penalty >= 50.0:
        return max(0.0, 10.0 - penalty)

    score = 7.0
    clean_words = [clean_word(w) for w in words]

    dangling_endings = {
        "the", "a", "an", "of", "in", "on", "at", "to", "for", "with", "from", "by",
        "about", "and", "but", "or", "so", "than", "as", "into", "onto", "is", "was",
        "were", "are", "be", "being", "been", "that's", "it's", "which", "whose",
        "where", "when", "why", "how", "if", "their", "your", "my", "his", "her",
        "like", "just", "or", "some", "this", "that", "these", "those"
    }

    last_word = clean_words[-1] if clean_words else ""
    first_word = clean_words[0] if clean_words else ""

    subjects = {"i", "you", "he", "she", "it", "we", "they", "people", "gpus", "economy", "world", "kid", "agent", "agents", "drive", "drives", "price", "prices", "maggie", "hardware", "models", "ram", "chrome", "youtube", "discord", "browser"}
    verbs = {"is", "are", "was", "were", "have", "has", "had", "do", "does", "did", "can", "could", "will", "would", "should", "think", "said", "missed", "buying", "need", "run", "blocks", "get", "got", "working", "going", "started", "wants", "hacked", "taking", "trust", "took", "hits", "tripling", "running", "freeze", "like", "love", "feel", "feels", "know", "knows"}

    has_sub = any(w in subjects for w in clean_words)
    has_v = any(w in verbs for w in clean_words)

    if has_sub and has_v and last_word not in dangling_endings:
        score += 3.0
    elif has_sub and has_v:
        score += 1.0

    if last_word in dangling_endings:
        score -= 2.5

    if first_word in {"and", "or", "but", "so", "of", "than", "which", "because", "as"}:
        score -= 1.0

    return max(0.0, min(10.0, score))

def eval_interline(line_a, line_b):
    words_a = [clean_word(w) for w in line_a.split() if clean_word(w)]
    words_b = [clean_word(w) for w in line_b.split() if clean_word(w)]
    if not words_a or not words_b:
        return 0.0

    last_a = words_a[-1]
    first_b = words_b[0]

    preps_conjs = {"of", "on", "in", "at", "to", "for", "with", "from", "by", "about", "the", "a", "an", "and", "but", "or", "because", "which", "that", "is", "was", "were", "are", "be", "been", "like", "their", "your", "my", "his", "her"}

    score = 5.0
    if last_a in preps_conjs:
        if first_b not in {"and", "or", "but", "so", "because", "which"}:
            score += 3.5
        else:
            score -= 2.0

    subjects = {"i", "you", "he", "she", "it", "we", "they", "people", "gpus", "economy", "maggie", "models", "agent", "agents", "weights", "neurons"}
    verbs = {"is", "are", "was", "were", "have", "has", "had", "do", "does", "did", "can", "could", "will", "would", "should", "think", "said", "missed", "buying", "need", "run", "blocks", "get", "got", "working", "going", "started", "wants", "hacked", "taking", "trust", "took", "hits"}

    if last_a in subjects and first_b in verbs:
        score += 4.0

    if last_a == first_b:
        score -= 3.0

    return max(0.0, min(10.0, score))

def partition_and_order(lines):
    blocks = []
    curr = [lines[0]]

    for i in range(1, len(lines)):
        prev = lines[i-1]
        nxt = lines[i]

        pen_p = get_glitch_penalty(prev)
        pen_n = get_glitch_penalty(nxt)

        if pen_p == 0 and pen_n == 0 and eval_interline(prev, nxt) >= 5.0:
            curr.append(nxt)
        else:
            blocks.append(curr)
            curr = [nxt]

    if curr:
        blocks.append(curr)

    def score_block(b):
        intras = [eval_intraline(l) for l in b]
        avg_intra = sum(intras) / len(intras)

        if len(b) > 1:
            inters = [eval_interline(b[i], b[i+1]) for i in range(len(b)-1)]
            avg_inter = sum(inters) / len(inters)
        else:
            avg_inter = 5.0

        max_pen = max(get_glitch_penalty(l) for l in b)

        last_clean = clean_word(b[-1].split()[-1]) if b[-1].split() else ""
        dangling_endings = {"the", "a", "an", "of", "in", "on", "at", "to", "for", "with", "from", "by", "about", "and", "but", "or", "so", "than", "as", "like", "just"}
        if last_clean in dangling_endings:
            avg_intra -= 1.0

        comp = 0.6 * avg_intra + 0.4 * avg_inter
        return comp - max_pen

    blocks_scored = [(score_block(b), b) for b in blocks]
    blocks_scored.sort(key=lambda x: x[0], reverse=True)

    final_lines = []
    for s, b in blocks_scored:
        final_lines.extend(b)

    return final_lines

if __name__ == "__main__":
    with open("unordered_poem_lines.txt", "r", encoding="utf-8") as f:
        lines = [l.strip() for l in f if l.strip()]

    reordered = partition_and_order(lines)

    with open("ordered_poem_lines.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(reordered) + "\n")

    print(f"Reordered {len(reordered)} lines output to ordered_poem_lines.txt.")
