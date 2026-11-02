/*
 * flux_envelope.h
 *
 * Spectral flux envelope maths shared by the separator and the report tool: the
 * 2048-point FFT, per-stem transience flux, peak-salience envelope modification,
 * and peak/trough marker detection.
 */

#ifndef FLUX_ENVELOPE_H
#define FLUX_ENVELOPE_H

#include <stdint.h>

typedef struct {
    double r;
    double i;
} Complex;

typedef struct {
    float r;
    float i;
} ComplexFloat;

void init_flux_fft_tables(void);
void fft_2048(Complex *x);

void compute_stem_transience_flux(
    const double *stem_data,
    uint32_t total_samples,
    uint32_t num_channels,
    uint32_t sample_rate,
    double *flux_out,
    int normalize
);

void modify_flux_envelope_channel(double *env, uint32_t total_samples);
void apply_flux_noise_gate(double *env, uint32_t total_samples, double threshold);

void detect_all_peaks(const double *env, uint32_t n, int **out_peaks, int *out_count);
void detect_all_troughs(const double *env, uint32_t n, int **out_troughs, int *out_count);

#endif /* FLUX_ENVELOPE_H */
