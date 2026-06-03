import torch
from transformers import pipeline
import time
import sys

# Global variable to cache the pipeline
_pipe = None

def get_pipeline():
    global _pipe
    if _pipe is None:
        model_id = "TinyLlama/TinyLlama-1.1B-Chat-v1.0"
        print(f"Loading model {model_id} on CPU...")
        start_time = time.time()
        # Using float32 for maximum compatibility on CPU
        _pipe = pipeline("text-generation", model=model_id, torch_dtype=torch.float32, device="cpu")
        load_time = time.time() - start_time
        print(f"Model loaded in {load_time:.2f} seconds.")
    return _pipe

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

    # Sometimes TinyLlama might still include some tags or artifacts depending on the version/config
    # but return_full_text=False usually solves it for the pipeline.

    return response, gen_time

def get_max_context_length():
    pipe = get_pipeline()
    return getattr(pipe.model.config, "max_position_embeddings", 2048)

def count_tokens(text):
    pipe = get_pipeline()
    return len(pipe.tokenizer.encode(text))

def count_query_tokens(query, system_prompt="You are a helpful and concise assistant."):
    pipe = get_pipeline()
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user", "content": query},
    ]
    prompt = pipe.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
    return len(pipe.tokenizer.encode(prompt))

if __name__ == "__main__":
    if len(sys.argv) > 1:
        query = " ".join(sys.argv[1:])
    else:
        query = "Explain what a Self-Similarity Matrix is in the context of audio analysis."

    response, duration = run_query(query)

    print("\n" + "="*50)
    print(f"QUERY: {query}")
    print("="*50)
    print(response)
    print("="*50)
    print(f"Generation took {duration:.2f} seconds.")

    input("\nPress Enter to exit...")
