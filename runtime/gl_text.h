/* High-resolution text: QuickDraw keeps a scaled copy of the GWorlds the UI
 * toolkit draws font glyphs into (quickdraw.c); texture uploads that contain
 * those glyphs get the scaled glyphs instead (gl_text.c). */
#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *px;      /* the GWorld's 32-bit pixels (alpha in byte 3) */
    int w, h;
    uint32_t rowbytes;
    const uint8_t *hi;      /* the same drawing at `scale` times the size */
    uint32_t hi_rowbytes;
    int scale;
    const uint8_t *drawn;   /* per column: the scaled copy was drawn there (else it is only an upscale) */
} QDTextRow;

int qd_text_rows(QDTextRow *out, int max);
