#ifndef CT_EXPORTER_H
#define CT_EXPORTER_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <process.h>
typedef CRITICAL_SECTION GroupMutex;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_mutex_t GroupMutex;
#endif

#include "cumulative_transience.h"

void group_mutex_init(GroupMutex* mutex);
void group_mutex_lock(void* lock_obj);
void group_mutex_unlock(void* lock_obj);
void group_mutex_destroy(GroupMutex* mutex);

void print_progress_bar(int current, int total, const char* label);

int mkdir_p(const char* path);

int export_ctbin_from_res(const char* output_filepath, const FullAnalysisResult* res, int sr, int window_ms);
int export_ctbin(const char* output_filepath, const float* y, int len, int sr, int window_ms);

int export_all_assets_and_html(
    const char* audio_filepath,
    const char* output_dir,
    const char* stem_name,
    const float* y,
    int len,
    int sr,
    int pass1_window_ms
);

typedef struct {
    const char* audio_filepath;
    const char* stem_name;
    const float* mono_data;
    int len;
    int sr;
} GroupStemInput;

int export_group_assets_and_html(
    const GroupStemInput* stems,
    int num_stems,
    const char* parent_dir,
    const char* group_name,
    int pass1_window_ms,
    const char* original_audio_filepath
);

#ifdef __cplusplus
}
#endif

#endif // CT_EXPORTER_H
