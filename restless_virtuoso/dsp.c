#include "dsp.h"
#include "asio_abi.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.283185307179586f

static int format_bits(SampleFormat format)
{
    switch (format) {
    case SAMPLE_INT16: return 16;
    case SAMPLE_INT24: return 24;
    case SAMPLE_INT32: return 32;
    case SAMPLE_INT32_16: return 16;
    case SAMPLE_INT32_18: return 18;
    case SAMPLE_INT32_20: return 20;
    case SAMPLE_INT32_24: return 24;
    case SAMPLE_FLOAT32: return 32;
    default: return 0;
    }
}

int sample_format_bytes(SampleFormat format)
{
    switch (format) {
    case SAMPLE_INT16: return 2;
    case SAMPLE_INT24: return 3;
    case SAMPLE_INT32:
    case SAMPLE_INT32_16:
    case SAMPLE_INT32_18:
    case SAMPLE_INT32_20:
    case SAMPLE_INT32_24:
    case SAMPLE_FLOAT32: return 4;
    default: return 0;
    }
}

SampleFormat sample_format_from_asio(int type)
{
    switch (type) {
    case ASIOSTInt16LSB: return SAMPLE_INT16;
    case ASIOSTInt24LSB: return SAMPLE_INT24;
    case ASIOSTInt32LSB: return SAMPLE_INT32;
    case ASIOSTInt32LSB16: return SAMPLE_INT32_16;
    case ASIOSTInt32LSB18: return SAMPLE_INT32_18;
    case ASIOSTInt32LSB20: return SAMPLE_INT32_20;
    case ASIOSTInt32LSB24: return SAMPLE_INT32_24;
    case ASIOSTFloat32LSB: return SAMPLE_FLOAT32;
    default: return SAMPLE_UNSUPPORTED;
    }
}

const char *sample_format_name(int type)
{
    switch (type) {
    case ASIOSTInt16LSB: return "Int16LSB";
    case ASIOSTInt24LSB: return "Int24LSB";
    case ASIOSTInt32LSB: return "Int32LSB";
    case ASIOSTInt32LSB16: return "Int32LSB16";
    case ASIOSTInt32LSB18: return "Int32LSB18";
    case ASIOSTInt32LSB20: return "Int32LSB20";
    case ASIOSTInt32LSB24: return "Int32LSB24";
    case ASIOSTFloat32LSB: return "Float32LSB";
    case ASIOSTInt16MSB: return "Int16MSB";
    case ASIOSTInt24MSB: return "Int24MSB";
    case ASIOSTInt32MSB: return "Int32MSB";
    case ASIOSTInt32MSB16: return "Int32MSB16";
    case ASIOSTInt32MSB18: return "Int32MSB18";
    case ASIOSTInt32MSB20: return "Int32MSB20";
    case ASIOSTInt32MSB24: return "Int32MSB24";
    case ASIOSTFloat32MSB: return "Float32MSB";
    case ASIOSTFloat64MSB: return "Float64MSB";
    case ASIOSTFloat64LSB: return "Float64LSB";
    default: return "unknown";
    }
}

void sample_to_float(const void *src, SampleFormat format, float *dst, int frames)
{
    const unsigned char *p = src;

    if (format == SAMPLE_FLOAT32) {
        memcpy(dst, src, (size_t)frames * sizeof(float));
        return;
    }

    const int bytes = sample_format_bytes(format);
    const float scale = 1.0f / (float)(1ull << (format_bits(format) - 1));

    for (int i = 0; i < frames; i++, p += bytes) {
        int32_t value;
        if (bytes == 2) {
            int16_t sample;
            memcpy(&sample, p, sizeof sample);
            value = sample;
        } else if (bytes == 3) {
            value = p[0] | (p[1] << 8) | (p[2] << 16);
            if (value & 0x800000) value -= 0x1000000;
        } else {
            memcpy(&value, p, sizeof value);
        }
        dst[i] = (float)value * scale;
    }
}

void sample_from_float(const float *src, void *dst, SampleFormat format, int frames)
{
    unsigned char *p = dst;

    if (format == SAMPLE_FLOAT32) {
        memcpy(dst, src, (size_t)frames * sizeof(float));
        return;
    }

    const int bytes = sample_format_bytes(format);
    const int bits = format_bits(format);
    const float scale = bits == 32 ? 2147483520.0f : (float)((1u << (bits - 1)) - 1u);

    for (int i = 0; i < frames; i++, p += bytes) {
        float value = src[i];
        if (value > 1.0f) value = 1.0f;
        else if (value < -1.0f) value = -1.0f;
        int32_t sample = (int32_t)lrintf(value * scale);
        if (bytes == 2) {
            int16_t narrowed = (int16_t)sample;
            memcpy(p, &narrowed, sizeof narrowed);
        } else if (bytes == 3) {
            p[0] = (unsigned char)sample;
            p[1] = (unsigned char)(sample >> 8);
            p[2] = (unsigned char)(sample >> 16);
        } else {
            memcpy(p, &sample, sizeof sample);
        }
    }
}

typedef struct {
    float phase, increment, envelope, amplitude;
    int note;
    int active, releasing;
} Voice;

static Voice voices[SYNTH_VOICES];
static int sampleRate = 48000;
static float attackStep = 1.0f / (0.004f * 48000.0f);
static float releaseStep = 1.0f / (0.12f * 48000.0f);
static int nextVoice;

static const float synthLevel = 0.25f;

void synth_set_rate(int rate)
{
    if (rate < 1) return;
    sampleRate = rate;
    attackStep = 1.0f / (0.004f * rate);
    releaseStep = 1.0f / (0.12f * rate);
    for (int i = 0; i < SYNTH_VOICES; i++)
        if (voices[i].active)
            voices[i].increment = 440.0f * powf(2.0f, (voices[i].note - 69) / 12.0f) / rate;
}

void synth_note_on(int note, float velocity)
{
    int slot = -1;

    for (int i = 0; i < SYNTH_VOICES && slot < 0; i++)
        if (voices[i].active && voices[i].note == note) slot = i;
    for (int i = 0; i < SYNTH_VOICES && slot < 0; i++)
        if (!voices[i].active) slot = i;
    if (slot < 0) {
        slot = nextVoice;
        nextVoice = (nextVoice + 1) % SYNTH_VOICES;
    }

    Voice *voice = &voices[slot];
    voice->phase = 0.0f;
    voice->increment = 440.0f * powf(2.0f, (note - 69) / 12.0f) / sampleRate;
    voice->envelope = 0.0f;
    voice->amplitude = velocity * velocity;
    voice->note = note;
    voice->active = 1;
    voice->releasing = 0;
}

void synth_note_off(int note)
{
    for (int i = 0; i < SYNTH_VOICES; i++)
        if (voices[i].active && voices[i].note == note)
            voices[i].releasing = 1;
}

void synth_render(float *dst, int frames)
{
    for (int i = 0; i < SYNTH_VOICES; i++) {
        Voice *voice = &voices[i];
        if (!voice->active) continue;

        float phase = voice->phase;
        float envelope = voice->envelope;
        int releasing = voice->releasing;

        for (int n = 0; n < frames; n++) {
            dst[n] += sinf(phase * TWO_PI) * envelope * voice->amplitude * synthLevel;

            phase += voice->increment;
            if (phase >= 1.0f) phase -= 1.0f;

            if (releasing) {
                envelope -= releaseStep;
                if (envelope <= 0.0f) {
                    envelope = 0.0f;
                    voice->active = 0;
                    break;
                }
            } else if (envelope < 1.0f) {
                envelope += attackStep;
                if (envelope > 1.0f) envelope = 1.0f;
            }
        }

        voice->phase = phase;
        voice->envelope = envelope;
        voice->releasing = releasing;
    }
}

int parse_channels(const char *spec, int available, int *out, int max)
{
    int count = 0;
    const char *p = spec;

    for (;;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        char *end;
        long first = strtol(p, &end, 10);
        if (end == p) return -1;
        p = end;

        long last = first;
        while (*p == ' ') p++;
        if (*p == '-') {
            p++;
            last = strtol(p, &end, 10);
            if (end == p) return -1;
            p = end;
        }

        if (first < 1 || last < first || last > available) return -1;
        for (long channel = first; channel <= last; channel++) {
            if (count >= max) return -1;
            out[count++] = (int)(channel - 1);
        }

        while (*p == ' ') p++;
        if (*p && *p != ',') return -1;
    }

    return count;
}
