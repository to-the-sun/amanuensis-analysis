#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include "crossfade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <windows.h>
#define mkdir_cross(dir) _mkdir(dir)
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#else
#include <unistd.h>
#define mkdir_cross(dir) mkdir(dir, 0755)
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFDIR) == S_IFDIR)
#endif

static int iequals(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static int has_wav_ext(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return 0;
    return iequals(filename + len - 4, ".wav");
}

static int get_parent_directory(const char* dir_path, char* out_parent, size_t out_size) {
    if (!dir_path || dir_path[0] == '\0') {
        out_parent[0] = '\0';
        return 0;
    }
    char tmp[4096];
    strncpy(tmp, dir_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
        len--;
    }

    char* last_slash = strrchr(tmp, '/');
    char* last_backslash = strrchr(tmp, '\\');
    char* last_sep = (last_backslash && (!last_slash || last_backslash > last_slash)) ? last_backslash : last_slash;

    if (!last_sep) {
        out_parent[0] = '\0';
        return 0;
    }

    if (last_sep == tmp) {
        snprintf(out_parent, out_size, "/");
    } else if (last_sep == tmp + 2 && tmp[1] == ':') {
        snprintf(out_parent, out_size, "%c:\\", tmp[0]);
    } else {
        size_t parent_len = (size_t)(last_sep - tmp);
        if (parent_len >= out_size) parent_len = out_size - 1;
        strncpy(out_parent, tmp, parent_len);
        out_parent[parent_len] = '\0';
    }

    if (strcmp(out_parent, dir_path) == 0) {
        return 0;
    }
    return 1;
}

static void write_stem_pattern_wav_from_file(
    const char* stem_wav_path,
    const char* src_wav_path,
    double start_ms,
    double end_ms,
    double blank_space_ms
) {
    if (!stem_wav_path || !src_wav_path || src_wav_path[0] == '\0') return;

    drwav wav_in;
    if (!drwav_init_file(&wav_in, src_wav_path, NULL)) return;

    uint32_t num_ch = wav_in.channels;
    uint32_t pat_sr = wav_in.sampleRate;
    uint64_t pat_len = wav_in.totalPCMFrameCount;

    uint64_t start_smp = (uint64_t)round((start_ms / 1000.0) * pat_sr);
    uint64_t end_smp = (uint64_t)round((end_ms / 1000.0) * pat_sr);
    if (start_smp >= pat_len) start_smp = pat_len > 0 ? pat_len - 1 : 0;
    if (end_smp > pat_len) end_smp = pat_len;
    uint64_t frame_count = (end_smp > start_smp) ? (end_smp - start_smp) : 0;

    drwav_seek_to_pcm_frame(&wav_in, start_smp);

    drwav_data_format format;
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_PCM;
    format.channels = 1;
    format.sampleRate = pat_sr;
    format.bitsPerSample = 16;

    uint64_t blank_frames = (uint64_t)round((blank_space_ms / 1000.0) * pat_sr);

    drwav wav_stem_out;
    if (drwav_init_file_write(&wav_stem_out, stem_wav_path, &format, NULL)) {
        int16_t chunk[8192];
        memset(chunk, 0, sizeof(chunk));

        // Write blank frames (silence)
        uint64_t remaining_blank = blank_frames;
        while (remaining_blank > 0) {
            uint64_t write_cnt = (remaining_blank < 8192) ? remaining_blank : 8192;
            drwav_write_pcm_frames(&wav_stem_out, write_cnt, chunk);
            remaining_blank -= write_cnt;
        }

        // Initialize crossfade state for dynamic amplitude-based fade in/out
        double crossfade_low_ms = (999.0 / (double)pat_sr) * 1000.0;
        double crossfade_high_ms = 4999.0;
        t_crossfade_state cf;
        crossfade_init(&cf, (double)pat_sr, crossfade_low_ms, crossfade_high_ms);

        int warm_samples = (int)(pat_sr * (cf.high_ms / 1000.0));
        if (warm_samples < 1) warm_samples = 1;
        for (int w = 0; w < warm_samples; w++) {
            double m1, m2, s_val;
            int busy_status;
            crossfade_process(&cf, 0.0, 0.0, 0.0, &m1, &m2, &s_val, &busy_status);
        }

        float f32_chunk[8192 * 16];
        uint64_t remaining_audio = frame_count;
        while (remaining_audio > 0) {
            uint64_t to_read = (remaining_audio < 8192) ? remaining_audio : 8192;
            drwav_uint64 read_cnt = drwav_read_pcm_frames_f32(&wav_in, to_read, f32_chunk);
            for (uint64_t i = 0; i < to_read; i++) {
                float smp = 0.0f;
                if (i < read_cnt) {
                    double sum = 0.0;
                    for (uint32_t c = 0; c < num_ch; c++) sum += f32_chunk[i * num_ch + c];
                    smp = (float)(sum / num_ch);
                }
                double mix1, mix2, sum_out;
                int busy_status;
                crossfade_process(&cf, 1.0, 0.0, (double)smp, &mix1, &mix2, &sum_out, &busy_status);

                if (sum_out > 1.0) sum_out = 1.0;
                if (sum_out < -1.0) sum_out = -1.0;
                chunk[i] = (int16_t)(sum_out * 32767.0);
            }
            drwav_write_pcm_frames(&wav_stem_out, to_read, chunk);
            remaining_audio -= to_read;
        }

        uint64_t max_fade_frames = (uint64_t)round((cf.high_ms / 1000.0) * pat_sr);
        uint64_t fade_written = 0;
        int busy_status = 1;
        while (fade_written < max_fade_frames) {
            uint64_t to_read = ((max_fade_frames - fade_written) < 8192) ? (max_fade_frames - fade_written) : 8192;
            drwav_uint64 read_cnt = drwav_read_pcm_frames_f32(&wav_in, to_read, f32_chunk);
            for (uint64_t i = 0; i < to_read; i++) {
                float smp = 0.0f;
                if (i < read_cnt) {
                    double sum = 0.0;
                    for (uint32_t c = 0; c < num_ch; c++) sum += f32_chunk[i * num_ch + c];
                    smp = (float)(sum / num_ch);
                }
                double mix1, mix2, sum_out;
                crossfade_process(&cf, 0.0, 0.0, (double)smp, &mix1, &mix2, &sum_out, &busy_status);

                if (sum_out > 1.0) sum_out = 1.0;
                if (sum_out < -1.0) sum_out = -1.0;
                chunk[i] = (int16_t)(sum_out * 32767.0);
            }
            drwav_write_pcm_frames(&wav_stem_out, to_read, chunk);
            fade_written += to_read;
        }

        drwav_uninit(&wav_stem_out);
    }
    drwav_uninit(&wav_in);
}

static int find_flux_dir_and_recombined_file(
    const char* stems_dir,
    char* out_flux_filepath,
    size_t out_size
) {
    if (!stems_dir || !out_flux_filepath || out_size == 0) return 0;

    // Navigate up from stems_dir to find [flux] sibling
    char current[4096];
    strncpy(current, stems_dir, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int depth = 0; depth < 5; depth++) {
        char flux_cand[4096];
        snprintf(flux_cand, sizeof(flux_cand), "%s" PATH_SEP_STR "[flux]", current);

        DIR* d = opendir(flux_cand);
        if (!d) {
            snprintf(flux_cand, sizeof(flux_cand), "%s" PATH_SEP_STR "flux", current);
            d = opendir(flux_cand);
        }

        if (d) {
            struct dirent* entry;
            while ((entry = readdir(d)) != NULL) {
                const char* name = entry->d_name;
                if (!has_wav_ext(name)) continue;

                char lower[1024];
                size_t n_len = strlen(name);
                if (n_len >= sizeof(lower)) n_len = sizeof(lower) - 1;
                for (size_t i = 0; i < n_len; i++) lower[i] = (char)tolower((unsigned char)name[i]);
                lower[n_len] = '\0';

                if (strstr(lower, "recombined") != NULL) {
                    snprintf(out_flux_filepath, out_size, "%s" PATH_SEP_STR "%s", flux_cand, name);
                    closedir(d);
                    return 1;
                }
            }
            closedir(d);
        }

        char parent[4096];
        if (!get_parent_directory(current, parent, sizeof(parent))) break;
        strncpy(current, parent, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';
    }

    return 0;
}

int main(int argc, char** argv) {
    printf("============================================================\n");
    printf("  Standalone C Recombined Pattern WAV Builder\n");
    printf("============================================================\n\n");

    char stems_dir[4096] = "";
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        strncpy(stems_dir, argv[1], sizeof(stems_dir) - 1);
        stems_dir[sizeof(stems_dir) - 1] = '\0';
    }

    if (stems_dir[0] == '\0' || strcmp(stems_dir, ".") == 0 || strcmp(stems_dir, "..") == 0) {
        char abs_path[4096] = "";
#if defined(_WIN32) || defined(_WIN64)
        if (_getcwd(abs_path, sizeof(abs_path)) != NULL) {
#else
        if (getcwd(abs_path, sizeof(abs_path)) != NULL) {
#endif
            strncpy(stems_dir, abs_path, sizeof(stems_dir) - 1);
            stems_dir[sizeof(stems_dir) - 1] = '\0';
        } else {
            strcpy(stems_dir, ".");
        }
    }

    size_t sd_len = strlen(stems_dir);
    while (sd_len > 1 && (stems_dir[sd_len - 1] == '/' || stems_dir[sd_len - 1] == '\\')) {
        stems_dir[sd_len - 1] = '\0';
        sd_len--;
    }

    DIR* dir = opendir(stems_dir);
    if (!dir) {
        printf("Error: Could not open stems directory: %s\n", stems_dir);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    char rec_flux_filepath[4096] = "";
    if (!find_flux_dir_and_recombined_file(stems_dir, rec_flux_filepath, sizeof(rec_flux_filepath))) {
        printf("Error: Could not locate recombined flux WAV file in sibling [flux] folder.\n");
        closedir(dir);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    printf("Located recombined flux audio source: %s\n\n", rec_flux_filepath);

    struct dirent* entry;
    int created_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (!has_wav_ext(name)) continue;

        char lower[1024];
        size_t n_len = strlen(name);
        if (n_len >= sizeof(lower)) n_len = sizeof(lower) - 1;
        for (size_t i = 0; i < n_len; i++) lower[i] = (char)tolower((unsigned char)name[i]);
        lower[n_len] = '\0';

        if (strstr(lower, "recombined") != NULL) continue;
        if (strstr(lower, "pattern") == NULL) continue;

        char pat_stem_path[4096];
        snprintf(pat_stem_path, sizeof(pat_stem_path), "%s" PATH_SEP_STR "%s", stems_dir, name);

        drwav pat_wav;
        if (!drwav_init_file(&pat_wav, pat_stem_path, NULL)) continue;

        double duration_ms = ((double)pat_wav.totalPCMFrameCount / (double)pat_wav.sampleRate) * 1000.0;
        drwav_uninit(&pat_wav);

        const char* dot = strrchr(name, '.');
        size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);

        char companion_name[1024];
        snprintf(companion_name, sizeof(companion_name), "%.*s_recombined.wav", (int)stem_len, name);

        char companion_path[4096];
        snprintf(companion_path, sizeof(companion_path), "%s" PATH_SEP_STR "%s", stems_dir, companion_name);

        // Slice companion WAV from recombined flux audio file
        write_stem_pattern_wav_from_file(companion_path, rec_flux_filepath, 0.0, duration_ms, 0.0);
        printf("  -> Generated companion WAV: %s\n", companion_name);
        created_count++;
    }
    closedir(dir);

    printf("\n============================================================\n");
    printf("COMPANION PATTERN WAV CREATION COMPLETE!\n");
    printf("Created %d companion recombined pattern WAV file(s).\n", created_count);
    printf("============================================================\n\n");

    printf("Press Enter to exit...");
    getchar();
    return 0;
}
