#include "sound_design.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double start_time;
    int velocity;
    int active;
} ActiveNote;

static void adsr_envelope(double* buffer, int duration_samples, int attack_samples, int decay_samples, double sustain_level, int release_samples, int is_sustained) {
    int a_len = attack_samples;
    int d_len = decay_samples;
    int r_len = is_sustained ? 0 : release_samples;

    int total_adr = a_len + d_len + r_len;
    int s_len;

    if (duration_samples < total_adr && total_adr > 0) {
        double scale = (double)duration_samples / total_adr;
        a_len = (int)(a_len * scale);
        d_len = (int)(d_len * scale);
        r_len = (int)(r_len * scale);
        s_len = 0;
    } else {
        s_len = duration_samples - a_len - d_len - r_len;
    }

    int current = 0;
    for (int i = 0; i < a_len && current < duration_samples; i++, current++) buffer[current] = (double)i / a_len;
    for (int i = 0; i < d_len && current < duration_samples; i++, current++) buffer[current] = 1.0 - (1.0 - sustain_level) * ((double)i / d_len);
    for (int i = 0; i < s_len && current < duration_samples; i++, current++) buffer[current] = sustain_level;
    if (!is_sustained) {
        for (int i = 0; i < r_len && current < duration_samples; i++, current++) buffer[current] = sustain_level * (1.0 - (double)i / r_len);
    }
    while (current < duration_samples) buffer[current++] = is_sustained ? sustain_level : 0.0;
}

static void render_note(double* output, int num_samples, int note_num, double start_time, double end_time, int velocity, int sample_rate, int is_sustained) {
    double freq = 440.0 * pow(2.0, (note_num - 69) / 12.0);
    int L = (int)(sample_rate / freq);
    if (L < 2) L = 2;

    // Karplus-Strong parameters
    double feedback = 0.994;

    // Duration: if sustained, go to end of buffer.
    // If not, allow some ring-out time for the pluck to decay.
    double ring_out = 2.0;
    double duration = is_sustained ? (end_time - start_time) : (end_time - start_time + ring_out);
    int note_samples = (int)(duration * sample_rate);

    int start_idx = (int)(start_time * sample_rate);
    if (start_idx >= num_samples) return;

    int end_idx = start_idx + note_samples;
    if (end_idx > num_samples) end_idx = num_samples;
    int actual_samples = end_idx - start_idx;

    if (actual_samples <= 0) return;

    double* delay_line = (double*)malloc(L * sizeof(double));
    // Use a deterministic seed for consistency in analysis results
    srand(note_num);
    for (int i = 0; i < L; i++) {
        delay_line[i] = ((double)rand() / RAND_MAX) * 2.0 - 1.0;
    }

    int ptr = 0;
    double prev_val = 0;

    for (int i = 0; i < actual_samples; i++) {
        double current_val = delay_line[ptr];

        // Moving average low-pass filter
        double filtered = (current_val + prev_val) * 0.5;
        prev_val = current_val;

        // Feedback into the delay line
        delay_line[ptr] = filtered * feedback;

        ptr++;
        if (ptr >= L) ptr = 0;

        // Simple output gain and velocity scaling
        output[start_idx + i] += filtered * (velocity / 127.0) * 0.4;
    }

    free(delay_line);
}

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out) {
    int num_samples = (int)(duration * sample_rate);
    *num_samples_out = num_samples;
    double* output = (double*)calloc(num_samples, sizeof(double));

    ActiveNote active_notes[128];
    for (int i = 0; i < 128; i++) active_notes[i].active = 0;

    for (int i = 0; i < num_messages; i++) {
        MidiMessage msg = midi_messages[i];
        if (msg.time >= duration) continue;

        if (strcmp(msg.type, "note_on") == 0 && msg.velocity > 0) {
            active_notes[msg.note].start_time = msg.time;
            active_notes[msg.note].velocity = msg.velocity;
            active_notes[msg.note].active = 1;
        } else if (strcmp(msg.type, "note_off") == 0 || (strcmp(msg.type, "note_on") == 0 && msg.velocity == 0)) {
            if (active_notes[msg.note].active) {
                render_note(output, num_samples, msg.note, active_notes[msg.note].start_time, msg.time, active_notes[msg.note].velocity, sample_rate, 0);
                active_notes[msg.note].active = 0;
            }
        }
    }

    for (int i = 0; i < 128; i++) {
        if (active_notes[i].active) {
            render_note(output, num_samples, i, active_notes[i].start_time, duration, active_notes[i].velocity, sample_rate, 1);
        }
    }

    // Final soft clipping to prevent harsh digital distortion
    for (int i = 0; i < num_samples; i++) {
        output[i] = tanh(output[i]);
    }

    return output;
}
