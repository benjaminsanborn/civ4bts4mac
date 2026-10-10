/* OpenAL: forwarded to the host framework. Object names (ALuint) pass through
 * unchanged; devices and contexts are host pointers behind guest handles. */
#include "handles.h"

#include <stdlib.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <OpenAL/al.h>
#include <OpenAL/alc.h>

#define TAG_ALDEV 0x414c4456u /* 'ALDV' */
#define TAG_ALCTX 0x414c4358u /* 'ALCX' */

/* Guest handles for host devices/contexts, so the same host pointer maps back to one handle. */
static struct { void *host; uint32_t g; } g_al[16];
static uint32_t al_handle(uint32_t tag, void *host) {
    if (!host) return 0;
    for (int i = 0; i < 16; i++)
        if (g_al[i].host == host) return g_al[i].g;
    for (int i = 0; i < 16; i++)
        if (!g_al[i].host) {
            g_al[i].host = host;
            g_al[i].g = gh_new(tag, host);
            return g_al[i].g;
        }
    rt_fatal("too many OpenAL objects");
}
static void al_forget(void *host) {
    for (int i = 0; i < 16; i++)
        if (g_al[i].host == host) {
            gh_free(g_al[i].g);
            g_al[i].host = NULL;
        }
}
#define DEV(n) ((ALCdevice *)gh_get(ARG(n), TAG_ALDEV))
#define CTX(n) ((ALCcontext *)gh_get(ARG(n), TAG_ALCTX))

/* CIV4_MUTE silences sound effects (the listener gain stays at 0). */
static int muted(void) {
    static int m = -1;
    if (m < 0) m = getenv("CIV4_MUTE") != NULL;
    return m;
}

/* ---- ALC ---- */
SHIM(_alcOpenDevice) { RET(al_handle(TAG_ALDEV, alcOpenDevice(ARG(0) ? CPTR(ARG(0)) : NULL))); }
SHIM(_alcCloseDevice) {
    ALCdevice *d = DEV(0);
    if (!d) { RET(ALC_FALSE); return; }
    ALCboolean r = alcCloseDevice(d);
    al_forget(d);
    RET(r);
}
SHIM(_alcCreateContext) {
    /* attribute list: zero-terminated ALCint pairs */
    ALCint attrs[64];
    int n = 0;
    if (ARG(1))
        while (n < 62 && (attrs[n] = (ALCint)R32_(g_mem, ARG(1) + 4 * n)) != 0) n++;
    attrs[n] = 0;
    RET(al_handle(TAG_ALCTX, alcCreateContext(DEV(0), ARG(1) ? attrs : NULL)));
}
SHIM(_alcDestroyContext) {
    ALCcontext *c = CTX(0);
    if (!c) return;
    alcDestroyContext(c);
    al_forget(c);
}
SHIM(_alcMakeContextCurrent) {
    ALCboolean r = alcMakeContextCurrent(CTX(0));
    if (r && ARG(0) && muted()) alListenerf(AL_GAIN, 0);
    RET(r);
}
SHIM(_alcGetCurrentContext) { RET(al_handle(TAG_ALCTX, alcGetCurrentContext())); }
SHIM(_alcGetContextsDevice) { RET(al_handle(TAG_ALDEV, alcGetContextsDevice(CTX(0)))); }
SHIM(_alcGetError) { RET(alcGetError(DEV(0))); }

/* ---- AL ---- */
SHIM(_alGetError) { RET(alGetError()); }
SHIM(_alDistanceModel) { alDistanceModel((ALenum)ARG(0)); }
SHIM(_alGenBuffers) { alGenBuffers((ALsizei)ARG(0), (ALuint *)PTR(ARG(1))); }
SHIM(_alDeleteBuffers) { alDeleteBuffers((ALsizei)ARG(0), (const ALuint *)PTR(ARG(1))); }
SHIM(_alGenSources) { alGenSources((ALsizei)ARG(0), (ALuint *)PTR(ARG(1))); }
SHIM(_alDeleteSources) { alDeleteSources((ALsizei)ARG(0), (const ALuint *)PTR(ARG(1))); }
SHIM(_alBufferData) {
    /* (buffer, format, const void *data, size, freq) */
    alBufferData((ALuint)ARG(0), (ALenum)ARG(1), PTR(ARG(2)), (ALsizei)ARG(3), (ALsizei)ARG(4));
}
SHIM(_alListenerf) {
    ALfloat v = ARGF32(1);
    if ((ALenum)ARG(0) == AL_GAIN && muted()) v = 0;
    alListenerf((ALenum)ARG(0), v);
}
SHIM(_alListener3f) { alListener3f((ALenum)ARG(0), ARGF32(1), ARGF32(2), ARGF32(3)); }
SHIM(_alListenerfv) {
    ALfloat v[6];
    ALenum p = (ALenum)ARG(0);
    int n = p == AL_ORIENTATION ? 6 : p == AL_GAIN ? 1 : 3;
    for (int i = 0; i < n; i++) v[i] = RF32_(g_mem, ARG(1) + 4 * i);
    if (p == AL_GAIN && muted()) v[0] = 0;
    alListenerfv(p, v);
}
SHIM(_alSourcef) { alSourcef((ALuint)ARG(0), (ALenum)ARG(1), ARGF32(2)); }
SHIM(_alSource3f) { alSource3f((ALuint)ARG(0), (ALenum)ARG(1), ARGF32(2), ARGF32(3), ARGF32(4)); }
SHIM(_alSourcei) { alSourcei((ALuint)ARG(0), (ALenum)ARG(1), (ALint)ARG(2)); }
SHIM(_alGetSourcef) {
    ALfloat v = 0;
    alGetSourcef((ALuint)ARG(0), (ALenum)ARG(1), &v);
    WF32_(g_mem, ARG(2), v);
}
SHIM(_alGetSourcei) {
    ALint v = 0;
    alGetSourcei((ALuint)ARG(0), (ALenum)ARG(1), &v);
    W32_(g_mem, ARG(2), (uint32_t)v);
}
SHIM(_alSourcePlay) { alSourcePlay((ALuint)ARG(0)); }
SHIM(_alSourcePause) { alSourcePause((ALuint)ARG(0)); }
SHIM(_alSourceStop) { alSourceStop((ALuint)ARG(0)); }
SHIM(_alSourceQueueBuffers) { alSourceQueueBuffers((ALuint)ARG(0), (ALsizei)ARG(1), (const ALuint *)PTR(ARG(2))); }
SHIM(_alSourceUnqueueBuffers) { alSourceUnqueueBuffers((ALuint)ARG(0), (ALsizei)ARG(1), (ALuint *)PTR(ARG(2))); }

const ShimDef shims_openal[] = {
    SHIM_ENTRY(_alcOpenDevice), SHIM_ENTRY(_alcCloseDevice), SHIM_ENTRY(_alcCreateContext),
    SHIM_ENTRY(_alcDestroyContext), SHIM_ENTRY(_alcMakeContextCurrent), SHIM_ENTRY(_alcGetCurrentContext),
    SHIM_ENTRY(_alcGetContextsDevice), SHIM_ENTRY(_alcGetError), SHIM_ENTRY(_alGetError),
    SHIM_ENTRY(_alDistanceModel), SHIM_ENTRY(_alGenBuffers), SHIM_ENTRY(_alDeleteBuffers),
    SHIM_ENTRY(_alGenSources), SHIM_ENTRY(_alDeleteSources), SHIM_ENTRY(_alBufferData), SHIM_ENTRY(_alListenerf),
    SHIM_ENTRY(_alListener3f), SHIM_ENTRY(_alListenerfv), SHIM_ENTRY(_alSourcef), SHIM_ENTRY(_alSource3f),
    SHIM_ENTRY(_alSourcei), SHIM_ENTRY(_alGetSourcef), SHIM_ENTRY(_alGetSourcei), SHIM_ENTRY(_alSourcePlay),
    SHIM_ENTRY(_alSourcePause), SHIM_ENTRY(_alSourceStop), SHIM_ENTRY(_alSourceQueueBuffers),
    SHIM_ENTRY(_alSourceUnqueueBuffers),
    {0, 0},
};
