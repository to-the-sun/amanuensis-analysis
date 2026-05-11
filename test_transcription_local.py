import unittest
from unittest.mock import MagicMock, patch
import numpy as np
import collections
import threading

# Minimal implementation of LocalUser as it's simple
class LocalUser:
    def __init__(self, display_name):
        self.display_name = display_name
    def __hash__(self):
        return hash(self.display_name)
    def __eq__(self, other):
        if isinstance(other, LocalUser):
            return self.display_name == other.display_name
        return False

# Minimal implementation of the Sink for testing logic without complex mocks
class WhisperTranscriptionSink:
    def __init__(self):
        self.user_buffers = collections.defaultdict(bytearray)
        self.last_audio_time = collections.defaultdict(float)
        self.lock = threading.Lock()
        self.sample_rate = 48000
        self.channels = 2

    def write_numpy(self, user, data_np: np.ndarray):
        pcm_data = (data_np * 32767).astype(np.int16).tobytes()
        with self.lock:
            self.user_buffers[user].extend(pcm_data)

    def _transcribe(self, audio_bytes, model):
        try:
            audio_np = np.frombuffer(audio_bytes, dtype=np.int16).reshape(-1, self.channels)
            audio_float32 = audio_np.astype(np.float32) / 32768.0
            audio_mono = audio_float32.mean(axis=1)
            audio_16k = audio_mono[::3]
            segments, info = model.transcribe(audio_16k, beam_size=5, language="en")
            segments = list(segments)
            full_text = "".join([s.text for s in segments])
            is_complete = any(s.text.strip().endswith(('.', '!', '?')) for s in segments) if segments else False
            return full_text, is_complete
        except Exception as e:
            return str(e), False

class TestTranscriptionLogic(unittest.TestCase):
    def test_local_user_equality(self):
        user1 = LocalUser("User1")
        user2 = LocalUser("User1")
        user3 = LocalUser("User2")
        self.assertEqual(user1, user2)
        self.assertNotEqual(user1, user3)

    def test_write_numpy(self):
        sink = WhisperTranscriptionSink()
        user = LocalUser("To The Sun")
        data_np = np.zeros((48000, 2), dtype=np.float32)
        sink.write_numpy(user, data_np)
        self.assertEqual(len(sink.user_buffers[user]), 192000)

    def test_transcribe_logic(self):
        sink = WhisperTranscriptionSink()
        mock_model = MagicMock()
        mock_segment = MagicMock()
        mock_segment.text = "Hello world."
        mock_model.transcribe.return_value = ([mock_segment], None)

        user = LocalUser("To The Sun")
        data_np = np.random.uniform(-1, 1, (48000, 2)).astype(np.float32)
        sink.write_numpy(user, data_np)

        audio_bytes = bytes(sink.user_buffers[user])
        text, is_complete = sink._transcribe(audio_bytes, mock_model)

        self.assertEqual(text, "Hello world.")
        self.assertTrue(is_complete)

if __name__ == '__main__':
    unittest.main()
