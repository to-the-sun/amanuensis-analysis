#ifndef ANALYSIS_UTILS_H
#define ANALYSIS_UTILS_H

#include <json-c/json.h>
#include "sound_design.h"

extern MidiMessage DEFAULT_MIDI_SEQUENCE[];
extern int DEFAULT_MIDI_SEQUENCE_LEN;

void save_wav(const char* filename, double* buffer, int num_samples, int sample_rate);
double calculate_distance(struct json_object* results1, struct json_object* results2);
struct json_object* analyze_audio(double* audio, int num_samples, int sr);

#endif
