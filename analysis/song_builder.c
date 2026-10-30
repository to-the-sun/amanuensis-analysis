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

    float* stem_samples;
    uint64_t stem_total_frames;
    unsigned int stem_sample_rate;

    uint64_t true_start;
    uint64_t true_end;
    uint64_t active_length;
    int original_index;
    bool selected;
    int selection_order;

    double judgment_stem_corr_sum;
    double judgment_stem_cap_sum;
    double judgment_stem_corr_pct;

    double judgment_overall_corr_sum;
    double judgment_overall_cap_sum;
    double judgment_overall_corr_pct;
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

static char* encode_base64_bytes(const uint8_t* data, size_t input_len) {
    static const char encoding_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_len = 4 * ((input_len + 2) / 3);
    char *encoded_data = (char*)malloc(output_len + 1);
    if (!encoded_data) return NULL;

    size_t i = 0, j = 0;
    while (i < input_len) {
        uint32_t octet_a = i < input_len ? data[i++] : 0;
        uint32_t octet_b = i < input_len ? data[i++] : 0;
        uint32_t octet_c = i < input_len ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = encoding_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = encoding_table[triple & 0x3F];
    }

    static const int mod_table[] = {0, 2, 1};
    for (int k = 0; k < mod_table[input_len % 3]; k++) {
        encoded_data[output_len - 1 - k] = '=';
    }
    encoded_data[output_len] = '\0';
    return encoded_data;
}

static double compute_correlation_ext(
    const PatternInfo* patterns,
    const int* candidate_indices,
    int count,
    double* out_total_corr,
    double* out_total_cap
) {
    if (count <= 0) {
        if (out_total_corr) *out_total_corr = 0.0;
        if (out_total_cap) *out_total_cap = 0.0;
        return 0.0;
    }

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
                double corr_mag = (sum_other < cap_i) ? sum_other : cap_i;

                total_corr_sum += corr_mag;
                total_cap_sum += cap_i;
            }
        }
    }

    if (out_total_corr) *out_total_corr = total_corr_sum;
    if (out_total_cap) *out_total_cap = total_cap_sum;

    if (total_cap_sum <= 1e-6) return 0.0;
    return (total_corr_sum / total_cap_sum) * 100.0;
}

static void compute_judgment_time_correlation_samples(
    PatternInfo* pat_i,
    const PatternInfo* patterns,
    const int* selected_indices,
    int num_selected
) {
    uint64_t total_frames = pat_i->total_frames;
    if (!pat_i->recombined_samples || total_frames == 0) {
        pat_i->judgment_stem_corr_sum = 0.0;
        pat_i->judgment_stem_cap_sum = 0.0;
        pat_i->judgment_stem_corr_pct = 0.0;
        return;
    }

    double stem_corr_sum = 0.0;
    double stem_cap_sum = 0.0;

    for (uint64_t s = 0; s < total_frames; s++) {
        double cap_i = fabsf(pat_i->recombined_samples[s]);
        double sum_other = 0.0;
        bool has_overlap = false;

        for (int j = 0; j < num_selected; j++) {
            int idx_j = selected_indices[j];
            const PatternInfo* pat_j = &patterns[idx_j];
            if (s < pat_j->total_frames && pat_j->recombined_samples) {
                double cap_j = fabsf(pat_j->recombined_samples[s]);
                if (cap_j > 1e-6) {
                    sum_other += cap_j;
                    has_overlap = true;
                }
            }
        }

        if (cap_i > 1e-6 && has_overlap) {
            double corr_mag = (sum_other < cap_i) ? sum_other : cap_i;
            stem_corr_sum += corr_mag;
            stem_cap_sum += cap_i;
        }
    }

    pat_i->judgment_stem_corr_sum = stem_corr_sum;
    pat_i->judgment_stem_cap_sum = stem_cap_sum;
    pat_i->judgment_stem_corr_pct = (stem_cap_sum > 1e-6) ? (stem_corr_sum / stem_cap_sum) * 100.0 : 0.0;
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

static void generate_correlation_html_report(
    const char* work_dir,
    const PatternInfo* patterns,
    int num_patterns,
    const int* selected_indices,
    int num_selected,
    double final_correlation
) {
    int* display_order = (int*)malloc((size_t)num_patterns * sizeof(int));
    if (!display_order) return;

    int order_count = 0;
    for (int s = 0; s < num_selected; s++) {
        display_order[order_count++] = selected_indices[s];
    }
    for (int i = 0; i < num_patterns; i++) {
        if (!patterns[i].selected) {
            display_order[order_count++] = i;
        }
    }

    char js_path[4096];
    snprintf(js_path, sizeof(js_path), "%.3500s" PATH_SEP_STR "song_correlation_waveform_data.js", work_dir);

    double max_song_duration = 0.0;
    uint32_t sample_rate = 44100;
    uint64_t max_total_samples = 0;

    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].recombined_samples && patterns[i].sample_rate > 0) {
            double d = (double)patterns[i].total_frames / patterns[i].sample_rate;
            if (d > max_song_duration) max_song_duration = d;
            if (patterns[i].sample_rate > 0) sample_rate = patterns[i].sample_rate;
            if (patterns[i].total_frames > max_total_samples) max_total_samples = patterns[i].total_frames;
        }
    }

    FILE* fjs = fopen(js_path, "w");
    if (fjs) {
        fprintf(fjs, "window.WAVEFORM_DATA = {\n");
        fprintf(fjs, "  stemWaveforms: [\n");
        for (int d = 0; d < num_patterns; d++) {
            int i = display_order[d];
            const PatternInfo* pat = &patterns[i];
            uint64_t total_frames = pat->total_frames;
            int16_t* pcm_stem = (int16_t*)calloc(total_frames, sizeof(int16_t));
            if (pat->stem_samples && pcm_stem) {
                uint64_t copy_len = pat->stem_total_frames < total_frames ? pat->stem_total_frames : total_frames;
                for (uint64_t s = 0; s < copy_len; s++) {
                    float val = pat->stem_samples[s];
                    if (val > 1.0f) val = 1.0f;
                    if (val < -1.0f) val = -1.0f;
                    pcm_stem[s] = (int16_t)round(val * 32767.0f);
                }
            }
            char* b64 = encode_base64_bytes((const uint8_t*)pcm_stem, total_frames * sizeof(int16_t));
            fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (d + 1 < num_patterns) ? "," : "");
            if (b64) free(b64);
            if (pcm_stem) free(pcm_stem);
        }
        fprintf(fjs, "  ],\n");

        fprintf(fjs, "  capacities: [\n");
        for (int d = 0; d < num_patterns; d++) {
            int i = display_order[d];
            const PatternInfo* pat = &patterns[i];
            uint64_t total_frames = pat->total_frames;
            int16_t* pcm_cap = (int16_t*)calloc(total_frames, sizeof(int16_t));
            if (pat->recombined_samples && pcm_cap) {
                for (uint64_t s = 0; s < total_frames; s++) {
                    float val = fabsf(pat->recombined_samples[s]);
                    if (val > 1.0f) val = 1.0f;
                    pcm_cap[s] = (int16_t)round(val * 32767.0f);
                }
            }
            char* b64 = encode_base64_bytes((const uint8_t*)pcm_cap, total_frames * sizeof(int16_t));
            fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (d + 1 < num_patterns) ? "," : "");
            if (b64) free(b64);
            if (pcm_cap) free(pcm_cap);
        }
        fprintf(fjs, "  ]\n");
        fprintf(fjs, "};\n");
        fclose(fjs);
    }

    char html_path[4096];
    snprintf(html_path, sizeof(html_path), "%.3500s" PATH_SEP_STR "song_correlation_report.html", work_dir);

    FILE* f = fopen(html_path, "w");
    if (!f) {
        printf("[Warning] Could not create HTML report file: %s\n", html_path);
        return;
    }

    fprintf(f, "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    fprintf(f, "<meta charset=\"UTF-8\">\n");
    fprintf(f, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    fprintf(f, "<title>CT Song Correlation Analysis Report</title>\n");
    fprintf(f, "<style>\n");
    fprintf(f, "  :root { --bg: #121216; --card-bg: #1a1a22; --border: #2a2a36; --text: #e1e1e6; --muted: #a0a0b0; --accent: #2196f3; }\n");
    fprintf(f, "  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; }\n");
    fprintf(f, "  body { background: var(--bg); color: var(--text); padding: 20px; line-height: 1.5; }\n");
    fprintf(f, "  header { background: var(--card-bg); padding: 20px; border-radius: 10px; border: 1px solid var(--border); margin-bottom: 20px; }\n");
    fprintf(f, "  h1 { font-size: 1.6rem; color: #fff; margin-bottom: 8px; }\n");
    fprintf(f, "  .summary-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin-top: 15px; }\n");
    fprintf(f, "  .stat-card { background: rgba(0,0,0,0.2); padding: 12px 16px; border-radius: 6px; border: 1px solid var(--border); }\n");
    fprintf(f, "  .stat-label { font-size: 0.8rem; color: var(--muted); text-transform: uppercase; letter-spacing: 0.5px; }\n");
    fprintf(f, "  .stat-value { font-size: 1.4rem; font-weight: bold; color: #fff; margin-top: 4px; }\n");
    fprintf(f, "  .toolbar { display: flex; align-items: center; gap: 15px; flex-wrap: wrap; margin-top: 15px; padding-top: 12px; border-top: 1px solid var(--border); }\n");
    fprintf(f, "  .btn { background: var(--accent); color: #fff; border: none; padding: 8px 16px; border-radius: 6px; font-weight: 600; cursor: pointer; transition: opacity 0.2s; text-decoration: none; display: inline-block; }\n");
    fprintf(f, "  .btn:hover { opacity: 0.9; }\n");
    fprintf(f, "  .view-info { font-size: 0.85rem; color: #4caf50; font-family: monospace; font-weight: bold; }\n");
    fprintf(f, "  .legend { display: flex; gap: 20px; flex-wrap: wrap; margin-top: 15px; font-size: 0.85rem; padding-top: 12px; border-top: 1px solid var(--border); }\n");
    fprintf(f, "  .legend-item { display: flex; align-items: center; gap: 8px; }\n");
    fprintf(f, "  .legend-color { width: 14px; height: 14px; border-radius: 3px; display: inline-block; }\n");
    fprintf(f, "  .stem-card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin-bottom: 20px; }\n");
    fprintf(f, "  .stem-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; margin-bottom: 12px; }\n");
    fprintf(f, "  .stem-title { font-size: 1.1rem; font-weight: bold; color: #fff; display: flex; align-items: center; gap: 10px; }\n");
    fprintf(f, "  .badge { font-size: 0.75rem; padding: 3px 8px; border-radius: 4px; font-weight: 600; text-transform: uppercase; }\n");
    fprintf(f, "  .badge-selected { background: #1b4d2e; color: #4caf50; border: 1px solid #2e7d32; }\n");
    fprintf(f, "  .badge-rejected { background: #4a151b; color: #f44336; border: 1px solid #c62828; }\n");
    fprintf(f, "  .stem-metrics { font-size: 0.85rem; color: var(--muted); font-family: monospace; display: flex; flex-direction: column; gap: 4px; text-align: right; }\n");
    fprintf(f, "  audio { height: 32px; width: 300px; outline: none; }\n");
    fprintf(f, "  .canvas-container { position: relative; width: 100%%; height: 180px; background: #0d0d11; border-radius: 6px; border: 1px solid #22222e; overflow: hidden; margin-top: 10px; user-select: none; }\n");
    fprintf(f, "  canvas { width: 100%%; height: 100%%; display: block; cursor: crosshair; }\n");
    fprintf(f, "  .tooltip { position: absolute; top: 8px; right: 12px; background: rgba(10, 10, 15, 0.9); border: 1px solid #333344; padding: 6px 10px; border-radius: 4px; font-size: 0.75rem; font-family: monospace; color: #fff; pointer-events: none; opacity: 0; transition: opacity 0.15s; z-index: 10; }\n");
    fprintf(f, "  .instructions { font-size: 0.8rem; color: var(--muted); margin-top: 10px; font-style: italic; }\n");
    fprintf(f, "</style>\n</head>\n<body>\n");

    fprintf(f, "<header>\n");
    fprintf(f, "  <h1>CT Song Correlation Analysis Report</h1>\n");
    fprintf(f, "  <div class=\"summary-grid\">\n");
    fprintf(f, "    <div class=\"stat-card\">\n");
    fprintf(f, "      <div class=\"stat-label\">Candidate Patterns</div>\n");
    fprintf(f, "      <div class=\"stat-value\">%d</div>\n", num_patterns);
    fprintf(f, "    </div>\n");
    fprintf(f, "    <div class=\"stat-card\">\n");
    fprintf(f, "      <div class=\"stat-label\">Selected Song Stems</div>\n");
    fprintf(f, "      <div class=\"stat-value\">%d</div>\n", num_selected);
    fprintf(f, "    </div>\n");
    fprintf(f, "    <div class=\"stat-card\">\n");
    fprintf(f, "      <div class=\"stat-label\">Final Song Correlation</div>\n");
    fprintf(f, "      <div class=\"stat-value\" style=\"color: #2196f3;\">%.2f%%</div>\n", final_correlation);
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"toolbar\">\n");
    fprintf(f, "    <button class=\"btn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", max_song_duration, max_song_duration);
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"legend\">\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: rgba(120,120,160,0.35); border: 1px solid rgba(150,150,180,0.5);\"></span> Output Audio Waveform (Background)</div>\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: #ff9800;\"></span> Stem Correlation Capacity (|S_i(t)|)</div>\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: #00e676;\"></span> Judgment-Time Overlapping Correlation (Min(|S_i(t)|, &sum;<sub>j &lt; i</sub> |S_j(t)|))</div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"instructions\">Click and drag horizontally on any graph to zoom in. Right-click on a graph to zoom out one level. Click 'Reset Zoom' to restore full view.</div>\n");
    fprintf(f, "</header>\n");

    fprintf(f, "<main>\n");

    for (int d = 0; d < num_patterns; d++) {
        int i = display_order[d];
        const PatternInfo* pat = &patterns[i];
        if (!pat->recombined_samples || pat->total_frames == 0) continue;

        double duration_s = pat->sample_rate > 0 ? (double)pat->total_frames / pat->sample_rate : 0.0;

        fprintf(f, "  <div class=\"stem-card\">\n");
        fprintf(f, "    <div class=\"stem-header\">\n");
        fprintf(f, "      <div class=\"stem-title\">\n");
        fprintf(f, "        <span>%s</span>\n", pat->stem_filename);
        if (pat->selected) {
            fprintf(f, "        <span class=\"badge badge-selected\">Selected in Song</span>\n");
        } else {
            fprintf(f, "        <span class=\"badge badge-rejected\">Rejected</span>\n");
        }
        fprintf(f, "      </div>\n");
        fprintf(f, "      <div style=\"display: flex; align-items: center; gap: 20px;\">\n");
        fprintf(f, "        <div class=\"stem-metrics\">\n");
        fprintf(f, "          <div><strong>Duration:</strong> %.2fs | <strong>Active:</strong> %.2fs</div>\n",
                duration_s,
                pat->sample_rate > 0 ? (double)pat->active_length / pat->sample_rate : 0.0);
        fprintf(f, "          <div><strong>Stem Corr / Cap at Judgment:</strong> %.2f / %.2f (%.2f%%)</div>\n",
                pat->judgment_stem_corr_sum,
                pat->judgment_stem_cap_sum,
                pat->judgment_stem_corr_pct);
        fprintf(f, "          <div><strong>Overall Song Corr / Cap if Included:</strong> %.2f / %.2f (%.2f%%)</div>\n",
                pat->judgment_overall_corr_sum,
                pat->judgment_overall_cap_sum,
                pat->judgment_overall_corr_pct);
        fprintf(f, "        </div>\n");
        fprintf(f, "        <audio controls src=\"%s\" id=\"audio-%d\"></audio>\n", pat->stem_filename, d);
        fprintf(f, "      </div>\n");
        fprintf(f, "    </div>\n");

        fprintf(f, "    <div class=\"canvas-container\" id=\"container-%d\">\n", d);
        fprintf(f, "      <canvas id=\"canvas-%d\"></canvas>\n", d);
        fprintf(f, "      <div class=\"tooltip\" id=\"tooltip-%d\"></div>\n", d);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
    }

    fprintf(f, "</main>\n");

    fprintf(f, "<script src=\"song_correlation_waveform_data.js\"></script>\n");
    fprintf(f, "<script>\n");
    fprintf(f, "const DATA = {\n");
    fprintf(f, "  sampleRate: %u,\n", sample_rate);
    fprintf(f, "  totalSamples: %llu,\n", (unsigned long long)max_total_samples);
    fprintf(f, "  duration: %.6f,\n", max_song_duration);
    fprintf(f, "  stems: [\n");

    for (int d = 0; d < num_patterns; d++) {
        int i = display_order[d];
        const PatternInfo* pat_i = &patterns[i];
        if (!pat_i->recombined_samples || pat_i->total_frames == 0) continue;

        double duration_s = pat_i->sample_rate > 0 ? (double)pat_i->total_frames / pat_i->sample_rate : 0.0;

        fprintf(f, "    {\n");
        fprintf(f, "      index: %d,\n", d);
        fprintf(f, "      filename: \"%s\",\n", pat_i->stem_filename);
        fprintf(f, "      selected: %s,\n", pat_i->selected ? "true" : "false");
        fprintf(f, "      selectionOrder: %d,\n", pat_i->selection_order);
        fprintf(f, "      duration: %.4f,\n", duration_s);
        fprintf(f, "      sampleRate: %u,\n", pat_i->sample_rate);
        fprintf(f, "      totalSamples: %llu\n", (unsigned long long)pat_i->total_frames);
        fprintf(f, "    }%s\n", (d + 1 < num_patterns) ? "," : "");
    }

    fprintf(f, "  ]\n");
    fprintf(f, "};\n\n");

    fprintf(f, "function base64ToFloat32(b64, targetLen) {\n");
    fprintf(f, "  if (!b64) return new Float32Array(targetLen);\n");
    fprintf(f, "  const binaryString = atob(b64);\n");
    fprintf(f, "  const len = binaryString.length;\n");
    fprintf(f, "  const bytes = new Uint8Array(len);\n");
    fprintf(f, "  for (let i = 0; i < len; i++) bytes[i] = binaryString.charCodeAt(i);\n");
    fprintf(f, "  const i16 = new Int16Array(bytes.buffer, 0, Math.floor(len / 2));\n");
    fprintf(f, "  const f32 = new Float32Array(i16.length);\n");
    fprintf(f, "  for (let i = 0; i < i16.length; i++) f32[i] = i16[i] / 32767.0;\n");
    fprintf(f, "  return f32;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSSPrecise(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sStr = s < 10 ? '0' + s.toFixed(3) : s.toFixed(3);\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function formatMSS(sec) {\n");
    fprintf(f, "  if (isNaN(sec) || sec < 0) sec = 0;\n");
    fprintf(f, "  const m = Math.floor(sec / 60);\n");
    fprintf(f, "  const s = sec %% 60;\n");
    fprintf(f, "  const sInt = Math.floor(s);\n");
    fprintf(f, "  const sStr = sInt < 10 ? '0' + sInt : '' + sInt;\n");
    fprintf(f, "  return `${m}:${sStr}`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function initAudioData() {\n");
    fprintf(f, "  if (window.WAVEFORM_DATA) {\n");
    fprintf(f, "    for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "      const st = DATA.stems[i];\n");
    fprintf(f, "      const n = st.totalSamples;\n");
    fprintf(f, "      if (window.WAVEFORM_DATA.stemWaveforms && window.WAVEFORM_DATA.stemWaveforms[i]) {\n");
    fprintf(f, "        st.stemWaveform = base64ToFloat32(window.WAVEFORM_DATA.stemWaveforms[i], n);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      if (window.WAVEFORM_DATA.capacities && window.WAVEFORM_DATA.capacities[i]) {\n");
    fprintf(f, "        st.capacity = base64ToFloat32(window.WAVEFORM_DATA.capacities[i], n);\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "    // Compute judgment-time overlapping correlation against earlier selected stems\n");
    fprintf(f, "    for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "      const st = DATA.stems[i];\n");
    fprintf(f, "      if (!st.capacity) continue;\n");
    fprintf(f, "      const priorSelected = DATA.stems.filter(p => p.selected && p.selectionOrder < st.selectionOrder && p.capacity);\n");
    fprintf(f, "      const corrBuf = new Float32Array(st.capacity.length);\n");
    fprintf(f, "      for (let s = 0; s < st.capacity.length; s++) {\n");
    fprintf(f, "        const cap_i = st.capacity[s];\n");
    fprintf(f, "        let sum_other = 0.0;\n");
    fprintf(f, "        for (const prior of priorSelected) {\n");
    fprintf(f, "          if (s < prior.capacity.length) sum_other += prior.capacity[s];\n");
    fprintf(f, "        }\n");
    fprintf(f, "        corrBuf[s] = Math.min(cap_i, sum_other);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      st.actualCorrelation = corrBuf;\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");

    fprintf(f, "initAudioData();\n\n");

    fprintf(f, "let viewStart = 0.0;\n");
    fprintf(f, "let viewEnd = DATA.duration;\n");
    fprintf(f, "let zoomHistory = [];\n");
    fprintf(f, "let activeDrag = null;\n");
    fprintf(f, "let hoverState = null;\n\n");

    fprintf(f, "function timeToX(t, width) {\n");
    fprintf(f, "  return ((t - viewStart) / (viewEnd - viewStart)) * width;\n");
    fprintf(f, "}\n");
    fprintf(f, "function xToTime(x, width) {\n");
    fprintf(f, "  return viewStart + (x / width) * (viewEnd - viewStart);\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function updateViewInfo() {\n");
    fprintf(f, "  const dur = viewEnd - viewStart;\n");
    fprintf(f, "  document.getElementById('viewInfo').innerHTML = `Visible: ${formatMSSPrecise(viewStart)} &ndash; ${formatMSSPrecise(viewEnd)} (Duration: ${formatMSSPrecise(dur)})`;\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function resetZoom() {\n");
    fprintf(f, "  zoomHistory = [];\n");
    fprintf(f, "  viewStart = 0.0;\n");
    fprintf(f, "  viewEnd = DATA.duration;\n");
    fprintf(f, "  updateViewInfo();\n");
    fprintf(f, "  renderAll();\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function zoomOut() {\n");
    fprintf(f, "  if (zoomHistory.length > 0) {\n");
    fprintf(f, "    const prev = zoomHistory.pop();\n");
    fprintf(f, "    viewStart = prev.start;\n");
    fprintf(f, "    viewEnd = prev.end;\n");
    fprintf(f, "    updateViewInfo();\n");
    fprintf(f, "    renderAll();\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function renderCanvas(index) {\n");
    fprintf(f, "  const canvas = document.getElementById(`canvas-${index}`);\n");
    fprintf(f, "  const audio = document.getElementById(`audio-${index}`);\n");
    fprintf(f, "  if (!canvas) return;\n");
    fprintf(f, "  const ctx = canvas.getContext('2d');\n");
    fprintf(f, "  const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "  const dpr = window.devicePixelRatio || 1;\n");
    fprintf(f, "  const width = rect.width;\n");
    fprintf(f, "  const height = rect.height;\n");
    fprintf(f, "  if (width === 0 || height === 0) return;\n");
    fprintf(f, "  if (canvas.width !== Math.floor(width * dpr) || canvas.height !== Math.floor(height * dpr)) {\n");
    fprintf(f, "    canvas.width = Math.floor(width * dpr);\n");
    fprintf(f, "    canvas.height = Math.floor(height * dpr);\n");
    fprintf(f, "  }\n");
    fprintf(f, "  ctx.save();\n");
    fprintf(f, "  ctx.scale(dpr, dpr);\n");
    fprintf(f, "  ctx.clearRect(0, 0, width, height);\n\n");

    fprintf(f, "  const stem = DATA.stems[index];\n");
    fprintf(f, "  if (!stem) { ctx.restore(); return; }\n");
    fprintf(f, "  const sr = stem.sampleRate;\n");
    fprintf(f, "  const totalN = stem.totalSamples;\n");
    fprintf(f, "  const duration = stem.duration;\n");
    fprintf(f, "  const midY = height / 2;\n\n");

    fprintf(f, "  const sampleStart = Math.max(0, Math.floor(viewStart * sr));\n");
    fprintf(f, "  const sampleEnd = Math.min(totalN - 1, Math.ceil(viewEnd * sr));\n");
    fprintf(f, "  const visCount = sampleEnd - sampleStart + 1;\n");
    fprintf(f, "  if (visCount <= 0) { ctx.restore(); return; }\n\n");

    fprintf(f, "  // --- 0. Draw Background Audio Waveform ---\n");
    fprintf(f, "  if (stem.stemWaveform) {\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(120, 120, 160, 0.28)';\n");
    fprintf(f, "    if (visCount <= width * 3) {\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.strokeStyle = 'rgba(140, 140, 180, 0.5)';\n");
    fprintf(f, "      ctx.lineWidth = 1.5;\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / sr;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const v = stem.stemWaveform[s];\n");
    fprintf(f, "        const y = midY - (v * midY * 0.85);\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "      if (visCount < width * 0.6) {\n");
    fprintf(f, "        ctx.fillStyle = '#82aaff';\n");
    fprintf(f, "        for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "          const x = timeToX(s / sr, width);\n");
    fprintf(f, "          const v = stem.stemWaveform[s];\n");
    fprintf(f, "          const y = midY - (v * midY * 0.85);\n");
    fprintf(f, "          ctx.beginPath(); ctx.arc(x, y, 2.5, 0, 2 * Math.PI); ctx.fill();\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * sr));\n");
    fprintf(f, "        const s2 = Math.min(totalN - 1, Math.ceil(t2 * sr));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < totalN) {\n");
    fprintf(f, "          let minV = stem.stemWaveform[s1], maxV = stem.stemWaveform[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            const v = stem.stemWaveform[s];\n");
    fprintf(f, "            if (v < minV) minV = v;\n");
    fprintf(f, "            if (v > maxV) maxV = v;\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y1 = midY - (maxV * midY * 0.85);\n");
    fprintf(f, "          const y2 = midY - (minV * midY * 0.85);\n");
    fprintf(f, "          ctx.fillRect(px, y1, 1.5, Math.max(1, y2 - y1));\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 1. Draw Stem Capacity Envelope (Orange) ---\n");
    fprintf(f, "  if (stem.capacity) {\n");
    fprintf(f, "    ctx.beginPath();\n");
    fprintf(f, "    ctx.strokeStyle = '#ff9800';\n");
    fprintf(f, "    ctx.lineWidth = 2;\n");
    fprintf(f, "    if (visCount <= width * 3) {\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / sr;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const cap = stem.capacity[s];\n");
    fprintf(f, "        const y = height - (cap * height * 0.88) - 4;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "      if (visCount < width * 0.6) {\n");
    fprintf(f, "        ctx.fillStyle = '#ff9800';\n");
    fprintf(f, "        for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "          const x = timeToX(s / sr, width);\n");
    fprintf(f, "          const cap = stem.capacity[s];\n");
    fprintf(f, "          const y = height - (cap * height * 0.88) - 4;\n");
    fprintf(f, "          ctx.beginPath(); ctx.arc(x, y, 2.5, 0, 2 * Math.PI); ctx.fill();\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * sr));\n");
    fprintf(f, "        const s2 = Math.min(totalN - 1, Math.ceil(t2 * sr));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < totalN) {\n");
    fprintf(f, "          let maxC = stem.capacity[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            if (stem.capacity[s] > maxC) maxC = stem.capacity[s];\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y = height - (maxC * height * 0.88) - 4;\n");
    fprintf(f, "          if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 2. Draw Judgment-Time Correlation Envelope (Green) ---\n");
    fprintf(f, "  if (stem.actualCorrelation) {\n");
    fprintf(f, "    ctx.beginPath();\n");
    fprintf(f, "    ctx.strokeStyle = '#00e676';\n");
    fprintf(f, "    ctx.lineWidth = 2;\n");
    fprintf(f, "    if (visCount <= width * 3) {\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / sr;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const corr = stem.actualCorrelation[s];\n");
    fprintf(f, "        const y = height - (corr * height * 0.88) - 4;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "      if (visCount < width * 0.6) {\n");
    fprintf(f, "        ctx.fillStyle = '#00e676';\n");
    fprintf(f, "        for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "          const x = timeToX(s / sr, width);\n");
    fprintf(f, "          const corr = stem.actualCorrelation[s];\n");
    fprintf(f, "          const y = height - (corr * height * 0.88) - 4;\n");
    fprintf(f, "          ctx.beginPath(); ctx.arc(x, y, 2.5, 0, 2 * Math.PI); ctx.fill();\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "    } else {\n");
    fprintf(f, "      const pxWidth = Math.ceil(width);\n");
    fprintf(f, "      for (let px = 0; px < pxWidth; px++) {\n");
    fprintf(f, "        const t1 = xToTime(px, width);\n");
    fprintf(f, "        const t2 = xToTime(px + 1, width);\n");
    fprintf(f, "        const s1 = Math.max(0, Math.floor(t1 * sr));\n");
    fprintf(f, "        const s2 = Math.min(totalN - 1, Math.ceil(t2 * sr));\n");
    fprintf(f, "        if (s1 <= s2 && s1 < totalN) {\n");
    fprintf(f, "          let maxCr = stem.actualCorrelation[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            if (stem.actualCorrelation[s] > maxCr) maxCr = stem.actualCorrelation[s];\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y = height - (maxCr * height * 0.88) - 4;\n");
    fprintf(f, "          if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 3. Audio Playhead ---\n");
    fprintf(f, "  if (audio && duration > 0 && audio.currentTime >= viewStart && audio.currentTime <= viewEnd) {\n");
    fprintf(f, "    const playX = timeToX(audio.currentTime, width);\n");
    fprintf(f, "    ctx.strokeStyle = '#ffffff';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(playX, 0); ctx.lineTo(playX, height); ctx.stroke();\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 4. Hover Crosshair ---\n");
    fprintf(f, "  if (hoverState && hoverState.index === index) {\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';\n");
    fprintf(f, "    ctx.setLineDash([4, 4]);\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(hoverState.mouseX, 0); ctx.lineTo(hoverState.mouseX, height); ctx.stroke();\n");
    fprintf(f, "    ctx.setLineDash([]);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 5. Active Drag Selection Highlight ---\n");
    fprintf(f, "  if (activeDrag && activeDrag.index === index) {\n");
    fprintf(f, "    const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(33, 150, 243, 0.25)';\n");
    fprintf(f, "    ctx.fillRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "    ctx.strokeStyle = '#2196f3';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    ctx.strokeRect(x1, 0, x2 - x1, height);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  ctx.restore();\n");
    fprintf(f, "}\n\n");

    fprintf(f, "function renderAll() {\n");
    fprintf(f, "  for (let i = 0; i < DATA.stems.length; i++) {\n");
    fprintf(f, "    renderCanvas(i);\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n\n");

    fprintf(f, "DATA.stems.forEach((stem, idx) => {\n");
    fprintf(f, "  const canvas = document.getElementById(`canvas-${idx}`);\n");
    fprintf(f, "  const tooltip = document.getElementById(`tooltip-${idx}`);\n");
    fprintf(f, "  const audio = document.getElementById(`audio-${idx}`);\n");
    fprintf(f, "  if (!canvas) return;\n\n");

    fprintf(f, "  canvas.addEventListener('mousedown', (e) => {\n");
    fprintf(f, "    if (e.button === 0) {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x = e.clientX - rect.left;\n");
    fprintf(f, "      activeDrag = { index: idx, startX: x, currentX: x };\n");
    fprintf(f, "      renderCanvas(idx);\n");
    fprintf(f, "    }\n");
    fprintf(f, "  });\n\n");

    fprintf(f, "  canvas.addEventListener('mousemove', (e) => {\n");
    fprintf(f, "    const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "    const x = Math.max(0, Math.min(rect.width, e.clientX - rect.left));\n");
    fprintf(f, "    hoverState = { index: idx, mouseX: x };\n");
    fprintf(f, "    if (activeDrag) activeDrag.currentX = x;\n");
    fprintf(f, "    renderCanvas(idx);\n\n");

    fprintf(f, "    const t = xToTime(x, rect.width);\n");
    fprintf(f, "    const s = Math.min(stem.totalSamples - 1, Math.max(0, Math.round(t * stem.sampleRate)));\n");
    fprintf(f, "    const capVal = (stem.capacity && stem.capacity.length > s) ? stem.capacity[s] : 0;\n");
    fprintf(f, "    const corrVal = (stem.actualCorrelation && stem.actualCorrelation.length > s) ? stem.actualCorrelation[s] : 0;\n");
    fprintf(f, "    const waveVal = (stem.stemWaveform && stem.stemWaveform.length > s) ? stem.stemWaveform[s] : 0;\n");
    fprintf(f, "    tooltip.style.opacity = '1';\n");
    fprintf(f, "    tooltip.innerHTML = `Time: ${formatMSSPrecise(t)} (Sample #${s}) | Cap: ${capVal.toFixed(4)} | Overlap Corr: ${corrVal.toFixed(4)} | Waveform: ${waveVal.toFixed(4)}`;\n");
    fprintf(f, "  });\n\n");

    fprintf(f, "  canvas.addEventListener('mouseleave', () => {\n");
    fprintf(f, "    if (activeDrag && activeDrag.index === idx) activeDrag = null;\n");
    fprintf(f, "    hoverState = null;\n");
    fprintf(f, "    tooltip.style.opacity = '0';\n");
    fprintf(f, "    renderCanvas(idx);\n");
    fprintf(f, "  });\n\n");

    fprintf(f, "  canvas.addEventListener('mouseup', (e) => {\n");
    fprintf(f, "    if (activeDrag && activeDrag.index === idx) {\n");
    fprintf(f, "      const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "      const x1 = Math.min(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "      const x2 = Math.max(activeDrag.startX, activeDrag.currentX);\n");
    fprintf(f, "      activeDrag = null;\n");
    fprintf(f, "      if (x2 - x1 > 4) {\n");
    fprintf(f, "        const t1 = xToTime(x1, rect.width);\n");
    fprintf(f, "        const t2 = xToTime(x2, rect.width);\n");
    fprintf(f, "        zoomHistory.push({ start: viewStart, end: viewEnd });\n");
    fprintf(f, "        viewStart = Math.max(0, t1);\n");
    fprintf(f, "        viewEnd = Math.min(DATA.duration, t2);\n");
    fprintf(f, "        updateViewInfo();\n");
    fprintf(f, "        renderAll();\n");
    fprintf(f, "      } else {\n");
    fprintf(f, "        const clickTime = xToTime(x1, rect.width);\n");
    fprintf(f, "        if (audio) audio.currentTime = clickTime;\n");
    fprintf(f, "        renderCanvas(idx);\n");
    fprintf(f, "      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  });\n\n");

    fprintf(f, "  canvas.addEventListener('contextmenu', (e) => {\n");
    fprintf(f, "    e.preventDefault();\n");
    fprintf(f, "    zoomOut();\n");
    fprintf(f, "  });\n");

    fprintf(f, "  if (audio) {\n");
    fprintf(f, "    audio.addEventListener('timeupdate', renderAll);\n");
    fprintf(f, "    audio.addEventListener('play', renderAll);\n");
    fprintf(f, "    audio.addEventListener('pause', renderAll);\n");
    fprintf(f, "  }\n");
    fprintf(f, "});\n\n");

    fprintf(f, "window.addEventListener('resize', renderAll);\n");
    fprintf(f, "updateViewInfo();\n");
    fprintf(f, "renderAll();\n");
    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");

    free(display_order);
    fclose(f);
    printf("Successfully generated correlation HTML report: %s\n", html_path);
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
        } else {
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
        }

        // Load Stem WAV Audio Data for Waveform Display
        drwav_uint64 stem_pcm_frames = 0;
        unsigned int stem_channels = 0;
        unsigned int stem_sr = 0;

        float* stem_data = drwav_open_file_and_read_pcm_frames_f32(
            pat->stem_path,
            &stem_channels,
            &stem_sr,
            &stem_pcm_frames,
            NULL
        );

        if (stem_data && stem_pcm_frames > 0) {
            if (stem_channels > 1) {
                float* mono_stem = (float*)malloc(stem_pcm_frames * sizeof(float));
                for (drwav_uint64 f = 0; f < stem_pcm_frames; f++) {
                    float sum = 0.0f;
                    for (unsigned int c = 0; c < stem_channels; c++) {
                        sum += stem_data[f * stem_channels + c];
                    }
                    mono_stem[f] = sum / (float)stem_channels;
                }
                drwav_free(stem_data, NULL);
                stem_data = mono_stem;
            }
            pat->stem_samples = stem_data;
            pat->stem_total_frames = stem_pcm_frames;
            pat->stem_sample_rate = stem_sr;
        } else {
            pat->stem_samples = NULL;
            pat->stem_total_frames = 0;
            pat->stem_sample_rate = 0;
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

    // Initialize pattern defaults
    for (int i = 0; i < num_patterns; i++) {
        patterns[i].selected = false;
        patterns[i].selection_order = num_patterns;
        patterns[i].judgment_stem_corr_sum = 0.0;
        patterns[i].judgment_stem_cap_sum = 0.0;
        patterns[i].judgment_stem_corr_pct = 0.0;
        patterns[i].judgment_overall_corr_sum = 0.0;
        patterns[i].judgment_overall_cap_sum = 0.0;
        patterns[i].judgment_overall_corr_pct = 0.0;
        if (patterns[i].active_length == 0) {
            printf("[Skip] %s has 0 active length.\n", patterns[i].stem_filename);
        }
    }

    // Step 1: Compare every pattern pair to find the pair with the greatest correlation
    int best_pair_i = -1;
    int best_pair_j = -1;
    double max_pair_corr = -1.0;
    double max_pair_corr_sum = 0.0;
    double max_pair_cap_sum = 0.0;

    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].active_length == 0) continue;
        for (int j = i + 1; j < num_patterns; j++) {
            if (patterns[j].active_length == 0) continue;

            int pair[2] = { i, j };
            double corr_sum = 0.0;
            double cap_sum = 0.0;
            double pair_corr = compute_correlation_ext(patterns, pair, 2, &corr_sum, &cap_sum);

            if (pair_corr > max_pair_corr) {
                max_pair_corr = pair_corr;
                best_pair_i = i;
                best_pair_j = j;
                max_pair_corr_sum = corr_sum;
                max_pair_cap_sum = cap_sum;
            }
        }
    }

    if (best_pair_i != -1 && best_pair_j != -1 && max_pair_corr > 0.0) {
        // Add best_pair_i first
        compute_judgment_time_correlation_samples(&patterns[best_pair_i], patterns, NULL, 0);
        test_set[0] = best_pair_i;
        double overall_corr_sum1 = 0.0, overall_cap_sum1 = 0.0;
        double proposed_corr1 = compute_correlation_ext(patterns, test_set, 1, &overall_corr_sum1, &overall_cap_sum1);

        patterns[best_pair_i].selected = true;
        patterns[best_pair_i].selection_order = num_selected; // 0
        patterns[best_pair_i].judgment_overall_corr_sum = overall_corr_sum1;
        patterns[best_pair_i].judgment_overall_cap_sum = overall_cap_sum1;
        patterns[best_pair_i].judgment_overall_corr_pct = proposed_corr1;
        selected_indices[num_selected++] = best_pair_i;

        // Add best_pair_j second
        compute_judgment_time_correlation_samples(&patterns[best_pair_j], patterns, selected_indices, 1);
        test_set[0] = best_pair_i;
        test_set[1] = best_pair_j;
        patterns[best_pair_j].selected = true;
        patterns[best_pair_j].selection_order = num_selected; // 1
        patterns[best_pair_j].judgment_overall_corr_sum = max_pair_corr_sum;
        patterns[best_pair_j].judgment_overall_cap_sum = max_pair_cap_sum;
        patterns[best_pair_j].judgment_overall_corr_pct = max_pair_corr;
        selected_indices[num_selected++] = best_pair_j;

        current_song_correlation = max_pair_corr;

        printf("[ACCEPT] %s (Rank %d) & %s (Rank %d) selected as seed pair. Song Correlation: %.2f%%\n",
               patterns[best_pair_i].stem_filename, best_pair_i + 1,
               patterns[best_pair_j].stem_filename, best_pair_j + 1,
               current_song_correlation);
    } else {
        // Fallback for single active pattern or no overlapping pairs
        int seed_idx = -1;
        for (int i = 0; i < num_patterns; i++) {
            if (patterns[i].active_length > 0) {
                seed_idx = i;
                break;
            }
        }

        if (seed_idx != -1) {
            compute_judgment_time_correlation_samples(&patterns[seed_idx], patterns, selected_indices, 0);

            test_set[0] = seed_idx;
            double overall_corr_sum = 0.0;
            double overall_cap_sum = 0.0;
            double proposed_corr = compute_correlation_ext(
                patterns,
                test_set,
                1,
                &overall_corr_sum,
                &overall_cap_sum
            );

            patterns[seed_idx].selected = true;
            patterns[seed_idx].selection_order = num_selected; // 0
            patterns[seed_idx].judgment_overall_corr_sum = overall_corr_sum;
            patterns[seed_idx].judgment_overall_cap_sum = overall_cap_sum;
            patterns[seed_idx].judgment_overall_corr_pct = proposed_corr;

            selected_indices[num_selected++] = seed_idx;
            current_song_correlation = proposed_corr;

            printf("[ACCEPT] %s (Rank %d) added as seed pattern. Song Correlation: %.2f%%\n",
                   patterns[seed_idx].stem_filename, seed_idx + 1, current_song_correlation);
        }
    }

    // Step 2: Iteratively find the unselected candidate pattern that yields highest song correlation increase
    while (num_selected < num_patterns) {
        int best_candidate = -1;
        double best_proposed_corr = current_song_correlation;
        double best_overall_corr_sum = 0.0;
        double best_overall_cap_sum = 0.0;
        double best_stem_corr_sum = 0.0;
        double best_stem_cap_sum = 0.0;
        double best_stem_corr_pct = 0.0;

        for (int i = 0; i < num_patterns; i++) {
            if (patterns[i].selected || patterns[i].active_length == 0) {
                continue;
            }

            compute_judgment_time_correlation_samples(&patterns[i], patterns, selected_indices, num_selected);

            for (int k = 0; k < num_selected; k++) {
                test_set[k] = selected_indices[k];
            }
            test_set[num_selected] = i;

            double overall_corr_sum = 0.0;
            double overall_cap_sum = 0.0;
            double proposed_corr = compute_correlation_ext(
                patterns,
                test_set,
                num_selected + 1,
                &overall_corr_sum,
                &overall_cap_sum
            );

            patterns[i].judgment_overall_corr_sum = overall_corr_sum;
            patterns[i].judgment_overall_cap_sum = overall_cap_sum;
            patterns[i].judgment_overall_corr_pct = proposed_corr;

            if (proposed_corr > best_proposed_corr) {
                best_candidate = i;
                best_proposed_corr = proposed_corr;
                best_overall_corr_sum = overall_corr_sum;
                best_overall_cap_sum = overall_cap_sum;
                best_stem_corr_sum = patterns[i].judgment_stem_corr_sum;
                best_stem_cap_sum = patterns[i].judgment_stem_cap_sum;
                best_stem_corr_pct = patterns[i].judgment_stem_corr_pct;
            }
        }

        if (best_candidate != -1 && best_proposed_corr > current_song_correlation) {
            patterns[best_candidate].selected = true;
            patterns[best_candidate].selection_order = num_selected;
            patterns[best_candidate].judgment_stem_corr_sum = best_stem_corr_sum;
            patterns[best_candidate].judgment_stem_cap_sum = best_stem_cap_sum;
            patterns[best_candidate].judgment_stem_corr_pct = best_stem_corr_pct;
            patterns[best_candidate].judgment_overall_corr_sum = best_overall_corr_sum;
            patterns[best_candidate].judgment_overall_cap_sum = best_overall_cap_sum;
            patterns[best_candidate].judgment_overall_corr_pct = best_proposed_corr;

            selected_indices[num_selected++] = best_candidate;
            printf("[ACCEPT] %s (Rank %d) added. Song Correlation increased: %.2f%% -> %.2f%%\n",
                   patterns[best_candidate].stem_filename, best_candidate + 1, current_song_correlation, best_proposed_corr);
            current_song_correlation = best_proposed_corr;
        } else {
            break;
        }
    }

    // Mark remaining unselected patterns and log rejection
    for (int i = 0; i < num_patterns; i++) {
        if (!patterns[i].selected) {
            patterns[i].selection_order = num_selected;
            if (patterns[i].active_length > 0) {
                printf("[REJECT] %s (Rank %d) best proposed correlation %.2f%% <= current %.2f%%\n",
                       patterns[i].stem_filename, i + 1, patterns[i].judgment_overall_corr_pct, current_song_correlation);
            }
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

    if (num_selected > 0) {
        printf("\nMixing and normalizing combined song WAV...\n");
        uint64_t max_frames = 0;
        unsigned int out_sr = 44100;

        for (int s = 0; s < num_selected; s++) {
            int idx = selected_indices[s];
            const PatternInfo* pat = &patterns[idx];
            if (pat->stem_total_frames > max_frames) {
                max_frames = pat->stem_total_frames;
            }
            if (pat->stem_sample_rate > 0) {
                out_sr = pat->stem_sample_rate;
            }
        }

        if (max_frames > 0) {
            float* mix_buffer = (float*)calloc(max_frames, sizeof(float));
            if (mix_buffer) {
                for (int s = 0; s < num_selected; s++) {
                    int idx = selected_indices[s];
                    const PatternInfo* pat = &patterns[idx];
                    if (pat->stem_samples && pat->stem_total_frames > 0) {
                        for (uint64_t f = 0; f < pat->stem_total_frames; f++) {
                            mix_buffer[f] += pat->stem_samples[f];
                        }
                    }
                }

                float max_amp = 0.0f;
                for (uint64_t f = 0; f < max_frames; f++) {
                    float amp = fabsf(mix_buffer[f]);
                    if (amp > max_amp) {
                        max_amp = amp;
                    }
                }

                if (max_amp > 0.0f) {
                    float scale = 1.0f / max_amp;
                    for (uint64_t f = 0; f < max_frames; f++) {
                        mix_buffer[f] *= scale;
                    }
                }

                int16_t* pcm16 = (int16_t*)malloc(max_frames * sizeof(int16_t));
                if (pcm16) {
                    for (uint64_t f = 0; f < max_frames; f++) {
                        float val = mix_buffer[f];
                        if (val > 1.0f) val = 1.0f;
                        if (val < -1.0f) val = -1.0f;
                        pcm16[f] = (int16_t)roundf(val * 32767.0f);
                    }

                    uint64_t first_non_zero = 0;
                    while (first_non_zero < max_frames && pcm16[first_non_zero] == 0) {
                        first_non_zero++;
                    }

                    uint64_t frames_to_write = max_frames - first_non_zero;
                    if (frames_to_write == 0 && max_frames > 0) {
                        first_non_zero = 0;
                        frames_to_write = max_frames;
                    }

                    char finished_song_path[4096];
                    snprintf(finished_song_path, sizeof(finished_song_path), "%.3500s" PATH_SEP_STR "finished_song.wav", song_dir);

                    drwav_data_format format;
                    format.container = drwav_container_riff;
                    format.format = DR_WAVE_FORMAT_PCM;
                    format.channels = 1;
                    format.sampleRate = out_sr;
                    format.bitsPerSample = 16;

                    drwav wav_out;
                    if (drwav_init_file_write(&wav_out, finished_song_path, &format, NULL)) {
                        drwav_write_pcm_frames(&wav_out, frames_to_write, pcm16 + first_non_zero);
                        drwav_uninit(&wav_out);
                        printf("  Generated finished song WAV: %s (Normalized peak = 1.0, Omitted %llu leading zero samples)\n", finished_song_path, (unsigned long long)first_non_zero);
                    } else {
                        printf("  [Error] Failed creating finished song WAV: %s\n", finished_song_path);
                    }

                    free(pcm16);
                }
                free(mix_buffer);
            }
        }
    }

    printf("\nGenerating correlation HTML report...\n");
    generate_correlation_html_report(
        work_dir,
        patterns,
        num_patterns,
        selected_indices,
        num_selected,
        current_song_correlation
    );

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
        if (patterns[i].stem_samples) {
            drwav_free(patterns[i].stem_samples, NULL);
        }
    }
    free(patterns);
    free(selected_indices);
    free(test_set);

    printf("\nPress Enter to exit...\n");
    getchar();
    return 0;
}
