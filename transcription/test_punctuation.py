import unittest
import re

# Mock classes to mimic the structural layout of our bots
class MockAquaTranscriptionSink:
    def _poetic_parse(self, text):
        # Remove any periods that come directly after a capital letter.
        text = re.sub(r'(?<=[A-Z])\.', '', text)
        # Split sentences only on periods, exclamation points, and question marks.
        parts = re.split(r'([.!?])', text)

        lines = []
        for part in parts:
            if part in ".!?":
                if lines:
                    lines[-1] += part
                continue

            clean = part.strip()
            if clean:
                lines.append(clean)
        return '\n'.join(lines)


class MockDesktopTranscriberBot:
    def _poetic_parse(self, text):
        # Remove any periods that come directly after a capital letter.
        text = re.sub(r'(?<=[A-Z])\.', '', text)
        # Split sentences only on periods, exclamation points, and question marks.
        parts = re.split(r'([.!?])', text)
        lines = []
        for part in parts:
            if part in ".!?":
                if lines:
                    lines[-1] += part
                continue
            clean = part.strip()
            if clean:
                lines.append(clean)
        return '\n'.join(lines)


class TestPunctuationRemoval(unittest.TestCase):
    def setUp(self):
        self.sink = MockAquaTranscriptionSink()
        self.bot = MockDesktopTranscriberBot()

    def test_single_capital_period(self):
        input_text = "Hello A. World."
        # Expected: "A" doesn't have a period after it anymore, so "Hello A World."
        expected_output = "Hello A World."
        self.assertEqual(self.sink._poetic_parse(input_text), expected_output)
        self.assertEqual(self.bot._poetic_parse(input_text), expected_output)

    def test_abbreviation_usa(self):
        input_text = "Welcome to the U.S.A."
        # Expected: "Welcome to the USA"
        expected_output = "Welcome to the USA"
        self.assertEqual(self.sink._poetic_parse(input_text), expected_output)
        self.assertEqual(self.bot._poetic_parse(input_text), expected_output)

    def test_mix_of_case(self):
        input_text = "This is a sentence. It has a B. in it. What about a lowercase a. or b.?"
        # "sentence." -> keep period (after lowercase 'e')
        # "B." -> remove period (after uppercase 'B')
        # "lowercase a." -> keep period? Wait, the rule says "remove any periods that come directly after a capital letter".
        # So "lowercase a." has 'a' (lowercase), so the period should NOT be removed.
        # "b.?" -> 'b' is lowercase, '?' is question mark. No period directly after capital anyway.
        # Wait, if we keep period after 'a.', does the parser split on it? Yes, splits on periods, exclamation points, and question marks.
        # Let's see:
        # "This is a sentence. It has a B in it. What about a lowercase a. or b.?"
        # The period in "sentence." remains, "B." becomes "B", "a." remains "a.", so it splits into lines.
        # Let's trace it manually:
        # text = re.sub(r'(?<=[A-Z])\.', '', input_text)
        # -> "This is a sentence. It has a B in it. What about a lowercase a. or b.?"
        # parts = re.split(r'([.!?])', text)
        # -> ["This is a sentence", ".", " It has a B in it", ".", " What about a lowercase a", ".", " or b", "?", ""]
        # recombined parts:
        # -> "This is a sentence.\nIt has a B in it.\nWhat about a lowercase a.\nor b?"

        expected_output = "This is a sentence.\nIt has a B in it.\nWhat about a lowercase a.\nor b.?"
        self.assertEqual(self.sink._poetic_parse(input_text), expected_output)
        self.assertEqual(self.bot._poetic_parse(input_text), expected_output)

    def test_no_capital_periods(self):
        input_text = "Just a normal sentence! With exclamation. And question?"
        expected_output = "Just a normal sentence!\nWith exclamation.\nAnd question?"
        self.assertEqual(self.sink._poetic_parse(input_text), expected_output)
        self.assertEqual(self.bot._poetic_parse(input_text), expected_output)

if __name__ == "__main__":
    unittest.main()
