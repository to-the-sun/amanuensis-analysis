/*
 * flux_report.c
 *
 * Builds the [flux] reports on demand, so flux_separator no longer writes the
 * multi-gigabyte <basename>_flux_waveform_data.js (and the report that loads it)
 * while it separates audio.
 *
 * Double-click flux_report.exe inside a pitch_stems folder after flux_separator
 * has run. Everything it needs is already on disk:
 *   [flux]/<basename>_flux_envelopes.bin                 raw envelopes + markers
 *   <basename>_pitch_<nnn>_<note>_modified_flux.wav      modified envelopes
 *   <basename>.wav, one folder up                        the original waveform
 *
 * For every source file found it writes [flux]/<basename>_flux_waveform_data.js,
 * [flux]/<basename>_index.html and, once all files are done, the batch
 * dashboard [flux]/index.html.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <dirent.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <direct.h>
#define MKDIR(dir) _mkdir(dir)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(dir) mkdir(dir, 0755)
#endif

#include "flux_shared.h"
#include "flux_report_html.h"

#define ENVELOPE_SUFFIX "_flux_envelopes.bin"
#define ENVELOPE_MAGIC "FLUXENV1"
#define MAX_STEMS_PER_FILE 128

typedef struct {
    char basename[512];
    char envelope_path[2048];
} SourceFile;

static void ensure_dir_exists(const char *path) {
    MKDIR(path);
}

/* Keeps a double-clicked console window open, without blocking scripted runs. */
static void pause_before_exit(void) {
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    system("pause");
#else
    if (isatty(fileno(stdin))) {
        printf("\nPress ENTER to exit...\n");
        fflush(stdout);
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }
#endif
}

/* Collects every <basename>_flux_envelopes.bin in the [flux] folder. */
static int scan_sources(const char *flux_dir, SourceFile **sources, int *source_count) {
    DIR *d = opendir(flux_dir);
    if (!d) return 0;

    int capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        if (!ends_with_icase(name, ENVELOPE_SUFFIX)) continue;

        size_t base_length = strlen(name) - strlen(ENVELOPE_SUFFIX);
        if (base_length == 0 || base_length + 1 > sizeof(sources[0]->basename)) continue;

        if (*source_count >= capacity) {
            int grown_capacity = (capacity == 0) ? 8 : capacity * 2;
            SourceFile *grown = (SourceFile*)realloc(*sources, (size_t)grown_capacity * sizeof(SourceFile));
            if (!grown) {
                closedir(d);
                return 0;
            }
            *sources = grown;
            capacity = grown_capacity;
        }

        SourceFile *source = &(*sources)[(*source_count)++];
        memset(source, 0, sizeof(SourceFile));
        memcpy(source->basename, name, base_length);
        source->basename[base_length] = '\0';
        snprintf(source->envelope_path, sizeof(source->envelope_path), "%s/%s", flux_dir, name);
    }

    closedir(d);
    return 1;
}

/*
 * read_wav scales 16-bit PCM by 1/32768, while this codebase writes it as
 * round(value * 32767), so multiplying by 32768 recovers the stored sample.
 */
static int16_t wav_channel_to_i16(const WAVFile *wav, uint32_t sample_index, uint32_t channel) {
    double scaled = round(wav->data[(size_t)sample_index * wav->num_channels + channel] * 32768.0);
    if (scaled > 32767.0) scaled = 32767.0;
    if (scaled < 0.0) scaled = 0.0;
    return (int16_t)scaled;
}

/* Averages a flux WAV's channels into the mono int16 curve the report draws. */
static void fill_mono_i16_from_wav(const WAVFile *wav, uint32_t total_samples, int16_t *out) {
    for (uint32_t s = 0; s < total_samples; s++) {
        double sum = 0.0;
        for (uint16_t c = 0; c < wav->num_channels; c++) {
            sum += (double)wav_channel_to_i16(wav, s, c);
        }
        out[s] = (int16_t)round(sum / (double)wav->num_channels);
    }
}

static int read_i32(FILE *f, int32_t *value) {
    return fread(value, sizeof(int32_t), 1, f) == 1;
}

static void free_stem_arrays(FluxStem *stems, int count) {
    for (int i = 0; i < count; i++) {
        if (stems[i].raw_flux_i16) free(stems[i].raw_flux_i16);
        if (stems[i].mod_flux_i16) free(stems[i].mod_flux_i16);
        if (stems[i].peaks) free(stems[i].peaks);
        if (stems[i].troughs) free(stems[i].troughs);
    }
}

/*
 * Rebuilds one source file's report from its stored envelope data plus the
 * modified flux WAVs and original audio on disk, then writes the summary row the
 * batch dashboard shows.
 */
static int build_report_for_source(const SourceFile *source, const char *stem_dir, const char *flux_dir, FileBatchResult *result) {
    clock_t start_clock = clock();

    memset(result, 0, sizeof(FileBatchResult));
    snprintf(result->basename, sizeof(result->basename), "%s", source->basename);
    snprintf(result->input_path, sizeof(result->input_path), "%s.wav", source->basename);

    FILE *envelope_file = fopen(source->envelope_path, "rb");
    if (!envelope_file) {
        printf("  Error: could not open '%s'.\n", source->envelope_path);
        return 0;
    }

    char magic[8];
    uint32_t sample_rate = 0;
    uint32_t total_samples = 0;
    uint16_t envelope_channels = 0;
    uint16_t reserved = 0;
    uint32_t stem_count = 0;

    if (fread(magic, 1, 8, envelope_file) != 8 ||
        memcmp(magic, ENVELOPE_MAGIC, 8) != 0 ||
        fread(&sample_rate, sizeof(uint32_t), 1, envelope_file) != 1 ||
        fread(&total_samples, sizeof(uint32_t), 1, envelope_file) != 1 ||
        fread(&envelope_channels, sizeof(uint16_t), 1, envelope_file) != 1 ||
        fread(&reserved, sizeof(uint16_t), 1, envelope_file) != 1 ||
        fread(&stem_count, sizeof(uint32_t), 1, envelope_file) != 1) {
        printf("  Error: '%s' is not readable flux report data.\n", source->envelope_path);
        fclose(envelope_file);
        return 0;
    }
    (void)reserved;

    if (sample_rate == 0 || total_samples == 0 || stem_count == 0 || stem_count > MAX_STEMS_PER_FILE) {
        printf("  Error: '%s' has unusable report data.\n", source->envelope_path);
        fclose(envelope_file);
        return 0;
    }

    FluxStem *stems = (FluxStem*)calloc((size_t)stem_count, sizeof(FluxStem));
    if (!stems) {
        fclose(envelope_file);
        return 0;
    }

    char original_path[2048];
    WAVFile *original = NULL;
    const char *original_candidates[] = { "%s/../%s.wav", "%s/%s.wav" };
    for (size_t i = 0; i < sizeof(original_candidates) / sizeof(original_candidates[0]); i++) {
        snprintf(original_path, sizeof(original_path), original_candidates[i], stem_dir, source->basename);
        FILE *check = fopen(original_path, "rb");
        if (check) {
            fclose(check);
            original = read_wav(original_path);
            break;
        }
    }
    if (!original) {
        printf("  Warning: original audio '%s.wav' was not found; the report will show no original waveform.\n",
               source->basename);
    }

    int built = 0;
    int data_ok = 1;

    for (uint32_t i = 0; i < stem_count && data_ok; i++) {
        int32_t pitch = 0;
        int32_t num_peaks = 0;
        int32_t num_troughs = 0;

        if (!read_i32(envelope_file, &pitch) || pitch < 0 || pitch > 127) {
            data_ok = 0;
            break;
        }

        FluxStem *stem = &stems[built];
        stem->pitch = (int)pitch;
        get_note_name(stem->pitch, stem->note_name, sizeof(stem->note_name));
        snprintf(stem->mod_filename, sizeof(stem->mod_filename),
                 "%s_pitch_%03d_%s_modified_flux.wav", source->basename, stem->pitch, stem->note_name);

        if (!read_i32(envelope_file, &num_peaks) || num_peaks < 0 || num_peaks > (int32_t)total_samples) {
            data_ok = 0;
            break;
        }
        if (num_peaks > 0) {
            stem->peaks = (int*)malloc((size_t)num_peaks * sizeof(int));
            if (!stem->peaks || fread(stem->peaks, sizeof(int32_t), (size_t)num_peaks, envelope_file) != (size_t)num_peaks) {
                data_ok = 0;
                break;
            }
            stem->num_peaks = (int)num_peaks;
        }

        if (!read_i32(envelope_file, &num_troughs) || num_troughs < 0 || num_troughs > (int32_t)total_samples) {
            data_ok = 0;
            break;
        }
        if (num_troughs > 0) {
            stem->troughs = (int*)malloc((size_t)num_troughs * sizeof(int));
            if (!stem->troughs || fread(stem->troughs, sizeof(int32_t), (size_t)num_troughs, envelope_file) != (size_t)num_troughs) {
                data_ok = 0;
                break;
            }
            stem->num_troughs = (int)num_troughs;
        }

        stem->raw_flux_i16 = (int16_t*)malloc((size_t)total_samples * sizeof(int16_t));
        if (!stem->raw_flux_i16 ||
            fread(stem->raw_flux_i16, sizeof(int16_t), (size_t)total_samples, envelope_file) != (size_t)total_samples) {
            data_ok = 0;
            break;
        }

        built++;
    }
    fclose(envelope_file);

    if (!data_ok) {
        printf("  Error: '%s' ended unexpectedly; no report was written.\n", source->envelope_path);
        free_stem_arrays(stems, built + 1);
        free(stems);
        if (original) {
            free(original->data);
            free(original);
        }
        return 0;
    }

    /* Modified envelopes come straight from the WAVs next to the pitch stems. */
    for (int i = 0; i < built; i++) {
        char mod_path[2048];
        snprintf(mod_path, sizeof(mod_path), "%s/%s", stem_dir, stems[i].mod_filename);

        WAVFile *mod_audio = read_wav(mod_path);
        if (!mod_audio) {
            printf("  Warning: could not read '%s'; its modified envelope is left out of the report.\n", mod_path);
            continue;
        }
        if (mod_audio->sample_rate != sample_rate || mod_audio->total_samples != total_samples) {
            printf("  Warning: '%s' does not match the original audio; its modified envelope is left out.\n", mod_path);
        } else {
            stems[i].mod_flux_i16 = (int16_t*)malloc((size_t)total_samples * sizeof(int16_t));
            if (stems[i].mod_flux_i16) {
                fill_mono_i16_from_wav(mod_audio, total_samples, stems[i].mod_flux_i16);
            }
        }
        free(mod_audio->data);
        free(mod_audio);
    }

    double duration = (double)total_samples / (double)sample_rate;

    generate_flux_html_report(
        flux_dir,
        source->basename,
        sample_rate,
        envelope_channels,
        total_samples,
        duration,
        (original && original->sample_rate == sample_rate &&
         original->num_channels == envelope_channels &&
         original->total_samples == total_samples) ? original->data : NULL,
        stems,
        built
    );

    double elapsed = (double)(clock() - start_clock) / CLOCKS_PER_SEC;
    if (elapsed < 0.001) elapsed = 0.001;

    result->sample_rate = sample_rate;
    result->num_channels = envelope_channels;
    result->total_samples = total_samples;
    result->duration = duration;
    result->stem_count = built;
    result->elapsed_sec = elapsed;
    result->speed_ratio = duration / elapsed;
    result->success = 1;

    free_stem_arrays(stems, built);
    free(stems);
    if (original) {
        free(original->data);
        free(original);
    }
    return 1;
}

int main(void) {
    printf("====================================================\n");
    printf("            SPECTRAL FLUX REPORT BUILDER            \n");
    printf("====================================================\n\n");

    const char *stem_dir = ".";
    char flux_dir[1024];
    snprintf(flux_dir, sizeof(flux_dir), "%s/[flux]", stem_dir);

    SourceFile *sources = NULL;
    int source_count = 0;
    if (!scan_sources(flux_dir, &sources, &source_count)) {
        printf("Error: could not read the [flux] folder.\n");
        pause_before_exit();
        return 1;
    }

    if (source_count == 0) {
        printf("No flux report data was found in %s.\n", flux_dir);
        printf("Run flux_separator first, then double-click flux_report in the pitch_stems folder it creates.\n");
        free(sources);
        pause_before_exit();
        return 1;
    }

    ensure_dir_exists(flux_dir);

    printf("Source files found: %d\n\n", source_count);

    FileBatchResult *results = (FileBatchResult*)calloc((size_t)source_count, sizeof(FileBatchResult));
    if (!results) {
        printf("Error: out of memory.\n");
        free(sources);
        pause_before_exit();
        return 1;
    }

    clock_t batch_start_clock = clock();
    int built = 0;

    for (int i = 0; i < source_count; i++) {
        printf("[%d/%d] %s\n", i + 1, source_count, sources[i].basename);
        fflush(stdout);
        if (build_report_for_source(&sources[i], stem_dir, flux_dir, &results[i])) {
            built++;
            printf("  Wrote %s/%s_flux_waveform_data.js and %s/%s_index.html (%.1fs)\n",
                   flux_dir, sources[i].basename, flux_dir, sources[i].basename,
                   results[i].elapsed_sec);
        }
        fflush(stdout);
    }

    double total_elapsed = (double)(clock() - batch_start_clock) / CLOCKS_PER_SEC;

    /* Compact the successful rows so the dashboard only lists real reports. */
    int row = 0;
    for (int i = 0; i < source_count; i++) {
        if (results[i].success) {
            if (row != i) results[row] = results[i];
            row++;
        }
    }

    generate_master_flux_html_report(flux_dir, results, built, total_elapsed);

    printf("\n====================================================\n");
    printf("                REPORT BUILD COMPLETE               \n");
    printf("====================================================\n");
    printf("  Reports Written:      %d / %d source files\n", built, source_count);
    printf("  Total Build Time:     %.2f seconds\n", total_elapsed);
    printf("  Report Folder:        %s/\n", flux_dir);
    printf("  Batch Dashboard:      %s/index.html\n", flux_dir);
    printf("====================================================\n");

    free(sources);
    free(results);

    pause_before_exit();
    return (built == source_count) ? 0 : 1;
}
