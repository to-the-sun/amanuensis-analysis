import llama_query

sample_transcription = "I was thinking about the way the sun hits the water in the morning and how it makes everything look like it's covered in diamonds"

prompt = f"Parse the following transcription into smaller coherent phrases, like song lyrics or a poem:\n\n{sample_transcription}"
system_prompt = "You are a poetic assistant. Return ONLY the parsed phrases, one per line. Do not include any other text."

response, duration = llama_query.run_query(prompt, system_prompt)

print("Original:", sample_transcription)
print("-" * 20)
print("Parsed:")
print(response)
print("-" * 20)
print(f"Took {duration:.2f} seconds")
