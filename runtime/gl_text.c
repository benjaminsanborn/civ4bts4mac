/* Sharp UI text at high resolution.
 *
 * The UI toolkit draws each font's glyphs into a GWorld (one row of glyphs),
 * copies full-height chunks of that row into RGBA texture atlases (alpha =
 * the glyph's coverage, a gray level that follows it) and draws them at its layout size, which
 * the display port stretches to the screen. quickdraw.c keeps a copy of each
 * glyph row drawn at the screen's scale; here, an atlas upload is searched for
 * chunks of those rows (by matching alpha columns) and, when it has some, the
 * texture is stored at that scale with the sharp glyphs in place and the rest
 * of the image upscaled the way the texture's filter would have. Texture
 * coordinates are normalized, so the game's drawing is unchanged.
 *
 * CIV4_SHARP_TEXT=0 turns this off; CIV4_DEBUG_SHARP logs what it finds. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/OpenGL.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"
#include "gl_text.h"

float agl_display_scale(void);

static int enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("CIV4_SHARP_TEXT");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* ---- per-texture state: size at the game's resolution and the scale it is stored at ---- */
typedef struct {
    CGLShareGroupObj group;
    GLuint name;
    int w, h, scale;
    uint8_t *lo, *hi; /* when scaled: the game's pixels and what is stored for them */
    GLenum fmt, type; /* ... in this format */
} Tex;
#define MAX_TEX 4096
static Tex g_tex[MAX_TEX];

static Tex *tex_find(GLuint name, int create) {
    if (!name) return NULL; /* the default texture (and 0 marks a free slot) */
    CGLShareGroupObj group = CGLGetShareGroup(CGLGetCurrentContext());
    uint32_t i = (name * 2654435761u) % MAX_TEX;
    for (int n = 0; n < MAX_TEX; n++, i = (i + 1) % MAX_TEX) {
        Tex *t = &g_tex[i];
        if (t->name == name && t->group == group) return t;
        if (!t->name) {
            if (!create) return NULL;
            *t = (Tex){group, name, 0, 0, 1, NULL, NULL, 0, 0};
            return t;
        }
    }
    return NULL;
}
static void tex_reset(Tex *t, int w, int h) {
    free(t->lo);
    free(t->hi);
    t->lo = t->hi = NULL;
    t->w = w;
    t->h = h;
    t->scale = 1;
}
static void tex_forget(GLuint name) {
    Tex *t = tex_find(name, 0);
    if (!t) return;
    tex_reset(t, 0, 0);
    /* Re-insert the rest of the probe run so lookups past this slot still work. */
    t->name = 0;
    for (uint32_t i = (uint32_t)(t - g_tex + 1) % MAX_TEX; g_tex[i].name; i = (i + 1) % MAX_TEX) {
        Tex moved = g_tex[i];
        g_tex[i].name = 0;
        uint32_t j = (moved.name * 2654435761u) % MAX_TEX;
        while (g_tex[j].name) j = (j + 1) % MAX_TEX;
        g_tex[j] = moved;
    }
}

/* ---- finding glyph-row chunks in an image ---- */
typedef struct { int ux, uy, len, row, rx; } Block;

#define HASH_B 0x100000001b3ull
static uint64_t col_hash(const uint8_t *alpha, size_t stride, int h) {
    uint64_t v = 0;
    for (int i = 0; i < h; i++) v = v * HASH_B + alpha[i * stride];
    return v;
}
static int col_blank(const uint8_t *alpha, size_t stride, int h) {
    for (int i = 0; i < h; i++)
        if (alpha[i * stride]) return 0;
    return 1;
}
static int col_equal(const uint8_t *a, size_t as, const uint8_t *b, size_t bs, int h) {
    for (int i = 0; i < h; i++)
        if (a[i * as] != b[i * bs]) return 0;
    return 1;
}

/* Hash table of a row height's non-blank glyph columns -> (row, x); a column
 * can occur more than once (repeated strokes, rows drawn twice). */
typedef struct { uint64_t key; int row, x; } Slot;

/* Image `u` (uw x uh, 4 bytes per pixel, alpha in byte 3, `stride` bytes per row). */
static int find_blocks(const uint8_t *u, int uw, int uh, size_t stride, const QDTextRow *rows, int nrows,
                       Block *out, int max) {
    int nb = 0;
    uint64_t *hashes = NULL;
    for (int hi = 0; hi < nrows; hi++) {
        int H = rows[hi].h;
        int seen = 0; /* each height once */
        for (int k = 0; k < hi; k++) seen |= rows[k].h == H;
        if (seen || H > uh || H < 4) continue;
        size_t cap = 64;
        for (int k = 0; k < nrows; k++) if (rows[k].h == H) cap += (size_t)rows[k].w;
        size_t nslots = 1;
        while (nslots < cap * 2) nslots <<= 1;
        Slot *tab = calloc(nslots, sizeof *tab);
        for (int k = 0; k < nrows; k++) {
            if (rows[k].h != H) continue;
            for (int x = 0; x < rows[k].w; x++) {
                const uint8_t *a = rows[k].px + 4 * x + 3;
                if (col_blank(a, rows[k].rowbytes, H)) continue;
                uint64_t key = col_hash(a, rows[k].rowbytes, H) | 1;
                size_t i = key & (nslots - 1);
                while (tab[i].key) i = (i + 1) & (nslots - 1);
                tab[i] = (Slot){key, k, x};
            }
        }
        /* Rolling column hashes of the image at every (y0, x). */
        int bands = uh - H + 1;
        hashes = realloc(hashes, sizeof *hashes * (size_t)bands * (size_t)uw);
        uint64_t top = 1;
        for (int i = 1; i < H; i++) top *= HASH_B;
        for (int x = 0; x < uw; x++) {
            const uint8_t *a = u + 4 * x + 3;
            uint64_t v = col_hash(a, stride, H);
            hashes[x] = v;
            for (int y = 1; y < bands; y++) {
                v = (v - a[(y - 1) * stride] * top) * HASH_B + a[(y - 1 + H) * stride];
                hashes[(size_t)y * uw + x] = v;
            }
        }
        for (int y = 0; y < bands && nb < max; y++) {
            for (int x = 0; x < uw && nb < max; x++) {
                uint64_t key = hashes[(size_t)y * uw + x] | 1;
                const uint8_t *ua = u + (size_t)y * stride + 3;
                if (col_blank(ua + 4 * x, stride, H)) continue;
                /* Of the places this column occurs, take the one that matches furthest
                 * both ways. */
                int best = -1, l = 0, rr = 0;
                for (size_t i = key & (nslots - 1); tab[i].key; i = (i + 1) & (nslots - 1)) {
                    if (tab[i].key != key) continue;
                    const QDTextRow *r = &rows[tab[i].row];
                    const uint8_t *ra = r->px + 3;
                    int rx = tab[i].x, cl = 0, cr = 0;
                    if (!r->drawn[rx] || !col_equal(ua + 4 * x, stride, ra + 4 * rx, r->rowbytes, H)) continue;
                    while (x - cl > 0 && rx - cl > 0 && r->drawn[rx - cl - 1] &&
                           col_equal(ua + 4 * (x - cl - 1), stride, ra + 4 * (rx - cl - 1), r->rowbytes, H)) cl++;
                    while (x + cr + 1 < uw && rx + cr + 1 < r->w && r->drawn[rx + cr + 1] &&
                           col_equal(ua + 4 * (x + cr + 1), stride, ra + 4 * (rx + cr + 1), r->rowbytes, H)) cr++;
                    if (best < 0 || cl + cr > l + rr) best = (int)i, l = cl, rr = cr;
                }
                if (best < 0) continue;
                int rx = tab[best].x;
                /* Glyphs in an atlas are gray, at least as light as they are opaque;
                 * anything else is a chance match with other art. */
                int ok = 1, ink = 0;
                for (int yy = 0; yy < H && ok; yy++) {
                    const uint8_t *p = u + (size_t)(y + yy) * stride + 4 * (x - l);
                    for (int xx = 0; xx <= l + rr; xx++, p += 4)
                        if (p[3]) {
                            ink++;
                            if (p[0] != p[1] || p[1] != p[2] || p[0] < p[3]) { ok = 0; break; }
                        }
                }
                if (ok && ink >= 3) out[nb++] = (Block){x - l, y, l + rr + 1, tab[best].row, rx - l};
                x += rr;
            }
        }
        free(tab);
    }
    free(hashes);
    return nb;
}

/* ---- building the scaled image ---- */
/* `u` scaled up `s` times, as linear filtering would draw it at that scale. */
static void upscale(const uint8_t *u, int uw, int uh, size_t stride, int s, uint8_t *out) {
    int ow = uw * s;
    for (int oy = 0; oy < uh * s; oy++) {
        float fy = (oy + 0.5f) / s - 0.5f;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < uh ? y0 + 1 : uh - 1;
        float ty = fy < 0 ? 0 : fy - y0;
        for (int ox = 0; ox < ow; ox++) {
            uint8_t *o = out + ((size_t)oy * ow + ox) * 4;
            float fx = (ox + 0.5f) / s - 0.5f;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < uw ? x0 + 1 : uw - 1;
            float tx = fx < 0 ? 0 : fx - x0;
            const uint8_t *a = u + (size_t)y0 * stride + 4 * x0, *b = u + (size_t)y0 * stride + 4 * x1;
            const uint8_t *c = u + (size_t)y1 * stride + 4 * x0, *d = u + (size_t)y1 * stride + 4 * x1;
            for (int k = 0; k < 4; k++) {
                float top = a[k] + (b[k] - a[k]) * tx, bot = c[k] + (d[k] - c[k]) * tx;
                o[k] = (uint8_t)(top + (bot - top) * ty + 0.5f);
            }
        }
    }
}

/* The atlas stores a glyph's coverage as alpha and a gray level that depends
 * on it; learn that mapping from the glyphs found. */
static void learn_gray(const uint8_t *u, size_t stride, const Block *b, int nb, const QDTextRow *rows, uint8_t lut[256]) {
    int known[256] = {0};
    for (int i = 0; i < nb; i++)
        for (int y = 0; y < rows[b[i].row].h; y++) {
            const uint8_t *p = u + (size_t)(b[i].uy + y) * stride + 4 * (size_t)b[i].ux;
            for (int x = 0; x < b[i].len; x++, p += 4)
                if (!known[p[3]]) known[p[3]] = 1, lut[p[3]] = p[0];
        }
    if (!known[0]) known[0] = 1, lut[0] = 0;
    if (!known[255]) known[255] = 1, lut[255] = 255;
    for (int a = 1, prev = 0; a < 256; a++) {
        if (!known[a]) continue;
        for (int k = prev + 1; k < a; k++) lut[k] = (uint8_t)(lut[prev] + (lut[a] - lut[prev]) * (k - prev) / (a - prev));
        prev = a;
    }
}

static void paste(uint8_t *out, int ow, const Block *b, const QDTextRow *r, int s, const uint8_t lut[256]) {
    for (int y = 0; y < r->h * s; y++) {
        const uint8_t *src = r->hi + (size_t)y * r->hi_rowbytes + 4 * (size_t)(b->rx * s);
        uint8_t *dst = out + ((size_t)(b->uy * s + y) * ow + (size_t)b->ux * s) * 4;
        for (int x = 0; x < b->len * s; x++, src += 4, dst += 4) {
            dst[3] = src[3];
            dst[0] = dst[1] = dst[2] = lut[src[3]];
        }
    }
}

/* The part of a texture at (x, y) whose pixels the game uploads (`u`), at the
 * text scale: upscaled, with the glyphs of the text rows in place, and where
 * the texture is already stored scaled, what was stored for pixels that did
 * not change (the toolkit reuses its glyph rows, so glyphs added to an atlas
 * earlier are no longer in them). NULL if the texture is not scaled and the
 * part has no glyphs, unless `force`. */
static uint8_t *scaled_part(const Tex *t, const uint8_t *u, int uw, int uh, size_t stride, int x, int y, int force,
                            int *scale_out) {
    QDTextRow all[256], rows[256];
    int nall = qd_text_rows(all, 256), nrows = 0;
    int s = t->scale > 1 ? t->scale : nall ? all[0].scale : 0;
    if (s < 2) return NULL;
    for (int i = 0; i < nall; i++)
        if (all[i].scale == s) rows[nrows++] = all[i];
    static Block blocks[8192];
    int nb = nrows ? find_blocks(u, uw, uh, stride, rows, nrows, blocks, 8192) : 0;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("CIV4_DEBUG_SHARP") != NULL;
    if (dbg) rt_log("sharp text: %dx%d at %d,%d of texture %u: %d glyph chunks from %d rows", uw, uh, x, y, t->name, nb, nrows);
    if (!nb && !force && t->scale == 1) return NULL;
    size_t ow = (size_t)uw * s;
    uint8_t *out = malloc(ow * (size_t)uh * s * 4), *found = calloc((size_t)uw * uh, 1);
    upscale(u, uw, uh, stride, s, out);
    uint8_t lut[256];
    learn_gray(u, stride, blocks, nb, rows, lut);
    for (int i = 0; i < nb; i++) {
        const Block *b = &blocks[i];
        paste(out, (int)ow, b, &rows[b->row], s, lut);
        for (int yy = 0; yy < rows[b->row].h; yy++) memset(found + (size_t)(b->uy + yy) * uw + b->ux, 1, (size_t)b->len);
    }
    if (t->lo)
        for (int yy = 0; yy < uh; yy++)
            for (int xx = 0; xx < uw; xx++) {
                if (found[(size_t)yy * uw + xx] ||
                    memcmp(t->lo + ((size_t)(y + yy) * t->w + x + xx) * 4, u + (size_t)yy * stride + 4 * (size_t)xx, 4))
                    continue;
                for (int k = 0; k < s; k++)
                    memcpy(out + ((size_t)(yy * s + k) * ow + (size_t)xx * s) * 4,
                           t->hi + ((size_t)((y + yy) * s + k) * t->w * s + (size_t)(x + xx) * s) * 4, 4 * (size_t)s);
            }
    free(found);
    *scale_out = s;
    return out;
}

/* Uploads with plain unpack state (and no client storage: the buffer is freed). */
typedef struct { GLint row_length, skip_pixels, skip_rows, alignment, client_storage; } Unpack;
static Unpack unpack_save(void) {
    Unpack p;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &p.row_length);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &p.skip_pixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &p.skip_rows);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &p.alignment);
    glGetIntegerv(GL_UNPACK_CLIENT_STORAGE_APPLE, &p.client_storage);
    return p;
}
static void unpack_set(const Unpack *p) {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, p->row_length);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, p->skip_pixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, p->skip_rows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, p->alignment);
    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, p->client_storage);
}
static const Unpack k_plain = {0, 0, 0, 4, 0};

/* Uploads this module can scale: 32-bit pixels with alpha in the fourth byte. */
static int eligible(GLenum target, GLint level, GLenum fmt, GLenum type) {
    if (target != GL_TEXTURE_2D || level != 0) return 0;
    if (fmt == GL_BGRA) return type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_BYTE;
    return fmt == GL_RGBA && type == GL_UNSIGNED_BYTE;
}
static GLuint bound_texture(void) {
    GLint name = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &name);
    return (GLuint)name;
}
/* The game's pixels and their row stride, given its unpack state; NULL if they are in a buffer object. */
static const uint8_t *game_pixels(uint32_t addr, int w, const Unpack *p, size_t *stride) {
    GLint pbo = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &pbo);
    if (pbo || !addr) return NULL;
    *stride = 4 * (size_t)(p->row_length ? p->row_length : w);
    return (const uint8_t *)PTR(addr) + (size_t)p->skip_rows * *stride + 4 * (size_t)p->skip_pixels;
}
static int worth_scanning(int w, int h) {
    return enabled() && agl_display_scale() >= 1.25f && (size_t)w * h <= (1u << 21);
}
static GLint internal_format(void) {
    GLint ifmt = GL_RGBA;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
    return ifmt;
}
/* Store the bound texture's level 0 (`pixels`, its whole size at `scale`) with plain unpack state. */
static void store(Tex *t, GLint ifmt, GLenum fmt, GLenum type, const uint8_t *pixels, int scale, const Unpack *game) {
    t->fmt = fmt;
    t->type = type;
    unpack_set(&k_plain);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, t->w * scale, t->h * scale, 0, fmt, type, pixels);
    unpack_set(game);
    t->scale = scale;
}
/* Keep what was uploaded for the part at (x, y): the game's pixels and the scaled ones. */
static void remember(Tex *t, int x, int y, int w, int h, const uint8_t *u, size_t stride, const uint8_t *hi) {
    int s = t->scale;
    if (!t->lo) {
        t->lo = malloc((size_t)t->w * t->h * 4);
        t->hi = malloc((size_t)t->w * s * t->h * s * 4);
    }
    for (int yy = 0; yy < h; yy++) memcpy(t->lo + ((size_t)(y + yy) * t->w + x) * 4, u + (size_t)yy * stride, 4 * (size_t)w);
    for (int yy = 0; yy < h * s; yy++)
        memcpy(t->hi + ((size_t)(y * s + yy) * t->w * s + (size_t)x * s) * 4, hi + (size_t)yy * w * s * 4, 4 * (size_t)w * s);
}

SHIM(_glTexImage2D) {
    GLenum target = ARG(0), fmt = ARG(6), type = ARG(7);
    GLint level = (GLint)ARG(1), ifmt = (GLint)ARG(2);
    int w = (int)ARG(3), h = (int)ARG(4);
    Tex *t = target == GL_TEXTURE_2D ? tex_find(bound_texture(), eligible(target, level, fmt, type)) : NULL;
    if (t && level == 0) {
        tex_reset(t, eligible(target, level, fmt, type) ? w : 0, h);
        Unpack p = unpack_save();
        size_t stride;
        const uint8_t *px = t->w ? game_pixels(ARG(8), w, &p, &stride) : NULL;
        int s;
        uint8_t *hi;
        if (px && worth_scanning(w, h) && (hi = scaled_part(t, px, w, h, stride, 0, 0, 0, &s))) {
            store(t, ifmt, fmt, type, hi, s, &p);
            remember(t, 0, 0, w, h, px, stride, hi);
            free(hi);
            return;
        }
    } else if (t && t->scale > 1 && level > 0) {
        /* Mipmaps of its own: level 0 goes back to the game's size so the levels fit. */
        Unpack p = unpack_save();
        uint8_t *lo = t->lo;
        t->lo = NULL;
        store(t, internal_format(), t->fmt, t->type, lo, 1, &p);
        free(lo);
        tex_reset(t, t->w, t->h);
    }
    glTexImage2D(target, level, ifmt, (GLsizei)w, (GLsizei)h, (GLint)ARG(5), fmt, type, PTR(ARG(8)));
}

SHIM(_glTexSubImage2D) {
    GLenum target = ARG(0), fmt = ARG(6), type = ARG(7);
    GLint level = (GLint)ARG(1), x = (GLint)ARG(2), y = (GLint)ARG(3);
    int w = (int)ARG(4), h = (int)ARG(5);
    Tex *t = eligible(target, level, fmt, type) ? tex_find(bound_texture(), 0) : NULL;
    Unpack p;
    size_t stride;
    const uint8_t *px = NULL;
    if (t && t->w && x >= 0 && y >= 0 && x + w <= t->w && y + h <= t->h) {
        p = unpack_save();
        px = game_pixels(ARG(8), w, &p, &stride);
    }
    int s;
    uint8_t *hi;
    if (px && t->scale > 1) {
        /* Stored scaled: scale this part too. */
        hi = scaled_part(t, px, w, h, stride, x, y, 1, &s);
        unpack_set(&k_plain);
        glTexSubImage2D(target, 0, x * s, y * s, w * s, h * s, fmt, type, hi);
        unpack_set(&p);
        remember(t, x, y, w, h, px, stride, hi);
        free(hi);
        return;
    }
    if (px && worth_scanning(t->w, t->h) && (hi = scaled_part(t, px, w, h, stride, x, y, 0, &s))) {
        /* This part has glyphs: store the whole texture scaled. */
        GLint ifmt = internal_format();
        if (x || y || w != t->w || h != t->h) {
            free(hi);
            uint8_t *whole = malloc((size_t)t->w * t->h * 4);
            unpack_set(&k_plain);
            glGetTexImage(GL_TEXTURE_2D, 0, fmt, type, whole);
            for (int yy = 0; yy < h; yy++)
                memcpy(whole + ((size_t)(y + yy) * t->w + x) * 4, px + (size_t)yy * stride, 4 * (size_t)w);
            hi = scaled_part(t, whole, t->w, t->h, 4 * (size_t)t->w, 0, 0, 1, &s);
            store(t, ifmt, fmt, type, hi, s, &p);
            remember(t, 0, 0, t->w, t->h, whole, 4 * (size_t)t->w, hi);
            free(whole);
        } else {
            store(t, ifmt, fmt, type, hi, s, &p);
            remember(t, 0, 0, w, h, px, stride, hi);
        }
        free(hi);
        return;
    }
    glTexSubImage2D(target, level, x, y, (GLsizei)w, (GLsizei)h, fmt, type, PTR(ARG(8)));
}

SHIM(_glDeleteTextures) {
    GLsizei n = (GLsizei)ARG(0);
    const GLuint *names = (const GLuint *)PTR(ARG(1));
    for (GLsizei i = 0; i < n; i++) tex_forget(names[i]);
    glDeleteTextures(n, names);
}

const ShimDef shims_gl_text[] = {
    SHIM_ENTRY(_glTexImage2D), SHIM_ENTRY(_glTexSubImage2D), SHIM_ENTRY(_glDeleteTextures), {0, 0},
};
