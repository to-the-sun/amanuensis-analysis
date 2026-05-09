import numpy as np

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
    midi_messages: list of mido.Message objects with absolute 'time' attribute.
    """
    num_samples = int(duration * sample_rate)
    output = np.zeros(num_samples)

    # Track active notes: {note_number: start_time}
    active_notes = {}

    # Harmonics for additive synthesis (ratio, amplitude)
    harmonics = [(1.0, 1.0), (2.0, 0.4), (3.0, 0.2), (4.0, 0.1)]

    # ADSR parameters (in seconds)
    attack_time = 0.05
    decay_time = 0.2
    sustain_level = 0.6
    release_time = 0.3

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
                note_duration = end_time - start_time + release_time
                note_samples = int(note_duration * sample_rate)

                # Bound note samples to output range
                start_idx = int(start_time * sample_rate)
                end_idx = min(start_idx + note_samples, num_samples)
                actual_samples = end_idx - start_idx

                if actual_samples <= 0:
                    continue

                t_vec = np.arange(actual_samples) / sample_rate
                wave = np.zeros(actual_samples)

                for ratio, amp in harmonics:
                    wave += amp * np.sin(2 * np.pi * freq * ratio * (t_vec + start_time))

                # Apply envelope
                env = adsr_envelope(
                    actual_samples,
                    int(attack_time * sample_rate),
                    int(decay_time * sample_rate),
                    sustain_level,
                    int(release_time * sample_rate)
                )

                # Match lengths if env is shorter than actual_samples due to truncation
                if len(env) < actual_samples:
                    env = np.pad(env, (0, actual_samples - len(env)))
                elif len(env) > actual_samples:
                    env = env[:actual_samples]

                note_audio = wave * env * (velocity / 127.0) * 0.2 # 0.2 for headroom
                output[start_idx:end_idx] += note_audio

    # Final clipping protection
    output = np.clip(output, -1.0, 1.0)

    return output
