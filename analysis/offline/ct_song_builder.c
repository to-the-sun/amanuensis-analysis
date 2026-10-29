#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <direct.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#define mkdir_win(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#define mkdir_win(path) mkdir(path, 0755)
#endif

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

typedef struct {
    char stem_filename[512];
    char recombined_filename[512];
    char stem_path[4096];
    char recombined_path[4096];

    float* recombined_samples;
    uint64_t total_frames;
    unsigned int sample_rate;

    uint64_t true_start;
    uint64_t true_end;
    uint64_t active_length;
    int original_index;
} PatternInfo;

static int mkdir_p(const char* path) {
    if (!path || !*path) return -1;
    char temp[4096];
    snprintf(temp, sizeof(temp), "%s", path);
    size_t len = strlen(temp);
    if (len == 0) return -1;
    if (temp[len - 1] == PATH_SEP) temp[len - 1] = '\0';

    for (char* p = temp + 1; *p; p++) {
        if (*p == PATH_SEP) {
            *p = '\0';
            mkdir_win(temp);
            *p = PATH_SEP;
        }
    }
    return mkdir_win(temp);
}

static bool file_exists(const char* filepath) {
    if (!filepath) return false;
    FILE* f = fopen(filepath, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

static bool copy_file(const char* src_path, const char* dst_path) {
    FILE* src = fopen(src_path, "rb");
    if (!src) return false;

    FILE* dst = fopen(dst_path, "wb");
    if (!dst) {
        fclose(src);
        return false;
    }

    char buffer[16384];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }

    fclose(src);
    fclose(dst);
    return true;
}

static double compute_correlation(const PatternInfo* patterns, const int* candidate_indices, int count) {
    if (count <= 0) return 0.0;

    double total_corr_sum = 0.0;
    double total_cap_sum = 0.0;

    for (int i = 0; i < count; i++) {
        int idx_i = candidate_indices[i];
        const PatternInfo* pat_i = &patterns[idx_i];

        if (!pat_i->recombined_samples || pat_i->total_frames == 0) continue;

        for (uint64_t t = 0; t < pat_i->total_frames; t++) {
            double cap_i = fabsf(pat_i->recombined_samples[t]);
            if (cap_i <= 1e-6) continue;

            double sum_other = 0.0;
            bool has_overlap = false;

            for (int j = 0; j < count; j++) {
                if (i == j) continue;
                int idx_j = candidate_indices[j];
                const PatternInfo* pat_j = &patterns[idx_j];

                if (t < pat_j->total_frames && pat_j->recombined_samples) {
                    double cap_j = fabsf(pat_j->recombined_samples[t]);
                    if (cap_j > 1e-6) {
                        sum_other += cap_j;
                        has_overlap = true;
                    }
                }
            }

            if (has_overlap) {
                double corr_sample = sum_other / cap_i;
                if (corr_sample > 1.0) corr_sample = 1.0;

                total_corr_sum += corr_sample;
                total_cap_sum += cap_i;
            }
        }
    }

    if (total_cap_sum <= 1e-6) return 0.0;
    return (total_corr_sum / total_cap_sum) * 100.0;
}

static int compare_patterns_by_length_desc(const void* a, const void* b) {
    const PatternInfo* pa = (const PatternInfo*)a;
    const PatternInfo* pb = (const PatternInfo*)b;

    if (pa->active_length > pb->active_length) return -1;
    if (pa->active_length < pb->active_length) return 1;
    return strcmp(pa->stem_filename, pb->stem_filename);
}

static int collect_patterns(const char* dir_path, PatternInfo** out_patterns) {
    int capacity = 32;
    int count = 0;
    PatternInfo* list = (PatternInfo*)malloc(capacity * sizeof(PatternInfo));

#if defined(_WIN32) || defined(_WIN64)
    char search_path[4096];
    snprintf(search_path, sizeof(search_path), "%.3500s" PATH_SEP_STR "*_recombined.wav", dir_path);
    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_path, &find_data);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                const char* rec_name = find_data.cFileName;
                size_t len = strlen(rec_name);
                const char* suffix = "_recombined.wav";
                size_t suffix_len = strlen(suffix);

                if (len > suffix_len && strcmp(rec_name + len - suffix_len, suffix) == 0) {
                    char stem_name[512];
                    size_t base_len = len - suffix_len;
                    snprintf(stem_name, sizeof(stem_name), "%.*s.wav", (int)base_len, rec_name);

                    char stem_full[4096];
                    char rec_full[4096];
                    snprintf(stem_full, sizeof(stem_full), "%.3500s" PATH_SEP_STR "%.500s", dir_path, stem_name);
                    snprintf(rec_full, sizeof(rec_full), "%.3500s" PATH_SEP_STR "%.500s", dir_path, rec_name);

                    if (file_exists(stem_full)) {
                        if (count >= capacity) {
                            capacity *= 2;
                            list = (PatternInfo*)realloc(list, capacity * sizeof(PatternInfo));
                        }
                        memset(&list[count], 0, sizeof(PatternInfo));
                        snprintf(list[count].stem_filename, sizeof(list[count].stem_filename), "%.500s", stem_name);
                        snprintf(list[count].recombined_filename, sizeof(list[count].recombined_filename), "%.500s", rec_name);
                        snprintf(list[count].stem_path, sizeof(list[count].stem_path), "%.4000s", stem_full);
                        snprintf(list[count].recombined_path, sizeof(list[count].recombined_path), "%.4000s", rec_full);
                        list[count].original_index = count;
                        count++;
                    }
                }
            }
        } while (FindNextFileA(hFind, &find_data));
        FindClose(hFind);
    }
#else
    DIR* dir = opendir(dir_path);
    if (dir) {
        struct dirent* entry;
        const char* suffix = "_recombined.wav";
        size_t suffix_len = strlen(suffix);

        while ((entry = readdir(dir)) != NULL) {
            const char* rec_name = entry->d_name;
            size_t len = strlen(rec_name);

            if (len > suffix_len && strcmp(rec_name + len - suffix_len, suffix) == 0) {
                char stem_name[512];
                size_t base_len = len - suffix_len;
                snprintf(stem_name, sizeof(stem_name), "%.*s.wav", (int)base_len, rec_name);

                char stem_full[4096];
                char rec_full[4096];
                snprintf(stem_full, sizeof(stem_full), "%.3500s" PATH_SEP_STR "%.500s", dir_path, stem_name);
                snprintf(rec_full, sizeof(rec_full), "%.3500s" PATH_SEP_STR "%.500s", dir_path, rec_name);

                if (file_exists(stem_full)) {
                    if (count >= capacity) {
                        capacity *= 2;
                        list = (PatternInfo*)realloc(list, capacity * sizeof(PatternInfo));
                    }
                    memset(&list[count], 0, sizeof(PatternInfo));
                    snprintf(list[count].stem_filename, sizeof(list[count].stem_filename), "%.500s", stem_name);
                    snprintf(list[count].recombined_filename, sizeof(list[count].recombined_filename), "%.500s", rec_name);
                    snprintf(list[count].stem_path, sizeof(list[count].stem_path), "%.4000s", stem_full);
                    snprintf(list[count].recombined_path, sizeof(list[count].recombined_path), "%.4000s", rec_full);
                    list[count].original_index = count;
                    count++;
                }
            }
        }
        closedir(dir);
    }
#endif

    *out_patterns = list;
    return count;
}

int main(int argc, char** argv) {
    const char* work_dir = ".";
    if (argc > 1 && argv[1][0] != '\0') {
        work_dir = argv[1];
    }

    printf("=====================================================\n");
    printf("            CT Song Builder Algorithm               \n");
    printf("=====================================================\n");
    printf("Working directory: %s\n\n", work_dir);

    PatternInfo* patterns = NULL;
    int num_patterns = collect_patterns(work_dir, &patterns);

    if (num_patterns <= 0) {
        printf("No pattern WAV files matching '*_recombined.wav' found in '%s'.\n", work_dir);
        printf("\nPress Enter to exit...\n");
        getchar();
        return 0;
    }

    printf("Found %d pattern pair(s) in '%s'. Loading audio data...\n\n", num_patterns, work_dir);

    for (int i = 0; i < num_patterns; i++) {
        PatternInfo* pat = &patterns[i];

        drwav_uint64 total_pcm_frames = 0;
        unsigned int channels = 0;
        unsigned int sample_rate = 0;

        float* sample_data = drwav_open_file_and_read_pcm_frames_f32(
            pat->recombined_path,
            &channels,
            &sample_rate,
            &total_pcm_frames,
            NULL
        );

        if (!sample_data || total_pcm_frames == 0) {
            printf("[Warning] Could not load recombined audio: %s\n", pat->recombined_filename);
            pat->recombined_samples = NULL;
            pat->total_frames = 0;
            pat->sample_rate = 0;
            pat->true_start = 0;
            pat->true_end = 0;
            pat->active_length = 0;
            continue;
        }

        if (channels > 1) {
            float* mono_data = (float*)malloc(total_pcm_frames * sizeof(float));
            for (drwav_uint64 f = 0; f < total_pcm_frames; f++) {
                float sum = 0.0f;
                for (unsigned int c = 0; c < channels; c++) {
                    sum += sample_data[f * channels + c];
                }
                mono_data[f] = sum / (float)channels;
            }
            drwav_free(sample_data, NULL);
            sample_data = mono_data;
        }

        pat->recombined_samples = sample_data;
        pat->total_frames = total_pcm_frames;
        pat->sample_rate = sample_rate;

        bool found_start = false;
        uint64_t t_start = 0;
        uint64_t t_end = 0;

        for (uint64_t s = 0; s < total_pcm_frames; s++) {
            if (fabsf(sample_data[s]) > 1e-6f) {
                if (!found_start) {
                    t_start = s;
                    found_start = true;
                }
                t_end = s;
            }
        }

        if (found_start) {
            pat->true_start = t_start;
            pat->true_end = t_end;
            pat->active_length = t_end - t_start + 1;
        } else {
            pat->true_start = 0;
            pat->true_end = 0;
            pat->active_length = 0;
        }

        printf("  [%d] %s\n", i + 1, pat->stem_filename);
        printf("      Recombined WAV: %s\n", pat->recombined_filename);
        printf("      True Start: %llu smp, True End: %llu smp, Active Length: %llu smp (%.3f s)\n",
               (unsigned long long)pat->true_start,
               (unsigned long long)pat->true_end,
               (unsigned long long)pat->active_length,
               pat->sample_rate > 0 ? (double)pat->active_length / pat->sample_rate : 0.0);
    }

    printf("\nSorting patterns descending by active length...\n");
    qsort(patterns, num_patterns, sizeof(PatternInfo), compare_patterns_by_length_desc);

    printf("\nSorted Order:\n");
    for (int i = 0; i < num_patterns; i++) {
        printf("  Rank %d: %s (Active Length: %llu smp)\n",
               i + 1, patterns[i].stem_filename, (unsigned long long)patterns[i].active_length);
    }

    printf("\nEvaluating greedy pattern selection based on song correlation metric...\n\n");

    int* selected_indices = (int*)malloc((size_t)num_patterns * sizeof(int));
    int num_selected = 0;
    double current_song_correlation = 0.0;

    int* test_set = (int*)malloc((size_t)num_patterns * sizeof(int));

    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].active_length == 0) {
            printf("[Skip] %s has 0 active length.\n", patterns[i].stem_filename);
            continue;
        }

        for (int k = 0; k < num_selected; k++) {
            test_set[k] = selected_indices[k];
        }
        test_set[num_selected] = i;

        double proposed_corr = compute_correlation(patterns, test_set, num_selected + 1);

        if (num_selected == 0) {
            selected_indices[num_selected++] = i;
            current_song_correlation = proposed_corr;
            printf("[ACCEPT] %s (Rank %d) added as seed pattern. Song Correlation: %.2f%%\n",
                   patterns[i].stem_filename, i + 1, current_song_correlation);
        } else if (proposed_corr > current_song_correlation) {
            selected_indices[num_selected++] = i;
            printf("[ACCEPT] %s (Rank %d) added. Song Correlation increased: %.2f%% -> %.2f%%\n",
                   patterns[i].stem_filename, i + 1, current_song_correlation, proposed_corr);
            current_song_correlation = proposed_corr;
        } else {
            printf("[REJECT] %s (Rank %d) proposed correlation %.2f%% <= current %.2f%%\n",
                   patterns[i].stem_filename, i + 1, proposed_corr, current_song_correlation);
        }
    }

    char song_dir[4096];
    snprintf(song_dir, sizeof(song_dir), "%.3500s" PATH_SEP_STR "[song]", work_dir);
    mkdir_p(song_dir);

    printf("\nWriting selected song patterns to '%s'...\n", song_dir);

    int copy_success_count = 0;
    for (int s = 0; s < num_selected; s++) {
        int idx = selected_indices[s];
        const PatternInfo* pat = &patterns[idx];

        char dst_stem[4096];
        snprintf(dst_stem, sizeof(dst_stem), "%.3500s" PATH_SEP_STR "%.500s", song_dir, pat->stem_filename);

        bool ok_stem = copy_file(pat->stem_path, dst_stem);

        if (ok_stem) {
            printf("  Copied: %s\n", pat->stem_filename);
            copy_success_count++;
        } else {
            printf("  [Error] Failed copying %s into [song]\n", pat->stem_filename);
        }
    }

    printf("\n=====================================================\n");
    printf("                  Song Summary                       \n");
    printf("=====================================================\n");
    printf("Total Candidate Patterns Evaluated : %d\n", num_patterns);
    printf("Patterns Selected into Song        : %d (Successfully copied: %d)\n", num_selected, copy_success_count);
    printf("Final Song Correlation Metric     : %.2f%%\n", current_song_correlation);
    printf("Output Directory                  : %s\n", song_dir);
    printf("=====================================================\n");

    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].recombined_samples) {
            drwav_free(patterns[i].recombined_samples, NULL);
        }
    }
    free(patterns);
    free(selected_indices);
    free(test_set);

    printf("\nPress Enter to exit...\n");
    getchar();
    return 0;
}
