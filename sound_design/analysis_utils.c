#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sndfile.h>
#include <aubio/aubio.h>
#include "analysis_utils.h"

MidiMessage DEFAULT_MIDI_SEQUENCE[] = {
    {"note_on", 60, 100, 0.0},
    {"note_off", 60, 0,   0.5},
    {"note_on", 64, 100, 0.5},
    {"note_off", 64, 0,   1.0},
    {"note_on", 67, 100, 1.0},
    {"note_off", 67, 0,   1.5},
    {"note_on", 72, 120, 1.5},
    {"note_off", 72, 0,   3.0},
};
int DEFAULT_MIDI_SEQUENCE_LEN = 8;

void save_wav(const char* filename, double* buffer, int num_samples, int sample_rate) {
    SF_INFO sfinfo;
    sfinfo.channels = 1;
    sfinfo.samplerate = sample_rate;
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* outfile = sf_open(filename, SFM_WRITE, &sfinfo);
    if (!outfile) {
        fprintf(stderr, "Error: could not open output file %s\n", filename);
        return;
    }

    sf_write_double(outfile, buffer, num_samples);
    sf_close(outfile);
}

double calculate_distance(struct json_object* results1, struct json_object* results2) {
    struct json_object *mfcc1, *mfcc2;
    if (!json_object_object_get_ex(results1, "mfcc_means", &mfcc1) ||
        !json_object_object_get_ex(results2, "mfcc_means", &mfcc2)) {
        return -1.0;
    }

    int len1 = json_object_array_length(mfcc1);
    int len2 = json_object_array_length(mfcc2);
    int min_len = len1 < len2 ? len1 : len2;

    double sum_sq = 0.0;
    for (int i = 0; i < min_len; i++) {
        double val1 = json_object_get_double(json_object_array_get_idx(mfcc1, i));
        double val2 = json_object_get_double(json_object_array_get_idx(mfcc2, i));
        sum_sq += pow(val1 - val2, 2);
    }

    return sqrt(sum_sq);
}

struct json_object* analyze_audio(double* audio, int num_samples, int sr) {
    uint_t hop_size = (uint_t)(sr * 0.050);
    uint_t win_size = (uint_t)(sr * 0.050);

    fvec_t *in = new_fvec(hop_size);
    aubio_specdesc_t *sd_centroid = new_aubio_specdesc("centroid", win_size);
    aubio_specdesc_t *sd_kurtosis = new_aubio_specdesc("kurtosis", win_size);
    aubio_specdesc_t *sd_spread = new_aubio_specdesc("spread", win_size);
    aubio_pvoc_t *pv = new_aubio_pvoc(win_size, hop_size);
    cvec_t *fftgrain = new_cvec(win_size);
    fvec_t *out_centroid = new_fvec(1);
    fvec_t *out_kurtosis = new_fvec(1);
    fvec_t *out_spread = new_fvec(1);
    aubio_mfcc_t *mfcc = new_aubio_mfcc(win_size, 40, 13, sr);
    fvec_t *out_mfcc = new_fvec(13);
    aubio_tempo_t *tempo_obj = new_aubio_tempo("default", win_size, hop_size, sr);
    fvec_t *tempo_out = new_fvec(2);

    struct json_object *results = json_object_new_object();
    struct json_object *temporal_data = json_object_new_object();
    struct json_object *times_arr = json_object_new_array();
    struct json_object *rms_arr = json_object_new_array();
    struct json_object *centroid_arr = json_object_new_array();
    struct json_object *bandwidth_arr = json_object_new_array();
    struct json_object *flatness_arr = json_object_new_array();
    struct json_object *zcr_arr = json_object_new_array();
    struct json_object *mfccs_temporal = json_object_new_array();

    double total_rms = 0, peak_rms = 0;
    double total_centroid = 0, total_bandwidth = 0, total_flatness = 0, total_zcr = 0;
    double mfcc_means[13] = {0};
    int num_frames = 0;

    for (int i = 0; i < num_samples; i += hop_size) {
        int remaining = num_samples - i;
        int current_hop = remaining < hop_size ? remaining : hop_size;
        for (int j = 0; j < current_hop; j++) in->data[j] = (smpl_t)audio[i + j];
        for (int j = current_hop; j < hop_size; j++) in->data[j] = 0;

        double rms_val = aubio_level_lin(in);
        total_rms += rms_val;
        if (rms_val > peak_rms) peak_rms = rms_val;
        json_object_array_add(rms_arr, json_object_new_double(rms_val));

        aubio_pvoc_do(pv, in, fftgrain);
        aubio_specdesc_do(sd_centroid, fftgrain, out_centroid);
        aubio_specdesc_do(sd_kurtosis, fftgrain, out_kurtosis);
        aubio_specdesc_do(sd_spread, fftgrain, out_spread);

        double c_val = aubio_bintofreq(out_centroid->data[0], sr, win_size);
        double f_val = out_kurtosis->data[0];
        double s_val = out_spread->data[0];
        total_centroid += c_val;
        total_flatness += f_val;
        total_bandwidth += s_val;
        json_object_array_add(centroid_arr, json_object_new_double(c_val));
        json_object_array_add(flatness_arr, json_object_new_double(f_val));
        json_object_array_add(bandwidth_arr, json_object_new_double(s_val));

        double zcr_val = 0;
        for (int j = 1; j < current_hop; j++) {
            if ((in->data[j-1] < 0 && in->data[j] >= 0) || (in->data[j-1] >= 0 && in->data[j] < 0)) zcr_val++;
        }
        zcr_val /= current_hop;
        total_zcr += zcr_val;
        json_object_array_add(zcr_arr, json_object_new_double(zcr_val));

        aubio_mfcc_do(mfcc, fftgrain, out_mfcc);
        struct json_object *mfcc_frame = json_object_new_array();
        for (int j = 0; j < 13; j++) {
            mfcc_means[j] += out_mfcc->data[j];
            json_object_array_add(mfcc_frame, json_object_new_double(out_mfcc->data[j]));
        }
        json_object_array_add(mfccs_temporal, mfcc_frame);
        aubio_tempo_do(tempo_obj, in, tempo_out);
        json_object_array_add(times_arr, json_object_new_double((double)i / sr));
        num_frames++;
    }

    json_object_object_add(results, "average_rms", json_object_new_double(total_rms / num_frames));
    json_object_object_add(results, "peak_rms", json_object_new_double(peak_rms));
    json_object_object_add(results, "average_spectral_centroid", json_object_new_double(total_centroid / num_frames));
    json_object_object_add(results, "average_spectral_bandwidth", json_object_new_double(total_bandwidth / num_frames));
    json_object_object_add(results, "average_spectral_flatness", json_object_new_double(total_flatness / num_frames));
    json_object_object_add(results, "average_zero_crossing_rate", json_object_new_double(total_zcr / num_frames));

    struct json_object *mfcc_means_arr = json_object_new_array();
    for (int j = 0; j < 13; j++) json_object_array_add(mfcc_means_arr, json_object_new_double(mfcc_means[j] / num_frames));
    json_object_object_add(results, "mfcc_means", mfcc_means_arr);
    json_object_object_add(results, "estimated_tempo", json_object_new_double(aubio_tempo_get_bpm(tempo_obj)));

    json_object_object_add(temporal_data, "times", times_arr);
    json_object_object_add(temporal_data, "rms", rms_arr);
    json_object_object_add(temporal_data, "spectral_centroid", centroid_arr);
    json_object_object_add(temporal_data, "spectral_bandwidth", bandwidth_arr);
    json_object_object_add(temporal_data, "spectral_flatness", flatness_arr);
    json_object_object_add(temporal_data, "zero_crossing_rate", zcr_arr);
    json_object_object_add(temporal_data, "mfccs", mfccs_temporal);
    json_object_object_add(results, "temporal_data", temporal_data);

    del_fvec(in); del_aubio_specdesc(sd_centroid); del_aubio_specdesc(sd_kurtosis); del_aubio_specdesc(sd_spread);
    del_aubio_pvoc(pv); del_cvec(fftgrain); del_fvec(out_centroid); del_fvec(out_kurtosis); del_fvec(out_spread);
    del_aubio_mfcc(mfcc); del_fvec(out_mfcc); del_aubio_tempo(tempo_obj); del_fvec(tempo_out);

    return results;
}
