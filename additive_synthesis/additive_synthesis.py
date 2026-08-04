#!/usr/bin/env python3
"""
Additive Synthesis Visualizer & Player
Displays the step-by-step construction of a sound wave from sinusoids,
showing the updated final equation and playing the sound at each step.
Also compiles the steps into a synchronized MP4 video and animated GIF.
"""

import os
import sys
import json
import argparse
import numpy as np
import soundfile as sf

# Try to import sounddevice for real-time playback
SOUNDDEVICE_AVAILABLE = False
SD_ERROR = ""
try:
    import sounddevice as sd
    SOUNDDEVICE_AVAILABLE = True
except ImportError:
    SD_ERROR = "sounddevice module not found. Run 'pip install sounddevice'."
except OSError as e:
    SD_ERROR = f"PortAudio library not found or error loading it: {e}"

# Import matplotlib safely
import matplotlib
# We will set the backend based on interactive mode
import matplotlib.pyplot as plt


class AdditiveSynthesizer:
    """
    Manages a collection of sinusoids and performs additive synthesis,
    generating waveforms and LaTeX/text mathematical equations at each step.
    Supports time-varying amplitudes with exponential decay.
    """
    def __init__(self, sample_rate=44100, duration=1.5):
        self.sample_rate = sample_rate
        self.duration = duration
        # Time array for synthesis
        self.t = np.linspace(0, duration, int(sample_rate * duration), endpoint=False)
        self.sinusoids = []  # List of dicts: {"freq": float, "amp": float, "phase": float, "decay": float}

    def add_sinusoid(self, freq, amp, phase=0.0, decay=0.0):
        """Adds a sinusoid component to the synthesizer."""
        self.sinusoids.append({
            "freq": float(freq),
            "amp": float(amp),
            "phase": float(phase),
            "decay": float(decay)
        })

    def clear(self):
        """Clears all sinusoids."""
        self.sinusoids = []

    def get_signal_at_step(self, step_idx):
        """
        Generates the combined signal up to step_idx (1-based index).
        """
        if step_idx <= 0 or step_idx > len(self.sinusoids):
            return np.zeros_like(self.t)
        
        signal = np.zeros_like(self.t)
        for i in range(step_idx):
            signal += self.get_component_signal(i)
        return signal

    def get_component_signal(self, idx):
        """Generates only the signal of the single sinusoid component at idx."""
        if idx < 0 or idx >= len(self.sinusoids):
            return np.zeros_like(self.t)
        s = self.sinusoids[idx]
        amp_env = s["amp"]
        if s["decay"] > 0.0:
            amp_env = s["amp"] * np.exp(-s["decay"] * self.t)
        return amp_env * np.sin(2 * np.pi * s["freq"] * self.t + s["phase"])

    def get_equation(self, step_idx, latex=False):
        """
        Generates the mathematical equation for the signal up to step_idx.
        Formats amplitudes, frequencies, decay rates, and phases cleanly.
        """
        if step_idx <= 0 or step_idx > len(self.sinusoids):
            return "p(t) = 0"

        terms = []
        for i in range(step_idx):
            s = self.sinusoids[i]
            amp = s["amp"]
            freq = s["freq"]
            phase = s["phase"]
            decay = s["decay"]

            # Skip if amplitude is virtually zero
            if abs(amp) < 1e-9:
                continue

            # Format Amplitude / decay
            # We want to represent amplitude as A_i(t) = amp * e^(-decay * t)
            if decay > 0.0:
                if abs(amp - 1.0) < 1e-4:
                    amp_term_text = f"e^(-{decay:.1f}*t)"
                    amp_term_latex = f"e^{{-{decay:.1f} t}}"
                    amp_str = "" if i == 0 else "+"
                elif abs(amp + 1.0) < 1e-4:
                    amp_term_text = f"e^(-{decay:.1f}*t)"
                    amp_term_latex = f"e^{{-{decay:.1f} t}}"
                    amp_str = "-"
                else:
                    amp_term_text = f"{abs(amp):.3f}*e^(-{decay:.1f}*t)"
                    amp_term_latex = f"{abs(amp):.3f} e^{{-{decay:.1f} t}}"
                    amp_str = "+" if amp >= 0 else "-"
                    if i == 0 and amp >= 0:
                        amp_str = ""
            else:
                amp_term_text = ""
                amp_term_latex = ""
                if abs(amp - 1.0) < 1e-4:
                    amp_str = "" if i == 0 else "+"
                elif abs(amp + 1.0) < 1e-4:
                    amp_str = "-"
                else:
                    if i > 0:
                        amp_str = f"{amp:+.3f}"
                    else:
                        amp_str = f"{amp:.3f}"

            # Determine multipliers
            if amp_str in ["", "+", "-"]:
                amp_mult = ""
            else:
                amp_mult = "" if latex else "*"

            # Format Phase
            if abs(phase) < 1e-4:
                phase_str = ""
            else:
                # Check if phase can be cleanly expressed in terms of pi
                pi_frac = phase / np.pi
                if abs(round(pi_frac) - pi_frac) < 1e-2:
                    val = int(round(pi_frac))
                    if val == 1:
                        phase_str = " + \\pi" if latex else " + pi"
                    elif val == -1:
                        phase_str = " - \\pi" if latex else " - pi"
                    else:
                        phase_str = f" {val:+}*\\pi" if latex else f" {val:+}*pi"
                else:
                    phase_str = f" {phase:+.3f}"

            # Combine into term
            freq_str = f"{freq:.1f}" if freq % 1 != 0 else f"{int(freq)}"
            if decay > 0.0:
                if latex:
                    term = f"{amp_str}{amp_term_latex}\\sin(2\\pi \\cdot {freq_str} t{phase_str})"
                else:
                    term = f"{amp_str}{amp_term_text}*sin(2*pi*{freq_str}*t{phase_str})"
            else:
                if latex:
                    term = f"{amp_str}\\sin(2\\pi \\cdot {freq_str} t{phase_str})"
                else:
                    term = f"{amp_str}{amp_mult}sin(2*pi*{freq_str}*t{phase_str})"

            # Clean up potential duplicate signs
            term = term.replace("+-", "-").replace("-+", "-").replace("++", "+")
            terms.append(term)

        if not terms:
            return "p(t) = 0"

        # Join the terms nicely
        # For plain text, add spaces around operators for legibility
        if not latex:
            # We want to separate terms with space, and format "+ 0.500*" instead of "+0.500*"
            # Let's clean up term formatting before joining
            cleaned_terms = []
            for i, term in enumerate(terms):
                if i > 0 and term.startswith("+"):
                    cleaned_terms.append("+ " + term[1:])
                elif i > 0 and term.startswith("-"):
                    cleaned_terms.append("- " + term[1:])
                else:
                    cleaned_terms.append(term)
            full_eq = " ".join(cleaned_terms)
            # Remove redundant leading plus if any
            if full_eq.startswith("+ "):
                full_eq = full_eq[2:]
            elif full_eq.startswith("+"):
                full_eq = full_eq[1:]
            return f"p(t) = {full_eq}"
        else:
            full_eq = "".join(terms)
            if full_eq.startswith("+"):
                full_eq = full_eq[1:]
            return f"p(t) = {full_eq}"


def get_demo_sinusoids(wave_type, fund_freq, steps):
    """
    Generates standard synthesizer sinusoids for demo wave types:
    - square, sawtooth, triangle, chord, or juicy.
    """
    sinusoids = []
    if wave_type == "square":
        # Square wave: 4/pi * (sin(wt)/1 + sin(3wt)/3 + sin(5wt)/5 + ...)
        for n in range(1, steps * 2, 2):
            freq = fund_freq * n
            amp = 4.0 / (np.pi * n)
            sinusoids.append((freq, amp, 0.0, 0.0))
            if len(sinusoids) == steps:
                break
    elif wave_type == "sawtooth":
        # Sawtooth wave: 2/pi * (sin(wt)/1 - sin(2wt)/2 + sin(3wt)/3 - ...)
        for n in range(1, steps + 1):
            freq = fund_freq * n
            # (-1)^(n+1) * 2 / (pi * n)
            amp = (2.0 / (np.pi * n)) * ((-1) ** (n + 1))
            sinusoids.append((freq, amp, 0.0, 0.0))
    elif wave_type == "triangle":
        # Triangle wave: 8/pi^2 * (sin(wt)/1^2 - sin(3wt)/3^2 + sin(5wt)/5^2 - ...)
        for idx, n in enumerate(range(1, steps * 2, 2)):
            freq = fund_freq * n
            amp = (8.0 / (np.pi**2 * n**2)) * ((-1) ** idx)
            sinusoids.append((freq, amp, 0.0, 0.0))
            if len(sinusoids) == steps:
                break
    elif wave_type == "chord":
        # Standard major chord (C4, E4, G4, C5) over fund_freq as fundamental reference
        # We can construct C4 (~261.63 Hz), E4 (~329.63 Hz), G4 (~392.00 Hz), C5 (~523.25 Hz)
        # relative to the base fund_freq.
        intervals = [1.0, 1.25, 1.5, 2.0]  # Root, Major 3rd, Perfect 5th, Octave
        amplitudes = [1.0, 0.8, 0.7, 0.5]
        phases = [0.0, np.pi/4, np.pi/2, 0.0]
        for i in range(min(steps, len(intervals))):
            sinusoids.append((fund_freq * intervals[i], amplitudes[i], phases[i], 0.0))
    elif wave_type == "juicy":
        # A dynamic, rich chime/bell chord with a built-in "reverb tail" simulated purely via additive synthesis.
        # This is achieved by combining prominent fundamental/harmonic tones (which decay faster)
        # with dense, quiet, slowly-decaying detuned "reverberant/diffuse" sinusoids.
        # Format: (freq, amp, phase, decay)
        specs = [
            # 1. Warm fundamental root tone (chime body) - decays at a medium rate
            (fund_freq, 0.9, 0.0, 1.5),
            # 2. Shimmering perfect fifth (E.g. E4/G4) - decays slightly faster
            (fund_freq * 1.5, 0.6, np.pi/4, 2.0),
            # 3. Bright octave overtone with fast bell decay
            (fund_freq * 2.0, 0.4, np.pi/2, 3.5),
            # 4. Melodic major third overtone (rich timbre)
            (fund_freq * 1.25, 0.5, 0.0, 1.8),
            # 5. High sparkle ninth harmonic with rapid transient decay
            (fund_freq * 2.25, 0.3, np.pi/3, 4.5),
            # 6. Deep sub-octave fundamental that sustains longer for warm body
            (fund_freq * 0.5, 0.7, -np.pi/4, 0.8),
            # 7. Dense detuned "reverb tail" element 1 - slightly flat of root, slow decay, low amplitude
            (fund_freq * 0.99, 0.15, np.pi/6, 0.4),
            # 8. Dense detuned "reverb tail" element 2 - slightly sharp of octave, slow decay, low amplitude
            (fund_freq * 2.015, 0.12, -np.pi/3, 0.5),
        ]
        for i in range(min(steps, len(specs))):
            sinusoids.append(specs[i])
    else:
        raise ValueError(f"Unknown wave type: {wave_type}")
    
    return sinusoids


def run_additive_synthesis(args):
    """
    Orchestrates the additive synthesis process:
    - Generates sinusoids
    - Iterates step by step
    - Plots individual and accumulated waves
    - Updates and displays the overall mathematical equation
    - Plays the sound wave audibly (if enabled)
    - Saves PNG plots, WAV audio files, and a final animated GIF
    """
    # 1. Setup output directory
    os.makedirs(args.output_dir, exist_ok=True)
    print(f"Output files will be saved in: {os.path.abspath(args.output_dir)}")

    # 2. Initialize synthesizer
    synth = AdditiveSynthesizer(sample_rate=args.sample_rate, duration=args.duration)

    # 3. Load or generate sinusoids
    if args.custom:
        print(f"Loading custom sinusoids from JSON: {args.custom}")
        with open(args.custom, "r") as f:
            data = json.load(f)
            for s in data:
                synth.add_sinusoid(s["freq"], s["amp"], s.get("phase", 0.0), s.get("decay", 0.0))
    else:
        print(f"Generating demo sinusoids for '{args.demo}' wave (fundamental: {args.freq} Hz, steps: {args.steps})")
        sinusoids = get_demo_sinusoids(args.demo, args.freq, args.steps)
        for s in sinusoids:
            decay = s[3] if len(s) > 3 else 0.0
            synth.add_sinusoid(s[0], s[1], s[2], decay)

    num_steps = len(synth.sinusoids)
    if num_steps == 0:
        print("Error: No sinusoids to synthesize.")
        sys.exit(1)

    print(f"Total steps to visualize: {num_steps}")

    # Set up Matplotlib backend
    if not args.interactive:
        matplotlib.use("Agg")
    
    # List of saved image paths for building the animated GIF later
    saved_images = []

    # Iterate through each step
    for step in range(1, num_steps + 1):
        print(f"\n--- Step {step} of {num_steps} ---")
        
        # Get overall and component signals
        accum_signal = synth.get_signal_at_step(step)
        current_comp = synth.get_component_signal(step - 1)
        
        # Get mathematical equations
        text_eq = synth.get_equation(step, latex=False)
        latex_eq = synth.get_equation(step, latex=True)
        new_comp_eq = synth.get_equation(step, latex=False).split(" = ")[-1].split(" ")[-1]
        
        print(f"New component added: {synth.sinusoids[step-1]}")
        print(f"Updated Math Equation:\n  {text_eq}")

        # Normalize audio output to avoid clipping
        norm_factor = np.max(np.abs(accum_signal))
        audio_to_play = accum_signal / norm_factor if norm_factor > 1.0 else accum_signal

        # Audible feedback
        if not args.no_audio:
            if SOUNDDEVICE_AVAILABLE:
                print("Playing audio step...")
                try:
                    sd.play(audio_to_play, args.sample_rate)
                    sd.wait()
                except Exception as play_err:
                    print(f"Playback warning: {play_err}")
            else:
                print(f"Playback disabled: {SD_ERROR}")

        # Plotting the current step
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
        
        # Plot only 3 cycles of the fundamental frequency for clean visual waveform
        # Time duration for plotting: T_fundamental * 3
        fund_f = synth.sinusoids[0]["freq"]
        plot_duration = min(3.0 / fund_f, args.duration)
        plot_samples = int(args.sample_rate * plot_duration)
        t_plot = synth.t[:plot_samples]
        
        # Plot individual component waves in the top subplot
        for i in range(step):
            comp_wave = synth.get_component_signal(i)[:plot_samples]
            # Highlight the newly added component in gold, others in muted gray
            if i == step - 1:
                ax1.plot(t_plot, comp_wave, color="#f1c40f", linewidth=2.5, label=f"Added: {new_comp_eq}", zorder=4)
            else:
                ax1.plot(t_plot, comp_wave, color="#bdc3c7", alpha=0.6, linewidth=1.2, zorder=2)
                
        ax1.set_title("Sinusoid Components Added", fontsize=12, fontweight="bold")
        ax1.set_ylabel("Amplitude", fontsize=10)
        ax1.grid(True, linestyle=":", alpha=0.6)
        ax1.legend(loc="upper right", framealpha=0.9)
        
        # Plot the accumulated waveform in the bottom subplot
        ax2.plot(t_plot, accum_signal[:plot_samples], color="#2980b9", linewidth=2, label=f"Accumulated (N={step})", zorder=3)
        ax2.set_title("Overall Accumulated Waveform", fontsize=12, fontweight="bold")
        ax2.set_xlabel("Time (seconds)", fontsize=10)
        ax2.set_ylabel("Amplitude", fontsize=10)
        ax2.grid(True, linestyle=":", alpha=0.6)
        ax2.legend(loc="upper right")
        
        # Construct dynamic equation text block
        # We can display a neat multiline title
        display_eq = text_eq
        # If the equation is too long, truncate it nicely with ellipsis
        if len(display_eq) > 100:
            display_eq = display_eq[:97] + "..."
            
        fig.suptitle(
            f"Additive Synthesis: Step {step} of {num_steps}\n"
            f"Equation: {display_eq}",
            fontsize=13, fontweight="bold", y=0.98
        )
        
        plt.tight_layout()
        
        # Save step PNG
        img_path = os.path.join(args.output_dir, f"step_{step:02d}.png")
        plt.savefig(img_path, dpi=150, bbox_inches="tight")
        saved_images.append(img_path)
        
        if args.interactive:
            plt.show()
        else:
            plt.close()

        # Save audio step to WAV
        wav_path = os.path.join(args.output_dir, f"step_{step:02d}.wav")
        sf.write(wav_path, audio_to_play, args.sample_rate)

    # 4. Generate final animated GIF using Pillow
    print("\nCompiling individual frames into an animated GIF...")
    from PIL import Image
    try:
        frames = [Image.open(img) for img in saved_images]
        # Set longer duration on the final frame so the user can study the completed equation
        durations = [1200] * (len(frames) - 1) + [4000]
        gif_path = os.path.join(args.output_dir, "additive_synthesis_animation.gif")
        frames[0].save(
            gif_path,
            save_all=True,
            append_images=frames[1:],
            duration=durations,
            loop=0
        )
        print(f"Successfully generated animated GIF: {gif_path}")
    except Exception as gif_err:
        print(f"Warning: Could not compile animated GIF: {gif_err}")

    # 5. Generate final video using FFmpeg
    print("\nCompiling individual steps into an MP4 video...")
    import subprocess
    try:
        temp_mp4_files = []
        for step in range(1, num_steps + 1):
            img_path = os.path.join(args.output_dir, f"step_{step:02d}.png")
            wav_path = os.path.join(args.output_dir, f"step_{step:02d}.wav")
            step_mp4_path = os.path.join(args.output_dir, f"step_{step:02d}_temp.mp4")
            
            # Combine image + audio into a temporary MP4 segment
            cmd = [
                "ffmpeg", "-y",
                "-loop", "1",
                "-t", str(args.duration),
                "-i", img_path,
                "-i", wav_path,
                "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2",
                "-c:v", "libx264",
                "-tune", "stillimage",
                "-c:a", "aac",
                "-b:a", "192k",
                "-pix_fmt", "yuv420p",
                "-shortest",
                step_mp4_path
            ]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            temp_mp4_files.append(step_mp4_path)
            
        # Concat using filter_complex instead of concat demuxer to avoid audio dropouts / sync issues
        concat_inputs = []
        filter_str = ""
        for idx, temp_file in enumerate(temp_mp4_files):
            concat_inputs.extend(["-i", temp_file])
            filter_str += f"[{idx}:v][{idx}:a]"
        filter_str += f" concat=n={num_steps}:v=1:a=1 [v][a]"

        final_mp4_path = os.path.join(args.output_dir, "additive_synthesis_video.mp4")
        concat_cmd = [
            "ffmpeg", "-y"
        ] + concat_inputs + [
            "-filter_complex", filter_str,
            "-map", "[v]",
            "-map", "[a]",
            "-c:v", "libx264",
            "-tune", "stillimage",
            "-c:a", "aac",
            "-b:a", "192k",
            "-pix_fmt", "yuv420p",
            final_mp4_path
        ]

        res = subprocess.run(concat_cmd, capture_output=True, text=True)
        if os.path.exists(final_mp4_path) and os.path.getsize(final_mp4_path) > 0:
            print(f"Successfully generated master video: {final_mp4_path}")
        else:
            print(f"Warning: ffmpeg exited with {res.returncode} and final video could not be created.")
            print("FFmpeg stdout:", res.stdout)
            print("FFmpeg stderr:", res.stderr)
        
        # Cleanup temporary MP4s
        for temp_file in temp_mp4_files:
            os.remove(temp_file)
            
    except Exception as vid_err:
        print(f"Warning: Could not compile MP4 video: {vid_err}")

    # Copy the final step audio to final_synthesis.wav
    final_wav_path = os.path.join(args.output_dir, "final_synthesis.wav")
    sf.write(final_wav_path, audio_to_play, args.sample_rate)
    print(f"Saved final synthesized audio: {final_wav_path}")
    print("Process Complete!")


def main():
    parser = argparse.ArgumentParser(
        description="Visualize and play the step-by-step construction of an arbitrary sound via additive synthesis."
    )
    parser.add_argument(
        "--demo",
        type=str,
        choices=["square", "sawtooth", "triangle", "chord", "juicy"],
        default="juicy",
        help="Type of demo wave to synthesize (default: %(default)s)."
    )
    parser.add_argument(
        "--freq",
        type=float,
        default=220.0,
        help="Fundamental frequency in Hz (default: %(default)s)."
    )
    parser.add_argument(
        "--steps",
        type=int,
        default=8,
        help="Number of sinusoid components/harmonics to synthesize (default: %(default)s)."
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=1.5,
        help="Duration of the generated sound in seconds (default: %(default)s)."
    )
    parser.add_argument(
        "--sample-rate",
        type=int,
        default=44100,
        help="Sample rate in Hz (default: %(default)s)."
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="analysis/additive_synthesis_output",
        help="Directory to save image frames, audio, and animated GIF (default: %(default)s)."
    )
    parser.add_argument(
        "--custom",
        type=str,
        default=None,
        help="Path to a JSON file containing custom sinusoids: [{'freq': f, 'amp': a, 'phase': p}, ...]."
    )
    parser.add_argument(
        "--no-audio",
        action="store_true",
        help="Disable audible playback at each step."
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="Display the interactive Matplotlib window for each step."
    )
    
    args = parser.parse_args()
    run_additive_synthesis(args)


if __name__ == "__main__":
    main()
