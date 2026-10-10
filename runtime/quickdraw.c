/* QuickDraw offscreen drawing: GWorlds, PixMaps, colors, pen, rectangles and
 * text. The game's UI toolkit renders font glyphs with QuickDraw text into a
 * GWorld and reads the pixels back, so pixels must live in guest memory.
 *
 * A GWorld is a CGBitmapContext whose buffer is guest memory (32-bit ARGB,
 * big-endian, like QuickDraw's k32ARGBPixelFormat), flipped to y-down. Text
 * uses CoreText with QuickDraw's font family / face / size state. */
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <string.h>

#include "handles.h"

#define TAG_GWORLD 0x4757524cu /* 'GWRL' */

/* QuickDraw state shared by every port. */
typedef struct {
    uint16_t fore[3], back[3]; /* RGBColor */
    int16_t pen_h, pen_v, pen_w, pen_hgt;
    int16_t font, face, size, mode;
} PortState;

typedef struct {
    uint32_t g;
    int w, h, depth;    /* depth in bits per pixel */
    uint32_t format;    /* pixelFormat: a depth code, or an OSType for 32-bit layouts */
    int16_t left, top;  /* bounds origin */
    uint32_t pixels;    /* guest buffer */
    uint32_t rowbytes;
    int own_pixels;
    uint32_t pixmap;    /* guest PixMapHandle */
    CGContextRef ctx;
    PortState st;
} HGWorld;

static PortState g_window_state = {{0, 0, 0}, {0xffff, 0xffff, 0xffff}, 0, 0, 1, 1, 0, 0, 12, 1};
static uint32_t g_port; /* current port/GWorld guest handle */

static HGWorld *gw_of(uint32_t g) { return gh_get(g, TAG_GWORLD); }
static PortState *state_of(uint32_t port) {
    HGWorld *w = gw_of(port);
    return w ? &w->st : &g_window_state;
}
static PortState *cur(void) { return state_of(g_port); }
uint32_t qd_current_port(void) { return g_port; }

/* ---- fonts: family IDs <-> names ---- */
#define MAX_FONTS 128
static char *g_font_names[MAX_FONTS];
static int g_nfonts;

static int16_t family_id(const char *name) {
    for (int i = 0; i < g_nfonts; i++)
        if (!strcasecmp(g_font_names[i], name)) return (int16_t)(1000 + i);
    if (g_nfonts == MAX_FONTS) return 0;
    g_font_names[g_nfonts] = strdup(name);
    return (int16_t)(1000 + g_nfonts++);
}
static const char *family_name(int16_t id) {
    if (id >= 1000 && id - 1000 < g_nfonts) return g_font_names[id - 1000];
    return id == 0 ? "Lucida Grande" : "Helvetica"; /* systemFont / applFont */
}

static CTFontRef make_font(const PortState *s) {
    CFStringRef name = CFStringCreateWithCString(NULL, family_name(s->font), kCFStringEncodingMacRoman);
    CGFloat size = s->size > 0 ? s->size : 12;
    CTFontRef f = CTFontCreateWithName(name, size, NULL);
    CFRelease(name);
    CTFontSymbolicTraits traits = 0;
    if (s->face & 1) traits |= kCTFontBoldTrait;
    if (s->face & 2) traits |= kCTFontItalicTrait;
    if (traits) {
        CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(f, size, NULL, traits, traits);
        if (styled) { CFRelease(f); f = styled; }
    }
    return f;
}

static CTLineRef make_line(const PortState *s, const uint8_t *text, uint32_t n, CGColorRef color) {
    CFStringRef str = CFStringCreateWithBytes(NULL, text, (CFIndex)n, kCFStringEncodingMacRoman, false);
    if (!str) str = CFStringCreateWithCString(NULL, "", kCFStringEncodingMacRoman);
    CTFontRef font = make_font(s);
    const void *keys[] = {kCTFontAttributeName, kCTForegroundColorAttributeName};
    const void *vals[] = {font, color};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, color ? 2 : 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    CFRelease(attrs);
    CFRelease(font);
    CFRelease(str);
    return line;
}

static CGColorRef rgb(const uint16_t c[3]) {
    CGFloat comps[4] = {c[0] / 65535.0, c[1] / 65535.0, c[2] / 65535.0, 1.0};
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateDeviceRGB();
    return CGColorCreate(cs, comps);
}

/* ---- PixMap (guest struct, 2-byte packed, 50 bytes) ----
 * baseAddr 0, rowBytes 4, bounds 6, pmVersion 14, packType 16, packSize 18,
 * hRes 22, vRes 26, pixelType 30, pixelSize 32, cmpCount 34, cmpSize 36,
 * pixelFormat 38, pmTable 42, pmExt 46 */
static uint32_t make_pixmap(HGWorld *w) {
    uint32_t pm = gcalloc(1, 64), h = gmalloc(4);
    W32_(g_mem, h, pm);
    W32_(g_mem, pm + 0, w->pixels);
    W16_(g_mem, pm + 4, 0x8000 | (w->rowbytes & 0x3fff));
    int16_t b[4] = {w->top, w->left, (int16_t)(w->top + w->h), (int16_t)(w->left + w->w)};
    memcpy(PTR(pm + 6), b, 8);
    W32_(g_mem, pm + 22, 72u << 16);
    W32_(g_mem, pm + 26, 72u << 16);
    int direct = w->depth >= 16;
    W16_(g_mem, pm + 30, direct ? 16 : 0); /* RGBDirect or indexed */
    W16_(g_mem, pm + 32, (uint32_t)w->depth);
    W16_(g_mem, pm + 34, direct ? 3 : 1);
    W16_(g_mem, pm + 36, w->depth == 32 || w->depth == 24 ? 8 : w->depth == 16 ? 5 : (uint32_t)w->depth);
    W32_(g_mem, pm + 38, w->format); /* k1/k8/k16/k24/k32 codes equal the depth; else an OSType */
    if (!direct) {
        /* Indexed: a grayscale color table handle (ctSeed 0, ctFlags 2, ctSize n-1, entries {value, RGB}). */
        uint32_t n = 1u << w->depth, ct = gcalloc(1, 8 + 8 * n), cth = gmalloc(4);
        W32_(g_mem, cth, ct);
        W16_(g_mem, ct + 6, n - 1);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)(0xffff - i * 0xffff / (n - 1));
            W16_(g_mem, ct + 8 + 8 * i, i);
            W16_(g_mem, ct + 10 + 8 * i, v);
            W16_(g_mem, ct + 12 + 8 * i, v);
            W16_(g_mem, ct + 14 + 8 * i, v);
        }
        W32_(g_mem, pm + 42, cth);
    }
    return h;
}

static int depth_of(uint32_t format_or_depth) {
    switch (format_or_depth) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: return (int)format_or_depth;
    case 0: return 32;
    default: return 32; /* OSType formats ('BGRA', 'ARGB'...) are 32-bit */
    }
}

/* CG layout for a 32-bit pixel format, as it lies in (little-endian) memory. */
static CGBitmapInfo bitmap_info(uint32_t format) {
    switch (format) {
    case 'BGRA': return kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
    case 'RGBA': return kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big;
    case 'ABGR': return kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Little;
    default: return kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Big; /* k32ARGBPixelFormat */
    }
}

static HGWorld *new_gworld(uint32_t format, int left, int top, int right, int bottom, uint32_t pixels, uint32_t rowbytes) {
    HGWorld *w = calloc(1, sizeof *w);
    int depth = depth_of(format);
    w->depth = depth;
    w->format = format > 32 || format == 0 ? (format == 0 ? 32 : format) : (uint32_t)depth;
    w->left = (int16_t)left;
    w->top = (int16_t)top;
    w->w = right - left > 0 ? right - left : 1;
    w->h = bottom - top > 0 ? bottom - top : 1;
    w->rowbytes = rowbytes ? rowbytes : (((uint32_t)w->w * (uint32_t)depth + 31) / 32) * 4;
    if (pixels) {
        w->pixels = pixels;
    } else {
        w->pixels = gcalloc(1, w->rowbytes * (uint32_t)w->h);
        w->own_pixels = 1;
    }
    if (depth == 32) { /* only 32-bit GWorlds are drawn into */
        static CGColorSpaceRef cs;
        if (!cs) cs = CGColorSpaceCreateDeviceRGB();
        w->ctx = CGBitmapContextCreate(PTR(w->pixels), (size_t)w->w, (size_t)w->h, 8, w->rowbytes, cs,
                                       bitmap_info(w->format));
        /* QuickDraw coordinates: origin at bounds top-left, y down. */
        CGContextTranslateCTM(w->ctx, 0, w->h);
        CGContextScaleCTM(w->ctx, 1, -1);
        CGContextTranslateCTM(w->ctx, -left, -top);
        CGContextSetTextMatrix(w->ctx, CGAffineTransformMakeScale(1, -1));
    }
    w->st = g_window_state;
    w->st.pen_h = (int16_t)left;
    w->st.pen_v = (int16_t)top;
    w->g = gh_new(TAG_GWORLD, w);
    w->pixmap = make_pixmap(w);
    return w;
}

SHIM(_NewGWorld) {
    /* (GWorldPtr *out, short depth, const Rect *bounds, CTabHandle, GDHandle, GWorldFlags) */
    int16_t r[4];
    memcpy(r, PTR(ARG(2)), 8);
    HGWorld *w = new_gworld((uint16_t)ARG(1), r[1], r[0], r[3], r[2], 0, 0);
    W32_(g_mem, ARG(0), w->g);
    RET(0);
}
SHIM(_QTNewGWorldFromPtr) {
    /* (GWorldPtr *out, OSType pixelFormat, const Rect *bounds, CTabHandle, GDHandle, flags, void *base, long rowBytes) */
    int16_t r[4];
    memcpy(r, PTR(ARG(2)), 8);
    HGWorld *w = new_gworld(ARG(1), r[1], r[0], r[3], r[2], ARG(6), ARG(7));
    W32_(g_mem, ARG(0), w->g);
    RET(0);
}
SHIM(_DisposeGWorld) {
    HGWorld *w = gw_of(ARG(0));
    if (!w) return;
    if (g_port == w->g) g_port = 0;
    if (w->ctx) CGContextRelease(w->ctx);
    if (w->own_pixels) gfree(w->pixels);
    gfree(R32_(g_mem, w->pixmap));
    gfree(w->pixmap);
    gh_free(w->g);
    free(w);
}
SHIM(_GetGWorldPixMap) { HGWorld *w = gw_of(ARG(0)); RET(w ? w->pixmap : 0); }
SHIM(_GetPortPixMap) { HGWorld *w = gw_of(ARG(0)); RET(w ? w->pixmap : 0); }
SHIM(_GetGWorldDevice) { RET(0x6764); }
SHIM(_LockPixels) { RET(1); }
SHIM(_UnlockPixels) {}
SHIM(_GetPixBaseAddr) { RET(ARG(0) ? R32_(g_mem, R32_(g_mem, ARG(0))) : 0); }
SHIM(_GetPixRowBytes) { RET(ARG(0) ? R16_(g_mem, R32_(g_mem, ARG(0)) + 4) & 0x3fff : 0); }
SHIM(_GetPixBounds) {
    if (ARG(0)) memcpy(PTR(ARG(1)), PTR(R32_(g_mem, ARG(0)) + 6), 8);
    RET(ARG(1));
}
SHIM(_GetPortBitMapForCopyBits) {
    /* const BitMap*: the PixMap itself (dereferenced handle). */
    HGWorld *w = gw_of(ARG(0));
    RET(w ? R32_(g_mem, w->pixmap) : 0);
}

/* Current port. Window ports keep shared default state. */
SHIM(_GetGWorld) {
    W32_(g_mem, ARG(0), g_port);
    if (ARG(1)) W32_(g_mem, ARG(1), 0x6764);
}
SHIM(_SetGWorld) { g_port = ARG(0); }
SHIM(_GetPort) { W32_(g_mem, ARG(0), g_port); }
SHIM(_SetPort) { g_port = ARG(0); }

/* ---- colors ---- */
static void get_rgb(uint32_t g, uint16_t c[3]) { memcpy(c, PTR(g), 6); }
static void put_rgb(uint32_t g, const uint16_t c[3]) { memcpy(PTR(g), c, 6); }
SHIM(_RGBForeColor) { get_rgb(ARG(0), cur()->fore); }
SHIM(_RGBBackColor) { get_rgb(ARG(0), cur()->back); }
SHIM(_GetForeColor) { put_rgb(ARG(0), cur()->fore); }
SHIM(_GetBackColor) { put_rgb(ARG(0), cur()->back); }
SHIM(_GetPortForeColor) { put_rgb(ARG(1), state_of(ARG(0))->fore); RET(ARG(1)); }
SHIM(_GetPortBackColor) { put_rgb(ARG(1), state_of(ARG(0))->back); RET(ARG(1)); }

/* ---- pen and shapes (drawing only lands in GWorlds) ---- */
SHIM(_MoveTo) { cur()->pen_h = (int16_t)ARG(0); cur()->pen_v = (int16_t)ARG(1); }
SHIM(_PenNormal) { cur()->pen_w = cur()->pen_hgt = 1; }
SHIM(_PenSize) { cur()->pen_w = (int16_t)ARG(0); cur()->pen_hgt = (int16_t)ARG(1); }
SHIM(_GetPenState) {
    /* PenState {Point pnLoc; Point pnSize; short pnMode; Pattern pnPat} */
    PortState *s = cur();
    int16_t v[5] = {s->pen_v, s->pen_h, s->pen_hgt, s->pen_w, 8 /* patCopy */};
    memcpy(PTR(ARG(0)), v, 10);
    memset(PTR(ARG(0) + 10), 0xff, 8);
}

static CGRect qd_rect(uint32_t g) {
    int16_t r[4];
    memcpy(r, PTR(g), 8);
    return CGRectMake(r[1], r[0], r[3] - r[1], r[2] - r[0]);
}
static void fill(const uint16_t c[3], CGRect r) {
    HGWorld *w = gw_of(g_port);
    if (!w || !w->ctx) return;
    CGColorRef col = rgb(c);
    CGContextSetFillColorWithColor(w->ctx, col);
    CGContextFillRect(w->ctx, r);
    CGColorRelease(col);
}
SHIM(_EraseRect) { fill(cur()->back, qd_rect(ARG(0))); }
SHIM(_PaintRect) { fill(cur()->fore, qd_rect(ARG(0))); }
SHIM(_FillRect) {
    /* Patterns are treated as solid: all-zero bits = white, otherwise the fore color. */
    static const uint16_t white[3] = {0xffff, 0xffff, 0xffff};
    const uint8_t *pat = PTR(ARG(1));
    int any = 0;
    for (int i = 0; i < 8; i++) any |= pat[i];
    fill(any ? cur()->fore : white, qd_rect(ARG(0)));
}
SHIM(_FrameRect) {
    HGWorld *w = gw_of(g_port);
    if (!w || !w->ctx) return;
    CGColorRef col = rgb(cur()->fore);
    CGContextSetStrokeColorWithColor(w->ctx, col);
    CGContextStrokeRectWithWidth(w->ctx, CGRectInset(qd_rect(ARG(0)), 0.5, 0.5), 1);
    CGColorRelease(col);
}
SHIM(_LineTo) {
    PortState *s = cur();
    HGWorld *w = gw_of(g_port);
    if (w && w->ctx) {
        CGColorRef col = rgb(s->fore);
        CGContextSetStrokeColorWithColor(w->ctx, col);
        CGContextSetLineWidth(w->ctx, s->pen_w);
        CGContextMoveToPoint(w->ctx, s->pen_h + 0.5, s->pen_v + 0.5);
        CGContextAddLineToPoint(w->ctx, (int16_t)ARG(0) + 0.5, (int16_t)ARG(1) + 0.5);
        CGContextStrokePath(w->ctx);
        CGColorRelease(col);
    }
    s->pen_h = (int16_t)ARG(0);
    s->pen_v = (int16_t)ARG(1);
}

/* ---- text ---- */
SHIM(_TextFont) { cur()->font = (int16_t)ARG(0); }
SHIM(_TextFace) { cur()->face = (int16_t)ARG(0); }
SHIM(_TextSize) { cur()->size = (int16_t)ARG(0); }
SHIM(_TextMode) { cur()->mode = (int16_t)ARG(0); }
SHIM(_GetPortTextFont) { RET((uint16_t)state_of(ARG(0))->font); }
SHIM(_GetPortTextFace) { RET((uint16_t)state_of(ARG(0))->face); }
SHIM(_GetPortTextSize) { RET((uint16_t)state_of(ARG(0))->size); }
SHIM(_FMGetFontFamilyFromName) {
    /* (ConstStr255Param name) -> FMFontFamily */
    char name[256];
    uint32_t n = R8_(g_mem, ARG(0));
    memcpy(name, PTR(ARG(0) + 1), n);
    name[n] = 0;
    RET((uint16_t)family_id(name));
}
SHIM(_FMGetFontFamilyName) {
    /* (FMFontFamily, Str255 out) */
    const char *n = family_name((int16_t)ARG(0));
    size_t len = strlen(n);
    W8_(g_mem, ARG(1), (uint32_t)len);
    memcpy(PTR(ARG(1) + 1), n, len);
    RET(0);
}
SHIM(_GetFontInfo) {
    /* FontInfo {short ascent, descent, widMax, leading} */
    CTFontRef f = make_font(cur());
    int16_t fi[4] = {(int16_t)ceil(CTFontGetAscent(f)), (int16_t)ceil(CTFontGetDescent(f)),
                     (int16_t)ceil(CTFontGetBoundingBox(f).size.width), (int16_t)ceil(CTFontGetLeading(f))};
    memcpy(PTR(ARG(0)), fi, 8);
    CFRelease(f);
}
static double text_width(const uint8_t *text, uint32_t n) {
    CTLineRef line = make_line(cur(), text, n, NULL);
    double w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
    CFRelease(line);
    return w;
}
SHIM(_TextWidth) { RET((uint32_t)(int16_t)lround(text_width((const uint8_t *)PTR(ARG(0)) + (int16_t)ARG(1), (uint16_t)ARG(2)))); }
SHIM(_CharWidth) { uint8_t c = (uint8_t)ARG(0); RET((uint32_t)(int16_t)lround(text_width(&c, 1))); }
SHIM(_StringWidth) { RET((uint32_t)(int16_t)lround(text_width((const uint8_t *)PTR(ARG(0)) + 1, R8_(g_mem, ARG(0))))); }
SHIM(_DrawText) {
    /* (textBuf, firstByte, byteCount) at the pen (baseline), advancing it */
    PortState *s = cur();
    const uint8_t *text = (const uint8_t *)PTR(ARG(0)) + (int16_t)ARG(1);
    uint32_t n = (uint16_t)ARG(2);
    HGWorld *w = gw_of(g_port);
    CGColorRef col = rgb(s->fore);
    CTLineRef line = make_line(s, text, n, col);
    if (w && w->ctx) {
        CGContextSetShouldAntialias(w->ctx, s->size > 9);
        CGContextSetTextPosition(w->ctx, s->pen_h, s->pen_v);
        CTLineDraw(line, w->ctx);
    }
    s->pen_h = (int16_t)(s->pen_h + lround(CTLineGetTypographicBounds(line, NULL, NULL, NULL)));
    CFRelease(line);
    CGColorRelease(col);
}

/* ---- CopyBits between pixmaps (srcCopy, nearest-neighbour scaling) ---- */
SHIM(_CopyBits) {
    /* (const BitMap *src, const BitMap *dst, const Rect *srcRect, const Rect *dstRect, short mode, RgnHandle mask) */
    uint32_t sp = ARG(0), dp = ARG(1);
    int16_t sr[4], dr[4];
    memcpy(sr, PTR(ARG(2)), 8);
    memcpy(dr, PTR(ARG(3)), 8);
    /* Both are PixMaps: baseAddr 0, rowBytes 4, bounds 6. Copy with nearest-neighbour scaling. */
    uint32_t sbase = R32_(g_mem, sp), dbase = R32_(g_mem, dp);
    uint32_t srow = R16_(g_mem, sp + 4) & 0x3fff, drow = R16_(g_mem, dp + 4) & 0x3fff;
    int16_t sb[4], db[4];
    memcpy(sb, PTR(sp + 6), 8);
    memcpy(db, PTR(dp + 6), 8);
    int dw = dr[3] - dr[1], dh = dr[2] - dr[0], sw = sr[3] - sr[1], sh = sr[2] - sr[0];
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !sbase || !dbase) return;
    for (int y = 0; y < dh; y++) {
        int syy = sr[0] + y * sh / dh - sb[0], dyy = dr[0] + y - db[0];
        if (syy < 0 || dyy < 0 || syy >= sb[2] - sb[0] || dyy >= db[2] - db[0]) continue;
        for (int x = 0; x < dw; x++) {
            int sxx = sr[1] + x * sw / dw - sb[1], dxx = dr[1] + x - db[1];
            if (sxx < 0 || dxx < 0 || sxx >= sb[3] - sb[1] || dxx >= db[3] - db[1]) continue;
            W32_(g_mem, dbase + (uint32_t)dyy * drow + 4u * (uint32_t)dxx,
                 R32_(g_mem, sbase + (uint32_t)syy * srow + 4u * (uint32_t)sxx));
        }
    }
}

/* The drawing context of a 32-bit GWorld (QuickDraw coordinates, y down), or NULL. */
CGContextRef qd_gworld_context(uint32_t port) {
    HGWorld *w = gw_of(port);
    return w ? w->ctx : NULL;
}

/* Bounds of a GWorld port, for GetPortBounds; 0 if `port` is not a GWorld. */
int qd_gworld_bounds(uint32_t port, int16_t r[4]) {
    HGWorld *w = gw_of(port);
    if (!w) return 0;
    r[0] = w->top;
    r[1] = w->left;
    r[2] = (int16_t)(w->top + w->h);
    r[3] = (int16_t)(w->left + w->w);
    return 1;
}

/* ---- color tables: CTabHandle -> ColorTable {ctSeed 4, ctFlags 2, ctSize 2, ColorSpec[n] {value 2, rgb 6}} ---- */
static uint32_t new_ctable(uint32_t n, int gray) {
    uint32_t ct = gcalloc(1, 8 + 8 * n), h = gmalloc(4);
    W32_(g_mem, h, ct);
    W32_(g_mem, ct, 0x1000 + n);
    W16_(g_mem, ct + 4, 0);
    W16_(g_mem, ct + 6, n - 1);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t r, g, b;
        if (gray || n < 256) {
            r = g = b = (uint16_t)(0xffff - i * 0xffff / (n > 1 ? n - 1 : 1));
        } else {
            /* Mac OS standard 8-bit palette: 6x6x6 color cube, then ramps, then black. */
            static const uint16_t steps[6] = {0xffff, 0xcccc, 0x9999, 0x6666, 0x3333, 0x0000};
            if (i < 215) {
                r = steps[i / 36]; g = steps[(i / 6) % 6]; b = steps[i % 6];
            } else if (i < 255) {
                static const uint16_t ramp[10] = {0xeeee, 0xdddd, 0xbbbb, 0xaaaa, 0x8888, 0x7777, 0x5555, 0x4444, 0x2222, 0x1111};
                uint32_t k = i - 215, v = ramp[k % 10];
                r = k < 10 ? (uint16_t)v : 0; g = k >= 10 && k < 20 ? (uint16_t)v : 0; b = k >= 20 && k < 30 ? (uint16_t)v : 0;
                if (k >= 30) r = g = b = (uint16_t)v;
            } else {
                r = g = b = 0;
            }
        }
        W16_(g_mem, ct + 8 + 8 * i, i);
        W16_(g_mem, ct + 10 + 8 * i, r);
        W16_(g_mem, ct + 12 + 8 * i, g);
        W16_(g_mem, ct + 14 + 8 * i, b);
    }
    return h;
}
SHIM(_GetCTable) {
    /* ctID: 1, 2, 4, 8 = standard color tables; +32 = grayscale variants */
    int16_t id = (int16_t)ARG(0);
    int gray = id > 32;
    int depth = gray ? id - 32 : id;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) depth = 8;
    RET(new_ctable(1u << depth, gray));
}
SHIM(_DisposeCTable) {
    if (ARG(0)) {
        gfree(R32_(g_mem, ARG(0)));
        gfree(ARG(0));
    }
}

/* ---- fixed point ---- */
SHIM(_X2Fix) { RET((uint32_t)(int32_t)lround(ARGF64(0) * 65536.0)); }
SHIM(_Fix2X) { RETF((int32_t)ARG(0) / 65536.0); }
SHIM(_Long2Fix) { RET(ARG(0) << 16); }
SHIM(_Fix2Long) { RET((uint32_t)(((int32_t)ARG(0) + 0x8000) >> 16)); }
SHIM(_FixMul) { RET((uint32_t)(int32_t)(((int64_t)(int32_t)ARG(0) * (int32_t)ARG(1)) >> 16)); }

const ShimDef shims_quickdraw[] = {
    SHIM_ENTRY(_NewGWorld), SHIM_ENTRY(_QTNewGWorldFromPtr), SHIM_ENTRY(_DisposeGWorld), SHIM_ENTRY(_GetGWorldPixMap),
    SHIM_ENTRY(_GetPortPixMap), SHIM_ENTRY(_GetGWorldDevice), SHIM_ENTRY(_LockPixels), SHIM_ENTRY(_UnlockPixels),
    SHIM_ENTRY(_GetPixBaseAddr), SHIM_ENTRY(_GetPixRowBytes), SHIM_ENTRY(_GetPixBounds),
    SHIM_ENTRY(_GetPortBitMapForCopyBits), SHIM_ENTRY(_GetGWorld), SHIM_ENTRY(_SetGWorld), SHIM_ENTRY(_GetPort),
    SHIM_ENTRY(_SetPort), SHIM_ENTRY(_RGBForeColor), SHIM_ENTRY(_RGBBackColor), SHIM_ENTRY(_GetForeColor),
    SHIM_ENTRY(_GetBackColor), SHIM_ENTRY(_GetPortForeColor), SHIM_ENTRY(_GetPortBackColor), SHIM_ENTRY(_MoveTo),
    SHIM_ENTRY(_PenNormal), SHIM_ENTRY(_PenSize), SHIM_ENTRY(_GetPenState), SHIM_ENTRY(_EraseRect),
    SHIM_ENTRY(_PaintRect), SHIM_ENTRY(_FillRect), SHIM_ENTRY(_FrameRect), SHIM_ENTRY(_LineTo), SHIM_ENTRY(_TextFont),
    SHIM_ENTRY(_TextFace), SHIM_ENTRY(_TextSize), SHIM_ENTRY(_TextMode), SHIM_ENTRY(_GetPortTextFont),
    SHIM_ENTRY(_GetPortTextFace), SHIM_ENTRY(_GetPortTextSize), SHIM_ENTRY(_FMGetFontFamilyFromName),
    SHIM_ENTRY(_FMGetFontFamilyName), SHIM_ENTRY(_GetFontInfo), SHIM_ENTRY(_TextWidth), SHIM_ENTRY(_CharWidth),
    SHIM_ENTRY(_StringWidth), SHIM_ENTRY(_DrawText), SHIM_ENTRY(_CopyBits), SHIM_ENTRY(_X2Fix), SHIM_ENTRY(_Fix2X),
    SHIM_ENTRY(_Long2Fix), SHIM_ENTRY(_Fix2Long), SHIM_ENTRY(_FixMul), SHIM_ENTRY(_GetCTable),
    SHIM_ENTRY(_DisposeCTable),
    {0, 0},
};
