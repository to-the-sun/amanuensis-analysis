#if !defined(_WIN64)
#error "asio_monitor is a 64-bit Windows program"
#endif

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <conio.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asio_abi.h"
#include "dsp.h"

#define MAX_SELECTED 32
#define MAX_METERS 4
#define MIDI_QUEUE 1024

typedef struct {
    const char *driver, *in, *out, *midiIn, *midiOut;
    double gainDb;
    int rate, buffer, synth, list;
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
    float *inputFloats, *synthFloats, *mixFloats;

    HMIDIIN midiIn;
    HMIDIOUT midiOut;
    int midiInIndex, midiOutIndex;
    int synthEnabled;
    int buffersCreated, started;

    volatile LONG quit, mute, reset;
    volatile float gain, peak[MAX_SELECTED];
    float gainDb;
    int lineWidth;
} Monitor;

static Monitor g;
static ASIOCallbacks callbacks;

static volatile uint32_t midiHead, midiTail;
static uint32_t midiQueue[MIDI_QUEUE];

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
    while (midi_pop(&message)) {
        int status = (int)(message & 0xf0);
        int note = (int)(message >> 8) & 0x7f;
        int velocity = (int)(message >> 16) & 0x7f;
        if (status == 0x90 && velocity > 0) synth_note_on(note, velocity / 127.0f);
        else if (status == 0x80 || status == 0x90) synth_note_off(note);
    }
}

static void process_buffer(int32_t index)
{
    const int frames = (int)g.bufferSize;
    const float gain = g.mute ? 0.0f : g.gain;
    const float synthGain = g.mute ? 0.0f : 1.0f;

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
    if (g.synthEnabled) {
        drain_midi();
        synth_render(g.synthFloats, frames);
    }

    for (int j = 0; j < g.nOut; j++) {
        int source = g.nIn ? j % g.nIn : -1;
        const float *input = source >= 0 ? g.inputFloats + (size_t)source * frames : NULL;
        void *output = g.bufferInfo[g.nIn + j].buffers[index];

        for (int i = 0; i < frames; i++) {
            float value = (input ? input[i] * gain : 0.0f) + g.synthFloats[i] * synthGain;
            g.mixFloats[i] = value > 1.0f ? 1.0f : value < -1.0f ? -1.0f : value;
        }
        sample_from_float(g.mixFloats, output, g.outFormat[j], frames);
    }
}

static void on_buffer_switch(int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->outputReady(g.asio);
}

static ASIOTime *on_buffer_switch_time_info(ASIOTime *params, int32_t index, ASIOBool directProcess)
{
    (void)directProcess;
    process_buffer(index);
    if (g.postOutput) g.asio->outputReady(g.asio);
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
    if (g.midiOut) midiOutShortMsg(g.midiOut, (DWORD)param1);
    midi_push((uint32_t)param1);
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
    windowClass.lpszClassName = "AsioMonitorWindow";
    RegisterClassA(&windowClass);
    return CreateWindowExA(0, windowClass.lpszClassName, "asio_monitor", 0, 0, 0, 0, 0,
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
        fprintf(stderr, "ASIO driver '%s' is not registered.\n", name);
        return -1;
    }

    HRESULT result = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &clsid, (void **)&g.asio);
    if (FAILED(result) || !g.asio) {
        fprintf(stderr, "Could not create the ASIO driver object (0x%08lx).\n", (unsigned long)result);
        g.asio = NULL;
        return -1;
    }

    if (!g.asio->init(g.asio, window)) {
        char error[124] = {0};
        g.asio->getErrorMessage(g.asio, error);
        error[123] = 0;
        fprintf(stderr, "ASIOInit failed: %s\n", error[0] ? error : "no error message from the driver");
        g.asio->Release(g.asio);
        g.asio = NULL;
        return -1;
    }

    g.asio->getDriverName(g.asio, g.driverName);
    g.driverName[63] = 0;

    if (g.asio->getChannels(g.asio, &g.inCount, &g.outCount) != ASE_OK) {
        fprintf(stderr, "ASIOGetChannels failed.\n");
        return -1;
    }

    g.postOutput = g.asio->outputReady(g.asio) == ASE_OK;
    return 0;
}

static void print_channels(const char *label, ASIOBool isInput, int count)
{
    printf("%s:\n", label);
    if (!count) {
        printf("  (none)\n");
        return;
    }

    for (int channel = 0; channel < count; channel++) {
        ASIOChannelInfo info;
        memset(&info, 0, sizeof info);
        info.channel = channel;
        info.isInput = isInput;

        char name[33] = "(unnamed)";
        if (g.asio->getChannelInfo(g.asio, &info) == ASE_OK) {
            memcpy(name, info.name, 32);
            name[32] = 0;
        }
        printf("  %2d  %s\n", channel + 1, name);
    }
}

static void print_midi_inputs(void)
{
    UINT count = midiInGetNumDevs();
    printf("MIDI inputs:\n");
    if (!count) {
        printf("  (none)\n");
        return;
    }

    for (UINT i = 0; i < count; i++) {
        MIDIINCAPS caps;
        if (midiInGetDevCaps(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        printf("  %2u  %s\n", i, caps.szPname);
    }
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
        fprintf(stderr, "Invalid %s channel list '%s' (the driver has %d).\n", what, spec, available);
        return -1;
    }
    *count = parsed;
    return 0;
}

static int setup_rate(int requested)
{
    if (requested > 0) {
        if (g.asio->canSampleRate(g.asio, (ASIOSampleRate)requested) != ASE_OK) {
            fprintf(stderr, "The driver does not support %d Hz.\n", requested);
            return -1;
        }
        ASIOError error = g.asio->setSampleRate(g.asio, (ASIOSampleRate)requested);
        if (error != ASE_OK) {
            fprintf(stderr, "ASIOSetSampleRate failed (%s).\n", asio_error_text(error));
            return -1;
        }
    }

    ASIOSampleRate rate = 0;
    g.asio->getSampleRate(g.asio, &rate);
    if (rate <= 0) rate = requested > 0 ? requested : 48000;
    g.rate = rate;
    synth_set_rate((int)rate);
    return 0;
}

static int setup_buffer(int requested)
{
    int32_t minimum = 0, maximum = 0, preferred = 0, granularity = 0;
    ASIOError error = g.asio->getBufferSize(g.asio, &minimum, &maximum, &preferred, &granularity);
    if (error != ASE_OK) {
        fprintf(stderr, "ASIOGetBufferSize failed (%s).\n", asio_error_text(error));
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

    if (g.asio->getChannelInfo(g.asio, &info) != ASE_OK) {
        fprintf(stderr, "ASIOGetChannelInfo failed for %s %d.\n", isInput ? "input" : "output", channel + 1);
        return SAMPLE_UNSUPPORTED;
    }

    SampleFormat format = sample_format_from_asio(info.type);
    if (format == SAMPLE_UNSUPPORTED)
        fprintf(stderr, "%s %d uses an unsupported sample format (%s).\n",
                isInput ? "Input" : "Output", channel + 1, sample_format_name(info.type));
    return format;
}

static void free_buffers(void)
{
    free(g.inputFloats);
    free(g.synthFloats);
    free(g.mixFloats);
    g.inputFloats = g.synthFloats = g.mixFloats = NULL;
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
    g.synthFloats = malloc(frames * sizeof(float));
    g.mixFloats = malloc(frames * sizeof(float));
    if ((g.nIn && !g.inputFloats) || !g.synthFloats || !g.mixFloats) {
        fprintf(stderr, "Out of memory for %d-frame buffers.\n", g.bufferSize);
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

    ASIOError error = g.asio->createBuffers(g.asio, g.bufferInfo, g.nIn + g.nOut,
                                            g.bufferSize, &callbacks);
    if (error != ASE_OK) {
        fprintf(stderr, "ASIOCreateBuffers failed (%s).\n", asio_error_text(error));
        free_buffers();
        return -1;
    }
    g.buffersCreated = 1;

    int32_t latencyIn = 0, latencyOut = 0;
    g.asio->getLatencies(g.asio, &latencyIn, &latencyOut);
    g.latencyIn = latencyIn;
    g.latencyOut = latencyOut;
    return 0;
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
        fprintf(stderr, "MIDI input '%s' not found (use --list).\n", spec);
        return -1;
    }
    if (midiInOpen(&g.midiIn, (UINT)index, (DWORD_PTR)midi_in_proc, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        fprintf(stderr, "Could not open MIDI input %d.\n", index);
        return -1;
    }
    g.midiInIndex = index;
    midiInStart(g.midiIn);
    return 0;
}

static int open_midi_output(const char *spec)
{
    int index = resolve_midi_out(spec);
    if (index < 0) {
        fprintf(stderr, "MIDI output '%s' not found (use --list).\n", spec);
        return -1;
    }
    if (midiOutOpen(&g.midiOut, (UINT)index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "Could not open MIDI output %d.\n", index);
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
        if (midiInGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) printf("  %u  %s\n", i, caps.szPname);
    }
    if (!inputs) printf("  (none)\n");

    printf("MIDI outputs:\n");
    UINT outputs = midiOutGetNumDevs();
    for (UINT i = 0; i < outputs; i++) {
        MIDIOUTCAPS caps;
        if (midiOutGetDevCaps(i, &caps, sizeof caps) == MMSYSERR_NOERROR) printf("  %u  %s\n", i, caps.szPname);
    }
    if (!outputs) printf("  (none)\n");
}

static void adjust_gain(float delta)
{
    g.gainDb += delta;
    if (g.gainDb < -60.0f) g.gainDb = -60.0f;
    if (g.gainDb > 24.0f) g.gainDb = 24.0f;
    g.gain = powf(10.0f, g.gainDb / 20.0f);
}

static void draw_meters(void)
{
    char line[512];
    int length = snprintf(line, sizeof line, "%s %+.1f dB  ", g.mute ? "MUTED" : "gain", g.gainDb);
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
        if (g.started) g.asio->stop(g.asio);
        if (g.buffersCreated) g.asio->disposeBuffers(g.asio);
        g.asio->Release(g.asio);
        g.asio = NULL;
    }
    free_buffers();
}

static void usage(void)
{
    printf(
        "asio_monitor - play ASIO inputs on ASIO outputs\n"
        "\n"
        "Usage:\n"
        "  asio_monitor --list\n"
        "  asio_monitor [options]\n"
        "\n"
        "Options:\n"
        "  -d, --driver NAME|INDEX  ASIO driver (default: first installed)\n"
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
            fprintf(stderr, "Unknown option '%s'.\n\n", argument);
            usage();
            return 1;
        }
    }

    if (options->gainDb < -60.0 || options->gainDb > 24.0) {
        fprintf(stderr, "--gain must be between -60 and +24 dB.\n");
        return 1;
    }
    if (options->rate < 0 || options->buffer < 0) {
        fprintf(stderr, "--rate and --buffer must not be negative.\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Options options;
    memset(&options, 0, sizeof options);
    options.synth = 1;

    int parsed = parse_args(argc, argv, &options);
    if (parsed == 2) return 0;
    if (parsed != 0) return 1;
    if (options.list) {
        list_devices();
        return 0;
    }

    memset(&g, 0, sizeof g);
    g.gain = (float)pow(10.0, options.gainDb / 20.0);
    g.gainDb = (float)options.gainDb;
    g.synthEnabled = options.synth;
    g.midiInIndex = -1;
    g.midiOutIndex = -1;

    callbacks.bufferSwitch = on_buffer_switch;
    callbacks.sampleRateDidChange = on_sample_rate_changed;
    callbacks.asioMessage = on_asio_message;
    callbacks.bufferSwitchTimeInfo = on_buffer_switch_time_info;

    SetConsoleTitleA("asio_monitor");
    SetConsoleCtrlHandler(ctrl_handler, TRUE);

    HWND window = create_window(GetModuleHandleA(NULL));
    if (!window) {
        fprintf(stderr, "Could not create the ASIO message window.\n");
        return 1;
    }

    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "COM initialization failed (0x%08lx).\n", (unsigned long)com);
        DestroyWindow(window);
        return 1;
    }

    char names[64][128];
    int driverCount = list_drivers(names, 64);
    const char *driver = resolve_driver(options.driver, names, driverCount);
    if (!driver) {
        if (!driverCount) fprintf(stderr, "No ASIO driver is installed.\n");
        else {
            fprintf(stderr, "ASIO driver '%s' not found. Installed drivers:\n", options.driver);
            for (int i = 0; i < driverCount; i++) fprintf(stderr, "  %d  %s\n", i, names[i]);
        }
        shutdown_monitor();
        DestroyWindow(window);
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    int exitCode = 1;
    if (open_driver(driver, window) != 0) goto done;

    printf("%s\n\n", g.driverName);
    print_channels("Audio inputs", ASIOTrue, g.inCount);
    print_channels("Audio outputs", ASIOFalse, g.outCount);
    print_midi_inputs();
    printf("\n");

    if (select_channels(options.in, g.inCount, g.inCh, &g.nIn, 2, "input") != 0) goto done;
    if (select_channels(options.out, g.outCount, g.outCh, &g.nOut, 2, "output") != 0) goto done;

    if (g.nOut == 0) {
        fprintf(stderr, "At least one output channel is required.\n");
        goto done;
    }
    if (g.nIn == 0 && !options.midiIn) {
        fprintf(stderr, "No input channels and no MIDI input, so there is nothing to play.\n");
        goto done;
    }

    if (options.midiIn && open_midi_input(options.midiIn) != 0) goto done;
    if (options.midiOut && open_midi_output(options.midiOut) != 0) goto done;

    if (setup_rate(options.rate) != 0) goto done;
    if (setup_buffer(options.buffer) != 0) goto done;
    if (create_buffers() != 0) goto done;

    printf("  %d Hz, %d frames (%.2f ms), latency %d/%d frames\n",
           (int)g.rate, g.bufferSize, 1000.0 * g.bufferSize / g.rate, g.latencyIn, g.latencyOut);
    printf("  in ");
    print_selection(g.inCh, g.nIn);
    printf(" -> out ");
    print_selection(g.outCh, g.nOut);
    printf(", gain %+.1f dB\n", g.gainDb);

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

    if (g.asio->start(g.asio) != ASE_OK) {
        fprintf(stderr, "ASIOStart failed.\n");
        goto done;
    }
    g.started = 1;

    printf("m mute  +/- gain  p control panel  q quit\n");

    ULONGLONG lastDraw = 0;
    while (!g.quit) {
        pump_messages();

        if (InterlockedExchange(&g.reset, 0)) {
            log_line("driver settings changed, reconfiguring");
            if (g.started) {
                g.asio->stop(g.asio);
                g.started = 0;
            }
            if (g.buffersCreated) {
                g.asio->disposeBuffers(g.asio);
                g.buffersCreated = 0;
            }
            free_buffers();

            if (setup_rate(0) != 0 || setup_buffer(options.buffer) != 0 ||
                create_buffers() != 0 || g.asio->start(g.asio) != ASE_OK) {
                log_line("could not reopen the ASIO buffers; restart the program");
                break;
            }
            g.started = 1;
            lastDraw = 0;
        }

        while (_kbhit()) {
            int key = _getch();
            if (key == 'q' || key == 'Q' || key == 27) InterlockedExchange(&g.quit, 1);
            else if (key == 'm' || key == 'M') InterlockedExchange(&g.mute, !g.mute);
            else if (key == '+' || key == '=') adjust_gain(1.0f);
            else if (key == '-' || key == '_') adjust_gain(-1.0f);
            else if (key == 'p' || key == 'P') g.asio->controlPanel(g.asio);
        }

        ULONGLONG now = GetTickCount64();
        if (now - lastDraw >= 100) {
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
