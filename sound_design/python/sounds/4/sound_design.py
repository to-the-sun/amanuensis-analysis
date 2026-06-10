import numpy as np
from scipy import signal

SOUND_DESIGN_VERSION = 4

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
    Uses FM Synthesis configured for percussive bell-like timbres.
    """
    num_samples = int(duration * sample_rate)
    output = np.zeros(num_samples)

    # Track active notes: {note_number: start_time}
    active_notes = {}

    # FM Bell parameters
    mod_ratio = 2.718 # Inharmonic ratio for metallic character
    mod_index_max = 8.0
    mod_index_min = 0.5

    # ADSR for Amplitude (Percussive)
    amp_attack = 0.002
    amp_decay = 0.8
    amp_sustain = 0.05
    amp_release = 0.3

    # Modulation Index Envelope (Percussive strike)
    mod_attack = 0.001
    mod_decay = 0.15
    mod_sustain = 0.1
    mod_release = 0.1

    def render_note(note_num, start_time, end_time, velocity, is_sustained=False):
        """Helper to render a single note into the output buffer."""
        freq = 440.0 * (2.0 ** ((note_num - 69) / 12.0))

        if is_sustained:
            note_duration = end_time - start_time
        else:
            note_duration = end_time - start_time + amp_release

        note_samples = int(note_duration * sample_rate)

        # Bound note samples to output range
        start_idx = int(start_time * sample_rate)
        end_idx = min(start_idx + note_samples, num_samples)
        actual_samples = end_idx - start_idx

        if actual_samples <= 0:
            return

        t_vec = np.arange(actual_samples) / sample_rate
        absolute_t = t_vec + start_time

        # 1. Modulation Index Envelope
        mod_env = adsr_envelope(
            actual_samples,
            int(mod_attack * sample_rate),
            int(mod_decay * sample_rate),
            mod_sustain,
            int(mod_release * sample_rate),
            is_sustained=is_sustained
        )
        if len(mod_env) < actual_samples:
            mod_env = np.pad(mod_env, (0, actual_samples - len(mod_env)))
        else:
            mod_env = mod_env[:actual_samples]

        # Scaling mod_env to range [mod_index_min, mod_index_max]
        current_mod_index = mod_index_min + (mod_index_max - mod_index_min) * mod_env

        # 2. Carrier and Modulator FM
        # Carrier = sin(2*pi*fc*t + I(t)*sin(2*pi*fm*t))
        mod_freq = freq * mod_ratio
        modulator = current_mod_index * np.sin(2 * np.pi * mod_freq * absolute_t)
        wave = np.sin(2 * np.pi * freq * absolute_t + modulator)

        # 3. Amplitude Envelope
        amp_env = adsr_envelope(
            actual_samples,
            int(amp_attack * sample_rate),
            int(amp_decay * sample_rate),
            amp_sustain,
            int(amp_release * sample_rate),
            is_sustained=is_sustained
        )
        if len(amp_env) < actual_samples:
            amp_env = np.pad(amp_env, (0, actual_samples - len(amp_env)))
        else:
            amp_env = amp_env[:actual_samples]

        note_audio = wave * amp_env * (velocity / 127.0) * 0.2
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

    # Final saturation
    output = np.tanh(output)

    return output
