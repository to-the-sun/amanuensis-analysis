#include "crossfade.h"
#include <math.h>

void ramp_init(t_ramp_state *x, double samplerate, double high_ms) {
    double samples_per_ms = samplerate / 1000.0;
    if (samples_per_ms <= 0) samples_per_ms = 44.1;
    x->length = samples_per_ms * high_ms;
    x->toggle = 0.0;
    x->go = 0.0;
    x->last_amp = 0.0;
}

double ramp_process(t_ramp_state *x, double signal, double movement, long long elapsed, double samplerate, double low_ms, double high_ms, double *fade_out) {
    double samples_per_ms = samplerate / 1000.0;
    if (samples_per_ms <= 0) samples_per_ms = 44.1;
    double high_samples = samples_per_ms * high_ms;
    double low_samples = samples_per_ms * low_ms;

    if (movement != 0.0) {
        x->length = high_samples;
        x->go = (double)elapsed;
        x->toggle = (movement > 0.0) ? 1.0 : 0.0;
    }

    double abs_sig = fabs(signal);
    // slide(abs_sig, 0, low_samples)
    double amp;
    if (abs_sig > x->last_amp) {
        amp = abs_sig; // up=0 means immediate
    } else {
        if (low_samples > 1.0) {
            amp = x->last_amp + (abs_sig - x->last_amp) / low_samples;
        } else {
            amp = abs_sig;
        }
    }
    x->last_amp = amp;

    // length = clip(amp * high_samples, low_samples, x->length)
    double target_length = amp * high_samples;
    if (target_length < low_samples) target_length = low_samples;
    if (target_length > x->length) target_length = x->length;
    x->length = target_length;

    double age = (double)elapsed - x->go;
    double fade = (x->length > 0) ? (age / x->length) : 1.0;
    if (fade > 1.0) fade = 1.0;
    if (fade < 0.0) fade = 0.0;

    fade = fabs(x->toggle - fade);

    *fade_out = fade;
    return signal * fade;
}

void crossfade_init(t_crossfade_state *x, double samplerate, double low_ms, double high_ms) {
    if (low_ms < 0.001) low_ms = 0.001;
    if (high_ms < low_ms) high_ms = low_ms;
    x->samplerate = (samplerate > 0) ? samplerate : 44100.0;
    x->low_ms = low_ms;
    x->high_ms = high_ms;
    ramp_init(&x->ramp1, x->samplerate, high_ms);
    ramp_init(&x->ramp2, x->samplerate, high_ms);
    x->direction = -1.0;
    x->last_control = 0.0;
    x->elapsed = 0;
}

void crossfade_update_params(t_crossfade_state *x, double samplerate, double low_ms, double high_ms) {
    if (low_ms < 0.001) low_ms = 0.001;
    if (high_ms < low_ms) high_ms = low_ms;
    if (samplerate > 0) x->samplerate = samplerate;
    x->low_ms = low_ms;
    x->high_ms = high_ms;
}

void crossfade_process(t_crossfade_state *x, double control, double s1, double s2, double *mix1, double *mix2, double *sum, int *busy) {
    double f1, f2;
    *mix1 = ramp_process(&x->ramp1, s1, x->direction, x->elapsed, x->samplerate, x->low_ms, x->high_ms, &f1);
    *mix2 = ramp_process(&x->ramp2, s2, x->direction * -1.0, x->elapsed, x->samplerate, x->low_ms, x->high_ms, &f2);

    int r1_done = (x->ramp1.toggle > 0.5) ? (f1 <= 0.0) : (f1 >= 1.0);
    int r2_done = (x->ramp2.toggle > 0.5) ? (f2 <= 0.0) : (f2 >= 1.0);
    int finished = r1_done && r2_done;

    x->direction = 0.0;
    if (finished) {
        double diff = control - x->last_control;
        if (diff != 0.0) {
            x->direction = diff;
        }
    }
    x->last_control = control;

    *sum = *mix1 + *mix2;
    *busy = !finished;
    x->elapsed++;
}
