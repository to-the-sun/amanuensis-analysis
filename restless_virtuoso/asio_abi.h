/* ASIO 2.3 host ABI for x64 Windows: declarations only, no SDK code. */
#ifndef ASIO_ABI_H
#define ASIO_ABI_H

#include <stdint.h>

typedef int32_t ASIOBool;
typedef int32_t ASIOError;
typedef int32_t ASIOSampleType;
typedef double ASIOSampleRate;
typedef int64_t ASIOSamples;
typedef int64_t ASIOTimeStamp;

enum { ASIOFalse = 0, ASIOTrue = 1 };

enum {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

enum {
    ASIOSTInt16MSB = 0, ASIOSTInt24MSB = 1, ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3, ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8, ASIOSTInt32MSB18 = 9, ASIOSTInt32MSB20 = 10, ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16, ASIOSTInt24LSB = 17, ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19, ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24, ASIOSTInt32LSB18 = 25, ASIOSTInt32LSB20 = 26, ASIOSTInt32LSB24 = 27
};

enum {
    kAsioSelectorSupported = 1, kAsioEngineVersion, kAsioResetRequest,
    kAsioBufferSizeChange, kAsioResyncRequest, kAsioLatenciesChanged,
    kAsioSupportsTimeInfo, kAsioSupportsTimeCode, kAsioMMCCommand,
    kAsioSupportsInputMonitor, kAsioSupportsInputGain, kAsioSupportsInputMeter,
    kAsioSupportsOutputGain, kAsioSupportsOutputMeter, kAsioOverload
};

typedef struct ASIOChannelInfo {
    int32_t channel;
    ASIOBool isInput;
    ASIOBool isActive;
    int32_t channelGroup;
    ASIOSampleType type;
    char name[32];
} ASIOChannelInfo;

typedef struct ASIOBufferInfo {
    ASIOBool isInput;
    int32_t channelNum;
    void *buffers[2];
} ASIOBufferInfo;

typedef struct ASIOTime ASIOTime;

typedef struct ASIOCallbacks {
    void (*bufferSwitch)(int32_t index, ASIOBool directProcess);
    void (*sampleRateDidChange)(ASIOSampleRate rate);
    int32_t (*asioMessage)(int32_t selector, int32_t value, void *message, double *opt);
    ASIOTime *(*bufferSwitchTimeInfo)(ASIOTime *params, int32_t index, ASIOBool directProcess);
} ASIOCallbacks;

typedef struct IASIO IASIO;

struct IASIO {
    ASIOError (*QueryInterface)(IASIO *self, const void *iid, void **out);
    uint32_t (*AddRef)(IASIO *self);
    uint32_t (*Release)(IASIO *self);
    ASIOBool (*init)(IASIO *self, void *sysHandle);
    void (*getDriverName)(IASIO *self, char *name);
    int32_t (*getDriverVersion)(IASIO *self);
    void (*getErrorMessage)(IASIO *self, char *text);
    ASIOError (*start)(IASIO *self);
    ASIOError (*stop)(IASIO *self);
    ASIOError (*getChannels)(IASIO *self, int32_t *inputs, int32_t *outputs);
    ASIOError (*getLatencies)(IASIO *self, int32_t *inputLatency, int32_t *outputLatency);
    ASIOError (*getBufferSize)(IASIO *self, int32_t *min, int32_t *max, int32_t *preferred, int32_t *granularity);
    ASIOError (*canSampleRate)(IASIO *self, ASIOSampleRate rate);
    ASIOError (*getSampleRate)(IASIO *self, ASIOSampleRate *rate);
    ASIOError (*setSampleRate)(IASIO *self, ASIOSampleRate rate);
    ASIOError (*getClockSources)(IASIO *self, void *clocks, int32_t *count);
    ASIOError (*setClockSource)(IASIO *self, int32_t reference);
    ASIOError (*getSamplePosition)(IASIO *self, ASIOSamples *position, ASIOTimeStamp *timestamp);
    ASIOError (*getChannelInfo)(IASIO *self, ASIOChannelInfo *info);
    ASIOError (*createBuffers)(IASIO *self, ASIOBufferInfo *infos, int32_t numChannels,
                               int32_t bufferSize, ASIOCallbacks *callbacks);
    ASIOError (*disposeBuffers)(IASIO *self);
    ASIOError (*controlPanel)(IASIO *self);
    ASIOError (*future)(IASIO *self, int32_t selector, void *opt);
    ASIOError (*outputReady)(IASIO *self);
};

#endif
