import os
import io
import re
import logging
import torch
import torch.nn as nn
from torch.nn import functional as F
import sentencepiece as spm
from huggingface_hub import hf_hub_download

logger = logging.getLogger("poem_llm")

# Model configuration for Jyotiprakash4357/poem-llm-small
REPO_ID = "Jyotiprakash4357/poem-llm-small"
EMBED_SIZE = 256
N_LAYERS = 6
N_HEADS = 6
CONTEXT = 256
BIAS = True
DROPOUT = 0.0

# GPU / CPU Device selection
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")
if torch.cuda.is_available():
    logger.info(f"Poem LLM utilizing GPU device: {torch.cuda.get_device_name(0)}")
else:
    logger.info("Poem LLM utilizing CPU device (CUDA unavailable)")

_model = None
_tokenizer = None

class Head(nn.Module):
    def __init__(self, head_size):
        super().__init__()
        self.queries = nn.Linear(EMBED_SIZE, head_size, bias=BIAS)
        self.keys = nn.Linear(EMBED_SIZE, head_size, bias=BIAS)
        self.values = nn.Linear(EMBED_SIZE, head_size, bias=BIAS)
        self.register_buffer('tril', torch.tril(torch.ones(CONTEXT, CONTEXT)))
        self.dropout = nn.Dropout(DROPOUT)

    def forward(self, x):
        BS, SL, VS = x.shape
        q = self.queries(x)
        k = self.keys(x)
        v = self.values(x)
        attn_w = q @ k.transpose(-2, -1) * k.shape[-1]**-0.5
        attn_w = attn_w.masked_fill(self.tril[:SL, :SL] == 0, float('-inf'))
        attn_w = F.softmax(attn_w, dim=-1)
        attn_w = self.dropout(attn_w)
        return attn_w @ v

class Multihead(nn.Module):
    def __init__(self, n_heads, head_size):
        super().__init__()
        self.heads = nn.ModuleList([Head(head_size) for _ in range(n_heads)])
        self.combine = nn.Linear(head_size * n_heads, EMBED_SIZE, bias=BIAS)
        self.dropout = nn.Dropout(DROPOUT)

    def forward(self, x):
        x = torch.cat([head(x) for head in self.heads], dim=-1)
        x = self.combine(x)
        return self.dropout(x)

class ForwardLayer(nn.Module):
    def __init__(self, embed_size):
        super().__init__()
        self.network = nn.Sequential(
            nn.Linear(embed_size, 6 * embed_size, bias=BIAS),
            nn.GELU(),
            nn.Linear(6 * embed_size, embed_size, bias=BIAS),
            nn.Dropout(DROPOUT)
        )

    def forward(self, x):
        return self.network(x)

class Block(nn.Module):
    def __init__(self, n_heads):
        super().__init__()
        head_size = EMBED_SIZE // n_heads
        self.ma = Multihead(n_heads, head_size)
        self.feed_forward = ForwardLayer(EMBED_SIZE)
        self.ln1 = nn.LayerNorm(EMBED_SIZE)
        self.ln2 = nn.LayerNorm(EMBED_SIZE)

    def forward(self, x):
        x = x + self.ma(self.ln1(x))
        x = x + self.feed_forward(self.ln2(x))
        return x

class GPT(nn.Module):
    def __init__(self, vocab_size):
        super().__init__()
        self.vocab_size = vocab_size
        self.embeddings = nn.Embedding(vocab_size, EMBED_SIZE)
        self.positions = nn.Embedding(CONTEXT, EMBED_SIZE)
        self.blocks = nn.Sequential(*[Block(N_HEADS) for _ in range(N_LAYERS)])
        self.ln = nn.LayerNorm(EMBED_SIZE)
        self.final_linear = nn.Linear(EMBED_SIZE, vocab_size, bias=BIAS)

    def forward(self, input_ids, targets=None):
        loss = None
        BS, SL = input_ids.shape
        emb = self.embeddings(input_ids)
        pos = self.positions(torch.arange(SL, device=input_ids.device))
        x = emb + pos
        x = self.blocks(x)
        x = self.ln(x)
        logits = self.final_linear(x)
        if targets is not None:
            BS, SL, VS = logits.shape
            logits = logits.view(BS * SL, VS)
            targets = targets.view(BS * SL)
            loss = F.cross_entropy(logits, targets)
        return logits, loss

def get_tokenizer_and_model():
    """
    Lazily loads and caches the SentencePiece tokenizer and PyTorch GPT model on the target device.
    """
    global _model, _tokenizer
    if _model is not None and _tokenizer is not None:
        return _tokenizer, _model

    logger.info(f"Loading tokenizer and model for {REPO_ID} on device: {DEVICE}...")
    tokenizer_path = hf_hub_download(repo_id=REPO_ID, filename="poems_tokenizer.model")
    _tokenizer = spm.SentencePieceProcessor(model_file=tokenizer_path)
    vocab_size = _tokenizer.get_piece_size()

    model = GPT(vocab_size).to(DEVICE)
    model_path = hf_hub_download(repo_id=REPO_ID, filename="poems_latest.pt")
    with open(model_path, 'rb') as f:
        buffer = io.BytesIO(f.read())
    checkpoint = torch.load(buffer, map_location=DEVICE)
    model.load_state_dict(checkpoint['model_state_dict'])
    model.eval()
    _model = model
    logger.info(f"Successfully loaded {REPO_ID} on {DEVICE}.")
    return _tokenizer, _model

@torch.no_grad()
def score_line_intra(line: str) -> float:
    """
    Computes token cross-entropy loss (intra-line coherence) for a single line of text.
    Lower loss indicates higher grammatical and poetic coherence.
    """
    sp, model = get_tokenizer_and_model()
    tokens = sp.Encode(line)
    if len(tokens) < 2:
        return 999.0

    if len(tokens) > CONTEXT:
        tokens = tokens[:CONTEXT]

    input_ids = torch.tensor(tokens[:-1], dtype=torch.long, device=DEVICE).unsqueeze(0)
    target_ids = torch.tensor(tokens[1:], dtype=torch.long, device=DEVICE).unsqueeze(0)

    logits, _ = model(input_ids)
    loss = F.cross_entropy(logits.view(-1, sp.get_piece_size()), target_ids.view(-1))
    return loss.item()

@torch.no_grad()
def score_inter_line(line1: str, line2: str) -> float:
    """
    Computes cross-entropy loss for line2 conditioned on preceding line1 context.
    Lower loss indicates smoother inter-line transition and higher coherence.
    """
    sp, model = get_tokenizer_and_model()
    tokens1 = sp.Encode(line1)
    tokens2 = sp.Encode(line2)

    combined_tokens = tokens1 + sp.Encode('\n') + tokens2
    if len(combined_tokens) > CONTEXT:
        combined_tokens = combined_tokens[-CONTEXT:]

    num_l2 = len(tokens2)
    if len(combined_tokens) < 2 or num_l2 == 0:
        return 999.0

    input_ids = torch.tensor(combined_tokens[:-1], dtype=torch.long, device=DEVICE).unsqueeze(0)
    target_ids = torch.tensor(combined_tokens[1:], dtype=torch.long, device=DEVICE).unsqueeze(0)

    logits, _ = model(input_ids)

    target_l2 = target_ids[:, -num_l2:]
    logits_l2 = logits[:, -num_l2:, :]

    loss = F.cross_entropy(logits_l2.view(-1, sp.get_piece_size()), target_l2.view(-1))
    return loss.item()

def score_lines_coherence(lines: list[str]) -> list[str]:
    """
    Reorders poem lines judging grammatical and poetic coherence both intra-line and inter-line.
    Returns lines sorted from most coherent (lowest loss) to least coherent.
    """
    if not lines:
        return []
    if len(lines) == 1:
        return lines

    # Pre-calculate intra-line loss for each line
    intra_scores = {line: score_line_intra(line) for line in lines}

    remaining = list(lines)
    # Pick starting line with best (lowest) intra-line loss
    remaining.sort(key=lambda l: intra_scores[l])
    ordered = [remaining.pop(0)]

    # Sequentially pick the next line that optimizes combined intra-line and inter-line transition score
    while remaining:
        prev_line = ordered[-1]
        best_next = None
        best_score = float('inf')

        for candidate in remaining:
            inter_loss = score_inter_line(prev_line, candidate)
            intra_loss = intra_scores[candidate]
            # Combined score giving weight to both intra and inter coherence
            combined_score = 0.5 * intra_loss + 0.5 * inter_loss
            if combined_score < best_score:
                best_score = combined_score
                best_next = candidate

        ordered.append(best_next)
        remaining.remove(best_next)

    return ordered

def reorder_poem_file(file_path: str, prompt: str = None, output_file_path: str = None) -> str:
    """
    Reads an unordered poem text file, evaluates intra and inter-line coherence using Jyotiprakash4357/poem-llm-small,
    and writes the reordered lines (most coherent at top) to output_file_path (or file_path if not specified).
    """
    if not os.path.exists(file_path):
        raise FileNotFoundError(f"File not found: {file_path}")

    with open(file_path, "r", encoding="utf-8") as f:
        file_content = f.read()

    lines = [line.strip() for line in file_content.splitlines() if line.strip()]
    if not lines:
        logger.warning(f"File {file_path} contained no non-empty lines.")
        reordered_content = ""
    else:
        reordered_lines = score_lines_coherence(lines)
        reordered_content = "\n".join(reordered_lines)

    target_path = output_file_path if output_file_path else file_path
    with open(target_path, "w", encoding="utf-8") as f:
        f.write(reordered_content)

    logger.info(f"Reordered poem saved to {target_path} using Jyotiprakash4357/poem-llm-small on device {DEVICE}")
    return target_path
