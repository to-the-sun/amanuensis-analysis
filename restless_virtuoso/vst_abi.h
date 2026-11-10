/* VST 2.4 plugin ABI for x64 Windows: declarations only, no SDK code. */
#ifndef VST_ABI_H
#define VST_ABI_H

#include <stdint.h>

#define kVstEffectMagic 0x56737450 /* 'VstP' */

enum {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effProcessEvents = 25,
    effGetEffectName = 45
};

enum {
    audioMasterVersion = 1,
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize = 17,
    audioMasterGetCurrentProcessLevel = 23,
    audioMasterGetVendorString = 32,
    audioMasterGetProductString = 33,
    audioMasterGetVendorVersion = 34
};

enum {
    kVstMidiType = 1,
    effFlagsIsSynth = 1 << 8
};

typedef int32_t VstInt32;
typedef intptr_t VstIntPtr;

typedef struct AEffect AEffect;

typedef VstIntPtr (*VstDispatcherProc)(AEffect *, VstInt32, VstInt32, VstIntPtr, void *, float);
typedef void (*VstProcessProc)(AEffect *, float **, float **, VstInt32);
typedef void (*VstProcessProcDouble)(AEffect *, double **, double **, VstInt32);
typedef void (*VstSetParameterProc)(AEffect *, VstInt32, float);
typedef float (*VstGetParameterProc)(AEffect *, VstInt32);
typedef VstIntPtr (*VstHostCallbackProc)(AEffect *, VstInt32, VstInt32, VstIntPtr, void *, float);

struct AEffect {
    VstInt32 magic;
    VstDispatcherProc dispatcher;
    VstProcessProc process;
    VstSetParameterProc setParameter;
    VstGetParameterProc getParameter;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;
    VstIntPtr resvd1;
    VstIntPtr resvd2;
    VstInt32 initialDelay;
    VstInt32 realQualities;
    VstInt32 offQualities;
    float ioRatio;
    void *object;
    void *user;
    VstInt32 uniqueID;
    VstInt32 version;
    VstProcessProc processReplacing;
    VstProcessProcDouble processDoubleReplacing;
};

typedef struct VstEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
} VstEvent;

typedef struct VstMidiEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
} VstMidiEvent;

typedef struct VstEvents {
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent *events[2];
} VstEvents;

#endif
