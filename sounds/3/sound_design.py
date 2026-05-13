import numpy as np
from scipy import signal

SOUND_DESIGN_VERSION = 3

def adsr_envelope(duration_samples, attack_samples, decay_samples, sustain_level, release_samples):
    """Generate an ADSR envelope."""
    # Attack
    attack = np.linspace(0, 1, attack_samples)

    # Decay
    decay = np.linspace(1, sustain_level, decay_samples)

    # Release
    release = np.linspace(sustain_level, 0, release_samples)

    # Sustain
    sustain_duration = duration_samples - attack_samples - decay_samples - release_samples
    if sustain_duration < 0:
        # If duration is too short, scale everything down
        total = attack_samples + decay_samples + release_samples
        scale = duration_samples / total
        attack = np.linspace(0, 1, int(attack_samples * scale))
        decay = np.linspace(1, sustain_level, int(decay_samples * scale))
        release = np.linspace(sustain_level, 0, int(release_samples * scale))
        sustain = np.array([])
    else:
        sustain = np.full(sustain_duration, sustain_level)

    return np.concatenate([attack, decay, sustain, release])

def render_midi(midi_messages, duration, sample_rate):
    """
    Synthesize audio from a list of MIDI messages.
    Uses Subtractive Synthesis: Sawtooth wave -> Resonant Low-Pass Filter with Cutoff Envelope.
    """
    num_samples = int(duration * sample_rate)
    output = np.zeros(num_samples)

    # Track active notes: {note_number: start_time}
    active_notes = {}

    # Synth parameters
    # ADSR for Amplitude
    amp_attack = 0.01
    amp_decay = 0.1
    amp_sustain = 0.4
    amp_release = 0.2

    # Filter Envelope parameters
    filt_attack = 0.05
    filt_decay = 0.2
    filt_sustain = 0.1
    filt_release = 0.2

    base_cutoff = 100 # Hz
    env_amount = 3000 # Hz

    for msg in midi_messages:
        t = int(msg.time * sample_rate)
        if t >= num_samples:
            continue

        if msg.type == 'note_on' and msg.velocity > 0:
            active_notes[msg.note] = (msg.time, msg.velocity)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in active_notes:
                start_time, velocity = active_notes.pop(msg.note)
                end_time = msg.time

                # Render the note
                freq = 440.0 * (2.0 ** ((msg.note - 69) / 12.0))
                note_duration = end_time - start_time + max(amp_release, filt_release)
                note_samples = int(note_duration * sample_rate)

                # Bound note samples to output range
                start_idx = int(start_time * sample_rate)
                end_idx = min(start_idx + note_samples, num_samples)
                actual_samples = end_idx - start_idx

                if actual_samples <= 0:
                    continue

                t_vec = np.arange(actual_samples) / sample_rate

                # 1. Oscillator: Sawtooth wave
                wave = signal.sawtooth(2 * np.pi * freq * (t_vec + start_time))

                # 2. Filter Cutoff Envelope
                filt_env = adsr_envelope(
                    actual_samples,
                    int(filt_attack * sample_rate),
                    int(filt_decay * sample_rate),
                    filt_sustain,
                    int(filt_release * sample_rate)
                )
                if len(filt_env) < actual_samples:
                    filt_env = np.pad(filt_env, (0, actual_samples - len(filt_env)))
                else:
                    filt_env = filt_env[:actual_samples]

                # Apply filter modulation (processed in blocks)
                filtered_wave = np.zeros(actual_samples)
                block_size = 128
                zi = np.zeros((1, 2)) # For 2nd order SOS filter (1 section)

                for i in range(0, actual_samples, block_size):
                    block_end = min(i + block_size, actual_samples)
                    cutoff = base_cutoff + env_amount * np.mean(filt_env[i:block_end])
                    cutoff = max(20, min(cutoff, sample_rate * 0.45))

                    sos = signal.butter(2, cutoff, 'lp', fs=sample_rate, output='sos')
                    block_out, zi = signal.sosfilt(sos, wave[i:block_end], zi=zi)
                    filtered_wave[i:block_end] = block_out

                # 3. Amplitude Envelope
                amp_env = adsr_envelope(
                    actual_samples,
                    int(amp_attack * sample_rate),
                    int(amp_decay * sample_rate),
                    amp_sustain,
                    int(amp_release * sample_rate)
                )
                if len(amp_env) < actual_samples:
                    amp_env = np.pad(amp_env, (0, actual_samples - len(amp_env)))
                else:
                    amp_env = amp_env[:actual_samples]

                note_audio = filtered_wave * amp_env * (velocity / 127.0) * 0.15
                output[start_idx:end_idx] += note_audio

    # Final saturation
    output = np.tanh(output)

    return output
