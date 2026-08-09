#!/usr/bin/env python3
"""
Automated Psychoacoustic and Acoustic Analysis Test Suite for Additive Synthesis
Performs quantitative checks on synthesized sinusoids, including:
1. Sethares Roughness (sensory dissonance model)
2. Lower Interval Limit (LIL) violations in low registers
3. Low-frequency beating (unstable, muddy phase cancelations below 250 Hz)
4. Decay-Frequency Coupling (checking that higher frequencies decay faster)
"""

import os
import json
import unittest
import numpy as np


def sethares_roughness_pair(f1, f2, a1, a2):
    """
    Computes Sethares sensory roughness/dissonance for a single pair of frequencies.
    William Sethares, "Relating Tuning and Timbre"
    """
    # Sort frequencies
    if f1 > f2:
        f1, f2 = f2, f1
        a1, a2 = a2, a1

    if f1 == f2 or a1 == 0 or a2 == 0:
        return 0.0

    # Parameters from Sethares
    b1 = 3.5
    b2 = 5.75
    # Scale factor for frequency difference
    # s(f1) = x* / (s1 * f1 + s2) where x* = 0.24, s1 = 0.02, s2 = 4.43
    s = 0.24 / (0.02 * f1 + 4.43)

    pd = s * (f2 - f1)
    d = np.exp(-b1 * pd) - np.exp(-b2 * pd)

    # Sensory roughness is proportional to the product of amplitudes
    return a1 * a2 * d


def calculate_sethares_roughness(frequencies, amplitudes):
    """
    Calculates total Sethares sensory roughness across all pairs of sinusoids.
    """
    n = len(frequencies)
    if n < 2:
        return 0.0

    total_roughness = 0.0
    for i in range(n):
        for j in range(i + 1, n):
            total_roughness += sethares_roughness_pair(
                frequencies[i], frequencies[j],
                amplitudes[i], amplitudes[j]
            )
    return total_roughness


def detect_lil_violations(frequencies, amplitudes, amplitude_threshold=0.1):
    """
    Detects Lower Interval Limit (LIL) violations in low registers (below 250 Hz).
    Returns a list of violation details.
    """
    violations = []
    n = len(frequencies)

    # Filter and sort by frequency
    valid_indices = [i for i in range(n) if amplitudes[i] >= amplitude_threshold]
    valid_indices.sort(key=lambda idx: frequencies[idx])

    for idx_i, i in enumerate(valid_indices):
        for idx_j in range(idx_i + 1, len(valid_indices)):
            j = valid_indices[idx_j]
            f1, f2 = frequencies[i], frequencies[j]
            a1, a2 = amplitudes[i], amplitudes[j]

            if f1 >= 250.0:
                continue

            # Compute interval in semitones
            ratio = f2 / f1
            semitones = 12 * np.log2(ratio)

            # Skip if they are extremely close (less than 1.0 semitone) - handled by beating detection
            if semitones < 1.0:
                continue

            # Lower Interval Limits:
            # Below 100 Hz: Need at least 7 semitones (perfect fifth) or 12 semitones (octave)
            # 100 Hz to 150 Hz: Need at least 4 semitones (major third)
            # 150 Hz to 200 Hz: Need at least 3 semitones (minor third)
            # 200 Hz to 250 Hz: Need at least 2 semitones (major second)
            violation = False
            min_required = 0
            if f1 < 100.0:
                min_required = 7
                if semitones < 7:
                    violation = True
            elif f1 < 150.0:
                min_required = 4
                if semitones < 4:
                    violation = True
            elif f1 < 200.0:
                min_required = 3
                if semitones < 3:
                    violation = True
            elif f1 < 250.0:
                min_required = 2
                if semitones < 2:
                    violation = True

            if violation:
                violations.append({
                    "f1": f1, "f2": f2,
                    "a1": a1, "a2": a2,
                    "interval_semitones": semitones,
                    "min_required_semitones": min_required
                })

    return violations


def detect_low_frequency_beating(frequencies, amplitudes, amplitude_threshold=0.1, max_bass_freq=250.0, beating_hz_threshold=8.0):
    """
    Detects pairs in the bass range that are extremely close in frequency, causing slow,
    chaotic/muddy amplitude beating.
    """
    beating_pairs = []
    n = len(frequencies)

    for i in range(n):
        for j in range(i + 1, n):
            f1, f2 = frequencies[i], frequencies[j]
            a1, a2 = amplitudes[i], amplitudes[j]

            if a1 < amplitude_threshold or a2 < amplitude_threshold:
                continue

            # If at least one is in the bass range and they are very close
            if min(f1, f2) <= max_bass_freq:
                diff = abs(f2 - f1)
                if 0.0 < diff < beating_hz_threshold:
                    beating_pairs.append({
                        "f1": f1, "f2": f2,
                        "a1": a1, "a2": a2,
                        "beating_hz": diff
                    })

    return beating_pairs


def analyze_decay_frequency_coupling(frequencies, decays):
    """
    Checks the correlation between frequencies and decay rates.
    Higher frequencies should decay faster (positive correlation).
    Returns the Pearson correlation coefficient.
    """
    if len(frequencies) < 2:
        return 1.0

    # Check if all decays are 0 (e.g. infinite sustain square wave)
    if all(d == 0.0 for d in decays):
        return None

    # Compute correlation
    corr = np.corrcoef(frequencies, decays)[0, 1]
    return corr


class TestAdditivePresets(unittest.TestCase):
    """Test suite that executes our quantitative psychoacoustic tests on presets."""

    def load_preset(self, filename):
        filepath = os.path.join(os.path.dirname(__file__), filename)
        with open(filepath, "r") as f:
            data = json.load(f)
        # Parse frequencies, amplitudes, decays
        frequencies = [item["freq"] for item in data]
        amplitudes = [item["amp"] for item in data]
        decays = [item.get("decay", 0.0) for item in data]
        return frequencies, amplitudes, decays

    def test_square_preset(self):
        """Tests that the square wave preset is perfectly harmonic and consistent."""
        freqs, amps, decays = self.load_preset("square.json")

        # Square wave is perfectly harmonic, so frequencies must be integer multiples
        fund = freqs[0]
        for f in freqs:
            ratio = f / fund
            # Should be close to an odd integer
            nearest_odd = round(ratio)
            self.assertTrue(nearest_odd % 2 == 1)
            self.assertAlmostEqual(ratio, nearest_odd, places=2)

        # No low-frequency beating in harmonic waves
        beating = detect_low_frequency_beating(freqs, amps)
        self.assertEqual(len(beating), 0, f"Square wave has unexpected low-frequency beating: {beating}")

    def test_bell_preset(self):
        """Tests the bell preset structure."""
        freqs, amps, decays = self.load_preset("bell.json")

        # Bell sound has exponential decays
        corr = analyze_decay_frequency_coupling(freqs, decays)
        # We expect a positive correlation between frequency and decay
        if corr is not None:
            self.assertGreater(corr, 0.0, f"Bell preset decays do not increase with frequency: corr = {corr}")

    def test_complex_preset_constraints(self):
        """
        Tests the complex preset for acoustic clarity.
        We expect the optimized version of complex.json to have:
        1. No low-frequency beating (under 8 Hz) below 250 Hz for active components.
        2. No Lower Interval Limit (LIL) violations.
        3. Clear Decay-Frequency coupling (corr > 0.3).
        4. Reasonable total Sethares Roughness (e.g., < 0.25).
        """
        freqs, amps, decays = self.load_preset("complex.json")

        # 1. Low-frequency beating test
        beating = detect_low_frequency_beating(freqs, amps, amplitude_threshold=0.1, max_bass_freq=250.0, beating_hz_threshold=8.0)
        beating_details = "\n".join([f"  {b['f1']} Hz and {b['f2']} Hz beat at {b['beating_hz']:.2f} Hz" for b in beating])
        self.assertEqual(len(beating), 0, f"Complex preset has muddy low-frequency beating:\n{beating_details}")

        # 2. Lower Interval Limit (LIL) violations test
        lil_violations = detect_lil_violations(freqs, amps, amplitude_threshold=0.1)
        lil_details = "\n".join([f"  {v['f1']} Hz and {v['f2']} Hz: interval {v['interval_semitones']:.2f} semitones (min required {v['min_required_semitones']})" for v in lil_violations])
        self.assertEqual(len(lil_violations), 0, f"Complex preset has LIL violations below 250 Hz:\n{lil_details}")

        # 3. Decay-Frequency Coupling test
        corr = analyze_decay_frequency_coupling(freqs, decays)
        self.assertIsNotNone(corr, "Complex preset must have decaying components.")
        self.assertGreater(corr, 0.3, f"Complex preset lacks proper frequency-decay coupling: correlation is {corr:.2f}")

        # 4. Total Roughness test
        roughness = calculate_sethares_roughness(freqs, amps)
        self.assertLess(roughness, 0.25, f"Complex preset total roughness is too high: {roughness:.4f}")


if __name__ == "__main__":
    unittest.main()
