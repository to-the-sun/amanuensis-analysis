import os
import json
import importlib.util
import numpy as np
from mido import Message
import audio_engine

def load_module_from_path(module_name, file_path):
    spec = importlib.util.spec_from_file_location(module_name, file_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    sounds_dir = os.path.join(script_dir, "sounds")

    # Standard MIDI sequence from audio_engine
    midi_messages = audio_engine.DEFAULT_MIDI_SEQUENCE
    duration = 5.0
    sr = 44100

    all_results = {}
    subfolders = sorted([f for f in os.listdir(sounds_dir) if os.path.isdir(os.path.join(sounds_dir, f))], key=lambda x: int(x))

    for subfolder in subfolders:
        print(f"Processing sound {subfolder}...")
        folder_path = os.path.join(sounds_dir, subfolder)
        design_file = os.path.join(folder_path, "sound_design.py")

        if not os.path.exists(design_file):
            print(f"Skipping {subfolder}, sound_design.py not found.")
            continue

        # Load the specific sound_design module
        sd_module = load_module_from_path(f"sound_design_{subfolder}", design_file)

        # Render
        audio = sd_module.render_midi(midi_messages, duration, sr)

        # Analyze
        results = audio_engine.analyze_audio(audio, sr)
        all_results[subfolder] = results

    # Second pass: Calculate distances
    for subfolder in subfolders:
        if subfolder not in all_results:
            continue

        distances = {}
        for other_folder in subfolders:
            if other_folder != subfolder and other_folder in all_results:
                dist = audio_engine.calculate_distance(all_results[subfolder], all_results[other_folder])
                distances[other_folder] = dist

        if distances:
            all_results[subfolder]['distances'] = distances

        # Save back to JSON
        json_path = os.path.join(sounds_dir, subfolder, "analysis.json")
        with open(json_path, 'w') as f:
            json.dump(all_results[subfolder], f, indent=4)
        print(f"Updated {json_path}")

if __name__ == "__main__":
    main()
