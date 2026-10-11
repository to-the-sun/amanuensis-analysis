#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <windows.h>
#include <process.h>
#define mkdir_cross(dir) _mkdir(dir)
#else
#include <pthread.h>
#include <unistd.h>
#define mkdir_cross(dir) mkdir(dir, 0755)
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFDIR) == S_IFDIR)
#endif

#include "cumulative_transience.h"
#include "ct_exporter.h"

static void get_directory_and_stem(const char* filepath, char* out_dir, char* out_stem) {
    const char* last_slash = strrchr(filepath, '/');
    const char* last_backslash = strrchr(filepath, '\\');
    const char* filename = filepath;

    if (last_slash && last_slash >= filename) filename = last_slash + 1;
    if (last_backslash && last_backslash >= filename) filename = last_backslash + 1;

    const char* dot = strrchr(filename, '.');
    size_t stem_len = dot ? (size_t)(dot - filename) : strlen(filename);

    if (out_stem) {
        strncpy(out_stem, filename, stem_len);
        out_stem[stem_len] = '\0';
    }

    size_t dir_len = (size_t)(filename - filepath);
    if (dir_len > 0) {
        strncpy(out_dir, filepath, dir_len);
        out_dir[dir_len] = '\0';
    } else {
        strcpy(out_dir, "." PATH_SEP_STR);
    }
}

static int iequals(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int istarts_with(const char* str, const char* prefix) {
    while (*prefix) {
        if (!*str || tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        str++;
        prefix++;
    }
    return 1;
}

static int is_text_file(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot) return 0;
    if (iequals(dot, ".txt") || iequals(dot, ".json") || iequals(dot, ".csv") ||
        iequals(dot, ".log") || iequals(dot, ".xml")  || iequals(dot, ".text")) {
        return 1;
    }
    return 0;
}

static int copy_file(const char* src_path, const char* dst_path) {
    if (!src_path || !dst_path) return 0;
    if (strcmp(src_path, dst_path) == 0) return 1;

    FILE* src = fopen(src_path, "rb");
    if (!src) {
        printf("Error: Could not open source file for copying: %s\n", src_path);
        return 0;
    }

    FILE* dst = fopen(dst_path, "wb");
    if (!dst) {
        printf("Error: Could not open destination file for writing: %s\n", dst_path);
        fclose(src);
        return 0;
    }

    char buffer[65536];
    size_t bytes_read;
    int success = 1;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        if (fwrite(buffer, 1, bytes_read, dst) != bytes_read) {
            printf("Error: Failed writing data when copying to %s\n", dst_path);
            success = 0;
            break;
        }
    }

    fclose(src);
    fclose(dst);
    return success;
}

static void scan_and_copy_passes_files_from_dir(const char* dir_path, const char* target_passes_stem, size_t target_len, const char* output_dir) {
    char scan_dir[2048];
    if (dir_path && dir_path[0] != '\0') {
        strncpy(scan_dir, dir_path, sizeof(scan_dir) - 1);
        scan_dir[sizeof(scan_dir) - 1] = '\0';
    } else {
        strcpy(scan_dir, ".");
    }

    size_t scan_len = strlen(scan_dir);
    while (scan_len > 1 && (scan_dir[scan_len - 1] == '/' || scan_dir[scan_len - 1] == '\\')) {
        scan_dir[scan_len - 1] = '\0';
        scan_len--;
    }

    DIR* dir = opendir(scan_dir);
    if (!dir) {
        strcpy(scan_dir, ".");
        dir = opendir(scan_dir);
    }

    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;

        char full_entry_path[4096];
        snprintf(full_entry_path, sizeof(full_entry_path), "%s" PATH_SEP_STR "%s", scan_dir, name);
        struct stat st;
        if (stat(full_entry_path, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }

        if (!is_text_file(name)) {
            continue;
        }

        const char* dot = strrchr(name, '.');
        size_t entry_stem_len = dot ? (size_t)(dot - name) : strlen(name);
        char entry_stem[1024];
        if (entry_stem_len >= sizeof(entry_stem)) entry_stem_len = sizeof(entry_stem) - 1;
        strncpy(entry_stem, name, entry_stem_len);
        entry_stem[entry_stem_len] = '\0';

        if (istarts_with(entry_stem, target_passes_stem)) {
            if (entry_stem_len == target_len || !isalnum((unsigned char)entry_stem[target_len])) {
                char dst_file_path[4096];
                snprintf(dst_file_path, sizeof(dst_file_path), "%s" PATH_SEP_STR "%s", output_dir, name);
                if (copy_file(full_entry_path, dst_file_path)) {
                    printf("Copied matching text file: %s -> %s\n", full_entry_path, dst_file_path);
                }
            }
        }
    }
    closedir(dir);
}

static void copy_matching_passes_text_files(const char* dir_path, const char* stem_name, const char* output_dir) {
    if (!istarts_with(stem_name, "palette")) {
        return;
    }

    const char* rest_of_stem = stem_name + 7;
    char target_passes_stem[1024];
    snprintf(target_passes_stem, sizeof(target_passes_stem), "passes%s", rest_of_stem);
    size_t target_len = strlen(target_passes_stem);

    scan_and_copy_passes_files_from_dir(dir_path, target_passes_stem, target_len, output_dir);
}

static int has_wav_extension(const char* filename) {
    size_t len = strlen(filename);
    if (len < 4) return 0;
    const char* ext = filename + len - 4;
    return iequals(ext, ".wav");
}

static int is_excluded_filename(const char* filename) {
    if (iequals(filename, "preview.wav") || iequals(filename, "glued.wav")) {
        return 1;
    }
    return 0;
}

typedef struct {
    char** items;
    int count;
    int capacity;
} FileList;

static void file_list_init(FileList* list) {
    list->count = 0;
    list->capacity = 16;
    list->items = (char**)malloc(list->capacity * sizeof(char*));
}

static void file_list_add(FileList* list, const char* name) {
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->items = (char**)realloc(list->items, list->capacity * sizeof(char*));
    }
    list->items[list->count] = strdup(name);
    list->count++;
}

static void file_list_free(FileList* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char**)a, *(const char**)b);
}

static int process_single_file(const char* audio_filepath, int window_ms) {
    printf("\n------------------------------------------------------------\n");
    printf("Loading WAV audio file: %s\n", audio_filepath);

    unsigned int channels;
    unsigned int sample_rate;
    drwav_uint64 total_pcm_frames;

    float* p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        audio_filepath, &channels, &sample_rate, &total_pcm_frames, NULL
    );

    if (!p_sample_data) {
        printf("Error: Could not open or decode WAV file: %s\n", audio_filepath);
        return 0;
    }

    drwav_uint64 mono_frames = total_pcm_frames;

    float* mono_data = (float*)malloc(mono_frames * sizeof(float));
    if (!mono_data) {
        printf("Error: Memory allocation failed for audio buffer.\n");
        drwav_free(p_sample_data, NULL);
        return 0;
    }

    if (channels == 1) {
        memcpy(mono_data, p_sample_data, mono_frames * sizeof(float));
    } else {
        for (drwav_uint64 i = 0; i < mono_frames; i++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < channels; c++) {
                sum += p_sample_data[i * channels + c];
            }
            mono_data[i] = (float)(sum / channels);
        }
    }
    drwav_free(p_sample_data, NULL);

    double total_duration_s = (double)mono_frames / (double)sample_rate;
    printf("Loaded audio successfully: %.2f seconds (%llu frames, %u Hz, %u ch)\n",
           total_duration_s, (unsigned long long)mono_frames, sample_rate, channels);

    char dir_path[2048];
    char stem_name[1024];
    get_directory_and_stem(audio_filepath, dir_path, stem_name);

    char output_dir[4096];
    snprintf(output_dir, sizeof(output_dir), "%s[palettes]" PATH_SEP_STR "%s", dir_path, stem_name);
    mkdir_p(output_dir);

    const char* last_slash = strrchr(audio_filepath, '/');
    const char* last_backslash = strrchr(audio_filepath, '\\');
    const char* audio_filename = audio_filepath;
    if (last_slash && last_slash >= audio_filename) audio_filename = last_slash + 1;
    if (last_backslash && last_backslash >= audio_filename) audio_filename = last_backslash + 1;

    char dst_audio_path[4096];
    snprintf(dst_audio_path, sizeof(dst_audio_path), "%s" PATH_SEP_STR "%s", output_dir, audio_filename);

    if (copy_file(audio_filepath, dst_audio_path)) {
        printf("Copied audio file to destination: %s -> %s\n", audio_filepath, dst_audio_path);
    }

    copy_matching_passes_text_files(dir_path, stem_name, output_dir);

    printf("\nRunning standalone transience analysis, pattern finding, & asset exports in pure C...\n");

    int success = export_all_assets_and_html(
        audio_filepath,
        output_dir,
        stem_name,
        mono_data,
        (int)mono_frames,
        (int)sample_rate,
        window_ms
    );
    free(mono_data);

    if (!success) {
        printf("\nError: Standalone analysis failed during asset export for %s.\n", audio_filepath);
        return 0;
    }

    printf("\n============================================================\n");
    printf("ASSETS & HTML REPORT GENERATED IN PURE C FOR: %s\n", stem_name);
    printf("Output Directory: %s" PATH_SEP_STR "\n", output_dir);
    printf("Assets Exported:  .ctbin, manifest.json, snapshots.bin, snapshots.js,\n");
    printf("                  report_data.json, report_data.js,\n");
    printf("                  [loops]" PATH_SEP_STR " pattern WAVs,\n");
    printf("                  [stems]" PATH_SEP_STR " aligned pattern WAVs,\n");
    printf("                  %s_pattern_analysis.html\n", stem_name);
    printf("============================================================\n");

    return 1;
}

typedef struct {
    char full_path[4096];
    char filename[1024];
    int file_index;
    int total_files;
    int window_ms;
    int success;
} ThreadTask;

#if defined(_WIN32) || defined(_WIN64)
static unsigned int __stdcall worker_thread_win(void* arg) {
    ThreadTask* t = (ThreadTask*)arg;
    printf("\n[Thread %d/%d] Starting parallel processing: %s\n", t->file_index, t->total_files, t->filename);
    t->success = process_single_file(t->full_path, t->window_ms);
    return 0;
}
#else
static void* worker_thread_posix(void* arg) {
    ThreadTask* t = (ThreadTask*)arg;
    printf("\n[Thread %d/%d] Starting parallel processing: %s\n", t->file_index, t->total_files, t->filename);
    t->success = process_single_file(t->full_path, t->window_ms);
    return NULL;
}
#endif

typedef struct {
    int stem_index;
    char full_path[4096];
    char filename[1024];
    GroupStemInput* stem_out;
    float** mono_buffer_out;
    volatile int* decoded_counter;
    GroupMutex* counter_mutex;
    int total_stems;
    int success;
} GroupDecodeTask;

#if defined(_WIN32) || defined(_WIN64)
static unsigned int __stdcall decode_worker_win(void* arg) {
    GroupDecodeTask* t = (GroupDecodeTask*)arg;
    printf("[Thread %d/%d] Decoding stem audio: %s\n", t->stem_index + 1, t->total_stems, t->filename);
    unsigned int channels, sample_rate;
    drwav_uint64 total_pcm_frames;
    float* p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        t->full_path, &channels, &sample_rate, &total_pcm_frames, NULL
    );
    if (!p_sample_data) {
        printf("\nError: Could not open or decode WAV file: %s\n", t->full_path);
        t->success = 0;
        return 0;
    }
    float* mono_data = (float*)malloc(total_pcm_frames * sizeof(float));
    if (!mono_data) {
        drwav_free(p_sample_data, NULL);
        t->success = 0;
        return 0;
    }
    if (channels == 1) {
        memcpy(mono_data, p_sample_data, total_pcm_frames * sizeof(float));
    } else {
        for (drwav_uint64 f = 0; f < total_pcm_frames; f++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < channels; c++) sum += p_sample_data[f * channels + c];
            mono_data[f] = (float)(sum / channels);
        }
    }
    drwav_free(p_sample_data, NULL);

    char stem_dir[2048], stem_name[1024];
    get_directory_and_stem(t->full_path, stem_dir, stem_name);

    t->stem_out->audio_filepath = strdup(t->full_path);
    t->stem_out->stem_name = strdup(stem_name);
    t->stem_out->mono_data = mono_data;
    t->stem_out->len = (int)total_pcm_frames;
    t->stem_out->sr = (int)sample_rate;

    *(t->mono_buffer_out) = mono_data;
    t->success = 1;

    group_mutex_lock(t->counter_mutex);
    (*(t->decoded_counter))++;
    int cnt = *(t->decoded_counter);
    print_progress_bar(cnt, t->total_stems, "Decoding Audio Stems");
    group_mutex_unlock(t->counter_mutex);

    return 0;
}
#else
static void* decode_worker_posix(void* arg) {
    GroupDecodeTask* t = (GroupDecodeTask*)arg;
    printf("[Thread %d/%d] Decoding stem audio: %s\n", t->stem_index + 1, t->total_stems, t->filename);
    unsigned int channels, sample_rate;
    drwav_uint64 total_pcm_frames;
    float* p_sample_data = drwav_open_file_and_read_pcm_frames_f32(
        t->full_path, &channels, &sample_rate, &total_pcm_frames, NULL
    );
    if (!p_sample_data) {
        printf("\nError: Could not open or decode WAV file: %s\n", t->full_path);
        t->success = 0;
        return NULL;
    }
    float* mono_data = (float*)malloc(total_pcm_frames * sizeof(float));
    if (!mono_data) {
        drwav_free(p_sample_data, NULL);
        t->success = 0;
        return NULL;
    }
    if (channels == 1) {
        memcpy(mono_data, p_sample_data, total_pcm_frames * sizeof(float));
    } else {
        for (drwav_uint64 f = 0; f < total_pcm_frames; f++) {
            double sum = 0.0;
            for (unsigned int c = 0; c < channels; c++) sum += p_sample_data[f * channels + c];
            mono_data[f] = (float)(sum / channels);
        }
    }
    drwav_free(p_sample_data, NULL);

    char stem_dir[2048], stem_name[1024];
    get_directory_and_stem(t->full_path, stem_dir, stem_name);

    t->stem_out->audio_filepath = strdup(t->full_path);
    t->stem_out->stem_name = strdup(stem_name);
    t->stem_out->mono_data = mono_data;
    t->stem_out->len = (int)total_pcm_frames;
    t->stem_out->sr = (int)sample_rate;

    *(t->mono_buffer_out) = mono_data;
    t->success = 1;

    group_mutex_lock(t->counter_mutex);
    (*(t->decoded_counter))++;
    int cnt = *(t->decoded_counter);
    print_progress_bar(cnt, t->total_stems, "Decoding Audio Stems");
    group_mutex_unlock(t->counter_mutex);

    return NULL;
}
#endif

static int process_group_files(const FileList* wav_files, const char* base_dir, int window_ms, const char* original_audio_filepath) {
    if (!wav_files || wav_files->count <= 0) return 0;
    int num_stems = wav_files->count;
    printf("\n------------------------------------------------------------\n");
    printf("Starting Grouped Stems Analysis across %d file(s) in %s...\n", num_stems, base_dir);

    GroupStemInput* stems = (GroupStemInput*)calloc(num_stems, sizeof(GroupStemInput));
    float** mono_buffers = (float**)calloc(num_stems, sizeof(float*));
    GroupDecodeTask* decode_tasks = (GroupDecodeTask*)calloc(num_stems, sizeof(GroupDecodeTask));

    int max_threads = 4;
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    if (sysinfo.dwNumberOfProcessors > 0) max_threads = (int)sysinfo.dwNumberOfProcessors;
#else
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) max_threads = (int)nprocs;
#endif
    if (max_threads > 16) max_threads = 16;
    if (max_threads < 1) max_threads = 1;

    volatile int decoded_count = 0;
    GroupMutex counter_mutex;
    group_mutex_init(&counter_mutex);

    print_progress_bar(0, num_stems, "Decoding Audio Stems");

    for (int i = 0; i < num_stems; i++) {
        decode_tasks[i].stem_index = i;
        snprintf(decode_tasks[i].full_path, sizeof(decode_tasks[i].full_path), "%s%s", base_dir, wav_files->items[i]);
        strncpy(decode_tasks[i].filename, wav_files->items[i], sizeof(decode_tasks[i].filename) - 1);
        decode_tasks[i].stem_out = &stems[i];
        decode_tasks[i].mono_buffer_out = &mono_buffers[i];
        decode_tasks[i].decoded_counter = &decoded_count;
        decode_tasks[i].counter_mutex = &counter_mutex;
        decode_tasks[i].total_stems = num_stems;
        decode_tasks[i].success = 0;
    }

#if defined(_WIN32) || defined(_WIN64)
    for (int batch_start = 0; batch_start < num_stems; batch_start += max_threads) {
        int batch_count = num_stems - batch_start;
        if (batch_count > max_threads) batch_count = max_threads;
        HANDLE* threads = (HANDLE*)malloc(batch_count * sizeof(HANDLE));
        if (threads) {
            for (int i = 0; i < batch_count; i++) {
                threads[i] = (HANDLE)_beginthreadex(NULL, 0, decode_worker_win, &decode_tasks[batch_start + i], 0, NULL);
            }
            WaitForMultipleObjects(batch_count, threads, TRUE, INFINITE);
            for (int i = 0; i < batch_count; i++) {
                CloseHandle(threads[i]);
            }
            free(threads);
        }
    }
#else
    for (int batch_start = 0; batch_start < num_stems; batch_start += max_threads) {
        int batch_count = num_stems - batch_start;
        if (batch_count > max_threads) batch_count = max_threads;
        pthread_t* threads = (pthread_t*)malloc(batch_count * sizeof(pthread_t));
        if (threads) {
            for (int i = 0; i < batch_count; i++) {
                pthread_create(&threads[i], NULL, decode_worker_posix, &decode_tasks[batch_start + i]);
            }
            for (int i = 0; i < batch_count; i++) {
                pthread_join(threads[i], NULL);
            }
            free(threads);
        }
    }
#endif

    group_mutex_destroy(&counter_mutex);

    int decode_failed = 0;
    for (int i = 0; i < num_stems; i++) {
        if (!decode_tasks[i].success) {
            decode_failed = 1;
            break;
        }
    }
    free(decode_tasks);

    if (decode_failed) {
        printf("Error: Decoding failed for one or more stems.\n");
        for (int k = 0; k < num_stems; k++) {
            if (stems[k].audio_filepath) free((void*)stems[k].audio_filepath);
            if (stems[k].stem_name) free((void*)stems[k].stem_name);
            if (mono_buffers[k]) free(mono_buffers[k]);
        }
        free(stems);
        free(mono_buffers);
        return 0;
    }

    // Determine group name from directory or default
    const char* last_sep = strrchr(base_dir, PATH_SEP);
    char group_name[1024] = "all_stems";
    if (last_sep && strlen(last_sep + 1) > 0) {
        snprintf(group_name, sizeof(group_name), "%s", last_sep + 1);
    }

    printf("\nRunning grouped cumulative transience analysis & asset exports in pure C...\n");
    if (original_audio_filepath && original_audio_filepath[0] != '\0') {
        printf("Original audio file specified: %s\n", original_audio_filepath);
    }

    int success = export_group_assets_and_html(
        stems,
        num_stems,
        base_dir,
        group_name,
        window_ms,
        original_audio_filepath
    );

    for (int i = 0; i < num_stems; i++) {
        free((void*)stems[i].audio_filepath);
        free((void*)stems[i].stem_name);
        free(mono_buffers[i]);
    }
    free(stems);
    free(mono_buffers);

    if (success) {
        printf("\n============================================================\n");
        printf("GROUP ANALYSIS COMPLETE FOR %d STEMS!\n", num_stems);
        printf("Output Folder: %s[palettes]" PATH_SEP_STR "\n", base_dir);
        printf("Group HTML Report: group_%s_pattern_analysis.html\n", group_name);
        printf("============================================================\n");
    }

    return success;
}

int main(int argc, char** argv) {
    printf("============================================================\n");
    printf("  Standalone C Cumulative Transience Analyzer & Exporter\n");
    printf("============================================================\n\n");

    int is_group_mode = 0;
    char target_dir[2048] = "";
    char original_audio_filepath[4096] = "";
    int window_ms = 15000;

    for (int i = 1; i < argc; i++) {
        if (iequals(argv[i], "--group") || iequals(argv[i], "-g")) {
            is_group_mode = 1;
        } else if (iequals(argv[i], "--original") || iequals(argv[i], "-o") || iequals(argv[i], "--original-audio")) {
            if (i + 1 < argc) {
                strncpy(original_audio_filepath, argv[i + 1], sizeof(original_audio_filepath) - 1);
                original_audio_filepath[sizeof(original_audio_filepath) - 1] = '\0';
                i++;
            }
        } else if (istarts_with(argv[i], "--original=") || istarts_with(argv[i], "-o=") || istarts_with(argv[i], "--original-audio=")) {
            const char* eq = strchr(argv[i], '=');
            if (eq) {
                strncpy(original_audio_filepath, eq + 1, sizeof(original_audio_filepath) - 1);
                original_audio_filepath[sizeof(original_audio_filepath) - 1] = '\0';
            }
        } else {
            if (target_dir[0] == '\0') {
                strncpy(target_dir, argv[i], sizeof(target_dir) - 1);
                target_dir[sizeof(target_dir) - 1] = '\0';
            }
        }
    }

    if (target_dir[0] != '\0' && !is_group_mode) {
        // Single file drag-and-drop or command line invocation
        int success = process_single_file(target_dir, window_ms);
        return success ? 0 : 1;
    }

    // Determine directory to scan for WAV files
    char exe_dir[2048];
    if (target_dir[0] != '\0') {
        struct stat st;
        if (stat(target_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            snprintf(exe_dir, sizeof(exe_dir), "%s" PATH_SEP_STR, target_dir);
        } else {
            get_directory_and_stem(target_dir, exe_dir, NULL);
        }
    } else {
        // Fallback to current working directory if double clicked / run without explicit path argument
        strcpy(exe_dir, "." PATH_SEP_STR);
    }

    char scan_dir[2048];
    strncpy(scan_dir, exe_dir, sizeof(scan_dir) - 1);
    scan_dir[sizeof(scan_dir) - 1] = '\0';
    size_t scan_len = strlen(scan_dir);
    if (scan_len > 1 && (scan_dir[scan_len - 1] == '/' || scan_dir[scan_len - 1] == '\\')) {
        scan_dir[scan_len - 1] = '\0';
    }

    DIR* dir = opendir(scan_dir);
    if (!dir) {
        dir = opendir(".");
        strcpy(exe_dir, "." PATH_SEP_STR);
    }

    FileList wav_files;
    file_list_init(&wav_files);

    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* name = entry->d_name;
            if (!has_wav_extension(name)) continue;
            if (is_excluded_filename(name)) continue;

            // Check if entry is a directory
            char full_path[4096];
            snprintf(full_path, sizeof(full_path), "%s%s", exe_dir, name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                continue;
            }

            file_list_add(&wav_files, name);
        }
        closedir(dir);
    }

    if (wav_files.count == 0) {
        printf("No WAV files found to analyze in directory: %s\n", exe_dir);
        printf("(Explicitly excluded files named 'preview.wav' or 'glued.wav')\n\n");
        file_list_free(&wav_files);
        printf("Press Enter to exit...");
        getchar();
        return 0;
    }

    qsort(wav_files.items, wav_files.count, sizeof(char*), compare_strings);

    printf("Found %d WAV file(s) in %s (excluding preview.wav & glued.wav):\n", wav_files.count, exe_dir);
    for (int i = 0; i < wav_files.count; i++) {
        printf("  [%d/%d] %s\n", i + 1, wav_files.count, wav_files.items[i]);
    }

    if (is_group_mode) {
        int grp_success = process_group_files(
            &wav_files,
            exe_dir,
            window_ms,
            original_audio_filepath[0] != '\0' ? original_audio_filepath : NULL
        );
        file_list_free(&wav_files);
        printf("\nPress Enter to exit...");
        getchar();
        return grp_success ? 0 : 1;
    }

    int success_count = 0;
    int fail_count = 0;

    int num_files = wav_files.count;
    ThreadTask* tasks = (ThreadTask*)calloc(num_files, sizeof(ThreadTask));

    for (int i = 0; i < num_files; i++) {
        snprintf(tasks[i].full_path, sizeof(tasks[i].full_path), "%s%s", exe_dir, wav_files.items[i]);
        strncpy(tasks[i].filename, wav_files.items[i], sizeof(tasks[i].filename) - 1);
        tasks[i].file_index = i + 1;
        tasks[i].total_files = num_files;
        tasks[i].window_ms = window_ms;
        tasks[i].success = 0;
    }

    printf("\nStarting Multi-Threaded Batch Analysis across %d file(s)...\n", num_files);

    int max_threads = 4;
#if defined(_WIN32) || defined(_WIN64)
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    if (sysinfo.dwNumberOfProcessors > 0) max_threads = (int)sysinfo.dwNumberOfProcessors;
#else
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs > 0) max_threads = (int)nprocs;
#endif
    if (max_threads > 16) max_threads = 16;
    if (max_threads < 1) max_threads = 1;

    printf("Executing with max %d concurrent CPU worker thread(s)...\n", max_threads);

#if defined(_WIN32) || defined(_WIN64)
    for (int batch_start = 0; batch_start < num_files; batch_start += max_threads) {
        int batch_count = num_files - batch_start;
        if (batch_count > max_threads) batch_count = max_threads;
        HANDLE* threads = (HANDLE*)malloc(batch_count * sizeof(HANDLE));
        if (threads) {
            for (int i = 0; i < batch_count; i++) {
                threads[i] = (HANDLE)_beginthreadex(NULL, 0, worker_thread_win, &tasks[batch_start + i], 0, NULL);
            }
            WaitForMultipleObjects(batch_count, threads, TRUE, INFINITE);
            for (int i = 0; i < batch_count; i++) {
                CloseHandle(threads[i]);
            }
            free(threads);
        }
    }
#else
    for (int batch_start = 0; batch_start < num_files; batch_start += max_threads) {
        int batch_count = num_files - batch_start;
        if (batch_count > max_threads) batch_count = max_threads;
        pthread_t* threads = (pthread_t*)malloc(batch_count * sizeof(pthread_t));
        if (threads) {
            for (int i = 0; i < batch_count; i++) {
                pthread_create(&threads[i], NULL, worker_thread_posix, &tasks[batch_start + i]);
            }
            for (int i = 0; i < batch_count; i++) {
                pthread_join(threads[i], NULL);
            }
            free(threads);
        }
    }
#endif

    for (int i = 0; i < num_files; i++) {
        if (tasks[i].success) success_count++;
        else fail_count++;
    }
    free(tasks);

    file_list_free(&wav_files);

    printf("\n============================================================\n");
    printf("BATCH ANALYSIS COMPLETE!\n");
    printf("Processed %d file(s) successfully (%d failed).\n", success_count, fail_count);
    printf("============================================================\n\n");

    printf("Press Enter to exit...");
    getchar();
    return (fail_count == 0) ? 0 : 1;
}
