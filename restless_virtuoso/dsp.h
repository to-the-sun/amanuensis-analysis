#ifndef DSP_H
#define DSP_H

typedef enum {
    SAMPLE_UNSUPPORTED = -1,
    SAMPLE_INT16,
    SAMPLE_INT24,
    SAMPLE_INT32,
    SAMPLE_INT32_16,
    SAMPLE_INT32_18,
    SAMPLE_INT32_20,
    SAMPLE_INT32_24,
    SAMPLE_FLOAT32
} SampleFormat;

#define SYNTH_VOICES 16

SampleFormat sample_format_from_asio(int asioType);
const char *sample_format_name(int asioType);
int sample_format_bytes(SampleFormat format);

void sample_to_float(const void *src, SampleFormat format, float *dst, int frames);
void sample_from_float(const float *src, void *dst, SampleFormat format, int frames);

void synth_set_rate(int sampleRate);
void synth_note_on(int note, float velocity);
void synth_note_off(int note);
void synth_render(float *dst, int frames);

/* "1,2", "1-4", "3,5-6"; 1-based, returns count or -1. */
int parse_channels(const char *spec, int available, int *out, int max);

#endif
