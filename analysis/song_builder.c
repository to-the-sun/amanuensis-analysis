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
#include <process.h>
#include <direct.h>
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#define mkdir_win(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
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
    int count
) {
    if (count <= 0) {
        return 0.0;
    }

    uint64_t max_frames = 0;
    for (int i = 0; i < count; i++) {
        int idx = candidate_indices[i];
        const PatternInfo* pat = &patterns[idx];
        if (pat->recombined_samples && pat->total_frames > max_frames) {
            max_frames = pat->total_frames;
        }
    }

    if (max_frames == 0) {
        return 0.0;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    uint64_t qualifying_samples = 0;

    for (uint64_t t = 0; t < max_frames; t++) {
        double val = 0.0;
        int active_patterns = 0;
        for (int i = 0; i < count; i++) {
            int idx = candidate_indices[i];
            const PatternInfo* pat = &patterns[idx];
            if (t < pat->total_frames && pat->recombined_samples) {
                double smp_val = (double)fabsf(pat->recombined_samples[t]);
                if (smp_val > 1e-6) {
                    val += smp_val;
                    active_patterns++;
                }
            }
        }
        if (active_patterns > 1) {
            sum += val;
            sum_sq += val * val;
            qualifying_samples++;
        }
    }

    if (qualifying_samples == 0) {
        return 0.0;
    }

    double mean = sum / (double)qualifying_samples;
    double var = (sum_sq / (double)qualifying_samples) - (mean * mean);
    if (var < 0.0) var = 0.0;
    return sqrt(var);
}

typedef struct {
    double* val;
    int* active;
    uint64_t frames;
    double sum;
    double sum_sq;
    uint64_t qualifying;
} CorrAccumulator;

static double corr_stats_to_stddev(double sum, double sum_sq, uint64_t count) {
    if (count == 0) {
        return 0.0;
    }
    double mean = sum / (double)count;
    double var = (sum_sq / (double)count) - (mean * mean);
    if (var < 0.0) var = 0.0;
    return sqrt(var);
}

// Incrementally add one pattern to the summed envelope, keeping the running
// standard-deviation statistics identical to compute_correlation_ext().
static void accumulator_add(CorrAccumulator* acc, const PatternInfo* pat) {
    if (!pat->recombined_samples) return;

    uint64_t frames = pat->total_frames < acc->frames ? pat->total_frames : acc->frames;
    for (uint64_t t = 0; t < frames; t++) {
        double contribution = fabs((double)pat->recombined_samples[t]);
        if (contribution <= 1e-6) continue;

        double old_val = acc->val[t];
        int old_active = acc->active[t];
        double new_val = old_val + contribution;

        acc->val[t] = new_val;
        acc->active[t] = old_active + 1;

        if (old_active == 0) {
            // A single pattern at this sample does not qualify for the metric.
        } else if (old_active == 1) {
            acc->sum += new_val;
            acc->sum_sq += new_val * new_val;
            acc->qualifying++;
        } else {
            acc->sum += new_val - old_val;
            acc->sum_sq += new_val * new_val - old_val * old_val;
        }
    }
}

static void accumulator_remove(CorrAccumulator* acc, const PatternInfo* pat) {
    if (!pat->recombined_samples) return;

    uint64_t frames = pat->total_frames < acc->frames ? pat->total_frames : acc->frames;
    for (uint64_t t = 0; t < frames; t++) {
        double contribution = fabs((double)pat->recombined_samples[t]);
        if (contribution <= 1e-6) continue;

        double old_val = acc->val[t];
        int old_active = acc->active[t];
        double new_val = old_val - contribution;

        acc->val[t] = new_val;
        acc->active[t] = old_active - 1;

        if (old_active <= 1) {
            // Drops back to (or stays at) a single pattern: no longer qualifies.
        } else if (old_active == 2) {
            acc->sum -= old_val;
            acc->sum_sq -= old_val * old_val;
            acc->qualifying--;
        } else {
            acc->sum += new_val - old_val;
            acc->sum_sq += new_val * new_val - old_val * old_val;
        }
    }
}

#define SONG_TASK_PREFIX_MAX 3
#define SONG_MAX_WORKERS 64
#define SONG_PROGRESS_STEP 200000ULL
#define SONG_PROGRESS_FLUSH 8192ULL

// One unit of parallel work: a prefix of the depth-first search tree. Every
// combination the serial search would reach below that prefix belongs to the
// worker that claims it, so splitting the tree never changes which combinations
// are evaluated, in what order, or with which arithmetic.
typedef struct {
    int prefix[SONG_TASK_PREFIX_MAX];
    int length;
    uint64_t combinations;
} ComboTask;

typedef struct {
    const PatternInfo* patterns;
    int num_eligible;
    ComboTask* tasks;
    int num_tasks;
    uint64_t total_combinations;
    volatile long long next_task;
    volatile long long evaluated;
    volatile long long reported;
} ComboJob;

typedef struct {
    ComboJob* job;
    CorrAccumulator acc;
    int* best_indices;
    int best_count;
    double best_corr;
    int* scratch;
    uint64_t local_steps;
    bool verbose;
} ComboWorker;

#if defined(_WIN32) || defined(_WIN64)
static long long atomic_increment(volatile long long* counter) {
    return (long long)InterlockedIncrement64((volatile LONG64*)counter) - 1;
}

static long long atomic_add(volatile long long* counter, long long delta) {
    return (long long)InterlockedExchangeAdd64((volatile LONG64*)counter, delta) + delta;
}

static long long atomic_exchange(volatile long long* target, long long value) {
    return (long long)InterlockedExchange64((volatile LONG64*)target, value);
}
#else
static long long atomic_increment(volatile long long* counter) {
    return (long long)__sync_fetch_and_add(counter, 1);
}

static long long atomic_add(volatile long long* counter, long long delta) {
    return (long long)__sync_add_and_fetch(counter, delta);
}

static long long atomic_exchange(volatile long long* target, long long value) {
    return (long long)__sync_lock_test_and_set(target, value);
}
#endif

static int hardware_thread_count(void) {
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    int cores = (int)info.dwNumberOfProcessors;
#else
    int cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (cores < 1) cores = 1;
    if (cores > SONG_MAX_WORKERS) cores = SONG_MAX_WORKERS;
    return cores;
}

// Every worker keeps its own copy of the per-sample accumulator, so the worker
// count is bounded by the core count and by the memory that state may occupy.
static int song_worker_count(int num_eligible, uint64_t accum_frames) {
    if (num_eligible < 6) return 1;  // 63 combinations or fewer: threading only adds overhead.

    int cores = hardware_thread_count();
    if (cores <= 1) return 1;

    uint64_t per_worker = (accum_frames > 0 ? accum_frames : 1) * (uint64_t)(sizeof(double) + sizeof(int));
    uint64_t budget = 1024ULL * 1024ULL * 1024ULL;
    uint64_t affordable = per_worker > 0 ? budget / per_worker : (uint64_t)cores;
    if (affordable < 1) affordable = 1;
    if ((uint64_t)cores > affordable) cores = (int)affordable;
    if (cores < 1) cores = 1;
    return cores;
}

// A combination replaces the current best when it correlates more strongly; on a
// tie the smaller combination wins, then the one whose patterns rank earliest
// (i.e. the longest patterns first). The ordering is total, so the winner does
// not depend on the order in which workers finish.
static bool combination_is_better(int count, const int* indices, double corr,
                                  int best_count, const int* best_indices, double best_corr) {
    if (corr > best_corr) return true;
    if (corr < best_corr) return false;
    if (best_count == 0) return true;
    if (count != best_count) return count < best_count;
    for (int k = 0; k < count; k++) {
        if (indices[k] != best_indices[k]) {
            return indices[k] < best_indices[k];
        }
    }
    return false;
}

static void flush_progress(ComboWorker* worker) {
    ComboJob* job = worker->job;
    long long done = atomic_add(&job->evaluated, (long long)SONG_PROGRESS_FLUSH);
    if (job->total_combinations == 0) return;

    long long boundary = done - (done % (long long)SONG_PROGRESS_STEP);
    if (boundary <= 0 || (uint64_t)boundary >= job->total_combinations) return;
    if (atomic_exchange(&job->reported, boundary) == boundary) return;

    printf("[Progress] %lld / %llu combinations evaluated (%.2f%%)\n",
           boundary,
           (unsigned long long)job->total_combinations,
           100.0 * (double)boundary / (double)job->total_combinations);
}

static void consider_combination(ComboWorker* worker, int count) {
    if (count <= 0) return;

    double corr = corr_stats_to_stddev(worker->acc.sum, worker->acc.sum_sq, worker->acc.qualifying);
    if (!combination_is_better(count, worker->scratch, corr,
                               worker->best_count, worker->best_indices, worker->best_corr)) {
        return;
    }

    for (int k = 0; k < count; k++) {
        worker->best_indices[k] = worker->scratch[k];
    }
    worker->best_count = count;
    worker->best_corr = corr;

    if (worker->verbose) {
        uint64_t total = worker->job->total_combinations;
        if (total > 0) {
            printf("[Best] %2d pattern(s), correlation %.6f (combination %llu of %llu)\n",
                   count, corr, (unsigned long long)worker->local_steps, (unsigned long long)total);
        } else {
            printf("[Best] %2d pattern(s), correlation %.6f (combination %llu)\n",
                   count, corr, (unsigned long long)worker->local_steps);
        }
    }
}

// Depth-first enumeration of every subset of the candidate patterns. Because the
// accumulator is mutated in place and undone on the way back up, each of the
// 2^n - 1 combinations costs only the samples where the added pattern is active.
static void search_combinations_recursive(ComboWorker* worker, int start_index, int current_count) {
    ComboJob* job = worker->job;

    for (int i = start_index; i < job->num_eligible; i++) {
        worker->scratch[current_count] = i;
        accumulator_add(&worker->acc, &job->patterns[i]);
        worker->local_steps++;

        consider_combination(worker, current_count + 1);

        if (!worker->verbose) {
            if ((worker->local_steps % SONG_PROGRESS_FLUSH) == 0) {
                flush_progress(worker);
            }
        } else if (job->total_combinations > 0 &&
                   (worker->local_steps % SONG_PROGRESS_STEP) == 0 &&
                   worker->local_steps != job->total_combinations) {
            printf("[Progress] %llu / %llu combinations evaluated (%.2f%%)\n",
                   (unsigned long long)worker->local_steps,
                   (unsigned long long)job->total_combinations,
                   100.0 * (double)worker->local_steps / (double)job->total_combinations);
        }

        search_combinations_recursive(worker, i + 1, current_count + 1);

        accumulator_remove(&worker->acc, &job->patterns[i]);
    }
}

static int compare_task_combinations_desc(const void* a, const void* b) {
    const ComboTask* ta = (const ComboTask*)a;
    const ComboTask* tb = (const ComboTask*)b;
    if (ta->combinations > tb->combinations) return -1;
    if (ta->combinations < tb->combinations) return 1;
    return 0;
}

// Split the tree at `depth`: every prefix of exactly that many patterns becomes
// one task, covering the prefix itself plus every extension hanging below it.
// Tasks are handed out largest first so the longest subtree never starts last.
static ComboTask* build_combo_tasks(int num_eligible, int depth, int* out_count) {
    *out_count = 0;
    if (depth < 2 || depth > SONG_TASK_PREFIX_MAX || num_eligible < depth || num_eligible > 32) {
        return NULL;
    }

    uint64_t count = 1;
    for (int k = 0; k < depth; k++) {
        count = count * (uint64_t)(num_eligible - k) / (uint64_t)(k + 1);
    }
    if (count == 0) return NULL;

    ComboTask* tasks = (ComboTask*)calloc((size_t)count, sizeof(ComboTask));
    if (!tasks) return NULL;

    int index = 0;
    for (int i = 0; i < num_eligible; i++) {
        for (int j = i + 1; j < num_eligible; j++) {
            if (depth == 2) {
                ComboTask* task = &tasks[index++];
                task->prefix[0] = i;
                task->prefix[1] = j;
                task->length = 2;
                task->combinations = 1ULL << (num_eligible - 1 - j);
                continue;
            }
            for (int k = j + 1; k < num_eligible; k++) {
                ComboTask* task = &tasks[index++];
                task->prefix[0] = i;
                task->prefix[1] = j;
                task->prefix[2] = k;
                task->length = 3;
                task->combinations = 1ULL << (num_eligible - 1 - k);
            }
        }
    }

    qsort(tasks, (size_t)index, sizeof(ComboTask), compare_task_combinations_desc);
    *out_count = index;
    return tasks;
}

// Combinations with fewer patterns than the split depth sit above the tasks, so
// they are evaluated once rather than by every worker.
static void evaluate_shallow_combinations(ComboWorker* worker, int depth, int start_index, int current_count) {
    for (int i = start_index; i < worker->job->num_eligible; i++) {
        worker->scratch[current_count] = i;
        accumulator_add(&worker->acc, &worker->job->patterns[i]);
        worker->local_steps++;

        consider_combination(worker, current_count + 1);

        if (current_count + 2 < depth) {
            evaluate_shallow_combinations(worker, depth, i + 1, current_count + 1);
        }

        accumulator_remove(&worker->acc, &worker->job->patterns[i]);
    }
}

static void worker_evaluate_task(ComboWorker* worker, const ComboTask* task) {
    ComboJob* job = worker->job;

    for (int k = 0; k < task->length; k++) {
        worker->scratch[k] = task->prefix[k];
        accumulator_add(&worker->acc, &job->patterns[task->prefix[k]]);
    }

    worker->local_steps++;
    consider_combination(worker, task->length);

    search_combinations_recursive(worker, task->prefix[task->length - 1] + 1, task->length);

    for (int k = task->length - 1; k >= 0; k--) {
        accumulator_remove(&worker->acc, &job->patterns[task->prefix[k]]);
    }
}

static void combo_worker_run(ComboWorker* worker) {
    ComboJob* job = worker->job;

    for (;;) {
        long long index = atomic_increment(&job->next_task);
        if (index >= (long long)job->num_tasks) break;
        worker_evaluate_task(worker, &job->tasks[index]);
    }

    uint64_t remaining = worker->local_steps % SONG_PROGRESS_FLUSH;
    if (remaining > 0) {
        atomic_add(&job->evaluated, (long long)remaining);
    }
}

#if defined(_WIN32) || defined(_WIN64)
static unsigned __stdcall combo_worker_thread(void* arg) {
    combo_worker_run((ComboWorker*)arg);
    return 0;
}
#else
static void* combo_worker_thread(void* arg) {
    combo_worker_run((ComboWorker*)arg);
    return NULL;
}
#endif

// Task claiming is dynamic, so a worker that never starts simply leaves its share
// of the tree to the others.
static void run_combo_workers(ComboWorker* workers, int num_workers) {
#if defined(_WIN32) || defined(_WIN64)
    HANDLE* handles = (HANDLE*)calloc((size_t)num_workers, sizeof(HANDLE));
    if (!handles) {
        combo_worker_run(&workers[0]);
        return;
    }

    for (int w = 0; w < num_workers; w++) {
        handles[w] = (HANDLE)_beginthreadex(NULL, 0, combo_worker_thread, &workers[w], 0, NULL);
        if (!handles[w]) {
            combo_worker_run(&workers[w]);
        }
    }

    for (int w = 0; w < num_workers; w++) {
        if (handles[w]) {
            WaitForSingleObject(handles[w], INFINITE);
            CloseHandle(handles[w]);
        }
    }
    free(handles);
#else
    pthread_t* threads = (pthread_t*)calloc((size_t)num_workers, sizeof(pthread_t));
    bool* started = (bool*)calloc((size_t)num_workers, sizeof(bool));
    if (!threads || !started) {
        free(threads);
        free(started);
        combo_worker_run(&workers[0]);
        return;
    }

    for (int w = 0; w < num_workers; w++) {
        started[w] = pthread_create(&threads[w], NULL, combo_worker_thread, &workers[w]) == 0;
        if (!started[w]) {
            combo_worker_run(&workers[w]);
        }
    }

    for (int w = 0; w < num_workers; w++) {
        if (started[w]) pthread_join(threads[w], NULL);
    }
    free(started);
    free(threads);
#endif
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
    int num_selected,
    double final_correlation
) {
    int* display_order = (int*)malloc((size_t)num_patterns * sizeof(int));
    if (!display_order) return;

    for (int i = 0; i < num_patterns; i++) {
        display_order[i] = i;
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

        fprintf(fjs, "  fluxEnvelopes: [\n");
        for (int d = 0; d < num_patterns; d++) {
            int i = display_order[d];
            const PatternInfo* pat = &patterns[i];
            uint64_t total_frames = pat->total_frames;
            int16_t* pcm_flux = (int16_t*)calloc(total_frames, sizeof(int16_t));
            if (pat->recombined_samples && pcm_flux) {
                for (uint64_t s = 0; s < total_frames; s++) {
                    float val = fabsf(pat->recombined_samples[s]);
                    if (val > 1.0f) val = 1.0f;
                    pcm_flux[s] = (int16_t)round(val * 32767.0f);
                }
            }
            char* b64 = encode_base64_bytes((const uint8_t*)pcm_flux, total_frames * sizeof(int16_t));
            fprintf(fjs, "    \"%s\"%s\n", b64 ? b64 : "", (d + 1 < num_patterns) ? "," : "");
            if (b64) free(b64);
            if (pcm_flux) free(pcm_flux);
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
    fprintf(f, "      <div class=\"stat-label\">Final Song Correlation (Std Dev)</div>\n");
    fprintf(f, "      <div class=\"stat-value\" style=\"color: #2196f3;\">%.6f</div>\n", final_correlation);
    fprintf(f, "    </div>\n");
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"toolbar\">\n");
    fprintf(f, "    <button class=\"btn\" onclick=\"resetZoom()\">Reset Zoom</button>\n");
    fprintf(f, "    <span class=\"view-info\" id=\"viewInfo\">Visible: 0.000s &ndash; %.3fs (Duration: %.3fs)</span>\n", max_song_duration, max_song_duration);
    fprintf(f, "  </div>\n");
    fprintf(f, "  <div class=\"legend\">\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: rgba(120,120,160,0.35); border: 1px solid rgba(150,150,180,0.5);\"></span> Output Audio Waveform (Background)</div>\n");
    fprintf(f, "    <div class=\"legend-item\"><span class=\"legend-color\" style=\"background: #ff9800;\"></span> Flux Envelope (|S_i(t)|)</div>\n");
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
        fprintf(f, "          <div><strong>Song Correlation (Std Dev) if Included:</strong> %.6f</div>\n",
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
    fprintf(f, "      if (window.WAVEFORM_DATA.fluxEnvelopes && window.WAVEFORM_DATA.fluxEnvelopes[i]) {\n");
    fprintf(f, "        st.fluxEnvelope = base64ToFloat32(window.WAVEFORM_DATA.fluxEnvelopes[i], n);\n");
    fprintf(f, "      }\n");
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

    fprintf(f, "  // --- 1. Draw Stem Flux Envelope (Orange) ---\n");
    fprintf(f, "  if (stem.fluxEnvelope) {\n");
    fprintf(f, "    ctx.beginPath();\n");
    fprintf(f, "    ctx.strokeStyle = '#ff9800';\n");
    fprintf(f, "    ctx.lineWidth = 2;\n");
    fprintf(f, "    if (visCount <= width * 3) {\n");
    fprintf(f, "      for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "        const t = s / sr;\n");
    fprintf(f, "        const x = timeToX(t, width);\n");
    fprintf(f, "        const cap = stem.fluxEnvelope[s];\n");
    fprintf(f, "        const y = height - (cap * height * 0.88) - 4;\n");
    fprintf(f, "        if (s === sampleStart) ctx.moveTo(x, y); else ctx.lineTo(x, y);\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "      if (visCount < width * 0.6) {\n");
    fprintf(f, "        ctx.fillStyle = '#ff9800';\n");
    fprintf(f, "        for (let s = sampleStart; s <= sampleEnd; s++) {\n");
    fprintf(f, "          const x = timeToX(s / sr, width);\n");
    fprintf(f, "          const cap = stem.fluxEnvelope[s];\n");
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
    fprintf(f, "          let maxC = stem.fluxEnvelope[s1];\n");
    fprintf(f, "          for (let s = s1; s <= s2; s++) {\n");
    fprintf(f, "            if (stem.fluxEnvelope[s] > maxC) maxC = stem.fluxEnvelope[s];\n");
    fprintf(f, "          }\n");
    fprintf(f, "          const y = height - (maxC * height * 0.88) - 4;\n");
    fprintf(f, "          if (px === 0) ctx.moveTo(px, y); else ctx.lineTo(px, y);\n");
    fprintf(f, "        }\n");
    fprintf(f, "      }\n");
    fprintf(f, "      ctx.stroke();\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 2. Audio Playhead ---\n");
    fprintf(f, "  if (audio && duration > 0 && audio.currentTime >= viewStart && audio.currentTime <= viewEnd) {\n");
    fprintf(f, "    const playX = timeToX(audio.currentTime, width);\n");
    fprintf(f, "    ctx.strokeStyle = '#ffffff';\n");
    fprintf(f, "    ctx.lineWidth = 1.5;\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(playX, 0); ctx.lineTo(playX, height); ctx.stroke();\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 3. Hover Crosshair ---\n");
    fprintf(f, "  if (hoverState && hoverState.index === index) {\n");
    fprintf(f, "    ctx.strokeStyle = 'rgba(255, 255, 255, 0.6)';\n");
    fprintf(f, "    ctx.setLineDash([4, 4]);\n");
    fprintf(f, "    ctx.beginPath(); ctx.moveTo(hoverState.mouseX, 0); ctx.lineTo(hoverState.mouseX, height); ctx.stroke();\n");
    fprintf(f, "    ctx.setLineDash([]);\n");
    fprintf(f, "  }\n\n");

    fprintf(f, "  // --- 4. Active Drag Selection Highlight ---\n");
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
    fprintf(f, "    const fluxVal = (stem.fluxEnvelope && stem.fluxEnvelope.length > s) ? stem.fluxEnvelope[s] : 0;\n");
    fprintf(f, "    const waveVal = (stem.stemWaveform && stem.stemWaveform.length > s) ? stem.stemWaveform[s] : 0;\n");
    fprintf(f, "    tooltip.style.opacity = '1';\n");
    fprintf(f, "    tooltip.innerHTML = `Time: ${formatMSSPrecise(t)} (Sample #${s}) | Flux: ${fluxVal.toFixed(4)} | Waveform: ${waveVal.toFixed(4)}`;\n");
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

    printf("\nEvaluating every possible pattern combination based on song correlation metric...\n\n");

    int* selected_indices = (int*)malloc((size_t)num_patterns * sizeof(int));
    int num_selected = 0;
    double current_song_correlation = 0.0;

    int* test_set = (int*)malloc((size_t)num_patterns * sizeof(int));

    // Initialize pattern defaults
    for (int i = 0; i < num_patterns; i++) {
        patterns[i].selected = false;
        patterns[i].selection_order = num_patterns;
        patterns[i].judgment_overall_corr_pct = 0.0;
        if (patterns[i].active_length == 0) {
            printf("[Skip] %s has 0 active length.\n", patterns[i].stem_filename);
        }
    }

    // Patterns are sorted descending by active length, so the patterns carrying any
    // audio at all sit at the front of the list.
    int num_eligible = 0;
    for (int i = 0; i < num_patterns; i++) {
        if (patterns[i].active_length > 0) {
            num_eligible++;
        }
    }

    if (num_eligible == 0) {
        printf("No candidate patterns with audio content to combine.\n");
        current_song_correlation = NAN;
    } else {
        uint64_t total_combinations = (num_eligible < 63) ? ((1ULL << num_eligible) - 1ULL) : UINT64_MAX;

        if (total_combinations == UINT64_MAX) {
            printf("Exhaustively evaluating every combination of %d patterns (more than 2^62 combinations).\n\n",
                   num_eligible);
            total_combinations = 0;
        } else {
            printf("Exhaustively evaluating every combination of %d patterns (%llu combinations).\n\n",
                   num_eligible, (unsigned long long)total_combinations);
            if (num_eligible > 20) {
                printf("[Warning] An exhaustive search of %d patterns evaluates 2^%d - 1 combinations and may take a long time.\n\n",
                       num_eligible, num_eligible);
            }
        }

        uint64_t accum_frames = 0;
        for (int i = 0; i < num_eligible; i++) {
            if (patterns[i].recombined_samples && patterns[i].total_frames > accum_frames) {
                accum_frames = patterns[i].total_frames;
            }
        }

        int num_workers = song_worker_count(num_eligible, accum_frames);
        // Splitting three levels down keeps individual tasks small enough that no
        // single subtree can leave the other workers waiting on it.
        int split_depth = (num_workers > 1 && num_eligible <= 32)
            ? ((num_eligible >= 10) ? 3 : 2)
            : 0;

        ComboJob job;
        memset(&job, 0, sizeof(job));
        job.patterns = patterns;
        job.num_eligible = num_eligible;
        job.total_combinations = total_combinations;

        ComboWorker* workers = (ComboWorker*)calloc((size_t)num_workers, sizeof(ComboWorker));
        int ready_workers = 0;
        if (workers) {
            for (int w = 0; w < num_workers; w++) {
                workers[w].job = &job;
                workers[w].best_corr = -1.0;
                workers[w].best_indices = (int*)malloc((size_t)num_eligible * sizeof(int));
                workers[w].scratch = (int*)malloc((size_t)num_eligible * sizeof(int));
                workers[w].acc.frames = accum_frames;
                workers[w].acc.val = (double*)calloc((size_t)(accum_frames > 0 ? accum_frames : 1), sizeof(double));
                workers[w].acc.active = (int*)calloc((size_t)(accum_frames > 0 ? accum_frames : 1), sizeof(int));
                if (!workers[w].best_indices || !workers[w].scratch || !workers[w].acc.val || !workers[w].acc.active) {
                    break;
                }
                ready_workers++;
            }
        }

        int* best_indices = NULL;
        int best_count = 0;
        double best_corr = -1.0;
        uint64_t evaluated_combinations = 0;

        if (accum_frames == 0 || ready_workers == 0) {
            printf("\nExhaustive search skipped: no usable pattern audio to combine.\n");
        } else {
            if (split_depth > 0) {
                job.tasks = build_combo_tasks(num_eligible, split_depth, &job.num_tasks);
                if (!job.tasks || job.num_tasks < ready_workers * 2) {
                    free(job.tasks);
                    job.tasks = NULL;
                    job.num_tasks = 0;
                    split_depth = 0;
                }
            }

            best_indices = (int*)malloc((size_t)num_eligible * sizeof(int));

            if (split_depth > 0) {
                workers[0].verbose = false;
                printf("[Workers] Evaluating every combination across %d thread(s).\n\n", ready_workers);
                evaluate_shallow_combinations(&workers[0], split_depth, 0, 0);
                run_combo_workers(workers, ready_workers);
                evaluated_combinations = total_combinations;
            } else {
                workers[0].verbose = true;
                search_combinations_recursive(&workers[0], 0, 0);
                evaluated_combinations = workers[0].local_steps;
            }

            // Each worker found the best combination inside its own share of the
            // tree; the same total order picks the overall winner.
            for (int w = 0; w < ready_workers; w++) {
                if (workers[w].best_count <= 0) continue;
                if (best_indices && combination_is_better(workers[w].best_count, workers[w].best_indices, workers[w].best_corr,
                                                          best_count, best_indices, best_corr)) {
                    memcpy(best_indices, workers[w].best_indices, (size_t)workers[w].best_count * sizeof(int));
                    best_count = workers[w].best_count;
                    best_corr = workers[w].best_corr;
                }
            }

            printf("\nExhaustive search complete: %llu combination(s) evaluated.\n",
                   (unsigned long long)evaluated_combinations);

            num_selected = best_count;
            for (int s = 0; s < num_selected; s++) {
                int idx = best_indices[s];
                selected_indices[s] = idx;
                patterns[idx].selected = true;
                patterns[idx].selection_order = s;
            }

        // Recompute through the canonical function so the reported correlation matches
        // exactly what compute_correlation_ext() yields for the winning combination.
        current_song_correlation = (num_selected > 0)
            ? compute_correlation_ext(patterns, selected_indices, num_selected)
            : NAN;

        for (int s = 0; s < num_selected; s++) {
            patterns[selected_indices[s]].judgment_overall_corr_pct = current_song_correlation;
        }

        // Rejected patterns report the correlation the song would have if they were added
        // to the best combination, which by construction is never higher than the best.
        for (int i = 0; i < num_eligible; i++) {
            if (patterns[i].selected) continue;
            for (int k = 0; k < num_selected; k++) {
                test_set[k] = selected_indices[k];
            }
            test_set[num_selected] = i;
            patterns[i].judgment_overall_corr_pct = compute_correlation_ext(patterns, test_set, num_selected + 1);
        }

        printf("\nMost highly correlated combination selected:\n");
        if (num_selected > 0) {
            for (int s = 0; s < num_selected; s++) {
                int idx = selected_indices[s];
                printf("  %d. %s (Active Length: %llu smp) Song Correlation: %.6f\n",
                       s + 1, patterns[idx].stem_filename,
                       (unsigned long long)patterns[idx].active_length,
                       current_song_correlation);
            }
        } else {
            printf("  (none)\n");
        }

            for (int w = 0; w < ready_workers; w++) {
                free(workers[w].best_indices);
                free(workers[w].scratch);
                free(workers[w].acc.val);
                free(workers[w].acc.active);
            }
        }

        free(best_indices);
        free(workers);
        free(job.tasks);
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
        num_selected,
        current_song_correlation
    );

    printf("\n=====================================================\n");
    printf("                  Song Summary                       \n");
    printf("=====================================================\n");
    printf("Total Candidate Patterns Evaluated : %d\n", num_patterns);
    printf("Patterns Selected into Song        : %d (Successfully copied: %d)\n", num_selected, copy_success_count);
    printf("Final Song Correlation Metric (Std Dev): %.6f\n", current_song_correlation);
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
