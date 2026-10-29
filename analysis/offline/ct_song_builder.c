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

static void generate_correlation_html_report(
    const char* work_dir,
    const PatternInfo* patterns,
    int num_patterns,
    const int* selected_indices,
    int num_selected,
    double final_correlation
) {
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
    fprintf(f, "  .legend { display: flex; gap: 20px; flex-wrap: wrap; margin-top: 15px; font-size: 0.85rem; padding-top: 12px; border-top: 1px solid var(--border); }\n");
    fprintf(f, "  .legend-item { display: flex; align-items: center; gap: 8px; }\n");
    fprintf(f, "  .legend-color { width: 14px; height: 14px; border-radius: 3px; display: inline-block; }\n");
    fprintf(f, "  .stem-card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 10px; padding: 16px; margin-bottom: 20px; }\n");
    fprintf(f, "  .stem-header { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 10px; margin-bottom: 12px; }\n");
    fprintf(f, "  .stem-title { font-size: 1.1rem; font-weight: bold; color: #fff; display: flex; align-items: center; gap: 10px; }\n");
    fprintf(f, "  .badge { font-size: 0.75rem; padding: 3px 8px; border-radius: 4px; font-weight: 600; text-transform: uppercase; }\n");
    fprintf(f, "  .badge-selected { background: #1b4d2e; color: #4caf50; border: 1px solid #2e7d32; }\n");
    fprintf(f, "  .badge-rejected { background: #4a151b; color: #f44336; border: 1px solid #c62828; }\n");
    fprintf(f, "  .stem-metrics { font-size: 0.85rem; color: var(--muted); font-family: monospace; display: flex; gap: 15px; }\n");
    fprintf(f, "  audio { height: 32px; width: 300px; outline: none; }\n");
    fprintf(f, "  .canvas-container { position: relative; width: 100%%; height: 160px; background: #0d0d11; border-radius: 6px; border: 1px solid #22222e; overflow: hidden; margin-top: 10px; user-select: none; }\n");
    fprintf(f, "  canvas { width: 100%%; height: 100%%; display: block; cursor: crosshair; }\n");
    fprintf(f, "  .tooltip { position: absolute; top: 8px; right: 12px; background: rgba(10, 10, 15, 0.9); border: 1px solid #333344; padding: 6px 10px; border-radius: 4px; font-size: 0.75rem; font-family: monospace; color: #fff; pointer-events: none; opacity: 0; transition: opacity 0.15s; z-index: 10; }\n");
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
    fprintf(f, "  <div class=\"legend\">\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: rgba(120,120,150,0.35); border: 1px solid rgba(150,150,180,0.5);\"></span> Output Audio Waveform (Background)</div>\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: #ff9800;\"></span> Stem Correlation Capacity (|S_i(t)|)</div>\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: #00e676;\"></span> Actual Overlapping Correlation (&sum;<sub>j &ne; i</sub> |S_j(t)|)</div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "</header>\n");

    fprintf(f, "<main>\n");

    int target_bins = 1000;

    for (int i = 0; i < num_patterns; i++) {
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
        fprintf(f, "      <div style=\"display: flex; align-items: center; gap: 15px;\">\n");
        fprintf(f, "        <div class=\"stem-metrics\">Duration: %.2fs | Active: %.2fs</div>\n",
                duration_s,
                pat->sample_rate > 0 ? (double)pat->active_length / pat->sample_rate : 0.0);
        fprintf(f, "        <audio controls src=\"%s\" id=\"audio-%d\"></audio>\n", pat->stem_filename, i);
        fprintf(f, "      </div>\n");
        fprintf(f, "    </div>\n");

        fprintf(f, "    <div class=\"canvas-container\" id=\"container-%d\">\n", i);
        fprintf(f, "      <canvas id=\"canvas-%d\"></canvas>\n", i);
        fprintf(f, "      <div class=\"tooltip\" id=\"tooltip-%d\"></div>\n", i);
        fprintf(f, "    </div>\n");
        fprintf(f, "  </div>\n");
    }

    fprintf(f, "</main>\n");

    fprintf(f, "<script>\n");
    fprintf(f, "const patternsData = [\n");

    for (int i = 0; i < num_patterns; i++) {
        const PatternInfo* pat_i = &patterns[i];
        if (!pat_i->recombined_samples || pat_i->total_frames == 0) continue;

        uint64_t total_frames = pat_i->total_frames;
        double duration_s = pat_i->sample_rate > 0 ? (double)total_frames / pat_i->sample_rate : 0.0;

        fprintf(f, "  {\n");
        fprintf(f, "    index: %d,\n", i);
        fprintf(f, "    filename: \"%s\",\n", pat_i->stem_filename);
        fprintf(f, "    duration: %.4f,\n", duration_s);

        fprintf(f, "    waveformMin: [");
        for (int b = 0; b < target_bins; b++) {
            uint64_t start_frame = (uint64_t)(((double)b / target_bins) * total_frames);
            uint64_t end_frame = (uint64_t)(((double)(b + 1) / target_bins) * total_frames);
            if (end_frame <= start_frame) end_frame = start_frame + 1;
            if (end_frame > pat_i->stem_total_frames && pat_i->stem_samples) end_frame = pat_i->stem_total_frames;

            float min_val = 0.0f;
            if (pat_i->stem_samples && start_frame < pat_i->stem_total_frames) {
                min_val = pat_i->stem_samples[start_frame];
                for (uint64_t s = start_frame; s < end_frame; s++) {
                    if (pat_i->stem_samples[s] < min_val) min_val = pat_i->stem_samples[s];
                }
            }
            fprintf(f, "%.4f%s", min_val, (b < target_bins - 1) ? "," : "");
        }
        fprintf(f, "],\n");

        fprintf(f, "    waveformMax: [");
        for (int b = 0; b < target_bins; b++) {
            uint64_t start_frame = (uint64_t)(((double)b / target_bins) * total_frames);
            uint64_t end_frame = (uint64_t)(((double)(b + 1) / target_bins) * total_frames);
            if (end_frame <= start_frame) end_frame = start_frame + 1;
            if (end_frame > pat_i->stem_total_frames && pat_i->stem_samples) end_frame = pat_i->stem_total_frames;

            float max_val = 0.0f;
            if (pat_i->stem_samples && start_frame < pat_i->stem_total_frames) {
                max_val = pat_i->stem_samples[start_frame];
                for (uint64_t s = start_frame; s < end_frame; s++) {
                    if (pat_i->stem_samples[s] > max_val) max_val = pat_i->stem_samples[s];
                }
            }
            fprintf(f, "%.4f%s", max_val, (b < target_bins - 1) ? "," : "");
        }
        fprintf(f, "],\n");

        fprintf(f, "    capacity: [");
        for (int b = 0; b < target_bins; b++) {
            uint64_t start_frame = (uint64_t)(((double)b / target_bins) * total_frames);
            uint64_t end_frame = (uint64_t)(((double)(b + 1) / target_bins) * total_frames);
            if (end_frame <= start_frame) end_frame = start_frame + 1;
            if (end_frame > total_frames) end_frame = total_frames;

            double max_cap = 0.0;
            for (uint64_t s = start_frame; s < end_frame; s++) {
                double cap = fabsf(pat_i->recombined_samples[s]);
                if (cap > max_cap) max_cap = cap;
            }
            fprintf(f, "%.4f%s", max_cap, (b < target_bins - 1) ? "," : "");
        }
        fprintf(f, "],\n");

        fprintf(f, "    actualCorrelation: [");
        for (int b = 0; b < target_bins; b++) {
            uint64_t start_frame = (uint64_t)(((double)b / target_bins) * total_frames);
            uint64_t end_frame = (uint64_t)(((double)(b + 1) / target_bins) * total_frames);
            if (end_frame <= start_frame) end_frame = start_frame + 1;
            if (end_frame > total_frames) end_frame = total_frames;

            double max_corr = 0.0;
            for (uint64_t s = start_frame; s < end_frame; s++) {
                double sum_other = 0.0;
                for (int j = 0; j < num_selected; j++) {
                    int idx_j = selected_indices[j];
                    if (idx_j == i) continue;
                    const PatternInfo* pat_j = &patterns[idx_j];
                    if (s < pat_j->total_frames && pat_j->recombined_samples) {
                        double cap_j = fabsf(pat_j->recombined_samples[s]);
                        if (cap_j > 1e-6) {
                            sum_other += cap_j;
                        }
                    }
                }
                if (sum_other > max_corr) max_corr = sum_other;
            }
            fprintf(f, "%.4f%s", max_corr, (b < target_bins - 1) ? "," : "");
        }
        fprintf(f, "]\n  }%s\n", (i < num_patterns - 1) ? "," : "");
    }

    fprintf(f, "];\n\n");

    fprintf(f, "patternsData.forEach(item => {\n");
    fprintf(f, "  const canvas = document.getElementById('canvas-' + item.index);\n");
    fprintf(f, "  const tooltip = document.getElementById('tooltip-' + item.index);\n");
    fprintf(f, "  const audio = document.getElementById('audio-' + item.index);\n");
    fprintf(f, "  if (!canvas || !audio) return;\n\n");
    fprintf(f, "  const ctx = canvas.getContext('2d');\n");
    fprintf(f, "  let mouseX = -1;\n\n");
    fprintf(f, "  function resize() {\n");
    fprintf(f, "    const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "    canvas.width = rect.width * window.devicePixelRatio;\n");
    fprintf(f, "    canvas.height = rect.height * window.devicePixelRatio;\n");
    fprintf(f, "    render();\n");
    fprintf(f, "  }\n\n");
    fprintf(f, "  function render() {\n");
    fprintf(f, "    const w = canvas.width;\n");
    fprintf(f, "    const h = canvas.height;\n");
    fprintf(f, "    ctx.clearRect(0, 0, w, h);\n\n");
    fprintf(f, "    // Background Waveform\n");
    fprintf(f, "    ctx.fillStyle = 'rgba(120, 120, 160, 0.28)';\n");
    fprintf(f, "    const midY = h / 2;\n");
    fprintf(f, "    const numBins = item.waveformMin.length;\n");
    fprintf(f, "    for (let i = 0; i < numBins; i++) {\n");
    fprintf(f, "      const x = (i / numBins) * w;\n");
    fprintf(f, "      const binW = Math.max(1, w / numBins);\n");
    fprintf(f, "      const minVal = item.waveformMin[i];\n");
    fprintf(f, "      const maxVal = item.waveformMax[i];\n");
    fprintf(f, "      const y1 = midY - (maxVal * (h / 2) * 0.85);\n");
    fprintf(f, "      const y2 = midY - (minVal * (h / 2) * 0.85);\n");
    fprintf(f, "      ctx.fillRect(x, y1, binW, Math.max(1, y2 - y1));\n");
    fprintf(f, "    }\n\n");
    fprintf(f, "    // Correlation Capacity Envelope (Orange)\n");
    fprintf(f, "    ctx.beginPath();\n");
    fprintf(f, "    ctx.strokeStyle = '#ff9800';\n");
    fprintf(f, "    ctx.lineWidth = 2 * window.devicePixelRatio;\n");
    fprintf(f, "    for (let i = 0; i < numBins; i++) {\n");
    fprintf(f, "      const x = (i / (numBins - 1)) * w;\n");
    fprintf(f, "      const cap = item.capacity[i];\n");
    fprintf(f, "      const y = h - (cap * h * 0.88) - (4 * window.devicePixelRatio);\n");
    fprintf(f, "      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    ctx.stroke();\n\n");
    fprintf(f, "    // Actual Overlapping Correlation Envelope (Green)\n");
    fprintf(f, "    ctx.beginPath();\n");
    fprintf(f, "    ctx.strokeStyle = '#00e676';\n");
    fprintf(f, "    ctx.lineWidth = 2 * window.devicePixelRatio;\n");
    fprintf(f, "    for (let i = 0; i < numBins; i++) {\n");
    fprintf(f, "      const x = (i / (numBins - 1)) * w;\n");
    fprintf(f, "      const corr = item.actualCorrelation[i];\n");
    fprintf(f, "      const y = h - (corr * h * 0.88) - (4 * window.devicePixelRatio);\n");
    fprintf(f, "      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "    }\n");
    fprintf(f, "    ctx.stroke();\n\n");
    fprintf(f, "    // Audio Playhead\n");
    fprintf(f, "    if (item.duration > 0 && audio.currentTime > 0) {\n");
    fprintf(f, "      const playX = (audio.currentTime / item.duration) * w;\n");
    fprintf(f, "      ctx.strokeStyle = '#ffffff';\n");
    fprintf(f, "      ctx.lineWidth = 1.5 * window.devicePixelRatio;\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.moveTo(playX, 0);\n");
    fprintf(f, "      ctx.lineTo(playX, h);\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n\n");
    fprintf(f, "    // Hover Line & Tooltip\n");
    fprintf(f, "    if (mouseX >= 0) {\n");
    fprintf(f, "      ctx.strokeStyle = 'rgba(255, 255, 255, 0.4)';\n");
    fprintf(f, "      ctx.lineWidth = 1 * window.devicePixelRatio;\n");
    fprintf(f, "      ctx.beginPath();\n");
    fprintf(f, "      ctx.moveTo(mouseX, 0);\n");
    fprintf(f, "      ctx.lineTo(mouseX, h);\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");
    fprintf(f, "  canvas.addEventListener('mousemove', (e) => {\n");
    fprintf(f, "    const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "    const xRatio = (e.clientX - rect.left) / rect.width;\n");
    fprintf(f, "    mouseX = xRatio * canvas.width;\n");
    fprintf(f, "    const timeS = xRatio * item.duration;\n");
    fprintf(f, "    const binIdx = Math.min(item.capacity.length - 1, Math.max(0, Math.floor(xRatio * item.capacity.length)));\n");
    fprintf(f, "    const capVal = item.capacity[binIdx];\n");
    fprintf(f, "    const corrVal = item.actualCorrelation[binIdx];\n");
    fprintf(f, "    tooltip.style.opacity = '1';\n");
    fprintf(f, "    tooltip.innerHTML = `Time: ${timeS.toFixed(3)}s<br>Capacity: ${capVal.toFixed(3)}<br>Overlap Corr: ${corrVal.toFixed(3)}`;\n");
    fprintf(f, "    render();\n");
    fprintf(f, "  });\n\n");
    fprintf(f, "  canvas.addEventListener('mouseleave', () => {\n");
    fprintf(f, "    mouseX = -1;\n");
    fprintf(f, "    tooltip.style.opacity = '0';\n");
    fprintf(f, "    render();\n");
    fprintf(f, "  });\n\n");
    fprintf(f, "  canvas.addEventListener('click', (e) => {\n");
    fprintf(f, "    const rect = canvas.getBoundingClientRect();\n");
    fprintf(f, "    const xRatio = (e.clientX - rect.left) / rect.width;\n");
    fprintf(f, "    audio.currentTime = xRatio * item.duration;\n");
    fprintf(f, "  });\n\n");
    fprintf(f, "  audio.addEventListener('timeupdate', render);\n");
    fprintf(f, "  audio.addEventListener('play', render);\n");
    fprintf(f, "  audio.addEventListener('pause', render);\n");
    fprintf(f, "  window.addEventListener('resize', resize);\n");
    fprintf(f, "  resize();\n");
    fprintf(f, "});\n");
    fprintf(f, "</script>\n");
    fprintf(f, "</body>\n</html>\n");

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

    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].active_length == 0) {
            printf("[Skip] %s has 0 active length.\n", patterns[i].stem_filename);
            patterns[i].selected = false;
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
            patterns[i].selected = true;
            printf("[ACCEPT] %s (Rank %d) added as seed pattern. Song Correlation: %.2f%%\n",
                   patterns[i].stem_filename, i + 1, current_song_correlation);
        } else if (proposed_corr > current_song_correlation) {
            selected_indices[num_selected++] = i;
            patterns[i].selected = true;
            printf("[ACCEPT] %s (Rank %d) added. Song Correlation increased: %.2f%% -> %.2f%%\n",
                   patterns[i].stem_filename, i + 1, current_song_correlation, proposed_corr);
            current_song_correlation = proposed_corr;
        } else {
            patterns[i].selected = false;
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
        char dst_rec[4096];
        snprintf(dst_stem, sizeof(dst_stem), "%.3500s" PATH_SEP_STR "%.500s", song_dir, pat->stem_filename);
        snprintf(dst_rec, sizeof(dst_rec), "%.3500s" PATH_SEP_STR "%.500s", song_dir, pat->recombined_filename);

        bool ok_stem = copy_file(pat->stem_path, dst_stem);
        bool ok_rec = copy_file(pat->recombined_path, dst_rec);

        if (ok_stem && ok_rec) {
            printf("  Copied: %s and %s\n", pat->stem_filename, pat->recombined_filename);
            copy_success_count++;
        } else {
            printf("  [Error] Failed copying %s or %s into [song]\n", pat->stem_filename, pat->recombined_filename);
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
