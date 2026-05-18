import asyncio
import torch
from transformers import pipeline
import unittest
from unittest.mock import MagicMock, patch
import numpy as np

# We need to mock the environment for transcription_bot imports
import sys
mock_sc = MagicMock()
sys.modules['soundcard'] = mock_sc

import transcription_bot

class TestTranscriptionBotIntegration(unittest.TestCase):
    def setUp(self):
        self.bot = MagicMock()
        self.bot.loop = asyncio.get_event_loop()
        self.text_channel_id = 123

        # Patch pipeline to avoid downloading model during tests if possible,
        # or just let it load since we know it works in this env.
        # To be fast, we'll mock the llm_pipe.

    @patch('transcription_bot.pipeline')
    def test_llm_trigger(self, mock_pipeline):
        # Setup mock pipeline
        mock_pipe = MagicMock()
        mock_pipeline.return_value = mock_pipe

        # Mock tokenizer
        mock_tokenizer = MagicMock()
        mock_pipe.tokenizer = mock_tokenizer

        # Mock encode to return a list of length exceeding threshold
        mock_tokenizer.encode.side_effect = [
            [1]*10,    # first call (small)
            [1]*2000   # second call (large)
        ]

        # Mock apply_chat_template
        mock_tokenizer.apply_chat_template.return_value = "formatted prompt"

        # Mock pipe call
        mock_pipe.return_value = [{"generated_text": "<|assistant|>The moon is beautiful."}]

        # Mock channel
        mock_channel = MagicMock()
        self.bot.get_channel.return_value = mock_channel

        # Instantiate sink
        sink = transcription_bot.WhisperTranscriptionSink(self.bot, self.text_channel_id)
        sink.transcript_memory = ["Test user: Hello world"]

        # Mock run_in_executor
        async def mock_run_in_executor(executor, func, *args):
            return func()
        self.bot.loop.run_in_executor = mock_run_in_executor

        # Run the check
        asyncio.run(sink._check_llm_trigger())

        # Verify LLM was NOT triggered yet (encode returned 10)
        # Wait, I set it to return 10 then 2000.
        # Let's re-run or trace.

    @patch('transcription_bot.pipeline')
    async def async_test_llm_trigger(self, mock_pipeline):
        mock_pipe = MagicMock()
        mock_pipeline.return_value = mock_pipe
        mock_tokenizer = MagicMock()
        mock_pipe.tokenizer = mock_tokenizer
        mock_tokenizer.encode.return_value = [1]*2000
        mock_tokenizer.apply_chat_template.return_value = "formatted prompt"
        mock_pipe.return_value = [{"generated_text": "<|assistant|>The moon is beautiful."}]

        mock_channel = MagicMock()
        self.bot.get_channel.return_value = mock_channel

        sink = transcription_bot.WhisperTranscriptionSink(self.bot, self.text_channel_id)
        sink.transcript_memory = ["User: Poetic text"]

        await sink._check_llm_trigger()

        # Verify channel.send was called with the analysis
        mock_channel.send.assert_called_with("--- **Poetic Analysis** ---\nThe moon is beautiful.")
        self.assertEqual(sink.transcript_memory, [])

if __name__ == "__main__":
    # Since we are in an environment where we can run the real thing,
    # let's just do a small script to verify the logic.

    async def manual_test():
        print("Starting manual integration test...")
        bot = MagicMock()
        bot.get_channel.return_value = MagicMock()
        bot.get_channel.return_value.send = MagicMock(side_effect=lambda x: print(f"SENDING TO DISCORD: {x}"))

        # We'll use the real pipeline for this test since it's already cached
        sink = transcription_bot.WhisperTranscriptionSink(bot, 123)

        print("Adding text to memory...")
        sink.transcript_memory = ["To the Sun: The stars are like diamonds in the sky.", "mind_over_moss: Silence is the loudest sound."]

        print("Lowering threshold for testing...")
        sink.max_tokens = 5

        print("Triggering check...")
        await sink._check_llm_trigger()
        print("Test complete.")

    asyncio.run(manual_test())
