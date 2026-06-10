#ifndef SOUND_DESIGN_H
#define SOUND_DESIGN_H

#include <stdint.h>

#define SOUND_DESIGN_VERSION 5

typedef struct {
    char type[16];
    int note;
    int velocity;
    double time;
} MidiMessage;

double* render_midi(MidiMessage* midi_messages, int num_messages, double duration, int sample_rate, int* num_samples_out);

#endif // SOUND_DESIGN_H
