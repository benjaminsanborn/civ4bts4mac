/* AGL, CGL and CoreGraphics display shims.
 *
 * AGL contexts are NSOpenGLContexts (legacy 2.1 profile) attached to our
 * NSWindow content views. Display "mode switching" never touches the real
 * display: the chosen mode becomes the size of a borderless fullscreen
 * window whose GL backing store is that size and is scaled by the system. */
#define GL_SILENCE_DEPRECATION 1
#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl.h>

#include "cf.h"
#include "handles.h"
#include "platform.h"

/* AGL attribute constants (agl.h, no longer in the SDK). */
enum {
    AGL_NONE = 0, AGL_ALL_RENDERERS = 1, AGL_BUFFER_SIZE = 2, AGL_LEVEL = 3, AGL_RGBA = 4, AGL_DOUBLEBUFFER = 5,
    AGL_STEREO = 6, AGL_AUX_BUFFERS = 7, AGL_RED_SIZE = 8, AGL_GREEN_SIZE = 9, AGL_BLUE_SIZE = 10,
    AGL_ALPHA_SIZE = 11, AGL_DEPTH_SIZE = 12, AGL_STENCIL_SIZE = 13, AGL_ACCUM_RED_SIZE = 14,
    AGL_ACCUM_ALPHA_SIZE = 17, AGL_PIXEL_SIZE = 50, AGL_FULLSCREEN = 54, AGL_SAMPLE_BUFFERS_ARB = 55,
    AGL_SAMPLES_ARB = 56, AGL_MULTISAMPLE = 59, AGL_RENDERER_ID = 70, AGL_NO_RECOVERY = 72, AGL_ACCELERATED = 73,
    AGL_VIRTUAL_SCREEN = 82, AGL_DISPLAY_MASK = 84,
};
#define AGL_SWAP_INTERVAL 222

static int agl_has_value(int a) {
    return a == AGL_BUFFER_SIZE || a == AGL_LEVEL || a == AGL_AUX_BUFFERS || (a >= AGL_RED_SIZE && a <= AGL_ACCUM_ALPHA_SIZE) ||
           a == AGL_PIXEL_SIZE || a == AGL_SAMPLE_BUFFERS_ARB || a == AGL_SAMPLES_ARB || a == AGL_RENDERER_ID ||
           a == AGL_VIRTUAL_SCREEN || a == AGL_DISPLAY_MASK;
}

typedef struct {
    NSOpenGLPixelFormat *pf;
} HPix;

typedef struct {
    NSOpenGLContext *ctx;
    uint32_t port; /* drawable */
    uint32_t g;
} HCtx;

static HCtx *g_ctxs[64];
static int g_nctx;

/* Chosen display mode (CGDisplaySwitchToMode) and capture state. */
static int g_mode_w, g_mode_h, g_captured;
static NSWindow *g_fullscreen_window;

static HCtx *ctx_of(uint32_t g) {
    HCtx *c = gh_get(g, TAG_AGLCTX);
    if (!c && g) rt_fatal("bad AGLContext %08x", g);
    return c;
}

static uint32_t ctx_for_cgl(CGLContextObj cgl) {
    for (int i = 0; i < g_nctx; i++)
        if (g_ctxs[i] && [g_ctxs[i]->ctx CGLContextObj] == cgl) return g_ctxs[i]->g;
    return 0;
}

static uint32_t wrap_ctx(NSOpenGLContext *ctx) {
    HCtx *c = calloc(1, sizeof *c);
    c->ctx = ctx;
    c->g = gh_new(TAG_AGLCTX, c);
    if (g_nctx < 64) g_ctxs[g_nctx++] = c;
    return c->g;
}

/* ---- AGL ---- */
SHIM(_aglChoosePixelFormat) {
    /* (const AGLDevice *gdevs, GLint ndev, const GLint *attribs) */
    NSOpenGLPixelFormatAttribute a[64];
    int n = 0, color = 0, msaa_samples = 0, msaa_buffers = 0;
    a[n++] = NSOpenGLPFAOpenGLProfile;
    a[n++] = NSOpenGLProfileVersionLegacy;
    for (uint32_t p = ARG(2); p && R32_(g_mem, p) != AGL_NONE; p += 4) {
        int attr = (int)R32_(g_mem, p);
        int val = agl_has_value(attr) ? (int)R32_(g_mem, p + 4) : 0;
        switch (attr) {
        case AGL_DOUBLEBUFFER: a[n++] = NSOpenGLPFADoubleBuffer; break;
        case AGL_DEPTH_SIZE: a[n++] = NSOpenGLPFADepthSize; a[n++] = (NSOpenGLPixelFormatAttribute)val; break;
        case AGL_STENCIL_SIZE: a[n++] = NSOpenGLPFAStencilSize; a[n++] = (NSOpenGLPixelFormatAttribute)val; break;
        case AGL_ALPHA_SIZE: a[n++] = NSOpenGLPFAAlphaSize; a[n++] = (NSOpenGLPixelFormatAttribute)val; break;
        case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE: color += val; break;
        case AGL_PIXEL_SIZE: color = val; break;
        case AGL_ACCELERATED: a[n++] = NSOpenGLPFAAccelerated; break;
        case AGL_NO_RECOVERY: a[n++] = NSOpenGLPFANoRecovery; break;
        case AGL_SAMPLE_BUFFERS_ARB: msaa_buffers = val; break;
        case AGL_SAMPLES_ARB: msaa_samples = val; break;
        default: break;
        }
        if (agl_has_value(attr)) p += 4;
    }
    a[n++] = NSOpenGLPFAColorSize;
    a[n++] = (NSOpenGLPixelFormatAttribute)(color >= 24 ? 24 : 24);
    int base = n;
    if (msaa_buffers && msaa_samples) {
        a[n++] = NSOpenGLPFAMultisample;
        a[n++] = NSOpenGLPFASampleBuffers;
        a[n++] = (NSOpenGLPixelFormatAttribute)msaa_buffers;
        a[n++] = NSOpenGLPFASamples;
        a[n++] = (NSOpenGLPixelFormatAttribute)msaa_samples;
    }
    a[n] = 0;
    NSOpenGLPixelFormat *pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:a];
    if (!pf && n != base) { /* retry without multisampling */
        a[base] = 0;
        pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:a];
    }
    if (!pf) { RET(0); return; }
    HPix *h = calloc(1, sizeof *h);
    h->pf = pf;
    RET(gh_new(TAG_AGLPIX, h));
}
SHIM(_aglDestroyPixelFormat) {
    HPix *h = gh_get(ARG(0), TAG_AGLPIX);
    if (h) { h->pf = nil; free(h); gh_free(ARG(0)); }
}
SHIM(_aglCreateContext) {
    HPix *h = gh_get(ARG(0), TAG_AGLPIX);
    HCtx *share = gh_get(ARG(1), TAG_AGLCTX);
    if (!h) { RET(0); return; }
    NSOpenGLContext *ctx = [[NSOpenGLContext alloc] initWithFormat:h->pf shareContext:share ? share->ctx : nil];
    RET(ctx ? wrap_ctx(ctx) : 0);
}
SHIM(_aglDestroyContext) {
    HCtx *c = ctx_of(ARG(0));
    if (c) {
        if ([NSOpenGLContext currentContext] == c->ctx) [NSOpenGLContext clearCurrentContext];
        [c->ctx clearDrawable];
        for (int i = 0; i < g_nctx; i++)
            if (g_ctxs[i] == c) g_ctxs[i] = NULL;
        c->ctx = nil;
        gh_free(c->g);
        free(c);
    }
    RET(1);
}
SHIM(_aglSetCurrentContext) {
    HCtx *c = ctx_of(ARG(0));
    if (c) [c->ctx makeCurrentContext];
    else [NSOpenGLContext clearCurrentContext];
    RET(1);
}
SHIM(_aglGetCurrentContext) {
    NSOpenGLContext *cur = [NSOpenGLContext currentContext];
    RET(cur ? ctx_for_cgl([cur CGLContextObj]) : 0);
}
SHIM(_aglSetDrawable) {
    HCtx *c = ctx_of(ARG(0));
    if (!c) { RET(0); return; }
    c->port = ARG(1);
    if (!ARG(1)) {
        [c->ctx clearDrawable];
    } else {
        HWindow *w = gh_get(ARG(1), TAG_PORT);
        if (!w) rt_fatal("aglSetDrawable: not a window port %08x", ARG(1));
        [c->ctx setView:(__bridge NSView *)w->view];
        [c->ctx update];
    }
    RET(1);
}
SHIM(_aglGetDrawable) { HCtx *c = ctx_of(ARG(0)); RET(c ? c->port : 0); }
SHIM(_aglUpdateContext) { HCtx *c = ctx_of(ARG(0)); if (c) [c->ctx update]; RET(1); }
SHIM(_aglSwapBuffers) { HCtx *c = ctx_of(ARG(0)); if (c) [c->ctx flushBuffer]; }
SHIM(_aglSetInteger) {
    /* AGL context parameter numbers match CGL's (swap interval 222, backing size 304, ...). */
    HCtx *c = ctx_of(ARG(0));
    if (!c) { RET(0); return; }
    GLint v[4];
    if (ARG(2)) memcpy(v, PTR(ARG(2)), sizeof v);
    CGLSetParameter([c->ctx CGLContextObj], (CGLContextParameter)ARG(1), v);
    RET(1);
}
SHIM(_aglGetInteger) {
    HCtx *c = ctx_of(ARG(0));
    GLint v[4] = {0};
    if (c) CGLGetParameter([c->ctx CGLContextObj], (CGLContextParameter)ARG(1), v);
    if (ARG(2)) memcpy(PTR(ARG(2)), v, 4);
    RET(1);
}
SHIM(_aglGetVirtualScreen) { RET(0); }
SHIM(_aglSetFullScreen) {
    /* (ctx, width, height, freq, device): a borderless window over the main screen,
     * rendering at width x height. */
    HCtx *c = ctx_of(ARG(0));
    int w = (int)ARG(1) ? (int)ARG(1) : g_mode_w, h = (int)ARG(2) ? (int)ARG(2) : g_mode_h;
    NSScreen *s = [NSScreen screens][0];
    if (!g_fullscreen_window) {
        g_fullscreen_window = [[NSWindow alloc] initWithContentRect:[s frame]
                                                          styleMask:NSWindowStyleMaskBorderless
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
        [g_fullscreen_window setLevel:NSMainMenuWindowLevel + 1];
        [g_fullscreen_window setAcceptsMouseMovedEvents:YES];
        [g_fullscreen_window setReleasedWhenClosed:NO];
    }
    [g_fullscreen_window makeKeyAndOrderFront:nil];
    [c->ctx setView:[g_fullscreen_window contentView]];
    GLint size[2] = {w, h};
    CGLSetParameter([c->ctx CGLContextObj], kCGLCPSurfaceBackingSize, size);
    CGLEnable([c->ctx CGLContextObj], kCGLCESurfaceBackingSize);
    [c->ctx update];
    rt_log("aglSetFullScreen %dx%d", w, h);
    RET(1);
}
SHIM(_aglUseFont) { RET(0); }
SHIM(_aglCreatePBuffer) { rt_log("aglCreatePBuffer: pbuffers not supported"); RET(0); }

/* ---- CGL ---- */
SHIM(_CGLChoosePixelFormat) {
    /* (const CGLPixelFormatAttribute *attribs, CGLPixelFormatObj *pix, GLint *npix) */
    CGLPixelFormatAttribute a[64];
    int n = 0;
    for (uint32_t p = ARG(0); p && R32_(g_mem, p) && n < 62; p += 4) a[n++] = (CGLPixelFormatAttribute)R32_(g_mem, p);
    a[n] = 0;
    CGLPixelFormatObj pix = NULL;
    GLint npix = 0;
    CGLError e = CGLChoosePixelFormat(a, &pix, &npix);
    HPix *h = NULL;
    if (pix) {
        h = calloc(1, sizeof *h);
        h->pf = [[NSOpenGLPixelFormat alloc] initWithCGLPixelFormatObj:pix];
        CGLReleasePixelFormat(pix);
    }
    W32_(g_mem, ARG(1), h ? gh_new(TAG_AGLPIX, h) : 0);
    if (ARG(2)) W32_(g_mem, ARG(2), (uint32_t)npix);
    RET(e);
}
SHIM(_CGLDestroyPixelFormat) {
    HPix *h = gh_get(ARG(0), TAG_AGLPIX);
    if (h) { h->pf = nil; free(h); gh_free(ARG(0)); }
    RET(0);
}
SHIM(_CGLCreateContext) {
    HPix *h = gh_get(ARG(0), TAG_AGLPIX);
    HCtx *share = gh_get(ARG(1), TAG_AGLCTX);
    NSOpenGLContext *ctx = h ? [[NSOpenGLContext alloc] initWithFormat:h->pf shareContext:share ? share->ctx : nil] : nil;
    W32_(g_mem, ARG(2), ctx ? wrap_ctx(ctx) : 0);
    RET(ctx ? 0 : 10002 /* kCGLBadPixelFormat */);
}
SHIM(_CGLDestroyContext) {
    HCtx *c = ctx_of(ARG(0));
    if (c) { c->ctx = nil; gh_free(c->g); }
    RET(0);
}
SHIM(_CGLSetCurrentContext) {
    HCtx *c = ctx_of(ARG(0));
    if (c) [c->ctx makeCurrentContext];
    else [NSOpenGLContext clearCurrentContext];
    RET(0);
}
SHIM(_CGLGetCurrentContext) {
    CGLContextObj cur = CGLGetCurrentContext();
    RET(cur ? ctx_for_cgl(cur) : 0);
}
SHIM(_CGLSetParameter) {
    HCtx *c = ctx_of(ARG(0));
    GLint v[4] = {0};
    if (ARG(2)) memcpy(v, PTR(ARG(2)), sizeof v);
    RET(c ? CGLSetParameter([c->ctx CGLContextObj], (CGLContextParameter)ARG(1), v) : 0);
}
SHIM(_CGLClearDrawable) { HCtx *c = ctx_of(ARG(0)); if (c) [c->ctx clearDrawable]; RET(0); }

/* Renderer info, virtualized as a 2008-era Mac reported it: renderer 0 is the
 * accelerated GPU (the real one, by renderer ID), renderer 1 the software
 * renderer. Aspyr's adapter setup walks the list in hardware/software pairs. */
static GLint host_accelerated_renderer_id(void) {
    static GLint id;
    if (id) return id;
    CGLRendererInfoObj info;
    GLint n = 0;
    if (CGLQueryRendererInfo(CGDisplayIDToOpenGLDisplayMask(CGMainDisplayID()), &info, &n) == 0) {
        for (GLint i = 0; i < n && !id; i++) {
            GLint acc = 0, rid = 0;
            CGLDescribeRenderer(info, i, kCGLRPAccelerated, &acc);
            CGLDescribeRenderer(info, i, kCGLRPRendererID, &rid);
            if (acc) id = rid;
        }
        CGLDestroyRendererInfo(info);
    }
    return id;
}

SHIM(_CGLQueryRendererInfo) {
    W32_(g_mem, ARG(1), 0x524e0001u);
    if (ARG(2)) W32_(g_mem, ARG(2), 2);
    RET(0);
}
SHIM(_CGLDescribeRenderer) {
    /* (rend, rend_num, prop, GLint *value) */
    int hw = (int)ARG(1) == 0;
    GLint v = 0;
    switch (ARG(2)) {
    case kCGLRPRendererID: v = hw ? host_accelerated_renderer_id() : kCGLRendererGenericFloatID; break;
    case kCGLRPAccelerated: case kCGLRPGPUVertProcCapable: case kCGLRPGPUFragProcCapable: case kCGLRPCompliant:
    case kCGLRPWindow: case kCGLRPFullScreen: case kCGLRPMultiScreen: case kCGLRPOnline: case kCGLRPMPSafe:
        v = hw || ARG(2) == kCGLRPCompliant || ARG(2) == kCGLRPWindow || ARG(2) == kCGLRPMPSafe; break;
    case kCGLRPDisplayMask: v = 1; break;
    case kCGLRPMaxSamples: v = hw ? 4 : 0; break;
    case kCGLRPMaxSampleBuffers: v = hw ? 1 : 0; break;
    case kCGLRPVideoMemory: case kCGLRPTextureMemory: v = hw ? 512 << 20 : 0; break;
    case kCGLRPVideoMemoryMegabytes: case kCGLRPTextureMemoryMegabytes: v = hw ? 512 : 0; break;
    case kCGLRPRendererCount: v = 2; break;
    case kCGLRPBufferModes: v = kCGLDoubleBufferBit | kCGLSingleBufferBit; break;
    case kCGLRPColorModes: v = kCGLARGB8888Bit | kCGLRGB888A8Bit; break;
    case kCGLRPDepthModes: v = kCGL24Bit | kCGL32Bit; break;
    case kCGLRPStencilModes: v = kCGL8Bit; break;
    default:
        rt_log("CGLDescribeRenderer: property %u not virtualized", ARG(2));
        RET(10000 /* kCGLBadAttribute */);
        return;
    }
    W32_(g_mem, ARG(3), (uint32_t)v);
    RET(0);
}
SHIM(_CGLDestroyRendererInfo) { RET(0); }

/* ---- CoreGraphics displays ----
 * The guest sees one virtual display with an era-typical ID. Modern macOS gives
 * the main display ID 1, which collides with sentinel values in the game's
 * display code (IDirect3D_Mac treats 1 as "no previous display"). */
#define GUEST_DISPLAY_ID 0x04272c40u
static const int kModes[][2] = {{800, 600}, {1024, 768}, {1152, 720}, {1280, 720}, {1280, 800}, {1280, 1024},
                                {1366, 768}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200},
                                {2560, 1440}, {2560, 1600}};

static CFDictionaryRef make_mode(int w, int h) {
    int bpp = 32, mode = w * 10000 + h;
    double rate = 60;
    CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef nb = CFNumberCreate(NULL, kCFNumberIntType, &bpp), nm = CFNumberCreate(NULL, kCFNumberIntType, &mode);
    CFNumberRef nr = CFNumberCreate(NULL, kCFNumberDoubleType, &rate);
    const void *k[] = {CFSTR("Width"), CFSTR("Height"), CFSTR("BitsPerPixel"), CFSTR("BitsPerSample"),
                       CFSTR("SamplesPerPixel"), CFSTR("RefreshRate"), CFSTR("Mode"), CFSTR("UsableForDesktopGUI")};
    int bps = 8, spp = 3;
    CFNumberRef nbps = CFNumberCreate(NULL, kCFNumberIntType, &bps), nspp = CFNumberCreate(NULL, kCFNumberIntType, &spp);
    const void *v[] = {nw, nh, nb, nbps, nspp, nr, nm, kCFBooleanTrue};
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 8, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nm); CFRelease(nr); CFRelease(nbps); CFRelease(nspp);
    return d;
}

static CFArrayRef g_modes;
static CFDictionaryRef g_current_mode;

static void ensure_modes(void) {
    if (g_modes) return;
    NSRect sf = [[NSScreen screens][0] frame];
    int sw = (int)sf.size.width, sh = (int)sf.size.height;
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    int have_native = 0;
    for (size_t i = 0; i < sizeof kModes / sizeof kModes[0]; i++) {
        if (kModes[i][0] > sw || kModes[i][1] > sh) continue;
        CFDictionaryRef d = make_mode(kModes[i][0], kModes[i][1]);
        CFArrayAppendValue(a, d);
        CFRelease(d);
        if (kModes[i][0] == sw && kModes[i][1] == sh) have_native = 1;
    }
    if (!have_native) {
        CFDictionaryRef d = make_mode(sw, sh);
        CFArrayAppendValue(a, d);
        CFRelease(d);
    }
    g_modes = a;
    g_current_mode = CFArrayGetValueAtIndex(a, CFArrayGetCount(a) - 1);
    g_mode_w = sw;
    g_mode_h = sh;
}

static int mode_int(CFDictionaryRef d, CFStringRef k) {
    int v = 0;
    CFNumberRef n = CFDictionaryGetValue(d, k);
    if (n) CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

SHIM(_CGMainDisplayID) { RET(GUEST_DISPLAY_ID); }
SHIM(_CGGetActiveDisplayList) {
    if (ARG(1) && ARG(0) >= 1) W32_(g_mem, ARG(1), GUEST_DISPLAY_ID);
    if (ARG(2)) W32_(g_mem, ARG(2), 1);
    RET(0);
}
SHIM(_CGGetOnlineDisplayList) {
    if (ARG(1) && ARG(0) >= 1) W32_(g_mem, ARG(1), GUEST_DISPLAY_ID);
    if (ARG(2)) W32_(g_mem, ARG(2), 1);
    RET(0);
}
SHIM(_CGGetDisplaysWithPoint) {
    if (ARG(3) && ARG(2) >= 1) W32_(g_mem, ARG(3), GUEST_DISPLAY_ID);
    if (ARG(4)) W32_(g_mem, ARG(4), 1);
    RET(0);
}
SHIM(_CGGetDisplaysWithOpenGLDisplayMask) {
    if (ARG(2) && ARG(1) >= 1) W32_(g_mem, ARG(2), GUEST_DISPLAY_ID);
    if (ARG(3)) W32_(g_mem, ARG(3), 1);
    RET(0);
}
SHIM(_CGDisplayIDToOpenGLDisplayMask) { RET(1); }
SHIM(_CGDisplayPixelsWide) { ensure_modes(); RET(g_mode_w); }
SHIM(_CGDisplayPixelsHigh) { ensure_modes(); RET(g_mode_h); }
SHIM(_CGDisplayBitsPerPixel) { RET(32); }
SHIM(_CGDisplayBounds) {
    /* CGRect (16 bytes) returned through a hidden pointer; callee pops it. */
    ensure_modes();
    float r[4] = {0, 0, (float)g_mode_w, (float)g_mode_h};
    memcpy(PTR(ARG(0)), r, 16);
    RET(ARG(0));
    cpu->r[4] += 4;
}
SHIM(_CGDisplayAvailableModes) { ensure_modes(); RETCF(g_modes); }
SHIM(_CGDisplayCurrentMode) { ensure_modes(); RETCF(g_current_mode); }
static CFDictionaryRef best_mode(int w, int h, uint32_t exact_out) {
    ensure_modes();
    CFDictionaryRef best = NULL;
    long best_d = LONG_MAX;
    for (CFIndex i = 0; i < CFArrayGetCount(g_modes); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(g_modes, i);
        long dw = mode_int(d, CFSTR("Width")) - w, dh = mode_int(d, CFSTR("Height")) - h;
        long dist = dw * dw + dh * dh;
        if (dist < best_d) { best_d = dist; best = d; }
    }
    if (exact_out) W32_(g_mem, exact_out, best_d == 0);
    return best;
}
SHIM(_CGDisplayBestModeForParameters) { RETCF(best_mode((int)ARG(2), (int)ARG(3), ARG(4))); }
SHIM(_CGDisplayBestModeForParametersAndRefreshRate) { RETCF(best_mode((int)ARG(2), (int)ARG(3), ARG(6))); }
SHIM(_CGDisplayBestModeForParametersAndRefreshRateWithProperty) { RETCF(best_mode((int)ARG(2), (int)ARG(3), ARG(7))); }
SHIM(_CGDisplaySwitchToMode) {
    CFDictionaryRef d = CFI(1);
    g_current_mode = d;
    g_mode_w = mode_int(d, CFSTR("Width"));
    g_mode_h = mode_int(d, CFSTR("Height"));
    rt_log("CGDisplaySwitchToMode %dx%d (virtual)", g_mode_w, g_mode_h);
    RET(0);
}
SHIM(_CGCaptureAllDisplays) { g_captured = 1; RET(0); }
SHIM(_CGReleaseAllDisplays) {
    g_captured = 0;
    if (g_fullscreen_window) [g_fullscreen_window orderOut:nil];
    RET(0);
}
SHIM(_CGDisplayIsCaptured) { RET(g_captured); }
SHIM(_CGDisplayIsInMirrorSet) { RET(0); }
SHIM(_CGShieldingWindowLevel) { RET(NSMainMenuWindowLevel + 1); }
SHIM(_CGAcquireDisplayFadeReservation) { if (ARG(1)) W32_(g_mem, ARG(1), 1); RET(0); }
SHIM(_CGReleaseDisplayFadeReservation) { RET(0); }
SHIM(_CGDisplayFade) { RET(0); }
SHIM(_CGGetDisplayTransferByTable) {
    /* Identity gamma: (display, capacity, red*, green*, blue*, count*) */
    uint32_t n = ARG(1) < 256 ? ARG(1) : 256;
    for (uint32_t i = 0; i < n; i++) {
        float f = n > 1 ? (float)i / (float)(n - 1) : 0;
        WF32_(g_mem, ARG(2) + 4 * i, f);
        WF32_(g_mem, ARG(3) + 4 * i, f);
        WF32_(g_mem, ARG(4) + 4 * i, f);
    }
    if (ARG(5)) W32_(g_mem, ARG(5), n);
    RET(0);
}
SHIM(_CGSetDisplayTransferByTable) { RET(0); }

const ShimDef shims_agl[] = {
    SHIM_ENTRY(_aglChoosePixelFormat), SHIM_ENTRY(_aglDestroyPixelFormat), SHIM_ENTRY(_aglCreateContext),
    SHIM_ENTRY(_aglDestroyContext), SHIM_ENTRY(_aglSetCurrentContext), SHIM_ENTRY(_aglGetCurrentContext),
    SHIM_ENTRY(_aglSetDrawable), SHIM_ENTRY(_aglGetDrawable), SHIM_ENTRY(_aglUpdateContext),
    SHIM_ENTRY(_aglSwapBuffers), SHIM_ENTRY(_aglSetInteger), SHIM_ENTRY(_aglGetInteger),
    SHIM_ENTRY(_aglGetVirtualScreen), SHIM_ENTRY(_aglSetFullScreen), SHIM_ENTRY(_aglUseFont),
    SHIM_ENTRY(_aglCreatePBuffer), SHIM_ENTRY(_CGLChoosePixelFormat), SHIM_ENTRY(_CGLDestroyPixelFormat),
    SHIM_ENTRY(_CGLCreateContext), SHIM_ENTRY(_CGLDestroyContext), SHIM_ENTRY(_CGLSetCurrentContext),
    SHIM_ENTRY(_CGLGetCurrentContext), SHIM_ENTRY(_CGLSetParameter), SHIM_ENTRY(_CGLClearDrawable),
    SHIM_ENTRY(_CGLQueryRendererInfo), SHIM_ENTRY(_CGLDescribeRenderer), SHIM_ENTRY(_CGLDestroyRendererInfo),
    SHIM_ENTRY(_CGMainDisplayID), SHIM_ENTRY(_CGGetActiveDisplayList), SHIM_ENTRY(_CGGetOnlineDisplayList),
    SHIM_ENTRY(_CGGetDisplaysWithPoint), SHIM_ENTRY(_CGGetDisplaysWithOpenGLDisplayMask),
    SHIM_ENTRY(_CGDisplayIDToOpenGLDisplayMask), SHIM_ENTRY(_CGDisplayPixelsWide), SHIM_ENTRY(_CGDisplayPixelsHigh),
    SHIM_ENTRY(_CGDisplayBitsPerPixel), SHIM_ENTRY(_CGDisplayBounds), SHIM_ENTRY(_CGDisplayAvailableModes),
    SHIM_ENTRY(_CGDisplayCurrentMode), SHIM_ENTRY(_CGDisplayBestModeForParameters),
    SHIM_ENTRY(_CGDisplayBestModeForParametersAndRefreshRate),
    SHIM_ENTRY(_CGDisplayBestModeForParametersAndRefreshRateWithProperty), SHIM_ENTRY(_CGDisplaySwitchToMode),
    SHIM_ENTRY(_CGCaptureAllDisplays), SHIM_ENTRY(_CGReleaseAllDisplays), SHIM_ENTRY(_CGDisplayIsCaptured),
    SHIM_ENTRY(_CGDisplayIsInMirrorSet), SHIM_ENTRY(_CGShieldingWindowLevel),
    SHIM_ENTRY(_CGAcquireDisplayFadeReservation), SHIM_ENTRY(_CGReleaseDisplayFadeReservation),
    SHIM_ENTRY(_CGDisplayFade), SHIM_ENTRY(_CGGetDisplayTransferByTable), SHIM_ENTRY(_CGSetDisplayTransferByTable),
    {0, 0},
};
