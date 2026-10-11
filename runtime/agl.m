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
#import <QuartzCore/QuartzCore.h>
#include <sys/stat.h>

#include "cf.h"
#include "handles.h"
#include "events.h"
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

struct HPBuf;
typedef struct {
    NSOpenGLContext *ctx;
    uint32_t port; /* drawable */
    uint32_t g;
    struct HPBuf *pbuf; /* pbuffer drawable (aglSetPBuffer), or NULL */
} HCtx;

/* Pbuffers are FBOs. aglTexImagePBuffer makes the bound texture the FBO's color
 * attachment, so rendering to the pbuffer updates that texture directly. */
typedef struct HPBuf {
    uint32_t g;
    int w, h;
    GLenum target, ifmt;
    GLuint fbo, depth, tex;
    int own_tex;
} HPBuf;
static GLuint g_default_fbo; /* the current context's pbuffer FBO, or 0 */
GLuint agl_default_fbo(void) { return g_default_fbo; }
static void pbuf_dump(HPBuf *p);

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
    g_default_fbo = c && c->pbuf ? c->pbuf->fbo : 0;
    RET(1);
}
SHIM(_aglGetCurrentContext) {
    NSOpenGLContext *cur = [NSOpenGLContext currentContext];
    RET(cur ? ctx_for_cgl([cur CGLContextObj]) : 0);
}
SHIM(_aglSetDrawable) {
    HCtx *c = ctx_of(ARG(0));
    if (!c) { RET(0); return; }
    if (c->pbuf) {
        pbuf_dump(c->pbuf);
        c->pbuf = NULL;
        if ([NSOpenGLContext currentContext] == c->ctx) {
            g_default_fbo = 0;
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
            glDrawBuffer(GL_BACK);
            glReadBuffer(GL_BACK);
        }
        if (c->port == ARG(1) && ARG(1)) { RET(1); return; } /* same window: just leave the pbuffer */
    }
    c->port = ARG(1);
    if (!ARG(1)) {
        [c->ctx clearDrawable];
    } else {
        HWindow *w = gh_get(ARG(1), TAG_PORT);
        if (!w) rt_fatal("aglSetDrawable: not a window port %08x", ARG(1));
        [c->ctx setView:(__bridge NSView *)w->view];
        if (w->mode_w) {
            /* Display port: render at the game's mode size; the system scales it up. */
            GLint size[2] = {w->mode_w, w->mode_h};
            CGLSetParameter([c->ctx CGLContextObj], kCGLCPSurfaceBackingSize, size);
            CGLEnable([c->ctx CGLContextObj], kCGLCESurfaceBackingSize);
        }
        [c->ctx update];
    }
    RET(1);
}
SHIM(_aglGetDrawable) { HCtx *c = ctx_of(ARG(0)); RET(c ? c->port : 0); }
SHIM(_aglUpdateContext) { HCtx *c = ctx_of(ARG(0)); if (c) [c->ctx update]; RET(1); }
/* CIV4_FRAMES=dir: save the game's back buffer as PNG every couple of seconds
 * (inspection without screen capture). */
#import <ImageIO/ImageIO.h>
static void maybe_capture_frame(void) {
    static const char *dir;
    static int checked;
    static double last;
    if (!checked) {
        checked = 1;
        if ((dir = getenv("CIV4_FRAMES"))) {
            char path[1024];
            snprintf(path, sizeof path, "%s", dir);
            for (char *p = path + 1; *p; p++) /* mkdir -p */
                if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
            mkdir(path, 0755);
        }
    }
    if (!dir) return;
    double now = CACurrentMediaTime();
    if (now - last < 2.0) return;
    last = now;
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    GLint w = 0, h = 0;
    CGLContextObj cgl = CGLGetCurrentContext();
    GLint size[2] = {0, 0};
    if (cgl && CGLIsEnabled(cgl, kCGLCESurfaceBackingSize, &w) == 0 && w) {
        CGLGetParameter(cgl, kCGLCPSurfaceBackingSize, size);
    }
    w = size[0] ? size[0] : vp[2];
    h = size[1] ? size[1] : vp[3];
    if (w <= 0 || h <= 0) return;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* Flip vertically: GL rows are bottom-up. */
    for (GLint y = 0; y < h / 2; y++) {
        uint8_t tmp[16384];
        uint8_t *a = px + (size_t)y * w * 4, *b = px + (size_t)(h - 1 - y) * w * 4;
        for (GLint off = 0; off < w * 4; off += (GLint)sizeof tmp) {
            size_t n = (size_t)MIN((GLint)sizeof tmp, w * 4 - off);
            memcpy(tmp, a + off, n); memcpy(a + off, b + off, n); memcpy(b + off, tmp, n);
        }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bc = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(bc);
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06.1f.png", dir, now);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url && img ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    if (url) CFRelease(url);
    if (img) CGImageRelease(img);
    CGContextRelease(bc);
    CGColorSpaceRelease(cs);
    free(px);
}

SHIM(_aglSwapBuffers) {
    HCtx *c = ctx_of(ARG(0));
    if (!c) return;
    maybe_capture_frame();
    [c->ctx flushBuffer];
    /* Nothing paces frames while the window is hidden (no vsync): cap at 30 fps
     * so a game left in the background doesn't spin. */
    static double last_swap;
    double now_s = CACurrentMediaTime();
    HWindow *hw = gh_get(c->port, TAG_PORT);
    NSWindow *nw = hw ? (__bridge NSWindow *)hw->ns : nil;
    static int no_throttle = -1;
    if (no_throttle < 0) no_throttle = getenv("CIV4_NO_THROTTLE") != NULL; /* for measurements */
    if (!no_throttle && nw && !([nw occlusionState] & NSWindowOcclusionStateVisible) && now_s - last_swap < 1.0 / 30)
        usleep((useconds_t)((1.0 / 30 - (now_s - last_swap)) * 1e6));
    last_swap = CACurrentMediaTime();
    /* CIV4_FPS: every 5 seconds, log the frame rate and the slow frames (hitches). */
    static int fps = -1, frames, slow;
    static double t0, last, worst;
    if (fps < 0) fps = getenv("CIV4_FPS") != NULL;
    if (fps) {
        double now = CACurrentMediaTime();
        if (!t0) t0 = last = now;
        double dt = now - last;
        last = now;
        if (dt > worst) worst = dt;
        if (dt > 0.05) slow++;
        frames++;
        if (now - t0 >= 5) {
            rt_log("fps %.1f  worst frame %.0f ms  frames over 50 ms: %d  worst event gap %.0f ms", frames / (now - t0),
                   worst * 1000, slow, plat_take_worst_event_gap() * 1000);
            frames = slow = 0;
            worst = 0;
            t0 = now;
        }
    }
}
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
SHIM(_aglCreatePBuffer) {
    /* (GLint width, GLint height, GLenum target, GLenum internalFormat, long max_level, AGLPbuffer *out) */
    HPBuf *p = calloc(1, sizeof *p);
    p->w = (int)ARG(0);
    p->h = (int)ARG(1);
    p->target = ARG(2);
    p->ifmt = ARG(3);
    p->g = gh_new(TAG_PBUFFER, p);
    W32_(g_mem, ARG(5), p->g);
    RET(1);
}
/* CIV4_DEBUG_PBUF=dir: save the first pbuffer renders as PNGs (RGB and alpha side by side). */
static void pbuf_dump(HPBuf *p) {
    static const char *dir;
    static int checked, n;
    if (!checked) { checked = 1; dir = getenv("CIV4_DEBUG_PBUF"); if (dir) mkdir(dir, 0755); }
    if (!dir || n >= 12) return;
    int w = p->w, h = p->h;
    uint8_t *px = malloc((size_t)w * h * 4), *out = malloc((size_t)w * 2 * h * 4);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, p->fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    rt_log("pbuffer dump %d: %dx%d tex %u err %04x viewport %d,%d %dx%d", n, w, h, p->tex, glGetError(), vp[0], vp[1], vp[2], vp[3]);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)prev);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *s = px + ((size_t)(h - 1 - y) * w + x) * 4;
            uint8_t *d = out + ((size_t)y * w * 2 + x) * 4, *a = d + (size_t)w * 4;
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
            a[0] = a[1] = a[2] = s[3], a[3] = 255;
        }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bc = CGBitmapContextCreate(out, (size_t)w * 2, (size_t)h, 8, (size_t)w * 8, cs, kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(bc);
    char path[1024];
    snprintf(path, sizeof path, "%s/pbuf_%02d_%dx%d.png", dir, n++, w, h);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(bc); CGColorSpaceRelease(cs);
    free(px); free(out);
}

static void pbuf_attach(HPBuf *p) {
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, p->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, p->target, p->tex, 0);
    GLenum st = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT)
    {
        GLint tw = 0, th = 0, prev_tex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
        glBindTexture(p->target, p->tex);
        glGetTexLevelParameteriv(p->target, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(p->target, 0, GL_TEXTURE_HEIGHT, &th);
        glBindTexture(p->target, (GLuint)prev_tex);
        rt_log("pbuffer %dx%d target %04x format %04x tex %u (%dx%d): FBO incomplete (%04x)", p->w, p->h, p->target,
               p->ifmt, p->tex, tw, th, st);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)prev);
}
static void pbuf_alloc_tex(HPBuf *p, GLuint tex) {
    GLint prev = 0;
    glGetIntegerv(p->target == GL_TEXTURE_RECTANGLE_EXT ? GL_TEXTURE_BINDING_RECTANGLE_EXT : GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(p->target, tex);
    /* Metal-backed GL can't render to 3-channel textures: RGB pbuffers get RGBA storage. */
    GLint ifmt = p->ifmt == GL_RGB || p->ifmt == GL_RGB8 || p->ifmt == 3 ? GL_RGBA8 : (GLint)p->ifmt;
    /* NULL data means "no data" only while no pixel-unpack buffer is bound. */
    GLint unpack = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB, &unpack);
    if (unpack) glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
    /* Nor while Apple client storage is on (the game enables it for uploads). */
    GLint client = 0;
    glGetIntegerv(GL_UNPACK_CLIENT_STORAGE_APPLE, &client);
    if (client) glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
    while (glGetError()) {}
    glTexImage2D(p->target, 0, ifmt, p->w, p->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    GLenum err = glGetError();
    if (client) glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
    if (err) rt_log("pbuffer texture %dx%d format %04x: GL error %04x", p->w, p->h, ifmt, err);
    if (unpack) glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, (GLuint)unpack);
    glTexParameteri(p->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glBindTexture(p->target, (GLuint)prev);
}
static void pbuf_ensure_fbo(HPBuf *p) {
    if (p->fbo) return;
    if (!p->tex) {
        glGenTextures(1, &p->tex);
        p->own_tex = 1;
        pbuf_alloc_tex(p, p->tex);
    }
    glGenFramebuffersEXT(1, &p->fbo);
    glGenRenderbuffersEXT(1, &p->depth);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, p->depth);
    glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, p->w, p->h);
    glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
    GLint prev = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, p->fbo);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, p->depth);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, p->depth);
    glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, (GLuint)prev);
    pbuf_attach(p);
}
SHIM(_aglTexImagePBuffer) {
    /* (AGLContext, AGLPbuffer, GLint source): the texture bound to the pbuffer's target becomes its color buffer. */
    HPBuf *p = gh_get(ARG(1), TAG_PBUFFER);
    if (!p) { RET(0); return; }
    GLint tex = 0;
    glGetIntegerv(p->target == GL_TEXTURE_RECTANGLE_EXT ? GL_TEXTURE_BINDING_RECTANGLE_EXT : GL_TEXTURE_BINDING_2D, &tex);
    if (!tex || (GLuint)tex == p->tex) { RET(1); return; }
    if (p->own_tex) glDeleteTextures(1, &p->tex);
    p->tex = (GLuint)tex;
    p->own_tex = 0;
    pbuf_alloc_tex(p, p->tex);
    if (p->fbo) pbuf_attach(p);
    RET(1);
}
SHIM(_aglSetPBuffer) {
    /* (AGLContext, AGLPbuffer, GLint face, GLint level, GLint virtualScreen) */
    HCtx *c = ctx_of(ARG(0));
    HPBuf *p = gh_get(ARG(1), TAG_PBUFFER);
    if (!c || !p) { RET(0); return; }
    pbuf_ensure_fbo(p);
    c->pbuf = p;
    if ([NSOpenGLContext currentContext] == c->ctx) {
        g_default_fbo = p->fbo;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, p->fbo);
        glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    }
    RET(1);
}
SHIM(_aglDestroyPBuffer) {
    HPBuf *p = gh_get(ARG(0), TAG_PBUFFER);
    if (!p) { RET(0); return; }
    for (int i = 0; i < g_nctx; i++)
        if (g_ctxs[i] && g_ctxs[i]->pbuf == p) g_ctxs[i]->pbuf = NULL;
    if (g_default_fbo == p->fbo) {
        g_default_fbo = 0;
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
    }
    if (p->fbo) glDeleteFramebuffersEXT(1, &p->fbo);
    if (p->depth) glDeleteRenderbuffersEXT(1, &p->depth);
    if (p->own_tex && p->tex) glDeleteTextures(1, &p->tex);
    gh_free(p->g);
    free(p);
    RET(1);
}

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
                                {2560, 1440}, {2560, 1600},
                                /* MacBook Air/Pro built-in panels: scaled "looks like" sizes, notch-free
                                 * 16:10 areas, and full Retina resolutions */
                                {1470, 956}, {1512, 945}, {1512, 982}, {1710, 1107}, {1728, 1080}, {1728, 1117},
                                {1800, 1169}, {2056, 1329}, {2560, 1664}, {2880, 1864}, {3024, 1890}, {3024, 1964},
                                {3420, 2214}, {3456, 2160}, {3456, 2234}};

static CFIndex mode_area(CFDictionaryRef d);
static int mode_int(CFDictionaryRef d, CFStringRef k);
static CFComparisonResult by_area(const void *a, const void *b, void *ctx) {
    CFIndex x = mode_area(a), y = mode_area(b);
    return x < y ? kCFCompareLessThan : x > y ? kCFCompareGreaterThan : kCFCompareEqualTo;
}

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

static void ensure_modes(void);
void plat_display_mode(int *w, int *h) {
    ensure_modes();
    *w = g_mode_w;
    *h = g_mode_h;
}

static CFArrayRef g_modes;
static CFDictionaryRef g_current_mode;

static void ensure_modes(void) {
    if (g_modes) return;
    NSScreen *screen = [NSScreen screens][0];
    NSRect sf = [screen frame];
    double scale = [screen backingScaleFactor];
    int sw = (int)sf.size.width, sh = (int)sf.size.height;
    int pw = (int)(sw * scale), ph = (int)(sh * scale); /* largest mode: the panel's pixels */
    int notch = 0;
    if (@available(macOS 12.0, *)) notch = (int)[screen safeAreaInsets].top;
    /* Candidates: the table, plus this screen's own sizes (with and without the notch strip). */
    int extra[4][2] = {{sw, sh}, {pw, ph}, {sw, sh - notch}, {pw, (int)(ph - notch * scale)}};
    int cand[64][2], nc = 0;
    for (size_t i = 0; i < sizeof kModes / sizeof kModes[0]; i++) memcpy(cand[nc++], kModes[i], sizeof cand[0]);
    for (int i = 0; i < 4; i++) memcpy(cand[nc++], extra[i], sizeof cand[0]);
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    for (int i = 0; i < nc; i++) {
        if (cand[i][0] > pw || cand[i][1] > ph) continue;
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= cand[j][0] == cand[i][0] && cand[j][1] == cand[i][1];
        if (dup) continue;
        CFDictionaryRef d = make_mode(cand[i][0], cand[i][1]);
        CFArrayAppendValue(a, d);
        CFRelease(d);
    }
    CFArraySortValues(a, CFRangeMake(0, CFArrayGetCount(a)), by_area, NULL);
    g_modes = a;
    if (getenv("CIV4_DEBUG_MODES"))
        for (CFIndex i = 0; i < CFArrayGetCount(a); i++)
            rt_log("mode %dx%d", mode_int(CFArrayGetValueAtIndex(a, i), CFSTR("Width")),
                   mode_int(CFArrayGetValueAtIndex(a, i), CFSTR("Height")));
    /* The "current" (desktop) mode is the screen's size in points. */
    g_current_mode = CFArrayGetValueAtIndex(a, CFArrayGetCount(a) - 1);
    for (CFIndex i = 0; i < CFArrayGetCount(a); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(a, i);
        if (mode_area(d) == (CFIndex)sw * sh) g_current_mode = d;
    }
    g_mode_w = sw;
    g_mode_h = sh;
}

/* Resolution for a first run (no saved GameDisplayWidth/Height): the main
 * screen's size in points, minus the camera notch strip (the picture is kept
 * below it), so each game pixel maps to whole screen pixels (2x2 on Retina)
 * instead of being scaled by an odd factor, which blurs text. */
static NSScreen *main_screen(void) {
    [NSApplication sharedApplication]; /* the window server connection: NSScreen is empty before it */
    NSArray *screens = [NSScreen screens];
    return screens.count ? screens[0] : nil;
}
void plat_default_resolution(int *w, int *h) {
    NSScreen *screen = main_screen();
    if (!screen) { *w = *h = 0; return; }
    NSRect sf = [screen frame];
    int notch = 0;
    if (@available(macOS 12.0, *)) notch = (int)[screen safeAreaInsets].top;
    *w = (int)sf.size.width;
    *h = (int)sf.size.height - notch;
}

/* A saved mode of the full notched screen (points or pixels) can only be shown
 * scaled down slightly, below the notch; use the notch-free height instead. */
int plat_fix_saved_height(int w, int h) {
    NSScreen *screen = main_screen();
    if (!screen) return h;
    NSRect sf = [screen frame];
    double scale = [screen backingScaleFactor];
    int notch = 0;
    if (@available(macOS 12.0, *)) notch = (int)[screen safeAreaInsets].top;
    int sw = (int)sf.size.width, sh = (int)sf.size.height;
    if (notch && w == sw && h == sh) return sh - notch;
    if (notch && w == (int)(sw * scale) && h == (int)(sh * scale)) return (int)((sh - notch) * scale);
    return h;
}

static CFIndex mode_area(CFDictionaryRef d) { return (CFIndex)mode_int(d, CFSTR("Width")) * mode_int(d, CFSTR("Height")); }
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
    /* CIV4_RESOLUTION=WxH overrides the resolution the game asks for. */
    const char *force = getenv("CIV4_RESOLUTION");
    int fw, fh;
    if (force && sscanf(force, "%dx%d", &fw, &fh) == 2) w = fw, h = fh;
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
    SHIM_ENTRY(_aglCreatePBuffer), SHIM_ENTRY(_aglTexImagePBuffer), SHIM_ENTRY(_aglSetPBuffer), SHIM_ENTRY(_aglDestroyPBuffer), SHIM_ENTRY(_CGLChoosePixelFormat), SHIM_ENTRY(_CGLDestroyPixelFormat),
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
