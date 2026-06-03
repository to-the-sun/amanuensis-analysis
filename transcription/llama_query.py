import torch
from transformers import pipeline, AutoConfig, AutoTokenizer
import time
import sys

# Global variable to cache the pipeline and tokenizer
_pipe = None
_tokenizer = None
MODEL_ID = "TinyLlama/TinyLlama-1.1B-Chat-v1.0"

def get_tokenizer():
    global _tokenizer
    if _tokenizer is None:
        _tokenizer = AutoTokenizer.from_pretrained(MODEL_ID)
    return _tokenizer

def get_pipeline():
    global _pipe
    if _pipe is None:
        print(f"Loading model {MODEL_ID} on CPU...")
        start_time = time.time()
        # Using float32 for maximum compatibility on CPU
        _pipe = pipeline("text-generation", model=MODEL_ID, torch_dtype=torch.float32, device="cpu")
        load_time = time.time() - start_time
        print(f"Model loaded in {load_time:.2f} seconds.")
    return _pipe

def get_context_window_size():
    config = AutoConfig.from_pretrained(MODEL_ID)
    return getattr(config, "max_position_embeddings", 2048)

def count_tokens(text):
    tokenizer = get_tokenizer()
    return len(tokenizer.encode(text))

def get_prompt_overhead(system_prompt="You are a helpful and concise assistant."):
    tokenizer = get_tokenizer()
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": ""}, # Placeholder for user content
    ]
    prompt = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    return len(tokenizer.encode(prompt))

def run_query(query, system_prompt="You are a helpful and concise assistant."):
    pipe = get_pipeline()

    messages = [
        {
            "role": "system",
            "content": system_prompt,
        },
        {"role": "user", "content": query},
    ]

    prompt = pipe.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)

    print("Generating response...")
    start_time = time.time()
    # return_full_text=False prevents the prompt from being included in the output
    outputs = pipe(prompt, max_new_tokens=128, do_sample=True, temperature=0.7, top_k=50, top_p=0.95, return_full_text=False)
    gen_time = time.time() - start_time

    response = outputs[0]["generated_text"].strip()

    return response, gen_time

if __name__ == "__main__":
    if len(sys.argv) > 1:
        query = " ".join(sys.argv[1:])
    else:
        query = "Explain what a Self-Similarity Matrix is in the context of audio analysis."

    print(f"Context window size: {get_context_window_size()}")
    print(f"Prompt overhead: {get_prompt_overhead()}")

    response, duration = run_query(query)

    print("\n" + "="*50)
    print(f"QUERY: {query}")
    print("="*50)
    print(response)
    print("="*50)
    print(f"Generation took {duration:.2f} seconds.")

    input("\nPress Enter to exit...")
