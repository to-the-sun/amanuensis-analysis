# Additive Synthesis

Using additive synthesis, any single sound can be constructed as a sum of sines or broken down into a single mathematical expression. This repository provides an interactive visualization tool to showcase this concept step-by-step.

[en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)

## The Core Idea: Fourier Decomposition

Additive synthesis is just the practical side of a deep mathematical fact: under mild conditions, any reasonable sound pressure waveform $$p(t)$$ can be represented as a superposition of sinusoids. [reddit](https://www.reddit.com/r/Acoustics/comments/1f072be/how_can_one_mathematicically_describe_any_sound/)

- For a **periodic** tone (idealized single note that repeats forever), you get a **Fourier series**:
  $$
  p(t) = a_0 + \sum_{n=1}^{\infty} \bigl[a_n \cos(2\pi n f_0 t) + b_n \sin(2\pi n f_0 t)\bigr]
  $$
  or equivalently
  $$
  p(t) = \sum_{n=0}^{\infty} A_n \cos(2\pi n f_0 t + \phi_n)
  $$
  where $$f_0$$ is the fundamental frequency and each term is a sine (or cosine) at a harmonic frequency with its own amplitude and phase. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)

- For a **nonperiodic but finite** sound (a single pluck, hit, or breath that starts and stops), you use the **Fourier integral**:
  $$
  p(t) = \int_{-\infty}^{\infty} \hat{p}(f)\, e^{i 2\pi f t}\, df
  $$
  which is conceptually “a sum of sines at every frequency,” weighted by the spectrum $$\hat{p}(f)$$. [reddit](https://www.reddit.com/r/Acoustics/comments/1f072be/how_can_one_mathematicically_describe_any_sound/)

So in principle, yes: there is a single equation that describes the entire waveform as a sum (or integral) of sines. [reddit](https://www.reddit.com/r/Acoustics/comments/1f072be/how_can_one_mathematicically_describe_any_sound/)

---

## Interactive Visualizer and Player

The script `analysis/additive_synthesis.py` lets you visualize and hear a sound wave as it is constructed step-by-step from an accumulation of sinusoids. At each step, the script:
1. Displays the updated mathematical equation of the sound wave.
2. Plots both the individual added components and the overall accumulated waveform.
3. Plays the combined sound wave audibly through the speakers.
4. Generates an animated GIF and a final high-quality WAV audio file.

### Usage

To run the visualization with the default advanced `"juicy"` demo:
```bash
python3 additive_synthesis/additive_synthesis.py --demo juicy --steps 8 --freq 220
```

#### Command-line Options:
- `--demo {square,sawtooth,triangle,chord,juicy}`: The type of wave shape or chord to build step-by-step (default: `juicy`).
- `--reverb-mix RATIO`: Dry/wet reverb mix ratio, between 0.0 and 1.0 (default: `0.35`).
- `--reverb-room RATIO`: Room size feedback ratio for the reverb, between 0.0 and 1.0 (default: `0.75`).
- `--freq HZ`: Fundamental frequency of the sound in Hz (default: `220.0`).
- `--steps COUNT`: Number of sinusoid components/harmonics to synthesize (default: `8`).
- `--duration SECONDS`: Duration of the generated sound in seconds (default: `1.5`).
- `--output-dir PATH`: Directory to save the PNG frames, step WAVs, and animated GIF (default: `analysis/additive_synthesis_output`).
- `--custom PATH`: Path to a JSON file containing a list of arbitrary, custom sinusoids to synthesize (see custom format below).
- `--no-audio`: Disable audible audio playback at each step.
- `--interactive`: Open interactive Matplotlib windows for each step.

---

### Modular Custom Sinusoids

You can synthesize *any* arbitrary sound modularly by defining your own custom collection of sinusoids in a JSON file. Create a file, e.g., `my_sound.json`:

```json
[
  {"freq": 220.0, "amp": 1.0, "phase": 0.0},
  {"freq": 330.0, "amp": 0.6, "phase": 1.5708},
  {"freq": 440.0, "amp": 0.4, "phase": -0.7854}
]
```

Then run the script with the `--custom` option:
```bash
python3 analysis/additive_synthesis.py --custom my_sound.json
```

---

## What “Single Equation” Really Means in Practice

There are two important caveats:

1. **Infinite vs Finite Sums**  
   - Mathematically exact representations often require **infinitely many** sine terms. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)
   - In real additive synths, you approximate with a **large but finite** number of partials; the more you use, the closer you get to the original timbre. [youtube](https://www.youtube.com/watch?v=97jwN_MBEWI)

2. **Time-Varying Timbre**  
   Real notes change over time (attack, decay, evolving spectrum). To capture that in one expression, the amplitudes (and sometimes frequencies/phases) of the sine components must be functions of time:
   $$
   p(t) = \sum_{k=1}^{K} A_k(t) \cos\bigl(2\pi f_k(t) t + \phi_k(t)\bigr)
   $$
   This is still “one equation,” but it’s a compact way of writing a potentially huge, time-dependent sum. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)
