import unittest
import numpy as np
import os
import shutil
from analysis.additive_synthesis import AdditiveSynthesizer, get_demo_sinusoids

class TestAdditiveSynthesis(unittest.TestCase):
    def setUp(self):
        self.synth = AdditiveSynthesizer(sample_rate=16000, duration=1.0)

    def test_add_sinusoids(self):
        self.synth.add_sinusoid(440, 1.0, 0.0)
        self.synth.add_sinusoid(880, 0.5, np.pi)
        self.assertEqual(len(self.synth.sinusoids), 2)
        self.assertEqual(self.synth.sinusoids[0]["freq"], 440.0)
        self.assertEqual(self.synth.sinusoids[1]["amp"], 0.5)

    def test_get_signal_at_step(self):
        self.synth.add_sinusoid(440, 1.0, 0.0)
        self.synth.add_sinusoid(880, 0.5, 0.0)

        sig_1 = self.synth.get_signal_at_step(1)
        sig_2 = self.synth.get_signal_at_step(2)

        self.assertEqual(len(sig_1), 16000)
        self.assertEqual(len(sig_2), 16000)

        # Test math correctness
        t_ref = np.linspace(0, 1.0, 16000, endpoint=False)
        expected_1 = 1.0 * np.sin(2 * np.pi * 440 * t_ref)
        expected_2 = expected_1 + 0.5 * np.sin(2 * np.pi * 880 * t_ref)

        np.testing.assert_allclose(sig_1, expected_1, rtol=1e-5, atol=1e-5)
        np.testing.assert_allclose(sig_2, expected_2, rtol=1e-5, atol=1e-5)

    def test_get_component_signal(self):
        self.synth.add_sinusoid(440, 1.0, 0.0)
        self.synth.add_sinusoid(880, 0.5, 0.0)
        comp_0 = self.synth.get_component_signal(0)
        comp_1 = self.synth.get_component_signal(1)
        comp_invalid = self.synth.get_component_signal(2)

        t_ref = np.linspace(0, 1.0, 16000, endpoint=False)
        expected_0 = 1.0 * np.sin(2 * np.pi * 440 * t_ref)
        expected_1 = 0.5 * np.sin(2 * np.pi * 880 * t_ref)

        np.testing.assert_allclose(comp_0, expected_0, rtol=1e-5, atol=1e-5)
        np.testing.assert_allclose(comp_1, expected_1, rtol=1e-5, atol=1e-5)
        np.testing.assert_allclose(comp_invalid, np.zeros_like(t_ref), rtol=1e-5, atol=1e-5)

    def test_get_equation(self):
        self.synth.add_sinusoid(440, 1.0, 0.0)
        self.synth.add_sinusoid(880, 0.5, np.pi)

        # Plain text equations
        eq_1 = self.synth.get_equation(1, latex=False)
        eq_2 = self.synth.get_equation(2, latex=False)

        # LaTeX equations
        eq_1_latex = self.synth.get_equation(1, latex=True)
        eq_2_latex = self.synth.get_equation(2, latex=True)

        self.assertEqual(eq_1, "p(t) = sin(2*pi*440*t)")
        self.assertEqual(eq_2, "p(t) = sin(2*pi*440*t) + 0.500*sin(2*pi*880*t + pi)")

        self.assertEqual(eq_1_latex, "p(t) = \\sin(2\\pi \\cdot 440 t)")
        self.assertEqual(eq_2_latex, "p(t) = \\sin(2\\pi \\cdot 440 t)+0.500\\sin(2\\pi \\cdot 880 t + \\pi)")

    def test_get_demo_sinusoids(self):
        sq = get_demo_sinusoids("square", 220, 3)
        self.assertEqual(len(sq), 3)
        self.assertAlmostEqual(sq[0][0], 220.0)
        self.assertAlmostEqual(sq[1][0], 660.0)
        self.assertAlmostEqual(sq[2][0], 1100.0)
        self.assertAlmostEqual(sq[0][1], 4.0 / np.pi)

        saw = get_demo_sinusoids("sawtooth", 220, 3)
        self.assertEqual(len(saw), 3)
        self.assertAlmostEqual(saw[0][0], 220.0)
        self.assertAlmostEqual(saw[1][0], 440.0)
        self.assertAlmostEqual(saw[2][0], 660.0)

        tri = get_demo_sinusoids("triangle", 220, 3)
        self.assertEqual(len(tri), 3)
        self.assertAlmostEqual(tri[0][0], 220.0)
        self.assertAlmostEqual(tri[1][0], 660.0)
        self.assertAlmostEqual(tri[2][0], 1100.0)

if __name__ == "__main__":
    unittest.main()
