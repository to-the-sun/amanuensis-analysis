#if !defined(_WIN64)
#error "restless_virtuoso is a 64-bit Windows program"
#endif

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <shellapi.h>
#include <conio.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "asio_abi.h"
#include "dsp.h"
#include "vst_abi.h"
#define DR_WAV_IMPLEMENTATION
#include "../analysis/dr_wav.h"

#define MAX_SELECTED 32
#define QUEUE_FILES 1024
#define MAX_METERS 4
#define MIDI_QUEUE 1024
#define DEFAULT_DRIVER "Focusrite USB ASIO"

typedef struct {
    const char *driver, *in, *out, *midiIn, *midiOut;
    char instrument[MAX_PATH], samplesPath[MAX_PATH], vstsPath[MAX_PATH];
    double gainDb;
    int rate, buffer, synth, list, inputChannel, samplesRatio;
} Options;

typedef struct {
    IASIO *asio;
    char driverName[64];
    int32_t inCount, outCount;
    ASIOSampleRate rate;
    int32_t bufferSize;
    int32_t latencyIn, latencyOut;
    int postOutput;

    int nIn, nOut;
    int inCh[MAX_SELECTED], outCh[MAX_SELECTED];
    SampleFormat inFormat[MAX_SELECTED], outFormat[MAX_SELECTED];
    ASIOBufferInfo bufferInfo[MAX_SELECTED * 2];
    float *inputFloats, *fileFloats, *synthFloats, *mixFloats;

    HMIDIIN midiIn;
    HMIDIOUT midiOut;
    int midiInIndex, midiOutIndex;
    int midiChannel, midiStream;
    int synthEnabled, samplesSound, samplesRatio;
    int buffersCreated, started;

    volatile LONG quit, mute, reset;
    volatile float gain, volume, peak[MAX_SELECTED];
    float gainDb;
    int lineWidth;
} Monitor;

static Monitor g;
static ASIOCallbacks callbacks;

static FILE *errorLog;
static char logPath[MAX_PATH + 64];

static void recycle_file(const char *path)
{
    char from[MAX_PATH + 66] = {0};
    snprintf(from, sizeof from, "%s", path);

    SHFILEOPSTRUCTA operation;
    memset(&operation, 0, sizeof operation);
    operation.wFunc = FO_DELETE;
    operation.pFrom = from;
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationA(&operation);
}

static void recycle_old_logs(const char *folder, const char *stem)
{
    char pattern[MAX_PATH + 16];
    snprintf(pattern, sizeof pattern, "%s%s*.log", folder, stem);

    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) return;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cutoff;
    cutoff.LowPart = now.dwLowDateTime;
    cutoff.HighPart = now.dwHighDateTime;
    cutoff.QuadPart -= 7ULL * 24 * 60 * 60 * 10000000;

    do {
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        ULARGE_INTEGER written;
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        if (written.QuadPart >= cutoff.QuadPart) continue;

        char path[MAX_PATH + 64];
        snprintf(path, sizeof path, "%s%s", folder, entry.cFileName);
        recycle_file(path);
    } while (FindNextFileA(find, &entry));
    FindClose(find);
}

static void open_error_log(void)
{
    char module[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, module, MAX_PATH);
    if (!length || length >= MAX_PATH) return;

    char *stem = strrchr(module, '\\');
    stem = stem ? stem + 1 : module;
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    int folderLength = (int)(stem - module);
    char folder[MAX_PATH];
    memcpy(folder, module, (size_t)folderLength);
    folder[folderLength] = 0;

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    recycle_old_logs(folder, stem);
    if (SUCCEEDED(com)) CoUninitialize();

    SYSTEMTIME now;
    GetLocalTime(&now);

    for (int attempt = 1; attempt < 100; attempt++) {
        char suffix[8] = "";
        if (attempt > 1) snprintf(suffix, sizeof suffix, "-%d", attempt);
        snprintf(logPath, sizeof logPath, "%.*s%s-%04d-%02d-%02d_%02d-%02d-%02d%s.log",
                 folderLength, module, stem, now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond, suffix);
        if (GetFileAttributesA(logPath) == INVALID_FILE_ATTRIBUTES) break;
    }

    errorLog = fopen(logPath, "w");
    if (errorLog) {
        fprintf(errorLog, "--- restless_virtuoso run ---\n");
        fflush(errorLog);
    }
}

static void log_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    if (!errorLog) open_error_log();
    if (errorLog) {
        va_start(args, format);
        vfprintf(errorLog, format, args);
        va_end(args);
        fflush(errorLog);
    }
}

static void log_trace(const char *format, ...)
{
    if (!errorLog) open_error_log();
    if (!errorLog) return;

    va_list args;
    va_start(args, format);
    vfprintf(errorLog, format, args);
    va_end(args);
    fflush(errorLog);
}

static void append_log_line(const char *text)
{
    if (!logPath[0]) return;

    HANDLE file = CreateFileA(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    DWORD written;
    WriteFile(file, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(file);
}

static volatile LONG initReturned;

static DWORD WINAPI init_watchdog(void *unused)
{
    (void)unused;
    Sleep(10000);
    if (!InterlockedCompareExchange(&initReturned, 1, 1))
        append_log_line("ASIOInit still running after 10 seconds\n");
    return 0;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info)
{
    const void *address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = NULL;
    char modulePath[MAX_PATH] = "unknown module";
    char message[512];

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module))
        GetModuleFileNameA(module, modulePath, MAX_PATH);

    snprintf(message, sizeof message, "unhandled exception 0x%08lx at %p (%s+0x%lx)\n",
             (unsigned long)info->ExceptionRecord->ExceptionCode, address, modulePath,
             module ? (unsigned long)((ULONG_PTR)address - (ULONG_PTR)module) : 0);
    append_log_line(message);
    return EXCEPTION_EXECUTE_HANDLER;
}

static volatile uint32_t midiHead, midiTail;
static uint32_t midiQueue[MIDI_QUEUE];

#define VST_MAX_PLUGINS 1024
#define VST_MAX_EVENTS 256
#define VST_MAX_OUTPUTS 32

typedef struct {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent *events[VST_MAX_EVENTS];
    VstMidiEvent midi[VST_MAX_EVENTS];
} MidiEventList;

static MidiEventList midiEvents;
static AEffect *vstEffect;

static void queue_midi_event(uint32_t message)
{
    if (midiEvents.numEvents == VST_MAX_EVENTS) return;

    VstMidiEvent *event = &midiEvents.midi[midiEvents.numEvents];
    memset(event, 0, sizeof *event);
    event->type = kVstMidiType;
    event->byteSize = sizeof *event;
    event->midiData[0] = (char)(message & 0xff);
    event->midiData[1] = (char)(message >> 8 & 0xff);
    event->midiData[2] = (char)(message >> 16 & 0xff);
    midiEvents.events[midiEvents.numEvents++] = (VstEvent *)event;
}

static void trigger_drum(int note);

static const char *asio_error_text(ASIOError error)
{
    switch (error) {
    case ASE_OK: return "ok";
    case ASE_NotPresent: return "not present";
    case ASE_HWMalfunction: return "hardware malfunction";
    case ASE_InvalidParameter: return "invalid parameter";
    case ASE_InvalidMode: return "invalid mode";
    case ASE_SPNotAdvancing: return "sample position not advancing";
    case ASE_NoClock: return "no clock";
    case ASE_NoMemory: return "out of memory";
    default: return "unknown error";
    }
}

static void log_line(const char *format, ...)
{
    va_list args;
    putchar('\n');
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    g.lineWidth = 0;
}

/* Single producer (WinMM callback thread), single consumer (ASIO thread).
   x64's memory model keeps the plain volatile indices ordered. */
static void midi_push(uint32_t message)
{
    uint32_t head = midiHead;
    if (head - midiTail >= MIDI_QUEUE) return;
    midiQueue[head % MIDI_QUEUE] = message;
    midiHead = head + 1;
}

static int midi_pop(uint32_t *message)
{
    uint32_t tail = midiTail;
    if (tail == midiHead) return 0;
    *message = midiQueue[tail % MIDI_QUEUE];
    midiTail = tail + 1;
    return 1;
}

static void drain_midi(void)
{
    uint32_t message;
    midiEvents.numEvents = 0;
    while (midi_pop(&message)) {
        int status = (int)(message & 0xf0);
        int note = (int)(message >> 8) & 0x7f;
        int velocity = (int)(message >> 16) & 0x7f;
        if (status == 0x90 && velocity > 0) {
            synth_note_on(note, velocity / 127.0f);
            trigger_drum(note);
        } else if (status == 0x80 || status == 0x90) {
            synth_note_off(note);
        }
        if (vstEffect && status < 0xf0) queue_midi_event(message);
    }
}

typedef struct {
    float *samples;
    drwav_uint64 frames, position;
    unsigned channels, rate;
    double fraction;
} Track;

static wchar_t *long_path(const wchar_t *path)
{
    if (!wcsncmp(path, L"\\\\?\\", 4)) {
        size_t size = (wcslen(path) + 1) * sizeof(wchar_t);
        wchar_t *copy = malloc(size);
        if (copy) memcpy(copy, path, size);
        return copy;
    }

    wchar_t full[32768];
    size_t length = wcslen(path);
    int drive = length >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    int unc = length >= 2 && path[0] == L'\\' && path[1] == L'\\';
    const wchar_t *source = path;

    if (!drive && !unc) {
        DWORD result = GetFullPathNameW(path, 32768, full, NULL);
        if (!result || result >= 32768) return NULL;
        source = full;
        unc = full[0] == L'\\' && full[1] == L'\\';
    }

    length = wcslen(source);
    wchar_t *result = malloc((length + (unc ? 7 : 5)) * sizeof(wchar_t));
    if (!result) return NULL;
    if (unc) swprintf(result, length + 7, L"\\\\?\\UNC\\%ls", source + 2);
    else swprintf(result, length + 5, L"\\\\?\\%ls", source);
    for (wchar_t *character = result; *character; character++)
        if (*character == L'/') *character = L'\\';
    return result;
}

static wchar_t *widen(const char *text)
{
    int length = MultiByteToWideChar(CP_ACP, 0, text, -1, NULL, 0);
    if (length <= 0) return NULL;
    wchar_t *wide = malloc((size_t)length * sizeof(wchar_t));
    if (wide) MultiByteToWideChar(CP_ACP, 0, text, -1, wide, length);
    return wide;
}

static int is_folder_w(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void print_wide_path(const wchar_t *path)
{
    if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) printf("\\\\%ls", path + 8);
    else printf("%ls", path + (wcsncmp(path, L"\\\\?\\", 4) ? 0 : 4));
}

static int playlistMode, instrumentMode;
static wchar_t *audioFiles[QUEUE_FILES];
static int audioFileCount;
static const wchar_t *playlistFolder;
static Track *stems;
static int stemCount;

static int compare_names(const void *a, const void *b)
{
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

static int scan_audio_files(const wchar_t *folder)
{
    size_t folderLength = wcslen(folder);
    wchar_t *pattern = malloc((folderLength + 3) * sizeof(wchar_t));
    if (!pattern) return -1;
    swprintf(pattern, folderLength + 3, L"%ls\\*", folder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return -1;

    do {
        const wchar_t *extension = wcsrchr(entry.cFileName, L'.');
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !extension) continue;
        if (_wcsicmp(extension, L".wav") || audioFileCount == QUEUE_FILES) continue;

        size_t nameLength = wcslen(entry.cFileName);
        wchar_t *file = malloc((folderLength + nameLength + 2) * sizeof(wchar_t));
        if (!file) continue;
        swprintf(file, folderLength + nameLength + 2, L"%ls\\%ls", folder, entry.cFileName);
        audioFiles[audioFileCount++] = file;
    } while (FindNextFileW(find, &entry));
    FindClose(find);

    qsort(audioFiles, (size_t)audioFileCount, sizeof audioFiles[0], compare_names);
    return audioFileCount;
}

static int decode_track(Track *track, const wchar_t *path)
{
    unsigned channels = 0, rate = 0;
    drwav_uint64 frames = 0;
    float *samples = drwav_open_file_and_read_pcm_frames_f32_w(path, &channels, &rate, &frames, NULL);
    if (!samples || !channels || !rate || !frames) {
        drwav_free(samples, NULL);
        log_error("Could not decode '%ls'.\n", path);
        return -1;
    }

    memset(track, 0, sizeof *track);
    track->samples = samples;
    track->frames = frames;
    track->channels = channels;
    track->rate = rate;
    return 0;
}

static int start_playlist_w(const wchar_t *folder)
{
    wchar_t *path = long_path(folder);
    if (!path) {
        log_error("Could not resolve the folder path.\n");
        return -1;
    }
    if (scan_audio_files(path) <= 0) {
        log_error("Could not find any .wav files in '%ls'.\n", path);
        free(path);
        return -1;
    }
    playlistFolder = path;

    stems = calloc((size_t)audioFileCount, sizeof *stems);
    if (!stems) {
        log_error("Out of memory for %d audio files.\n", audioFileCount);
        return -1;
    }

    for (int i = 0; i < audioFileCount; i++)
        if (decode_track(&stems[stemCount], audioFiles[i]) == 0) stemCount++;

    if (!stemCount) {
        log_error("Every audio file in the folder failed to decode.\n");
        return -1;
    }
    return 0;
}

static int start_playlist(const char *folder)
{
    wchar_t *path = widen(folder);
    if (!path) {
        log_error("Could not convert the folder path.\n");
        return -1;
    }
    int result = start_playlist_w(path);
    free(path);
    return result;
}

static float track_sample(const Track *track, int output)
{
    if (!track->samples || track->position >= track->frames) return 0.0f;

    size_t channel = (size_t)output % track->channels;
    drwav_uint64 next = track->position + 1 < track->frames ? track->position + 1 : track->position;
    const float *a = track->samples + (size_t)track->position * track->channels + channel;
    const float *b = track->samples + (size_t)next * track->channels + channel;
    return *a + (*b - *a) * (float)track->fraction;
}

static void advance_tracks(void)
{
    int finished = 1;
    for (int i = 0; i < stemCount; i++) {
        Track *track = &stems[i];
        if (track->position >= track->frames) continue;

        track->fraction += (double)track->rate / g.rate;
        drwav_uint64 whole = (drwav_uint64)track->fraction;
        track->fraction -= (double)whole;
        track->position += whole;
        if (track->position < track->frames) finished = 0;
    }

    if (!finished) return;
    for (int i = 0; i < stemCount; i++) {
        stems[i].position = 0;
        stems[i].fraction = 0;
    }
}

static void render_playlist(float *dst, int outputs, int frames)
{
    for (int i = 0; i < frames; i++) {
        for (int j = 0; j < outputs; j++) {
            float value = 0.0f;
            for (int s = 0; s < stemCount; s++) value += track_sample(&stems[s], j);
            dst[(size_t)j * frames + i] = value;
        }
        advance_tracks();
    }
}

static void shuffle(int *items, int count)
{
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int swap = items[i];
        items[i] = items[j];
        items[j] = swap;
    }
}

typedef void (*PathVisitor)(const wchar_t *path);

static void scan_tree(const wchar_t *folder, const wchar_t *extension, PathVisitor visit)
{
    size_t folderLength = wcslen(folder);
    wchar_t *pattern = malloc((folderLength + 3) * sizeof(wchar_t));
    if (!pattern) return;
    swprintf(pattern, folderLength + 3, L"%ls\\*", folder);

    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) return;

    do {
        size_t nameLength = wcslen(entry.cFileName);
        wchar_t *path = malloc((folderLength + nameLength + 2) * sizeof(wchar_t));
        if (!path) continue;
        swprintf(path, folderLength + nameLength + 2, L"%ls\\%ls", folder, entry.cFileName);

        int directory = entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        const wchar_t *dot = wcsrchr(entry.cFileName, L'.');
        if (directory && !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            scan_tree(path, extension, visit);
        else if (!directory && dot && !_wcsicmp(dot, extension)) {
            visit(path);
            continue;
        }
        free(path);
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

static Track *drumSamples;
static int drumSampleCount, drumSampleCapacity;
static int drumNoteSample[128];
static Track drumVoices[128];

static void add_sample(const wchar_t *path)
{
    if (drumSampleCount == drumSampleCapacity) {
        int capacity = drumSampleCapacity ? drumSampleCapacity * 2 : 64;
        Track *grown = realloc(drumSamples, (size_t)capacity * sizeof *grown);
        if (!grown) {
            free((void *)path);
            return;
        }
        drumSamples = grown;
        drumSampleCapacity = capacity;
    }
    if (decode_track(&drumSamples[drumSampleCount], path) == 0) drumSampleCount++;
    free((void *)path);
}

static void load_drums(const char *folder)
{
    wchar_t *wide = widen(folder);
    wchar_t *path = wide ? long_path(wide) : NULL;
    free(wide);
    if (!path || !is_folder_w(path)) {
        log_error("samples_path '%s' is not a folder; drum samples are off.\n", folder);
        free(path);
        return;
    }

    scan_tree(path, L".wav", add_sample);
    free(path);
    if (!drumSampleCount) {
        log_error("No .wav files under samples_path '%s'.\n", folder);
        return;
    }

    for (int note = 0; note < 128; note++) drumNoteSample[note] = note % drumSampleCount;
    shuffle(drumNoteSample, 128);
}

static void trigger_drum(int note)
{
    if (!drumSampleCount) return;
    drumVoices[note] = drumSamples[drumNoteSample[note]];
    drumVoices[note].position = 0;
    drumVoices[note].fraction = 0;
}

static void render_drums(float *dst, int outputs, int frames)
{
    for (int i = 0; i < frames; i++) {
        for (int note = 0; note < 128; note++) {
            Track *voice = &drumVoices[note];
            if (!voice->samples || voice->position >= voice->frames) continue;

            for (int j = 0; j < outputs; j++)
                dst[(size_t)j * frames + i] += track_sample(voice, j);

            voice->fraction += (double)voice->rate / g.rate;
            drwav_uint64 whole = (drwav_uint64)voice->fraction;
            voice->fraction -= (double)whole;
            voice->position += whole;
        }
    }
}

static HMODULE vstLibraries[VST_MAX_PLUGINS];
static int vstLibraryCount, vstActiveLibrary = -1, vstOutputCount, vstProgram;
static float *vstZeros, *vstOutputMemory;
static float **vstInputs, **vstOutputs;

static intptr_t host_callback(AEffect *effect, int32_t opcode, int32_t index, intptr_t value,
                              void *ptr, float opt)
{
    (void)effect;
    (void)index;
    (void)value;
    (void)opt;

    switch (opcode) {
    case audioMasterVersion: return 2400;
    case audioMasterGetSampleRate: return (intptr_t)g.rate;
    case audioMasterGetBlockSize: return g.bufferSize;
    case audioMasterGetCurrentProcessLevel: return 2;
    case audioMasterGetVendorVersion: return 1;
    case audioMasterGetVendorString:
    case audioMasterGetProductString:
        if (ptr) strcpy((char *)ptr, "restless_virtuoso");
        return 1;
    default: return 0;
    }
}

static AEffect *create_effect(HMODULE library)
{
    AEffect *(*entry)(VstHostCallbackProc) = (void *)GetProcAddress(library, "VSTPluginMain");
    if (!entry) entry = (void *)GetProcAddress(library, "main");
    if (!entry) return NULL;

    AEffect *effect = entry(host_callback);
    if (!effect || effect->magic != kVstEffectMagic) return NULL;
    return effect;
}

static void vst_free_buffers(void)
{
    free(vstZeros);
    free(vstInputs);
    free(vstOutputMemory);
    free(vstOutputs);
    vstZeros = NULL;
    vstInputs = NULL;
    vstOutputMemory = NULL;
    vstOutputs = NULL;
}

static int vst_allocate(int frames)
{
    int slots = vstEffect->numInputs > 2 ? vstEffect->numInputs : 2;

    vstZeros = calloc((size_t)frames, sizeof(float));
    vstInputs = malloc((size_t)slots * sizeof *vstInputs);
    vstOutputs = malloc((size_t)vstOutputCount * sizeof *vstOutputs);
    vstOutputMemory = malloc((size_t)vstOutputCount * frames * sizeof *vstOutputMemory);
    if (!vstZeros || !vstInputs || !vstOutputs || !vstOutputMemory) {
        vst_free_buffers();
        return -1;
    }

    for (int i = 0; i < slots; i++) vstInputs[i] = vstZeros;
    for (int j = 0; j < vstOutputCount; j++) vstOutputs[j] = vstOutputMemory + (size_t)j * frames;
    return 0;
}

static int vst_start(void)
{
    if (!vstEffect) return 0;

    vstEffect->dispatcher(vstEffect, effSetSampleRate, 0, 0, NULL, (float)g.rate);
    vstEffect->dispatcher(vstEffect, effSetBlockSize, 0, g.bufferSize, NULL, 0);
    vstEffect->dispatcher(vstEffect, effMainsChanged, 0, 1, NULL, 0);
    if (vst_allocate((int)g.bufferSize) != 0) {
        log_error("Out of memory for the VST buffers.\n");
        return -1;
    }
    return 0;
}

static void vst_stop(void)
{
    if (!vstEffect) return;
    vstEffect->dispatcher(vstEffect, effMainsChanged, 0, 0, NULL, 0);
    vst_free_buffers();
}

static int start_vst(int library)
{
    AEffect *effect = create_effect(vstLibraries[library]);
    if (!effect) return -1;

    if (!effect->dispatcher || (!(effect->flags & effFlagsIsSynth) && effect->numInputs > 0) ||
        effect->numOutputs < 1 || effect->numOutputs > VST_MAX_OUTPUTS ||
        (!effect->processReplacing && !effect->process)) {
        effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
        return -1;
    }

    effect->dispatcher(effect, effOpen, 0, 0, NULL, 0);
    vstEffect = effect;
    vstActiveLibrary = library;
    vstOutputCount = effect->numOutputs;
    vstProgram = effect->numPrograms > 1 ? rand() % effect->numPrograms : 0;
    if (effect->numPrograms > 1)
        effect->dispatcher(effect, effSetProgram, 0, vstProgram, NULL, 0);

    if (vst_start() != 0) {
        vst_stop();
        effect->dispatcher(effect, effClose, 0, 0, NULL, 0);
        vstEffect = NULL;
        vstActiveLibrary = -1;
        return -1;
    }
    return 0;
}

static void add_vst(const wchar_t *path)
{
    if (vstLibraryCount == VST_MAX_PLUGINS) {
        free((void *)path);
        return;
    }

    HMODULE library = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!library) log_error("Could not load '%ls'.\n", path);
    else if (GetProcAddress(library, "VSTPluginMain") || GetProcAddress(library, "main"))
        vstLibraries[vstLibraryCount++] = library;
    else FreeLibrary(library);

    free((void *)path);
}

static void load_vsts(const char *folder)
{
    wchar_t *wide = widen(folder);
    wchar_t *path = wide ? long_path(wide) : NULL;
    free(wide);
    if (!path || !is_folder_w(path)) {
        log_error("vsts_path '%s' is not a folder; no VST is used.\n", folder);
        free(path);
        return;
    }

    scan_tree(path, L".dll", add_vst);
    free(path);
    if (!vstLibraryCount) {
        log_error("No VST plugins under vsts_path '%s'.\n", folder);
        return;
    }

    int order[VST_MAX_PLUGINS];
    for (int i = 0; i < vstLibraryCount; i++) order[i] = i;
    shuffle(order, vstLibraryCount);

    for (int i = 0; i < vstLibraryCount; i++)
        if (start_vst(order[i]) == 0) return;

    log_error("%d plugins under vsts_path '%s', but none could be started.\n",
              vstLibraryCount, folder);
}

static void render_vst(float *dst, int outputs, int frames)
{
    if (!vstEffect) return;

    if (midiEvents.numEvents)
        vstEffect->dispatcher(vstEffect, effProcessEvents, 0, 0, (VstEvents *)&midiEvents, 0);

    if (vstEffect->processReplacing) {
        vstEffect->processReplacing(vstEffect, vstInputs, vstOutputs, frames);
    } else {
        memset(vstOutputMemory, 0, (size_t)vstOutputCount * frames * sizeof *vstOutputMemory);
        vstEffect->process(vstEffect, vstInputs, vstOutputs, frames);
    }

    for (int j = 0; j < outputs; j++) {
        const float *source = vstOutputs[j % vstOutputCount];
        for (int i = 0; i < frames; i++) dst[(size_t)j * frames + i] += source[i];
    }
}

static void choose_sound(int samplesRatio)
{
    g.samplesSound = drumSampleCount && (!vstEffect || rand() % 100 < samplesRatio);
}

static void render_instruments(float *dst, int outputs, int frames)
{
    memset(dst, 0, (size_t)outputs * frames * sizeof *dst);
    if (g.samplesSound) render_drums(dst, outputs, frames);
    else render_vst(dst, outputs, frames);
}

static void close_vst_effect(void)
{
    vst_stop();
    if (vstEffect) {
        vstEffect->dispatcher(vstEffect, effClose, 0, 0, NULL, 0);
        vstEffect = NULL;
    }
    vstActiveLibrary = -1;
}

static void new_vst_sound(void)
{
    if (!vstLibraryCount) return;

    if (g.started) {
        g.asio->vtable->stop(g.asio);
        g.started = 0;
    }

    int previous = vstActiveLibrary;
    close_vst_effect();

    int order[VST_MAX_PLUGINS], count = 0;
    for (int i = 0; i < vstLibraryCount; i++)
        if (i != previous) order[count++] = i;
    shuffle(order, count);

    for (int i = 0; i < count; i++)
        if (start_vst(order[i]) == 0) break;
    if (!vstEffect && previous >= 0) start_vst(previous);

    if (g.asio->vtable->start(g.asio) != ASE_OK) {
        log_error("ASIOStart failed after changing the VST.\n");
        InterlockedExchange(&g.quit, 1);
    } else g.started = 1;
}

static void print_sound(const char *prefix)
{
    if (g.samplesSound) printf("  %s%d drum samples\n", prefix, drumSampleCount);
    else if (vstEffect) {
        char name[64] = "";
        vstEffect->dispatcher(vstEffect, effGetEffectName, 0, 0, name, 0);
        name[63] = 0;
        if (vstEffect->numPrograms > 0)
            printf("  %sVST %s, preset %d/%d\n", prefix, name[0] ? name : "plugin", vstProgram + 1,
                   vstEffect->numPrograms);
        else
            printf("  %sVST %s, default preset\n", prefix, name[0] ? name : "plugin");
    }
    fflush(stdout);
}

static void new_sound(void)
{
    choose_sound(g.samplesRatio);
    if (g.samplesSound) shuffle(drumNoteSample, 128);
    else new_vst_sound();

    printf("\n");
    print_sound("new sound: ");
    g.lineWidth = 0;
}

static void close_vst(void)
{
    close_vst_effect();
    for (int i = 0; i < vstLibraryCount; i++) FreeLibrary(vstLibraries[i]);
    vstLibraryCount = 0;
}

/* The Windows master volume, through Core Audio's endpoint volume. The SDK headers are
   C++ only, so only the vtable slots up to GetMasterVolumeLevelScalar are declared. */

static const GUID enumeratorClass = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID enumeratorInterface = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID endpointVolumeInterface = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    void *enumAudioEndpoints;
    HRESULT (STDMETHODCALLTYPE *GetDefaultAudioEndpoint)(void *, int, int, void **);
} EnumeratorVtbl;

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    HRESULT (STDMETHODCALLTYPE *Activate)(void *, const GUID *, DWORD, void *, void **);
} DeviceVtbl;

typedef struct {
    void *queryInterface, *addRef;
    ULONG (STDMETHODCALLTYPE *Release)(void *);
    void *registerControlChangeNotify, *unregisterControlChangeNotify, *getChannelCount,
         *setMasterVolumeLevel, *setMasterVolumeLevelScalar, *getMasterVolumeLevel;
    HRESULT (STDMETHODCALLTYPE *GetMasterVolumeLevelScalar)(void *, float *);
} EndpointVolumeVtbl;

static void *deviceEnumerator;

static void open_master_volume(void)
{
    if (FAILED(CoCreateInstance(&enumeratorClass, NULL, CLSCTX_ALL, &enumeratorInterface, &deviceEnumerator)))
        deviceEnumerator = NULL;
}

static float read_master_volume(void)
{
    if (!deviceEnumerator) return 1.0f;

    void *device = NULL, *endpoint = NULL;
    float volume = 1.0f;
    EnumeratorVtbl *enumerator = *(EnumeratorVtbl **)deviceEnumerator;

    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(deviceEnumerator, 0 /* eRender */, 1 /* eMultimedia */, &device)) && device) {
        DeviceVtbl *deviceVtable = *(DeviceVtbl **)device;
        if (SUCCEEDED(deviceVtable->Activate(device, &endpointVolumeInterface, CLSCTX_ALL, NULL, &endpoint)) && endpoint) {
            EndpointVolumeVtbl *volumeVtable = *(EndpointVolumeVtbl **)endpoint;
            if (FAILED(volumeVtable->GetMasterVolumeLevelScalar(endpoint, &volume))) volume = 1.0f;
            volumeVtable->Release(endpoint);
        }
        deviceVtable->Release(device);
    }
    return volume;
}

static void process_buffer(int32_t index)
{
    const int frames = (int)g.bufferSize;
    const float gain = g.mute ? 0.0f : g.gain;
    const float synthGain = g.mute ? 0.0f : 1.0f;
    const float volume = g.volume;

    for (int c = 0; c < g.nIn; c++) {
        float *samples = g.inputFloats + (size_t)c * frames;
        sample_to_float(g.bufferInfo[c].buffers[index], g.inFormat[c], samples, frames);

        float peak = 0.0f;
        for (int i = 0; i < frames; i++) {
            float value = fabsf(samples[i]);
            if (value > peak) peak = value;
        }
        float held = g.peak[c] * 0.97f;
        g.peak[c] = peak > held ? peak : held;
    }

    memset(g.synthFloats, 0, (size_t)frames * sizeof(float));
    if (g.synthEnabled || instrumentMode) drain_midi();
    if (g.synthEnabled) synth_render(g.synthFloats, frames);

    if (playlistMode) render_playlist(g.fileFloats, g.nOut, frames);
    else if (instrumentMode) render_instruments(g.fileFloats, g.nOut, frames);

    for (int j = 0; j < g.nOut; j++) {
        int source = g.nIn ? j % g.nIn : -1;
        const float *input = source >= 0 ? g.inputFloats + (size_t)source * frames : NULL;
        const float *music = g.fileFloats ? g.fileFloats + (size_t)j * frames : NULL;
        void *output = g.bufferInfo[g.nIn + j].buffers[index];

        for (int i = 0; i < frames; i++) {
            float value = (input ? input[i] * gain : 0.0f) + g.synthFloats[i] * synthGain;
            if (music) value += music[i];
            value *= volume;
            g.mixFloats[i] = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
        }
        sample_from_float(g.mixFloats, output, g.outFormat[j], frames);
    }
}

static void on_buffer_switch(int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->vtable->outputReady(g.asio);
}

static ASIOTime *on_buffer_switch_time_info(ASIOTime *params, int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->vtable->outputReady(g.asio);
    return params;
}

static void on_sample_rate_changed(ASIOSampleRate rate)
{
    if (rate > 0) InterlockedExchange(&g.reset, 1);
}

static int32_t on_asio_message(int32_t selector, int32_t value, void *message, double *opt)
{
    (void)message;
    (void)opt;

    switch (selector) {
    case kAsioSelectorSupported:
        return value == kAsioEngineVersion || value == kAsioResetRequest ||
               value == kAsioBufferSizeChange || value == kAsioResyncRequest ||
               value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo ||
               value == kAsioSupportsTimeCode;
    case kAsioEngineVersion:
        return 2;
    case kAsioResetRequest:
    case kAsioBufferSizeChange:
        InterlockedExchange(&g.reset, 1);
        return 1;
    case kAsioResyncRequest:
    case kAsioLatenciesChanged:
        return 1;
    case kAsioSupportsTimeInfo:
        return 1;
    case kAsioSupportsTimeCode:
        return 0;
    default:
        return 0;
    }
}

static void CALLBACK midi_in_proc(HMIDIIN handle, UINT message, DWORD_PTR instance,
                                  DWORD_PTR param1, DWORD_PTR param2)
{
    (void)handle;
    (void)instance;
    (void)param2;

    if (message != MIM_DATA) return;

    uint32_t data = (uint32_t)param1;
    int channel = (int)(data & 0x0f) + 1;
    if (g.midiChannel && channel != g.midiChannel) return;

    if (g.midiStream)
        log_line("MIDI ch %d  %02X %02X %02X", channel, (int)(data & 0xff),
                 (int)(data >> 8) & 0x7f, (int)(data >> 16) & 0x7f);
    if (g.midiOut) midiOutShortMsg(g.midiOut, data);
    midi_push(data);
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g.quit, 1);
    return TRUE;
}

static LRESULT CALLBACK wnd_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND create_window(HINSTANCE instance)
{
    WNDCLASSA windowClass;
    memset(&windowClass, 0, sizeof windowClass);
    windowClass.lpfnWndProc = wnd_proc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = "RestlessVirtuosoWindow";
    RegisterClassA(&windowClass);
    return CreateWindowExA(0, windowClass.lpszClassName, "restless_virtuoso", 0, 0, 0, 0, 0,
                           NULL, NULL, instance, NULL);
}

static void pump_messages(void)
{
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

static int list_drivers(char names[][128], int max)
{
    HKEY root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &root) != ERROR_SUCCESS)
        return 0;

    int count = 0;
    for (DWORD index = 0; count < max; index++) {
        char name[128];
        DWORD length = sizeof name;
        if (RegEnumKeyExA(root, index, name, &length, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;

        length = (DWORD)strlen(name);
        if (length > sizeof names[count] - 1) length = sizeof names[count] - 1;
        memcpy(names[count], name, length);
        names[count][length] = 0;
        count++;
    }

    RegCloseKey(root);
    return count;
}

static int driver_clsid(const char *name, CLSID *clsid)
{
    char path[512];
    snprintf(path, sizeof path, "SOFTWARE\\ASIO\\%s", name);

    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return -1;

    char text[128] = {0};
    DWORD size = sizeof text - 1;
    DWORD type = 0;
    LONG result = RegQueryValueExA(key, "CLSID", NULL, &type, (LPBYTE)text, &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ) return -1;

    wchar_t wide[128];
    if (!MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 128)) return -1;
    return SUCCEEDED(CLSIDFromString(wide, clsid)) ? 0 : -1;
}

static const char *resolve_driver(const char *argument, char names[][128], int count)
{
    if (count == 0) return NULL;
    if (!argument) return names[0];

    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < count ? names[index] : NULL;

    for (int i = 0; i < count; i++)
        if (!_stricmp(names[i], argument)) return names[i];
    return NULL;
}

static int open_driver(const char *name, HWND window)
{
    CLSID clsid;
    if (driver_clsid(name, &clsid) != 0) {
        log_error("ASIO driver '%s' is not registered.\n", name);
        return -1;
    }

    log_trace("creating driver object\n");
    HRESULT result = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void **)&g.asio);
    if (FAILED(result) || !g.asio) {
        log_error("Could not create the ASIO driver object (0x%08lx).\n", (unsigned long)result);
        g.asio = NULL;
        return -1;
    }
    log_trace("driver object 0x%08lx\n", (unsigned long)result);

    log_trace("initializing driver\n");
    HANDLE watchdog = errorLog ? CreateThread(NULL, 0, init_watchdog, NULL, 0, NULL) : NULL;
    ASIOBool initialized = g.asio->vtable->init(g.asio, window);
    InterlockedExchange(&initReturned, 1);
    if (watchdog) CloseHandle(watchdog);
    log_trace("init returned %d\n", initialized);
    if (!initialized) {
        char error[124] = {0};
        g.asio->vtable->getErrorMessage(g.asio, error);
        error[123] = 0;
        log_error("ASIOInit failed: %s\n", error[0] ? error : "no error message from the driver");
        g.asio->vtable->Release(g.asio);
        g.asio = NULL;
        return -1;
    }

    g.asio->vtable->getDriverName(g.asio, g.driverName);
    g.driverName[63] = 0;
    log_trace("driver name '%s'\n", g.driverName);

    log_trace("querying channels\n");
    ASIOError channels = g.asio->vtable->getChannels(g.asio, &g.inCount, &g.outCount);
    log_trace("channels %s, in %d out %d\n", asio_error_text(channels), g.inCount, g.outCount);
    if (channels != ASE_OK) {
        log_error("ASIOGetChannels failed.\n");
        return -1;
    }

    g.postOutput = g.asio->vtable->outputReady(g.asio) == ASE_OK;
    return 0;
}

static void print_selection(const int *channels, int count)
{
    for (int i = 0; i < count; i++) printf("%s%d", i ? "," : "", channels[i] + 1);
    if (!count) printf("none");
}

static int select_channels(const char *spec, int available, int *channels, int *count,
                           int defaultCount, const char *what)
{
    if (spec && !_stricmp(spec, "none")) {
        *count = 0;
        return 0;
    }

    if (!spec) {
        *count = available < defaultCount ? available : defaultCount;
        for (int i = 0; i < *count; i++) channels[i] = i;
        return 0;
    }

    int parsed = parse_channels(spec, available, channels, MAX_SELECTED);
    if (parsed < 0) {
        log_error("Invalid %s channel list '%s' (the driver has %d).\n", what, spec, available);
        return -1;
    }
    *count = parsed;
    return 0;
}

static int setup_rate(int requested)
{
    if (requested > 0) {
        if (g.asio->vtable->canSampleRate(g.asio, (ASIOSampleRate)requested) != ASE_OK) {
            log_error("The driver does not support %d Hz.\n", requested);
            return -1;
        }
        ASIOError error = g.asio->vtable->setSampleRate(g.asio, (ASIOSampleRate)requested);
        if (error != ASE_OK) {
            log_error("ASIOSetSampleRate failed (%s).\n", asio_error_text(error));
            return -1;
        }
    }

    ASIOSampleRate rate = 0;
    g.asio->vtable->getSampleRate(g.asio, &rate);
    if (rate <= 0) rate = requested > 0 ? requested : 48000;
    g.rate = rate;
    synth_set_rate((int)rate);
    return 0;
}

static int setup_buffer(int requested)
{
    int32_t minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    ASIOError error = g.asio->vtable->getBufferSize(g.asio, &minimum, &maximum, &preferred, &granularity);
    if (error != ASE_OK) {
        log_error("ASIOGetBufferSize failed (%s).\n", asio_error_text(error));
        return -1;
    }

    int32_t size = preferred;
    if (requested > 0) {
        if (granularity <= 0) {
            size = requested == preferred || requested == minimum || requested == maximum ? requested : preferred;
        } else {
            size = minimum + ((requested - minimum + granularity / 2) / granularity) * granularity;
            if (size < minimum) size = minimum;
            if (size > maximum) size = maximum;
        }
        if (size != requested)
            log_line("note: buffer size %d is not available, using %d frames", requested, size);
    }

    g.bufferSize = size;
    return 0;
}

static SampleFormat channel_format(int channel, ASIOBool isInput)
{
    ASIOChannelInfo info;
    memset(&info, 0, sizeof info);
    info.channel = channel;
    info.isInput = isInput;

    if (g.asio->vtable->getChannelInfo(g.asio, &info) != ASE_OK) {
        log_error("ASIOGetChannelInfo failed for %s %d.\n", isInput ? "input" : "output", channel + 1);
        return SAMPLE_UNSUPPORTED;
    }

    SampleFormat format = sample_format_from_asio(info.type);
    if (format == SAMPLE_UNSUPPORTED)
        log_error("%s %d uses an unsupported sample format (%s).\n",
                isInput ? "Input" : "Output", channel + 1, sample_format_name(info.type));
    return format;
}

static void free_buffers(void)
{
    free(g.inputFloats);
    free(g.fileFloats);
    free(g.synthFloats);
    free(g.mixFloats);
    g.inputFloats = g.fileFloats = g.synthFloats = g.mixFloats = NULL;
}

static int create_buffers(void)
{
    for (int c = 0; c < g.nIn; c++) {
        g.inFormat[c] = channel_format(g.inCh[c], ASIOTrue);
        if (g.inFormat[c] == SAMPLE_UNSUPPORTED) return -1;
    }
    for (int j = 0; j < g.nOut; j++) {
        g.outFormat[j] = channel_format(g.outCh[j], ASIOFalse);
        if (g.outFormat[j] == SAMPLE_UNSUPPORTED) return -1;
    }

    size_t frames = (size_t)g.bufferSize;
    free_buffers();
    g.inputFloats = g.nIn ? malloc(frames * g.nIn * sizeof(float)) : NULL;
    g.fileFloats = (playlistMode || instrumentMode) ? malloc(frames * g.nOut * sizeof(float)) : NULL;
    g.synthFloats = malloc(frames * sizeof(float));
    g.mixFloats = malloc(frames * sizeof(float));
    if ((g.nIn && !g.inputFloats) || ((playlistMode || instrumentMode) && !g.fileFloats) ||
        !g.synthFloats || !g.mixFloats) {
        log_error("Out of memory for %d-frame buffers.\n", g.bufferSize);
        free_buffers();
        return -1;
    }

    memset(g.bufferInfo, 0, sizeof g.bufferInfo);
    for (int c = 0; c < g.nIn; c++) {
        g.bufferInfo[c].isInput = ASIOTrue;
        g.bufferInfo[c].channelNum = g.inCh[c];
    }
    for (int j = 0; j < g.nOut; j++) {
        g.bufferInfo[g.nIn + j].isInput = ASIOFalse;
        g.bufferInfo[g.nIn + j].channelNum = g.outCh[j];
    }

    ASIOError error = g.asio->vtable->createBuffers(g.asio, g.bufferInfo, g.nIn + g.nOut,
                                            g.bufferSize, &callbacks);
    if (error != ASE_OK) {
        log_error("ASIOCreateBuffers failed (%s).\n", asio_error_text(error));
        free_buffers();
        return -1;
    }
    g.buffersCreated = 1;

    int32_t latencyIn = 0, latencyOut = 0;
    g.asio->vtable->getLatencies(g.asio, &latencyIn, &latencyOut);
    g.latencyIn = latencyIn;
    g.latencyOut = latencyOut;
    return 0;
}

static const char *midi_error_text(MMRESULT result)
{
    static char text[256];
    if (midiInGetErrorTextA(result, text, sizeof text) == MMSYSERR_NOERROR) return text;

    switch (result) {
    case MMSYSERR_ALLOCATED: return "the device is already in use";
    case MMSYSERR_BADDEVICEID: return "invalid device index";
    case MMSYSERR_NODRIVER: return "the device driver is not installed";
    case MMSYSERR_NOTENABLED: return "the device driver is disabled";
    case MMSYSERR_INVALPARAM: return "invalid parameter";
    default: return "unknown MIDI error";
    }
}

static const char *midi_error_hint(MMRESULT result)
{
    switch (result) {
    case MMSYSERR_ALLOCATED: return "another program is using the device; close it and retry";
    case MMSYSERR_NODRIVER: return "the driver failed to load; reconnect the device or restart Windows";
    case MMSYSERR_NOTENABLED: return "the driver is disabled in Device Manager";
    case MMSYSERR_BADDEVICEID: return "the device index is out of date; the device list changed";
    case MMSYSERR_INVALPARAM: return "the device vanished after it was listed; reconnect it";
    case MMSYSERR_NOMEM: return "the port could not be allocated; another program may hold it, or the device is stuck";
    case MMSYSERR_NOTSUPPORTED: return "the driver rejects this open mode";
    default: return "the code above is the only detail Windows reports";
    }
}

static const char *midi_in_label(int index, char *label, size_t size)
{
    MIDIINCAPS caps;
    label[0] = 0;
    if (midiInGetDevCaps((UINT)index, &caps, sizeof caps) == MMSYSERR_NOERROR)
        snprintf(label, size, " (%s)", caps.szPname);
    return label;
}

static void diagnose_midi_input(int index, MMRESULT failure)
{
    UINT count = midiInGetNumDevs();
    log_error("  %u MIDI inputs are listed now\n", count);

    if ((UINT)index >= count)
        log_error("  input %d is no longer listed; it was removed after the device list was read\n", index);
    else {
        MIDIINCAPS caps;
        if (midiInGetDevCaps((UINT)index, &caps, sizeof caps) != MMSYSERR_NOERROR)
            log_error("  input %d no longer reports any details\n", index);
        else
            log_error("  input %d is '%s', driver %u.%u, manufacturer 0x%04x, product 0x%04x\n",
                      index, caps.szPname, (unsigned)(caps.vDriverVersion >> 8),
                      (unsigned)(caps.vDriverVersion & 0xff), (unsigned)caps.wMid, (unsigned)caps.wPid);
    }

    if (failure == MMSYSERR_NODRIVER || failure == MMSYSERR_NOTENABLED) return;
    if ((UINT)index >= count) return;

    HMIDIIN probe = NULL;
    MMRESULT result = midiInOpen(&probe, (UINT)index, 0, 0, CALLBACK_NULL);
    if (result == MMSYSERR_NOERROR) {
        midiInClose(probe);
        log_error("  it opens without a callback, so the driver is rejecting callback mode\n");
    } else
        log_error("  it also fails to open without a callback: %s (error %u)\n",
                  midi_error_text(result), (unsigned)result);
}

static int resolve_midi_in(const char *argument)
{
    UINT count = midiInGetNumDevs();
    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < (long)count ? (int)index : -1;

    for (UINT i = 0; i < count; i++) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !_stricmp(caps.szPname, argument))
            return (int)i;
    }
    return -1;
}

static int resolve_midi_out(const char *argument)
{
    UINT count = midiOutGetNumDevs();
    char *end;
    long index = strtol(argument, &end, 10);
    if (*argument && !*end) return index >= 0 && index < (long)count ? (int)index : -1;

    for (UINT i = 0; i < count; i++) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !_stricmp(caps.szPname, argument))
            return (int)i;
    }
    return -1;
}

static int open_midi_input(const char *spec)
{
    int index = resolve_midi_in(spec);
    if (index < 0) {
        log_error("MIDI input '%s' not found (use --list).\n", spec);
        return -1;
    }

    char label[128];
    midi_in_label(index, label, sizeof label);

    MMRESULT result = MMSYSERR_NOERROR;
    for (int attempt = 1; attempt <= 3; attempt++) {
        g.midiIn = NULL;
        result = midiInOpen(&g.midiIn, (UINT)index, (DWORD_PTR)midi_in_proc, 0, CALLBACK_FUNCTION);
        if (result == MMSYSERR_NOERROR) break;
        log_error("MIDI input %d%s failed to open on attempt %d: %s (error %u).\n",
                  index, label, attempt, midi_error_text(result), (unsigned)result);
        Sleep(250);
    }
    if (result != MMSYSERR_NOERROR) {
        log_error("  likely cause: %s\n", midi_error_hint(result));
        diagnose_midi_input(index, result);
        return -1;
    }

    result = midiInStart(g.midiIn);
    if (result != MMSYSERR_NOERROR) {
        log_error("MIDI input %d%s opened but failed to start: %s (error %u).\n",
                  index, label, midi_error_text(result), (unsigned)result);
        log_error("  likely cause: %s\n", midi_error_hint(result));
        midiInClose(g.midiIn);
        g.midiIn = NULL;
        return -1;
    }
    g.midiInIndex = index;
    return 0;
}

static int open_midi_output(const char *spec)
{
    int index = resolve_midi_out(spec);
    if (index < 0) {
        log_error("MIDI output '%s' not found (use --list).\n", spec);
        return -1;
    }

    MIDIOUTCAPS caps;
    char label[128] = "";
    if (midiOutGetDevCaps((UINT)index, &caps, sizeof caps) == MMSYSERR_NOERROR)
        snprintf(label, sizeof label, " (%s)", caps.szPname);

    MMRESULT result = midiOutOpen(&g.midiOut, (UINT)index, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        log_error("Could not open MIDI output %d%s: %s (error %u).\n",
                  index, label, midi_error_text(result), (unsigned)result);
        log_error("  likely cause: %s\n", midi_error_hint(result));
        return -1;
    }
    g.midiOutIndex = index;
    return 0;
}

static void list_devices(void)
{
    char names[64][128];
    int drivers = list_drivers(names, 64);

    printf("ASIO drivers:\n");
    for (int i = 0; i < drivers; i++) printf("  %d  %s\n", i, names[i]);
    if (!drivers) printf("  (none)\n");

    printf("MIDI inputs:\n");
    UINT inputs = midiInGetNumDevs();
    for (UINT i = 0; i < inputs; i++) {
        MIDIINCAPS caps;
        MMRESULT result = midiInGetDevCaps(i, &caps, sizeof caps);
        if (result == MMSYSERR_NOERROR) printf("  %u  %s\n", i, caps.szPname);
        else printf("  %u  (no details: %s)\n", i, midi_error_text(result));
    }
    if (!inputs) printf("  (none)\n");
}

static void adjust_gain(float delta)
{
    g.gainDb += delta;
    if (g.gainDb < -60.0f) g.gainDb = -60.0f;
    if (g.gainDb > 24.0f) g.gainDb = 24.0f;
    g.gain = powf(10.0f, g.gainDb / 20.0f);
}

static HANDLE consoleInput;
static DWORD consoleMode;
static int consoleModeSet;
static COORD soundButton;

static void init_console_input(void)
{
    consoleInput = GetStdHandle(STD_INPUT_HANDLE);
    if (consoleInput == INVALID_HANDLE_VALUE || !GetConsoleMode(consoleInput, &consoleMode)) {
        consoleInput = NULL;
        return;
    }
    consoleModeSet = SetConsoleMode(consoleInput,
        ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT | ENABLE_PROCESSED_INPUT);
}

static void draw_sound_button(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    fflush(stdout);
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        soundButton = info.dwCursorPosition;

    printf("+-----------+\n"
           "| new sound |\n"
           "+-----------+\n");
    fflush(stdout);
}

static void read_console_input(void)
{
    if (!consoleInput) return;

    DWORD available = 0;
    while (GetNumberOfConsoleInputEvents(consoleInput, &available) && available) {
        INPUT_RECORD records[8];
        DWORD read = 0, wanted = available < 8 ? available : 8;
        if (!ReadConsoleInputA(consoleInput, records, wanted, &read) || !read) return;

        for (DWORD i = 0; i < read; i++) {
            INPUT_RECORD *record = &records[i];
            if (record->EventType == KEY_EVENT) {
                KEY_EVENT_RECORD *key = &record->Event.KeyEvent;
                if (!key->bKeyDown) continue;
                int character = key->uChar.AsciiChar;
                if (character == 'q' || character == 'Q' || character == 27) InterlockedExchange(&g.quit, 1);
                else if (character == 'm' || character == 'M') InterlockedExchange(&g.mute, !g.mute);
                else if (character == '+' || character == '=') adjust_gain(1.0f);
                else if (character == '-' || character == '_') adjust_gain(-1.0f);
                else if (character == 'p' || character == 'P') g.asio->vtable->controlPanel(g.asio);
            } else if (record->EventType == MOUSE_EVENT && instrumentMode) {
                MOUSE_EVENT_RECORD *mouse = &record->Event.MouseEvent;
                if (mouse->dwEventFlags || !(mouse->dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED)) continue;
                if (mouse->dwMousePosition.X < soundButton.X || mouse->dwMousePosition.X > soundButton.X + 12 ||
                    mouse->dwMousePosition.Y < soundButton.Y || mouse->dwMousePosition.Y > soundButton.Y + 2)
                    continue;
                new_sound();
            }
        }
    }
}

static void draw_meters(void)
{
    char line[512];
    int length = snprintf(line, sizeof line, "%s", g.mute ? "MUTED  " : "");
    int shown = g.nIn < MAX_METERS ? g.nIn : MAX_METERS;

    for (int c = 0; c < shown; c++) {
        float peak = g.peak[c];
        float db = peak > 0.000001f ? 20.0f * log10f(peak) : -60.0f;
        int bars = (int)((db + 60.0f) / 6.0f);
        if (bars < 0) bars = 0;
        if (bars > 10) bars = 10;

        char bar[11];
        memset(bar, '-', 10);
        bar[10] = 0;
        memset(bar, '#', (size_t)bars);

        length += snprintf(line + length, sizeof line - (size_t)length, "%2d [%s] %6.1f  ", g.inCh[c] + 1, bar, db);
        if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
    }

    if (g.nIn > shown && length < (int)sizeof line - 4)
        length += snprintf(line + length, sizeof line - (size_t)length, "...");

    if (length > (int)sizeof line - 1) length = (int)sizeof line - 1;
    if (length < g.lineWidth) {
        memset(line + length, ' ', (size_t)(g.lineWidth - length));
        length = g.lineWidth;
        line[length] = 0;
    }
    g.lineWidth = length;

    printf("\r%s", line);
    fflush(stdout);
}

static void shutdown_monitor(void)
{
    if (g.midiIn) {
        midiInStop(g.midiIn);
        midiInClose(g.midiIn);
        g.midiIn = NULL;
    }
    if (g.midiOut) {
        midiOutClose(g.midiOut);
        g.midiOut = NULL;
    }
    if (g.asio) {
        if (g.started) g.asio->vtable->stop(g.asio);
        if (g.buffersCreated) g.asio->vtable->disposeBuffers(g.asio);
        g.asio->vtable->Release(g.asio);
        g.asio = NULL;
    }
    if (deviceEnumerator) {
        (*(EnumeratorVtbl **)deviceEnumerator)->Release(deviceEnumerator);
        deviceEnumerator = NULL;
    }
    free_buffers();
    for (int i = 0; i < stemCount; i++) drwav_free(stems[i].samples, NULL);
    free(stems);
    for (int i = 0; i < drumSampleCount; i++) drwav_free(drumSamples[i].samples, NULL);
    free(drumSamples);
    close_vst();
    if (consoleModeSet) SetConsoleMode(consoleInput, consoleMode);
    consoleModeSet = 0;
}

static int stream_midi(const char *device, int channel)
{
    if (channel > 16) {
        log_error("MIDI channel %d is out of range (1-16).\n", channel);
        return 1;
    }
    g.midiChannel = channel;
    g.midiStream = 1;
    if (open_midi_input(device) != 0) return 1;

    if (channel) printf("streaming MIDI input '%s', channel %d\n", device, channel);
    else printf("streaming MIDI input '%s', all channels\n", device);

    while (!g.quit) {
        while (_kbhit()) {
            int key = _getch();
            if (key == 'q' || key == 'Q' || key == 27) InterlockedExchange(&g.quit, 1);
        }
        Sleep(5);
    }

    shutdown_monitor();
    return 0;
}

static void usage(void)
{
    printf(
        "restless_virtuoso - play ASIO inputs on ASIO outputs\n"
        "\n"
        "Usage:\n"
        "  restless_virtuoso --list\n"
        "  restless_virtuoso [options]\n"
        "\n"
        "Options:\n"
        "  -d, --driver NAME|INDEX  ASIO driver (default: " DEFAULT_DRIVER ", else first installed)\n"
        "  -i, --in LIST            input channels, 1-based: 1,2 or 1-4 (default 1,2; \"none\" for MIDI only)\n"
        "  -o, --out LIST           output channels (default 1,2)\n"
        "  -g, --gain DB            input gain (default 0)\n"
        "      --rate HZ            sample rate (default: the driver's current rate)\n"
        "      --buffer FRAMES      ASIO buffer size (default: the driver's preferred size)\n"
        "      --midi-in DEVICE     MIDI input by index or name\n"
        "      --midi-out DEVICE    forward MIDI input to this MIDI output\n"
        "      --no-synth           disable the built-in sine synth for MIDI input\n"
        "  -l, --list               list ASIO drivers and MIDI devices\n"
        "  -h, --help               show this help\n"
        "\n"
        "While running: m mute, +/- gain, p ASIO control panel, q quit.\n");
}

static int is_json_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !_stricmp(dot, ".json");
}

static int is_folder(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static const char *json_value(const char *text, const char *key)
{
    size_t length = strlen(key);
    for (const char *at = text; (at = strchr(at, '"')); at++) {
        at++;
        if (strncmp(at, key, length) || at[length] != '"') continue;
        at += length + 1;
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
        if (*at++ != ':') continue;
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') at++;
        return at;
    }
    return NULL;
}

static int json_string(const char *text, const char *key, char *out, size_t size)
{
    const char *value = json_value(text, key);
    if (!value || *value != '"') return 0;

    const char *end = strchr(value + 1, '"');
    if (!end || end == value + 1 || (size_t)(end - value - 1) >= size) return 0;

    size_t length = (size_t)(end - value - 1);
    memcpy(out, value + 1, length);
    out[length] = 0;
    return 1;
}

static int load_settings(const char *path, Options *options)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        log_error("Could not open settings '%s'.\n", path);
        return -1;
    }

    char text[4096];
    size_t length = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[length] = 0;

    if (!json_string(text, "instrument", options->instrument, sizeof options->instrument)) {
        log_error("Settings '%s' need an \"instrument\" string.\n", path);
        return -1;
    }
    json_string(text, "samples_path", options->samplesPath, sizeof options->samplesPath);
    json_string(text, "vsts_path", options->vstsPath, sizeof options->vstsPath);
    const char *ratio = json_value(text, "samples_ratio");
    options->samplesRatio = ratio ? atoi(ratio) : 50;

    const char *channel = json_value(text, "input-channel");
    char *stop = NULL;
    long number = channel ? strtol(channel, &stop, 10) : 0;
    if (!channel || stop == channel || number < 1) {
        log_error("Settings '%s' need a positive \"input-channel\".\n", path);
        return -1;
    }
    options->inputChannel = (int)number;
    return 0;
}

static int parse_args(int argc, char **argv, Options *options)
{
    for (int i = 1; i < argc; i++) {
        const char *argument = argv[i];
        int hasValue = i + 1 < argc;

        if (!strcmp(argument, "--list") || !strcmp(argument, "-l")) options->list = 1;
        else if (!strcmp(argument, "--help") || !strcmp(argument, "-h")) { usage(); return 2; }
        else if (!strcmp(argument, "--no-synth")) options->synth = 0;
        else if (!strcmp(argument, "--driver") || !strcmp(argument, "-d")) {
            if (!hasValue) return 1;
            options->driver = argv[++i];
        } else if (!strcmp(argument, "--in") || !strcmp(argument, "-i")) {
            if (!hasValue) return 1;
            options->in = argv[++i];
        } else if (!strcmp(argument, "--out") || !strcmp(argument, "-o")) {
            if (!hasValue) return 1;
            options->out = argv[++i];
        } else if (!strcmp(argument, "--midi-in")) {
            if (!hasValue) return 1;
            options->midiIn = argv[++i];
        } else if (!strcmp(argument, "--midi-out")) {
            if (!hasValue) return 1;
            options->midiOut = argv[++i];
        } else if (!strcmp(argument, "--gain") || !strcmp(argument, "-g")) {
            if (!hasValue) return 1;
            options->gainDb = strtod(argv[++i], NULL);
        } else if (!strcmp(argument, "--rate")) {
            if (!hasValue) return 1;
            options->rate = atoi(argv[++i]);
        } else if (!strcmp(argument, "--buffer")) {
            if (!hasValue) return 1;
            options->buffer = atoi(argv[++i]);
        } else {
            log_error("Unknown option '%s'.\n\n", argument);
            usage();
            return 1;
        }
    }

    if (options->gainDb < -60.0 || options->gainDb > 24.0) {
        log_error("--gain must be between -60 and +24 dB.\n");
        return 1;
    }
    if (options->rate < 0 || options->buffer < 0) {
        log_error("--rate and --buffer must not be negative.\n");
        return 1;
    }
    return 0;
}

static void pause_on_error(int exitCode)
{
    if (!exitCode) return;
    printf("\nPress Enter to close...");
    fflush(stdout);
    getchar();
}

static int run(int argc, char **argv, const wchar_t *folder)
{
    log_trace("run start\n");
    Options options;
    memset(&options, 0, sizeof options);
    options.synth = 1;

    const char *settings = !folder && argc > 1 && is_json_path(argv[1]) ? argv[1] : NULL;
    if (settings && load_settings(settings, &options) != 0) return 1;

    int parsed = parse_args(argc - (settings ? 1 : 0), argv + (settings ? 1 : 0), &options);
    if (parsed == 2) return 0;
    if (parsed != 0) return 1;
    log_trace("arguments parsed\n");
    if (options.list) {
        list_devices();
        return 0;
    }

    memset(&g, 0, sizeof g);
    g.gain = (float)pow(10.0, options.gainDb / 20.0);
    g.gainDb = (float)options.gainDb;
    g.volume = 1.0f;
    g.synthEnabled = options.synth;
    g.samplesRatio = options.samplesRatio;
    g.midiInIndex = -1;
    g.midiOutIndex = -1;

    callbacks.bufferSwitch = on_buffer_switch;
    callbacks.sampleRateDidChange = on_sample_rate_changed;
    callbacks.asioMessage = on_asio_message;
    callbacks.bufferSwitchTimeInfo = on_buffer_switch_time_info;

    SetConsoleTitleA("restless_virtuoso");
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    char names[64][128];
    int driverCount = list_drivers(names, 64);

    if (folder) {
        playlistMode = 1;
        if (start_playlist_w(folder) != 0) return 1;
    } else if (options.instrument[0]) {
        if (is_folder(options.instrument)) {
            playlistMode = 1;
            if (start_playlist(options.instrument) != 0) return 1;
        } else if (resolve_driver(options.instrument, names, driverCount)) {
            if (!options.driver) options.driver = options.instrument;
        } else if (resolve_midi_in(options.instrument) >= 0) {
            if (!options.samplesPath[0] && !options.vstsPath[0]) {
                int result = stream_midi(options.instrument, options.inputChannel);
                printf("\n");
                return result;
            }
            if (options.inputChannel > 16) {
                log_error("MIDI channel %d is out of range (1-16).\n", options.inputChannel);
                return 1;
            }
            instrumentMode = 1;
            g.synthEnabled = 0;
            g.midiChannel = options.inputChannel;
            if (!options.midiIn) options.midiIn = options.instrument;
        } else {
            log_error("Instrument '%s' is neither a folder of wav files, an installed ASIO driver "
                      "nor a MIDI input.\n", options.instrument);
            list_devices();
            return 1;
        }
    }

    HWND window = create_window(GetModuleHandleA(NULL));
    if (!window) {
        log_error("Could not create the ASIO message window.\n");
        return 1;
    }
    log_trace("window %p\n", (void *)window);

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        log_error("COM initialization failed (0x%08lx).\n", (unsigned long)com);
        DestroyWindow(window);
        return 1;
    }
    log_trace("com 0x%08lx\n", (unsigned long)com);
    open_master_volume();
    g.volume = read_master_volume();

    const char *driver = resolve_driver(options.driver ? options.driver : DEFAULT_DRIVER, names, driverCount);
    if (!driver && !options.driver) driver = resolve_driver(NULL, names, driverCount);
    if (!driver) {
        if (!driverCount) log_error("No ASIO driver is installed.\n");
        else {
            log_error("ASIO driver '%s' not found. Installed drivers:\n", options.driver);
            for (int i = 0; i < driverCount; i++) log_error("  %d  %s\n", i, names[i]);
        }
        shutdown_monitor();
        DestroyWindow(window);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }
    log_trace("driver '%s'\n", driver);

    int exitCode = 1;
    if (open_driver(driver, window) != 0) goto done;
    log_trace("driver opened\n");

    printf("%s\n\n", g.driverName);
    list_devices();
    printf("\n");
    log_trace("devices listed\n");

    if (playlistMode || instrumentMode) {
        if (select_channels(options.in, g.inCount, g.inCh, &g.nIn, 0, "input") != 0) goto done;
    } else if (options.inputChannel > 0 && !options.in) {
        if (options.inputChannel > g.inCount) {
            log_error("input-channel %d is out of range (the driver has %d inputs).\n",
                      options.inputChannel, g.inCount);
            goto done;
        }
        g.inCh[0] = options.inputChannel - 1;
        g.nIn = 1;
    } else if (select_channels(options.in, g.inCount, g.inCh, &g.nIn, 2, "input") != 0) {
        goto done;
    }
    if (select_channels(options.out, g.outCount, g.outCh, &g.nOut, 2, "output") != 0) goto done;

    if (g.nOut == 0) {
        log_error("At least one output channel is required.\n");
        goto done;
    }
    if (g.nIn == 0 && !options.midiIn && !playlistMode) {
        log_error("No input channels and no MIDI input, so there is nothing to play.\n");
        goto done;
    }

    if (options.midiIn && open_midi_input(options.midiIn) != 0) goto done;
    if (options.midiOut && open_midi_output(options.midiOut) != 0) goto done;

    if (setup_rate(options.rate) != 0) goto done;
    if (setup_buffer(options.buffer) != 0) goto done;
    if (instrumentMode) {
        if (options.samplesPath[0]) load_drums(options.samplesPath);
        if (options.vstsPath[0]) load_vsts(options.vstsPath);
        choose_sound(options.samplesRatio);
    }
    log_trace("rate and buffer set\n");
    if (create_buffers() != 0) goto done;
    log_trace("buffers created\n");

    printf("  %d Hz, %d frames (%.2f ms), latency %d/%d frames\n",
           (int)g.rate, g.bufferSize, 1000.0 * g.bufferSize / g.rate, g.latencyIn, g.latencyOut);
    printf("  in ");
    print_selection(g.inCh, g.nIn);
    printf(" -> out ");
    print_selection(g.outCh, g.nOut);
    printf("\n");
    if (playlistMode) {
        printf("  %d wav files mixed from ", stemCount);
        print_wide_path(playlistFolder);
        printf("\n");
    }
    print_sound("");

    if (g.midiInIndex >= 0) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps((UINT)g.midiInIndex, &caps, sizeof caps) == MMSYSERR_NOERROR)
            printf("  MIDI in %d (%s)%s\n", g.midiInIndex, caps.szPname, g.synthEnabled ? ", sine synth on" : "");
    }
    if (g.midiOutIndex >= 0) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps((UINT)g.midiOutIndex, &caps, sizeof caps) == MMSYSERR_NOERROR)
            printf("  MIDI thru -> %d (%s)\n", g.midiOutIndex, caps.szPname);
    }

    if (g.asio->vtable->start(g.asio) != ASE_OK) {
        log_error("ASIOStart failed.\n");
        goto done;
    }
    g.started = 1;
    log_trace("started\n");

    init_console_input();
    if (instrumentMode) draw_sound_button();
    printf("m mute  +/- gain  p control panel  q quit\n");

    ULONGLONG lastDraw = 0;
    while (!g.quit) {
        pump_messages();

        if (InterlockedExchange(&g.reset, 0)) {
            log_line("driver settings changed, reconfiguring");
            if (g.started) {
                g.asio->vtable->stop(g.asio);
                g.started = 0;
            }
            if (g.buffersCreated) {
                g.asio->vtable->disposeBuffers(g.asio);
                g.buffersCreated = 0;
            }
            free_buffers();
            vst_stop();

            if (setup_rate(0) != 0 || setup_buffer(options.buffer) != 0 ||
                vst_start() != 0 || create_buffers() != 0 ||
                g.asio->vtable->start(g.asio) != ASE_OK) {
                log_line("could not reopen the ASIO buffers; restart the program");
                break;
            }
            g.started = 1;
            lastDraw = 0;
        }

        read_console_input();

        ULONGLONG now = GetTickCount64();
        if (now - lastDraw >= 100) {
            g.volume = read_master_volume();
            draw_meters();
            lastDraw = now;
        }
        Sleep(5);
    }

    exitCode = 0;

done:
    shutdown_monitor();
    DestroyWindow(window);
    if (SUCCEEDED(com)) CoUninitialize();
    printf("\n");
    return exitCode;
}

static wchar_t *dropped_folder(void)
{
    int count = 0;
    LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return NULL;

    wchar_t *folder = NULL;
    if (count > 1) {
        wchar_t *path = long_path(arguments[1]);
        if (path && is_folder_w(path)) folder = path;
        else free(path);
    }
    LocalFree(arguments);
    return folder;
}

int main(int argc, char **argv)
{
    SetUnhandledExceptionFilter(crash_handler);
    open_error_log();
    srand((unsigned)GetTickCount64());
    wchar_t *folder = dropped_folder();
    int exitCode = run(argc - (folder ? 1 : 0), argv + (folder ? 1 : 0), folder);
    pause_on_error(exitCode);
    if (errorLog) fclose(errorLog);
    return exitCode;
}
