/*
 * flux_report_html.h
 *
 * The interactive spectral flux report emitter: one report plus its companion
 * JS waveform data file per source file, and the batch dashboard that links
 * every report in [flux]/.
 */

#ifndef FLUX_REPORT_HTML_H
#define FLUX_REPORT_HTML_H

#include <stddef.h>
#include <stdint.h>

#include "flux_shared.h"

typedef struct {
    int pitch;
    char note_name[32];
    double max_flux_peak;
    double rms_flux;
    int16_t *raw_flux_i16; // mono averaged int16_t PCM array for JS report data
    int16_t *mod_flux_i16; // mono averaged int16_t PCM array for JS report data
    int *peaks;
    int num_peaks;
    int *troughs;
    int num_troughs;
    char mod_filename[1024];
} FluxStem;

char* encode_base64_bytes(const uint8_t *data, size_t input_len);

void generate_flux_html_report(
    const char *flux_dir,
    const char *basename,
    uint32_t sample_rate,
    uint32_t num_channels,
    uint32_t total_samples,
    double duration,
    const double *orig_audio_buffer,
    const FluxStem *stems,
    int stem_count
);

void generate_master_flux_html_report(
    const char *flux_dir,
    const FileBatchResult *results,
    int num_results,
    double total_wall_clock_sec
);

#endif /* FLUX_REPORT_HTML_H */
