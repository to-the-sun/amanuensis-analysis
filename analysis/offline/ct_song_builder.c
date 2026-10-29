#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

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

static int mkdir_p(const char* path) {
    if (!path || !*path) return 0;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
        len--;
    }

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (strlen(tmp) > 0) {
                mkdir_cross(tmp);
            }
            *p = '/';
        }
    }
    return mkdir_cross(tmp);
}

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

static int copy_file(const char* src_path, const char* dst_path) {
    if (!src_path || !dst_path) return 0;
    if (strcmp(src_path, dst_path) == 0) return 1;

    FILE* src = fopen(src_path, "rb");
    if (!src) return 0;

    FILE* dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return 0;
    }

    char buffer[65536];
    size_t bytes_read;
    int success = 1;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dst) != bytes_read) {
            success = 0;
            break;
        }
    }

    fclose(src);
    fclose(dst);
    return success;
}

typedef struct {
    char filename[1024];
    char full_path[4096];
    char recombined_filename[1024];
    char recombined_full_path[4096];
    uint64_t total_frames;
    uint32_t sample_rate;
    double duration_ms;
    float* rec_flux_data;
    uint64_t rec_flux_frames;
    uint32_t rec_flux_sr;
} SongPatternItem;

static int compare_patterns_desc(const void* a, const void* b) {
    const SongPatternItem* pa = (const SongPatternItem*)a;
    const SongPatternItem* pb = (const SongPatternItem*)b;
    if (pb->duration_ms > pa->duration_ms) return 1;
    if (pb->duration_ms < pa->duration_ms) return -1;
    return 0;
}

static double compute_total_correlation(
    int num_items,
    const SongPatternItem* items,
    const int* active_mask
) {
    if (!items || !active_mask || num_items <= 0) return 0.0;

    double total_corr_sum = 0.0;
    double total_cap_sum = 0.0;

    for (int p = 0; p < num_items; p++) {
        if (!active_mask[p] || !items[p].rec_flux_data || items[p].rec_flux_frames == 0) continue;

        uint64_t len_p = items[p].rec_flux_frames;
        const float* buf_p = items[p].rec_flux_data;

        for (uint64_t i = 0; i < len_p; i++) {
            float cap = fabsf(buf_p[i]);
            if (cap <= 1e-7f) continue;

            float sum_other = 0.0f;
            int overlap_count = 0;

            for (int q = 0; q < num_items; q++) {
                if (q == p || !active_mask[q] || !items[q].rec_flux_data || items[q].rec_flux_frames == 0) continue;

                if (i < items[q].rec_flux_frames) {
                    sum_other += fabsf(items[q].rec_flux_data[i]);
                    overlap_count++;
                }
            }

            if (overlap_count > 0) {
                double corr = (double)sum_other / (double)cap;
                if (corr > 1.0) corr = 1.0;
                total_corr_sum += corr;
                total_cap_sum += (double)cap;
            }
        }
    }

    if (total_cap_sum <= 0.0) return 0.0;
    return total_corr_sum / total_cap_sum;
}

static float* read_mono_wav_pcm(const char* filepath, uint64_t* out_frames, uint32_t* out_sr) {
    if (!filepath || filepath[0] == '\0') return NULL;

    unsigned int ch = 0, sr = 0;
    drwav_uint64 frames = 0;
    float* pcm = drwav_open_file_and_read_pcm_frames_f32(filepath, &ch, &sr, &frames, NULL);
    if (!pcm || frames == 0 || sr == 0) {
        if (pcm) drwav_free(pcm, NULL);
        return NULL;
    }

    float* mono = (float*)malloc(frames * sizeof(float));
    if (!mono) {
        drwav_free(pcm, NULL);
        return NULL;
    }

    if (ch == 1) {
        memcpy(mono, pcm, frames * sizeof(float));
    } else {
        for (drwav_uint64 f = 0; f < frames; f++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < ch; c++) sum += pcm[f * ch + c];
            mono[f] = (float)(sum / ch);
        }
    }

    drwav_free(pcm, NULL);
    if (out_frames) *out_frames = frames;
    if (out_sr) *out_sr = sr;
    return mono;
}

int main(int argc, char** argv) {
    printf("============================================================\n");
    printf("  Standalone C Pattern Correlation Song Builder\n");
    printf("============================================================\n\n");

    char target_dir[4096] = ".";
    if (argc > 1 && argv[1] && argv[1][0] != '\0') {
        strncpy(target_dir, argv[1], sizeof(target_dir) - 1);
        target_dir[sizeof(target_dir) - 1] = '\0';
    }

    size_t td_len = strlen(target_dir);
    while (td_len > 1 && (target_dir[td_len - 1] == '/' || target_dir[td_len - 1] == '\\')) {
        target_dir[td_len - 1] = '\0';
        td_len--;
    }

    DIR* dir = opendir(target_dir);
    if (!dir) {
        printf("Error: Could not open directory: %s\n", target_dir);
        printf("\nPress Enter to exit...");
        getchar();
        return 1;
    }

    SongPatternItem raw_items[512];
    int item_count = 0;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (!has_wav_ext(name)) continue;

        char lower_name[1024];
        size_t n_len = strlen(name);
        if (n_len >= sizeof(lower_name)) n_len = sizeof(lower_name) - 1;
        for (size_t i = 0; i < n_len; i++) lower_name[i] = (char)tolower((unsigned char)name[i]);
        lower_name[n_len] = '\0';

        // Ignore recombined WAV files when searching for primary pattern WAV files
        if (strstr(lower_name, "recombined") != NULL) continue;
        if (strstr(lower_name, "pattern_") == NULL && strstr(lower_name, "pattern") == NULL) continue;

        char full_p[4096];
        snprintf(full_p, sizeof(full_p), "%s" PATH_SEP_STR "%s", target_dir, name);

        struct stat st;
        if (stat(full_p, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        drwav wav_info;
        if (!drwav_init_file(&wav_info, full_p, NULL)) continue;

        if (item_count < 512) {
            strncpy(raw_items[item_count].filename, name, sizeof(raw_items[item_count].filename) - 1);
            strncpy(raw_items[item_count].full_path, full_p, sizeof(raw_items[item_count].full_path) - 1);
            raw_items[item_count].total_frames = wav_info.totalPCMFrameCount;
            raw_items[item_count].sample_rate = wav_info.sampleRate;
            raw_items[item_count].duration_ms = ((double)wav_info.totalPCMFrameCount / (double)wav_info.sampleRate) * 1000.0;
            raw_items[item_count].rec_flux_data = NULL;
            raw_items[item_count].rec_flux_frames = 0;
            raw_items[item_count].rec_flux_sr = 0;
            raw_items[item_count].recombined_filename[0] = '\0';
            raw_items[item_count].recombined_full_path[0] = '\0';

            // Find companion recombined WAV file in target_dir
            const char* dot = strrchr(name, '.');
            size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
            char rec_name_cand[1024];
            snprintf(rec_name_cand, sizeof(rec_name_cand), "%.*s_recombined.wav", (int)stem_len, name);

            char rec_full_p[4096];
            snprintf(rec_full_p, sizeof(rec_full_p), "%s" PATH_SEP_STR "%s", target_dir, rec_name_cand);

            FILE* rf = fopen(rec_full_p, "rb");
            if (rf) {
                fclose(rf);
                strncpy(raw_items[item_count].recombined_filename, rec_name_cand, sizeof(raw_items[item_count].recombined_filename) - 1);
                strncpy(raw_items[item_count].recombined_full_path, rec_full_p, sizeof(raw_items[item_count].recombined_full_path) - 1);
            }

            item_count++;
        }
        drwav_uninit(&wav_info);
    }
    closedir(dir);

    if (item_count == 0) {
        printf("No pattern WAV files found in directory: %s\n", target_dir);
        printf("\nPress Enter to exit...");
        getchar();
        return 0;
    }

    printf("Found %d pattern WAV file(s) in %s:\n", item_count, target_dir);
    for (int i = 0; i < item_count; i++) {
        uint64_t flux_frames = 0;
        uint32_t flux_sr = 0;
        const char* flux_path = raw_items[i].recombined_full_path[0] != '\0' ? raw_items[i].recombined_full_path : raw_items[i].full_path;
        raw_items[i].rec_flux_data = read_mono_wav_pcm(flux_path, &flux_frames, &flux_sr);
        raw_items[i].rec_flux_frames = flux_frames;
        raw_items[i].rec_flux_sr = flux_sr;

        printf("  [%d/%d] %s (Duration: %.2f ms, Companion Flux: %s)\n",
               i + 1, item_count, raw_items[i].filename, raw_items[i].duration_ms,
               raw_items[i].recombined_filename[0] != '\0' ? raw_items[i].recombined_filename : "Primary Audio Fallback");
    }

    // Sort patterns in descending order by duration
    qsort(raw_items, item_count, sizeof(SongPatternItem), compare_patterns_desc);

    int* active_mask = (int*)calloc(item_count, sizeof(int));
    if (!active_mask) {
        printf("Error: Memory allocation failed for active mask.\n");
        for (int i = 0; i < item_count; i++) {
            if (raw_items[i].rec_flux_data) free(raw_items[i].rec_flux_data);
        }
        return 1;
    }

    active_mask[0] = 1;
    double best_corr = compute_total_correlation(item_count, raw_items, active_mask);

    for (int k = 1; k < item_count; k++) {
        active_mask[k] = 1;
        double test_corr = compute_total_correlation(item_count, raw_items, active_mask);
        if (test_corr > best_corr) {
            best_corr = test_corr;
        } else {
            active_mask[k] = 0;
        }
    }

    int selected_cnt = 0;
    for (int i = 0; i < item_count; i++) {
        if (active_mask[i]) selected_cnt++;
    }

    char song_out_dir[4096];
    snprintf(song_out_dir, sizeof(song_out_dir), "%s" PATH_SEP_STR "[song]", target_dir);
    mkdir_p(song_out_dir);

    printf("\n============================================================\n");
    printf("SONG BUILDING COMPLETE!\n");
    printf("Selected %d of %d pattern(s) into [song] subfolder.\n", selected_cnt, item_count);
    printf("Total Correlation: %.2f%%\n", best_corr * 100.0);
    printf("Output Subfolder: %s" PATH_SEP_STR "\n", song_out_dir);
    printf("============================================================\n\n");

    for (int i = 0; i < item_count; i++) {
        if (active_mask[i]) {
            char dst_pat[4096];
            snprintf(dst_pat, sizeof(dst_pat), "%s" PATH_SEP_STR "%s", song_out_dir, raw_items[i].filename);
            copy_file(raw_items[i].full_path, dst_pat);

            if (raw_items[i].recombined_full_path[0] != '\0') {
                char dst_rec[4096];
                snprintf(dst_rec, sizeof(dst_rec), "%s" PATH_SEP_STR "%s", song_out_dir, raw_items[i].recombined_filename);
                copy_file(raw_items[i].recombined_full_path, dst_rec);
            }
            printf("  -> Exported: %s %s\n", raw_items[i].filename, raw_items[i].recombined_filename[0] != '\0' ? raw_items[i].recombined_filename : "");
        }
        if (raw_items[i].rec_flux_data) free(raw_items[i].rec_flux_data);
    }

    free(active_mask);

    printf("\nPress Enter to exit...");
    getchar();
    return 0;
}
