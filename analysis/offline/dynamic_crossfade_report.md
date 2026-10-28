# Dynamic Crossfade Algorithm Documentation

This document details the exact architecture, data structures, and mathematical execution logic of the dynamic crossfade algorithm implemented in `analysis/offline/crossfade.h` and `analysis/offline/crossfade.c`.

---

## 1. Architectural Overview & Data Structures

The dynamic crossfade module provides sample-accurate dynamic gain ramp smoothing and dual-channel crossfading based on instantaneous signal amplitude and directional state changes. It consists of two primary C structures defined in `crossfade.h`:

```c
typedef struct _ramp_state {
    double length;    // Current duration of the ramp in samples
    double toggle;    // Target state direction: 1.0 (fade-in / ramp up) or 0.0 (fade-out / ramp down)
    double go;        // Timestamp (elapsed sample index) when the current transition was triggered
    double last_amp;  // Historical amplitude state for the envelope follower / slide filter
} t_ramp_state;

typedef struct _crossfade_state {
    t_ramp_state ramp1;  // Ramp state tracker for Channel 1 (Signal 1)
    t_ramp_state ramp2;  // Ramp state tracker for Channel 2 (Signal 2)
    double direction;    // Current transition trigger direction (+1.0, -1.0, or 0.0)
    double last_control; // Control parameter value from the previous sample
    long long elapsed;   // Master sample clock counter

    // User Parameters
    double samplerate;   // Audio sample rate in Hz (e.g., 44100.0)
    double low_ms;       // Minimum transition fade time and envelope decay time (in ms)
    double high_ms;      // Maximum transition fade time for peak signal amplitude (in ms)
} t_crossfade_state;
```

---

## 2. Parameter Initialization & Validation

Parameter initialization and validation are performed by `crossfade_init` and `ramp_init`:

- **`low_ms`**: Enforces a lower bound of `0.001` ms.
- **`high_ms`**: Enforces `high_ms >= low_ms`.
- **`samples_per_ms`**: Calculated as `samplerate / 1000.0` (falling back to `44.1` if non-positive).
- Default starting direction for `ramp1` and `ramp2` is initialized to `-1.0`.

---

## 3. Dynamic Envelope Follower & Ramp Calculation (`ramp_process`)

For every audio sample processed through a channel, `ramp_process` executes a three-stage pipeline:

### Stage A: Movement Trigger Detection
When `movement != 0.0`:
1. `x->length` resets to the upper bound `high_samples = (samplerate / 1000.0) * high_ms`.
2. `x->go` records the current sample timestamp (`elapsed`).
3. `x->toggle` sets the target state: `1.0` if `movement > 0.0` (fade-in), or `0.0` if `movement <= 0.0` (fade-out).

### Stage B: Dynamic Amplitude Measurement & Envelope Decay (Slide Filter)
To prevent harsh clicks during transitions of low-amplitude audio while allowing full dynamic crossfades for loud signals:
1. Calculates absolute signal amplitude: `abs_sig = fabs(signal)`.
2. Applies a one-way envelope follower / slide filter (`last_amp`):
   - **Instantaneous Attack**: If `abs_sig > x->last_amp`, amplitude updates immediately (`amp = abs_sig`).
   - **Linear Decay**: If `abs_sig <= x->last_amp`, amplitude decays slowly over `low_samples` (`amp = x->last_amp + (abs_sig - x->last_amp) / low_samples`).
3. Computes dynamic ramp duration:
   `target_length = amp * high_samples`
   Clamped strictly within `[low_samples, x->length]`. Higher audio amplitude scales the ramp time toward `high_ms`, whereas silence or quiet signals shrink the fade duration toward `low_ms`.

### Stage C: Linear Ramp Progress & Direction Inversion
1. Calculates sample age: `age = (double)elapsed - x->go`.
2. Normalizes age against active dynamic length: `fade = age / x->length` (clamped between `0.0` and `1.0`).
3. Computes final gain factor using state inversion:
   `fade = fabs(x->toggle - fade)`
4. Returns the gain-adjusted signal: `signal * fade` and outputs `*fade_out = fade`.

---

## 4. Crossfade Controller & Mix Summation (`crossfade_process`)

`crossfade_process` coordinates `ramp1` and `ramp2` in counter-phase across sample ticks:

1. **Dual Channel Ramp Processing**:
   - `s1` (Channel 1) is processed with direction `x->direction`.
   - `s2` (Channel 2) is processed with inverted direction `x->direction * -1.0`.
2. **Completion Check**:
   Evaluates whether both channel gain ramps have completed their transitions:
   - For `toggle > 0.5`: Completed when gain factor `f <= 0.0`.
   - For `toggle <= 0.5`: Completed when gain factor `f >= 1.0`.
   - Sets `finished = r1_done && r2_done`.
3. **State & Direction Update**:
   - `x->direction` resets to `0.0`.
   - If `finished == true` and `control` delta (`control - x->last_control != 0.0`) occurs, `x->direction` receives the control difference to trigger the next crossfade cycle.
4. **Output Assignment**:
   - `*sum = *mix1 + *mix2` (Summed audio sample output).
   - `*busy = !finished` (Active transition status flag).
   - Increments master sample clock counter `x->elapsed++`.
