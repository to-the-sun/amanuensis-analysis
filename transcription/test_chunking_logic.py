import asyncio
from unittest.mock import MagicMock, AsyncMock

# Mock llama_query
class MockLlamaQuery:
    def __init__(self):
        self.get_max_context_length = MagicMock(return_value=2048)
        self.count_tokens = MagicMock(side_effect=lambda x: len(x.split()))
        self.count_query_tokens = MagicMock(side_effect=lambda q, s="": len(q.split()) + 20)
        self.run_query = MagicMock(side_effect=lambda q, s="": (f"Poetic: {q[:20]}...", 1.0))

llama_query = MockLlamaQuery()

# Simulating the logic from TranscriptionBot.analyze_logic
async def simulate_analyze_logic(messages):
    print(f"Simulating analyze logic for {len(messages)} messages...")

    context_window_size = llama_query.get_max_context_length()
    MAX_NEW_TOKENS = 128
    current_poetic_phrase = ""
    chunk_messages = []
    chunk_count = 0

    for msg in messages:
        test_chunk = chunk_messages + [msg]
        test_text = "\n".join(test_chunk)

        if current_poetic_phrase:
            prompt = f"Previous most poetic phrase: {current_poetic_phrase}\n\nNew conversation text:\n{test_text}\n\nTask: Identify the single most poetic phrase from all the text provided above (including the previous best). Return ONLY that phrase and nothing else. No preamble, no explanation."
        else:
            prompt = f"Conversation text:\n{test_text}\n\nTask: Identify the single most poetic phrase from the text above. Return ONLY that phrase and nothing else. No preamble, no explanation."

        tokens = llama_query.count_query_tokens(prompt)

        # In our mock, each word is a token.
        # TinyLlama's context window is 2048.
        # If tokens + 128 > 2048, we chunk.
        if tokens + MAX_NEW_TOKENS > context_window_size:
            if chunk_messages:
                chunk_count += 1
                actual_text = "\n".join(chunk_messages)
                if current_poetic_phrase:
                    actual_prompt = f"Previous most poetic phrase: {current_poetic_phrase}\n\nNew conversation text:\n{actual_text}\n\nTask: Identify the single most poetic phrase from all the text provided above (including the previous best). Return ONLY that phrase and nothing else. No preamble, no explanation."
                else:
                    actual_prompt = f"Conversation text:\n{actual_text}\n\nTask: Identify the single most poetic phrase from the text above. Return ONLY that phrase and nothing else. No preamble, no explanation."

                print(f"Processing chunk {chunk_count} of {len(chunk_messages)} messages...")
                response, _ = llama_query.run_query(actual_prompt)
                current_poetic_phrase = response.strip()
                chunk_messages = [msg]
            else:
                chunk_messages = [msg]
        else:
            chunk_messages.append(msg)

    if chunk_messages:
        chunk_count += 1
        actual_text = "\n".join(chunk_messages)
        if current_poetic_phrase:
            actual_prompt = f"Previous most poetic phrase: {current_poetic_phrase}\n\nNew conversation text:\n{actual_text}\n\nTask: Identify the single most poetic phrase from all the text provided above (including the previous best). Return ONLY that phrase and nothing else. No preamble, no explanation."
        else:
            actual_prompt = f"Conversation text:\n{actual_text}\n\nTask: Identify the single most poetic phrase from the text above. Return ONLY that phrase and nothing else. No preamble, no explanation."

        print(f"Processing final chunk {chunk_count} of {len(chunk_messages)} messages...")
        response, _ = llama_query.run_query(actual_prompt)
        current_poetic_phrase = response.strip()

    print(f"Final Poetic Phrase: {current_poetic_phrase}")
    return current_poetic_phrase, chunk_count

async def main():
    # Create a large number of messages to force chunking
    # Mock tokens is len(split()) + 20.
    # To hit 2048 - 128 = 1920 tokens:
    # (MsgWordCount + 20) * MsgCount approx 1920.
    # If MsgWordCount = 50, then 70 * MsgCount = 1920 => MsgCount approx 27.
    # Let's create 100 messages of 50 words each.

    long_msg = " ".join(["word"] * 50)
    messages = [f"Message {i}: {long_msg}" for i in range(100)]

    final_phrase, chunks = await simulate_analyze_logic(messages)

    print(f"Total chunks processed: {chunks}")
    assert chunks > 1, f"Expected more than 1 chunk, got {chunks}"
    print("Simulation successful!")

if __name__ == "__main__":
    asyncio.run(main())
