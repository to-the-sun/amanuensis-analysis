import numpy as np
from scipy import signal

SOUND_DESIGN_VERSION = 2

def adsr_envelope(duration_samples, attack_samples, decay_samples, sustain_level, release_samples, is_sustained=False):
    """Generate an ADSR envelope. If is_sustained is True, the release phase is omitted."""

    # Calculate phase durations
    a_len = attack_samples
    d_len = decay_samples
    r_len = 0 if is_sustained else release_samples

    # Check if total duration is enough for A+D+R
    total_adr = a_len + d_len + r_len
    if duration_samples < total_adr and total_adr > 0:
        scale = duration_samples / total_adr
        a_len = int(a_len * scale)
        d_len = int(d_len * scale)
        r_len = int(r_len * scale)
        s_len = 0
    else:
        s_len = duration_samples - a_len - d_len - r_len

    # Generate phases
    attack = np.linspace(0, 1, a_len)
    decay = np.linspace(1, sustain_level, d_len)
    sustain = np.full(s_len, sustain_level)

    if is_sustained:
        return np.concatenate([attack, decay, sustain])[:duration_samples]
    else:
        release = np.linspace(sustain_level, 0, r_len)
        return np.concatenate([attack, decay, sustain, release])[:duration_samples]

def render_midi(midi_messages, duration, sample_rate):
    """
    Synthesize audio from a list of MIDI messages.
    Uses Frequency Modulation (FM) and noise for a "different" timbre (brighter, more aggressive).
    """
    num_samples = int(duration * sample_rate)
    output = np.zeros(num_samples)

    # Track active notes: {note_number: start_time}
    active_notes = {}

    # FM parameters
    mod_ratio = 3.5  # Non-integer for inharmonicity
    mod_index = 5.0  # Amount of modulation

    # ADSR parameters (in seconds)
    attack_time = 0.02
    decay_time = 0.1
    sustain_level = 0.4
    release_time = 0.15

    def render_note(note_num, start_time, end_time, velocity, is_sustained=False):
        """Helper to render a single note into the output buffer."""
        freq = 440.0 * (2.0 ** ((note_num - 69) / 12.0))

        if is_sustained:
            note_duration = end_time - start_time
        else:
            note_duration = end_time - start_time + release_time

        note_samples = int(note_duration * sample_rate)

        # Bound note samples to output range
        start_idx = int(start_time * sample_rate)
        end_idx = min(start_idx + note_samples, num_samples)
        actual_samples = end_idx - start_idx

        if actual_samples <= 0:
            return

        t_vec = np.arange(actual_samples) / sample_rate

        # FM Synthesis: Carrier = sin(2*pi*f*t + mod_index * sin(2*pi*f*mod_ratio*t))
        modulator = mod_index * np.sin(2 * np.pi * freq * mod_ratio * (t_vec + start_time))
        wave = np.sin(2 * np.pi * freq * (t_vec + start_time) + modulator)

        # Add a bit of filtered noise for texture
        noise = np.random.normal(0, 0.05, actual_samples)
        # Simple high-pass filter for noise (difference)
        noise = np.diff(noise, prepend=0)

        wave += noise

        # Apply envelope
        env = adsr_envelope(
            actual_samples,
            int(attack_time * sample_rate),
            int(decay_time * sample_rate),
            sustain_level,
            int(release_time * sample_rate),
            is_sustained=is_sustained
        )

        # Match lengths
        if len(env) < actual_samples:
            env = np.pad(env, (0, actual_samples - len(env)))
        elif len(env) > actual_samples:
            env = env[:actual_samples]

        note_audio = wave * env * (velocity / 127.0) * 0.15
        output[start_idx:end_idx] += note_audio

    for msg in midi_messages:
        t = int(msg.time * sample_rate)
        if t >= num_samples:
            continue

        if msg.type == 'note_on' and msg.velocity > 0:
            active_notes[msg.note] = (msg.time, msg.velocity)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            if msg.note in active_notes:
                start_time, velocity = active_notes.pop(msg.note)
                render_note(msg.note, start_time, msg.time, velocity, is_sustained=False)

    # Render any notes that are still active (sustained until end)
    for note_num, (start_time, velocity) in active_notes.items():
        render_note(note_num, start_time, duration, velocity, is_sustained=True)

    # Soft clipping / Saturation
    output = np.tanh(output)

    return output
