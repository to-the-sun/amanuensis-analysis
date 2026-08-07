#!/usr/bin/env python3
"""
Generative Additive Synthesis Executor
Executes the endless algorithmic 'generative' preset using the additive synthesis visualizer,
rendering individual steps and the final wave without requiring command-line arguments.
"""

import sys
import os

# Ensure the parent/sibling directories are in the Python search path if needed
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from additive_synthesis import run_additive_synthesis

class SynthesisConfig:
    """Config class that emulates argparse output namespace."""
    def __init__(self):
        self.demo = "generative"
        self.freq = 220.0
        self.steps = 12
        self.duration = 1.5
        self.sample_rate = 44100
        self.output_dir = "analysis/additive_synthesis_output"
        self.custom = None
        self.no_audio = True       # Disabled for non-interactive/headless environments
        self.interactive = False   # Disabled for non-interactive/headless environments

def main():
    print("=== Executing Generative Additive Synthesis (Zero-Argument Mode) ===")
    config = SynthesisConfig()
    run_additive_synthesis(config)

if __name__ == "__main__":
    main()
