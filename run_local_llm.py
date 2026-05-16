import torch
from transformers import pipeline
import time

def run_query(query):
    model_id = "TinyLlama/TinyLlama-1.1B-Chat-v1.0"

    print(f"Loading model {model_id} on CPU...")
    start_time = time.time()
    # Using float32 for maximum compatibility on CPU
    pipe = pipeline("text-generation", model=model_id, torch_dtype=torch.float32, device="cpu")
    load_time = time.time() - start_time
    print(f"Model loaded in {load_time:.2f} seconds.")

    messages = [
        {
            "role": "system",
            "content": "You are a helpful and concise assistant.",
        },
        {"role": "user", "content": query},
    ]

    prompt = pipe.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)

    print("Generating response...")
    start_time = time.time()
    outputs = pipe(prompt, max_new_tokens=128, do_sample=True, temperature=0.7, top_k=50, top_p=0.95)
    gen_time = time.time() - start_time

    response = outputs[0]["generated_text"]
    # Extract only the assistant's response if possible,
    # though TinyLlama template includes the full history.

    return response, gen_time

if __name__ == "__main__":
    query = "Explain what a Self-Similarity Matrix is in the context of audio analysis."
    response, duration = run_query(query)

    print("\n" + "="*50)
    print(f"QUERY: {query}")
    print("="*50)
    print(response)
    print("="*50)
    print(f"Generation took {duration:.2f} seconds.")
