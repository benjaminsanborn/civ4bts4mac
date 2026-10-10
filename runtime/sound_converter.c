/* Sound Manager SoundConverter, as used by Miles' OpenAL sample loader to
 * turn IMA ADPCM WAVs ('ms\0\x11', about one in eight of the game's sound
 * effects) into 16-bit PCM for alBufferData. Only that conversion (and PCM
 * passthrough) is needed.
 *
 * SoundComponentData (28 bytes): flags 0, format 4, numChannels 8 (short),
 * sampleSize 10 (short), sampleRate 12 (16.16), sampleCount 16, buffer 20. */
#include "handles.h"

#include <stdlib.h>
#include <string.h>

#define TAG_SNDCONV 0x534e4443u /* 'SNDC' */
#define noErr 0
#define paramErr (-50)
#define FOURCC4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define kMSIMAFormat FOURCC4('m', 's', 0, 0x11)

typedef struct {
    uint32_t g;
    uint32_t in_format;
    int channels, in_bits;
    int block_align, samples_per_block; /* IMA ADPCM */
} SndConv;

static const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static int16_t ima_step(int nibble, int *pred, int *index) {
    int step = kStepTable[*index], diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;
    *pred += diff;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += kIndexTable[nibble];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*pred;
}

/* Decode one Microsoft IMA ADPCM block (possibly short) into interleaved
 * little-endian int16. Returns frames written. */
static int decode_block(const uint8_t *in, int len, int ch, int max_frames, int16_t *out) {
    if (len < 4 * ch) return 0;
    int pred[2], index[2];
    for (int c = 0; c < ch; c++) {
        pred[c] = (int16_t)(in[4 * c] | in[4 * c + 1] << 8);
        index[c] = in[4 * c + 2] > 88 ? 88 : in[4 * c + 2];
        out[c] = (int16_t)pred[c];
    }
    int frames = 1;
    const uint8_t *p = in + 4 * ch, *end = in + len;
    /* Each channel contributes 4 bytes (8 samples) at a time, interleaved by channel. */
    while (p + 4 * ch <= end && frames + 8 <= max_frames) {
        for (int c = 0; c < ch; c++)
            for (int i = 0; i < 4; i++) {
                uint8_t b = p[4 * c + i];
                out[(frames + 2 * i) * ch + c] = ima_step(b & 15, &pred[c], &index[c]);
                out[(frames + 2 * i + 1) * ch + c] = ima_step(b >> 4, &pred[c], &index[c]);
            }
        p += 4 * ch;
        frames += 8;
    }
    return frames;
}

SHIM(_SoundConverterOpen) {
    /* (const SoundComponentData *in, const SoundComponentData *out, SoundConverter *sc) */
    uint32_t in = ARG(0), out = ARG(1);
    SndConv *s = calloc(1, sizeof *s);
    s->in_format = R32_(g_mem, in + 4);
    s->channels = (int16_t)R16_(g_mem, in + 8);
    s->in_bits = (int16_t)R16_(g_mem, in + 10);
    if (s->channels < 1 || s->channels > 2) s->channels = 1;
    uint32_t of = R32_(g_mem, out + 4);
    if (of != FOURCC4('N', 'O', 'N', 'E') && of != FOURCC4('s', 'o', 'w', 't'))
        rt_log("SoundConverterOpen: output format %08x treated as 16-bit PCM", of);
    s->g = gh_new(TAG_SNDCONV, s);
    W32_(g_mem, ARG(2), s->g);
    RET(noErr);
}
SHIM(_SoundConverterClose) {
    SndConv *s = gh_get(ARG(0), TAG_SNDCONV);
    if (s) { gh_free(s->g); free(s); }
    RET(noErr);
}
SHIM(_SoundConverterSetInfo) {
    /* (sc, OSType selector, void *info): 'wave' carries the WAV format block
     * after an 'ms\0\x11' atom: WAVEFORMATEX + samplesPerBlock. */
    SndConv *s = gh_get(ARG(0), TAG_SNDCONV);
    if (!s) { RET(paramErr); return; }
    if (ARG(1) == FOURCC4('w', 'a', 'v', 'e') && ARG(2)) {
        /* Walk the big-endian atom list ('frma', then the format's own atom). */
        for (uint32_t a = ARG(2), end = ARG(2) + 256; a + 8 <= end;) {
            uint32_t size = __builtin_bswap32(R32_(g_mem, a)), type = __builtin_bswap32(R32_(g_mem, a + 4));
            if (size < 8 || size > 256) break;
            if (type != kMSIMAFormat || size < 8 + 16) {
                a += size;
                continue;
            }
            {
                uint32_t w = a + 8; /* WAVEFORMATEX, little-endian */
                s->channels = (int16_t)R16_(g_mem, w + 2);
                s->block_align = R16_(g_mem, w + 12);
                s->samples_per_block = size >= 8 + 20 ? R16_(g_mem, w + 18) : 0;
                break;
            }
        }
        if (s->channels < 1 || s->channels > 2) s->channels = 1;
        if (s->block_align && !s->samples_per_block)
            s->samples_per_block = (s->block_align - 4 * s->channels) * 8 / (4 * s->channels) + 1;
    }
    RET(noErr);
}
SHIM(_SoundConverterGetBufferSizes) {
    /* (sc, inputBytesTarget, unsigned long *inputFrames, unsigned long *inputBytes, unsigned long *outputBytes) */
    SndConv *s = gh_get(ARG(0), TAG_SNDCONV);
    if (!s) { RET(paramErr); return; }
    uint32_t bytes = ARG(1), frames;
    if (s->in_format == kMSIMAFormat && s->block_align) {
        uint32_t blocks = bytes / s->block_align, rest = bytes % s->block_align;
        frames = blocks * s->samples_per_block;
        if (rest >= 4u * s->channels) frames += 1 + (rest - 4 * s->channels) / (4 * s->channels) * 8;
    } else {
        frames = bytes / (uint32_t)(s->channels * (s->in_bits > 8 ? 2 : 1));
    }
    if (ARG(2)) W32_(g_mem, ARG(2), frames);
    if (ARG(3)) W32_(g_mem, ARG(3), bytes);
    if (ARG(4)) W32_(g_mem, ARG(4), frames * (uint32_t)s->channels * 2);
    RET(noErr);
}
SHIM(_SoundConverterBeginConversion) { RET(gh_get(ARG(0), TAG_SNDCONV) ? noErr : paramErr); }
SHIM(_SoundConverterConvertBuffer) {
    /* (sc, const void *input, unsigned long inputFrames, void *output,
     *  unsigned long *outputFrames, unsigned long *outputBytes) */
    SndConv *s = gh_get(ARG(0), TAG_SNDCONV);
    if (!s) { RET(paramErr); return; }
    uint32_t want = ARG(2), done = 0;
    int16_t *out = PTR(ARG(3));
    const uint8_t *in = PTR(ARG(1));
    if (s->in_format == kMSIMAFormat && s->block_align) {
        while (done < want) {
            int n = decode_block(in, s->block_align, s->channels, (int)(want - done), out + done * (uint32_t)s->channels);
            if (n <= 0) break;
            done += (uint32_t)n;
            in += s->block_align;
        }
    } else if (s->in_bits <= 8) {
        for (uint32_t i = 0; i < want * (uint32_t)s->channels; i++) out[i] = (int16_t)((in[i] - 128) << 8);
        done = want;
    } else {
        memcpy(out, in, want * (uint32_t)s->channels * 2);
        done = want;
    }
    if (ARG(4)) W32_(g_mem, ARG(4), done);
    if (ARG(5)) W32_(g_mem, ARG(5), done * (uint32_t)s->channels * 2);
    RET(noErr);
}
SHIM(_SoundConverterEndConversion) {
    /* (sc, void *output, unsigned long *outputFrames, unsigned long *outputBytes): nothing buffered */
    if (ARG(2)) W32_(g_mem, ARG(2), 0);
    if (ARG(3)) W32_(g_mem, ARG(3), 0);
    RET(noErr);
}

const ShimDef shims_sound_converter[] = {
    SHIM_ENTRY(_SoundConverterOpen), SHIM_ENTRY(_SoundConverterClose), SHIM_ENTRY(_SoundConverterSetInfo),
    SHIM_ENTRY(_SoundConverterGetBufferSizes), SHIM_ENTRY(_SoundConverterBeginConversion),
    SHIM_ENTRY(_SoundConverterConvertBuffer), SHIM_ENTRY(_SoundConverterEndConversion),
    {0, 0},
};
