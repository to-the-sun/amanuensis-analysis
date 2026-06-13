import llama_query

sample_transcription = "I was thinking about the way the sun hits the water in the morning and how it makes everything look like it's covered in diamonds"

# Few-shot example to enforce the "split only" rule
prompt = f"""Task: Split the given transcription into short poetic lines. Use ONLY the words from the transcription. Do NOT add any new words.

Transcription: I have a dream that one day this nation will rise up and live out the true meaning of its creed
Poetic Lines:
I have a dream
that one day this nation
will rise up
and live out the true meaning
of its creed

Transcription: {sample_transcription}
Poetic Lines:"""

system_prompt = "You are a poetic assistant that reformats text. You return ONLY the reformatted lines and never add your own words."

# Use temperature 0.0 for deterministic, strict output
response, duration = llama_query.run_query(prompt, system_prompt, temperature=0.1, do_sample=False)

print("Original:", sample_transcription)
print("-" * 20)
print("Parsed:")
print(response)
print("-" * 20)
print(f"Took {duration:.2f} seconds")
