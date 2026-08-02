> using additive synthesis (if any sound can be constructed as a sum of sines), can any sound be broken down into a single equation? I'm just talking about single sounds, not melodies or drumbeats or anything beyond a single note-worth

Yes—any single, finite-duration sound can be written as one mathematical expression, but that expression is almost always an infinite or very large sum of sines (a Fourier series or Fourier integral), not a short closed-form formula. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)

## The core idea: Fourier decomposition

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

## What “single equation” really means in practice

There are two important caveats:

1. **Infinite vs finite sums**  
   - Mathematically exact representations often require **infinitely many** sine terms. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)
   - In real additive synths, you approximate with a **large but finite** number of partials; the more you use, the closer you get to the original timbre. [youtube](https://www.youtube.com/watch?v=97jwN_MBEWI)

2. **Time-varying timbre**  
   Real notes change over time (attack, decay, evolving spectrum). To capture that in one expression, the amplitudes (and sometimes frequencies/phases) of the sine components must be functions of time:
   $$
   p(t) = \sum_{k=1}^{K} A_k(t) \cos\bigl(2\pi f_k(t) t + \phi_k(t)\bigr)
   $$
   This is still “one equation,” but it’s a compact way of writing a potentially huge, time-dependent sum. [en.wikipedia](https://en.wikipedia.org/wiki/Additive_synthesis)

## So can any single sound be broken down into a single equation?

- **Mathematically**: yes, as a Fourier series (for periodic tones) or Fourier integral (for general finite sounds), i.e., one expression built from sines. [reddit](https://www.reddit.com/r/Acoustics/comments/1f072be/how_can_one_mathematicically_describe_any_sound/)
- **Practically**: you can approximate any single-note sound arbitrarily well with a finite additive-synthesis equation, but the exact formula may be infinite or extremely long and not “simple” in the everyday sense. [soundonsound](https://www.soundonsound.com/techniques/introduction-additive-synthesis)
