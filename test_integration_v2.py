import asyncio
import torch
from transformers import pipeline
import unittest
from unittest.mock import MagicMock, patch, AsyncMock
import numpy as np
import sys

# We need to mock the environment for transcription_bot imports
mock_sc = MagicMock()
sys.modules['soundcard'] = mock_sc

import transcription_bot

async def manual_test():
    print("Starting manual integration test for memory seeding...")
    bot = MagicMock()
    mock_channel = AsyncMock()
    bot.get_channel.return_value = mock_channel

    # Define a custom send that prints
    async def mock_send(content):
        print(f"SENDING TO DISCORD: {content}")
    mock_channel.send.side_effect = mock_send

    # We'll use the real pipeline for this test since it's already cached
    sink = transcription_bot.WhisperTranscriptionSink(bot, 123)

    print("Waiting for LLM initialization...")
    while sink.llm_pipe is None:
        await asyncio.sleep(1)

    print("Adding text to memory...")
    sink.transcript_memory = [
        "To the Sun: The stars are like diamonds in the sky.",
        "mind_over_moss: Silence is the loudest sound."
    ]

    print("Lowering threshold for testing...")
    sink.max_tokens = 1

    print("Triggering check...")
    await sink._check_llm_trigger()

    print(f"Final transcript_memory: {sink.transcript_memory}")

    if len(sink.transcript_memory) > 0 and "Legacy:" in sink.transcript_memory[0]:
        print("SUCCESS: Memory seeded with legacy poetic phrase.")
    else:
        print("FAILURE: Memory not seeded correctly.")

    print("Test complete.")

if __name__ == "__main__":
    asyncio.run(manual_test())
