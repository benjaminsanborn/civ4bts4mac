/* OpenGL calls that need more than pointer translation:
 * - array/element pointers are buffer offsets while a VBO/IBO is bound,
 * - mapped buffers get a guest-visible shadow copy,
 * - strings and string arrays cross the guest/host boundary. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/OpenGL.h>
#include <math.h>
#include <string.h>

#include "shim.h"

static GLuint bound(GLenum target);
void gl_retarget_begin(void);
void gl_retarget_end(void);

/* While a buffer object is bound, "pointers" are offsets into it. The live
 * binding is queried because glPopClientAttrib can restore it behind our back. */
static const void *array_ptr(uint32_t a) { return bound(GL_ARRAY_BUFFER_ARB) ? (const void *)(uintptr_t)a : PTR(a); }
static const void *element_ptr(uint32_t a) {
    return bound(GL_ELEMENT_ARRAY_BUFFER_ARB) ? (const void *)(uintptr_t)a : PTR(a);
}

SHIM(_glBindBufferARB) { glBindBufferARB(ARG(0), ARG(1)); }

/* ---- The display port's window renders at more pixels than the game's mode
 * (agl.m): rectangles on it are scaled from game to window pixels, and read
 * back in game pixels. ---- */
int agl_window_scale(float *sx, float *sy);

typedef struct { int x, y, w, h; } Rect;
static Rect to_window(int x, int y, int w, int h, float sx, float sy) {
    int x0 = (int)lroundf(x * sx), y0 = (int)lroundf(y * sy);
    return (Rect){x0, y0, (int)lroundf((x + w) * sx) - x0, (int)lroundf((y + h) * sy) - y0};
}
static void from_window(GLint *r, float sx, float sy) {
    int x0 = (int)lroundf(r[0] / sx), y0 = (int)lroundf(r[1] / sy);
    int x1 = (int)lroundf((r[0] + r[2]) / sx), y1 = (int)lroundf((r[1] + r[3]) / sy);
    r[0] = x0, r[1] = y0, r[2] = x1 - x0, r[3] = y1 - y0;
}

/* Switching between the window and an FBO keeps the viewport and scissor
 * box, which are in different pixels on each: convert them. */
static int g_rt_was;
static float g_rt_sx, g_rt_sy;
void gl_retarget_begin(void) { g_rt_was = agl_window_scale(&g_rt_sx, &g_rt_sy); }
void gl_retarget_end(void) {
    float sx, sy;
    int now = agl_window_scale(&sx, &sy);
    if (now == g_rt_was && (!now || (sx == g_rt_sx && sy == g_rt_sy))) return;
    GLint vp[4], sc[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_SCISSOR_BOX, sc);
    if (g_rt_was) from_window(vp, g_rt_sx, g_rt_sy), from_window(sc, g_rt_sx, g_rt_sy);
    if (now) {
        Rect v = to_window(vp[0], vp[1], vp[2], vp[3], sx, sy), c = to_window(sc[0], sc[1], sc[2], sc[3], sx, sy);
        glViewport(v.x, v.y, v.w, v.h);
        glScissor(c.x, c.y, c.w, c.h);
    } else {
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glScissor(sc[0], sc[1], sc[2], sc[3]);
    }
}

SHIM(_glViewport) {
    float sx, sy;
    if (!agl_window_scale(&sx, &sy)) { glViewport((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3)); return; }
    Rect r = to_window((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), sx, sy);
    glViewport(r.x, r.y, r.w, r.h);
}
SHIM(_glScissor) {
    float sx, sy;
    if (!agl_window_scale(&sx, &sy)) { glScissor((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3)); return; }
    Rect r = to_window((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), sx, sy);
    glScissor(r.x, r.y, r.w, r.h);
}
SHIM(_glGetIntegerv) {
    GLenum pname = ARG(0);
    GLint *out = PTR(ARG(1));
    glGetIntegerv(pname, out);
    float sx, sy;
    if ((pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) && agl_window_scale(&sx, &sy)) from_window(out, sx, sy);
}
SHIM(_glGetFloatv) {
    GLenum pname = ARG(0);
    GLfloat *out = PTR(ARG(1));
    glGetFloatv(pname, out);
    float sx, sy;
    if ((pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX) && agl_window_scale(&sx, &sy)) {
        GLint r[4] = {(GLint)out[0], (GLint)out[1], (GLint)out[2], (GLint)out[3]};
        from_window(r, sx, sy);
        for (int i = 0; i < 4; i++) out[i] = (GLfloat)r[i];
    }
}
SHIM(_glWindowPos2i) {
    float sx, sy;
    if (agl_window_scale(&sx, &sy)) glWindowPos2i((GLint)lroundf((GLint)ARG(0) * sx), (GLint)lroundf((GLint)ARG(1) * sy));
    else glWindowPos2i((GLint)ARG(0), (GLint)ARG(1));
}
SHIM(_glCopyPixels) {
    float sx, sy;
    if (!agl_window_scale(&sx, &sy)) {
        glCopyPixels((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), ARG(4));
        return;
    }
    static int once;
    if (!once++) rt_log("glCopyPixels on the scaled window");
    Rect r = to_window((GLint)ARG(0), (GLint)ARG(1), (GLsizei)ARG(2), (GLsizei)ARG(3), sx, sy);
    glCopyPixels(r.x, r.y, r.w, r.h, ARG(4));
}

/* A window region scaled down to w x h game pixels in a scratch FBO, which is
 * left bound for reading. The window can't be the source of a scaling blit,
 * so the region is first copied as is into a texture (as the game's own
 * copies do), and that is blitted down. */
typedef struct { GLuint fbo, obj; int w, h; } Scratch;
static Scratch g_full, g_down; /* full-size copy (texture), downscaled result (renderbuffer) */
static CGLContextObj g_scratch_ctx;
static void scratch_size(Scratch *s, int w, int h, int tex) {
    if (s->fbo && w <= s->w && h <= s->h) return;
    if (!s->fbo) {
        glGenFramebuffersEXT(1, &s->fbo);
        if (tex) glGenTextures(1, &s->obj);
        else glGenRenderbuffersEXT(1, &s->obj);
    }
    /* Sized once for any copy from the window (a few at most), not regrown per size. */
    s->w = w > s->w ? w : s->w;
    s->h = h > s->h ? h : s->h;
    if (s->w < 2048) s->w = 2048;
    if (s->h < 2048) s->h = 2048;
    GLint prev_fb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &prev_fb);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, s->fbo);
    if (tex) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glBindTexture(GL_TEXTURE_2D, s->obj);
        /* With the game's client storage on, allocating without data fails. */
        GLint client_storage = 0;
        glGetIntegerv(GL_UNPACK_CLIENT_STORAGE_APPLE, &client_storage);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, client_storage);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, s->obj, 0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    } else {
        GLint prev = 0;
        glGetIntegerv(GL_RENDERBUFFER_BINDING_EXT, &prev);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, s->obj);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, s->w, s->h);
        glFramebufferRenderbufferEXT(GL_DRAW_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, s->obj);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, (GLuint)prev);
    }
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, (GLuint)prev_fb);
}
static void downsample(int x, int y, int w, int h, float sx, float sy) {
    CGLContextObj cgl = CGLGetCurrentContext();
    if (cgl != g_scratch_ctx) { /* another context: its own objects (the old ones leak) */
        memset(&g_full, 0, sizeof g_full);
        memset(&g_down, 0, sizeof g_down);
        g_scratch_ctx = cgl;
    }
    Rect r = to_window(x, y, w, h, sx, sy);
    scratch_size(&g_full, r.w, r.h, 1);
    scratch_size(&g_down, w, h, 0);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, g_full.obj);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, r.x, r.y, r.w, r.h);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, g_full.fbo);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, g_down.fbo);
    glBlitFramebufferEXT(0, 0, r.w, r.h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, 0);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, g_down.fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}
static int color_format(GLenum f) {
    return f != GL_DEPTH_COMPONENT && f != GL_STENCIL_INDEX && f != GL_DEPTH_STENCIL_EXT;
}
SHIM(_glReadPixels) {
    GLint x = (GLint)ARG(0), y = (GLint)ARG(1), w = (GLsizei)ARG(2), h = (GLsizei)ARG(3);
    float sx, sy;
    if (!agl_window_scale(&sx, &sy) || w <= 0 || h <= 0) {
        glReadPixels(x, y, w, h, ARG(4), ARG(5), PTR(ARG(6)));
        return;
    }
    if (!color_format(ARG(4))) { /* nearest sample: one pixel per game pixel */
        static int once;
        if (!once++) rt_log("glReadPixels of format %04x on the scaled window", ARG(4));
        glReadPixels(x, y, w, h, ARG(4), ARG(5), PTR(ARG(6)));
        return;
    }
    downsample(x, y, w, h, sx, sy);
    glReadPixels(0, 0, w, h, ARG(4), ARG(5), PTR(ARG(6)));
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
}
SHIM(_glCopyTexSubImage2D) {
    /* (target, level, xoffset, yoffset, x, y, width, height) */
    GLint x = (GLint)ARG(4), y = (GLint)ARG(5), w = (GLsizei)ARG(6), h = (GLsizei)ARG(7);
    float sx, sy;
    if (!agl_window_scale(&sx, &sy) || w <= 0 || h <= 0) {
        glCopyTexSubImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), x, y, w, h);
        return;
    }
    downsample(x, y, w, h, sx, sy);
    glCopyTexSubImage2D(ARG(0), (GLint)ARG(1), (GLint)ARG(2), (GLint)ARG(3), 0, 0, w, h);
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
}
SHIM(_glCopyTexImage2D) {
    /* (target, level, internalformat, x, y, width, height, border) */
    GLint x = (GLint)ARG(3), y = (GLint)ARG(4), w = (GLsizei)ARG(5), h = (GLsizei)ARG(6);
    float sx, sy;
    if (!agl_window_scale(&sx, &sy) || w <= 0 || h <= 0 || !color_format(ARG(2))) {
        glCopyTexImage2D(ARG(0), (GLint)ARG(1), ARG(2), x, y, w, h, (GLint)ARG(7));
        return;
    }
    downsample(x, y, w, h, sx, sy);
    glCopyTexImage2D(ARG(0), (GLint)ARG(1), ARG(2), 0, 0, w, h, (GLint)ARG(7));
    glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
}

/* ---- pbuffers are FBOs (agl.m): while one is the drawable, it stands in for
 * the window-system framebuffer 0 and its GL_BACK/GL_FRONT buffers. ---- */
GLuint agl_default_fbo(void);
static GLenum window_buffer(GLenum b) {
    GLuint fbo = agl_default_fbo();
    if (!fbo) return b;
    GLint cur = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &cur);
    if ((GLuint)cur != fbo) return b;
    switch (b) {
    case GL_BACK: case GL_FRONT: case GL_BACK_LEFT: case GL_FRONT_LEFT: case GL_FRONT_AND_BACK: case GL_LEFT:
        return GL_COLOR_ATTACHMENT0_EXT;
    default: return b;
    }
}
SHIM(_glBindFramebufferEXT) {
    gl_retarget_begin();
    glBindFramebufferEXT(ARG(0), ARG(1) ? ARG(1) : agl_default_fbo());
    gl_retarget_end();
}
SHIM(_glDrawBuffer) { glDrawBuffer(window_buffer(ARG(0))); }
SHIM(_glReadBuffer) { glReadBuffer(window_buffer(ARG(0))); }

SHIM(_glVertexPointer) { glVertexPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), array_ptr(ARG(3))); }
SHIM(_glNormalPointer) { glNormalPointer(ARG(0), (GLsizei)ARG(1), array_ptr(ARG(2))); }
SHIM(_glColorPointer) { glColorPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), array_ptr(ARG(3))); }
SHIM(_glTexCoordPointer) { glTexCoordPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), array_ptr(ARG(3))); }
SHIM(_glSecondaryColorPointer) { glSecondaryColorPointer((GLint)ARG(0), ARG(1), (GLsizei)ARG(2), array_ptr(ARG(3))); }
SHIM(_glVertexAttribPointerARB) {
    glVertexAttribPointerARB(ARG(0), (GLint)ARG(1), ARG(2), (GLboolean)ARG(3), (GLsizei)ARG(4), array_ptr(ARG(5)));
}
SHIM(_glDrawRangeElements) {
    glDrawRangeElements(ARG(0), ARG(1), ARG(2), (GLsizei)ARG(3), ARG(4), element_ptr(ARG(5)));
}

/* ---- mapped buffers: shadow copies in guest memory ---- */
typedef struct {
    GLenum target;
    GLuint buffer;
    void *host;
    uint32_t guest, size;
    int explicit_flush; /* only flushed ranges are copied to the buffer */
} Mapping;
#define MAX_MAPS 16
static Mapping g_maps[MAX_MAPS];

static GLuint bound(GLenum target) {
    GLint b = 0;
    glGetIntegerv(target == GL_ARRAY_BUFFER_ARB ? GL_ARRAY_BUFFER_BINDING_ARB : GL_ELEMENT_ARRAY_BUFFER_BINDING_ARB, &b);
    return (GLuint)b;
}

SHIM(_glMapBufferARB) {
    GLenum target = ARG(0), access = ARG(1);
    void *h = glMapBufferARB(target, access);
    if (!h) { RET(0); return; }
    GLint size = 0;
    glGetBufferParameterivARB(target, GL_BUFFER_SIZE_ARB, &size);
    uint32_t g = gmemalign(16, (uint32_t)size);
    /* With BUFFER_FLUSHING_UNMAP_APPLE off, only the ranges the guest flushes
     * reach the buffer; the rest keeps its contents. */
    GLint flush = 1;
    glGetBufferParameterivARB(target, GL_BUFFER_FLUSHING_UNMAP_APPLE, &flush);
    flush = !flush;
    /* The shadow must start as the buffer's contents: a write-only map may still
     * change just part of it, and unmap copies the whole shadow back. With
     * explicit flushing only flushed ranges are copied, so it needn't. */
    if (access != GL_WRITE_ONLY_ARB || !flush) memcpy(PTR(g), h, (size_t)size);
    for (int i = 0; i < MAX_MAPS; i++) {
        if (!g_maps[i].host) {
            g_maps[i] = (Mapping){target, bound(target), h, g, (uint32_t)size, flush};
            RET(g);
            return;
        }
    }
    rt_fatal("too many mapped GL buffers");
}

static Mapping *mapping_for(GLenum target) {
    GLuint b = bound(target);
    for (int i = 0; i < MAX_MAPS; i++)
        if (g_maps[i].host && g_maps[i].target == target && g_maps[i].buffer == b) return &g_maps[i];
    return NULL;
}

SHIM(_glFlushMappedBufferRangeAPPLE) {
    Mapping *m = mapping_for(ARG(0));
    uint32_t off = ARG(1), len = ARG(2);
    if (m && off + len <= m->size) memcpy((uint8_t *)m->host + off, PTR(m->guest + off), len);
    glFlushMappedBufferRangeAPPLE(ARG(0), (GLintptr)off, (GLsizeiptr)len);
}

SHIM(_glUnmapBufferARB) {
    Mapping *m = mapping_for(ARG(0));
    if (m) {
        /* With explicit flushing the flushed ranges are already copied; the rest
         * of the shadow is not the buffer's contents and must not overwrite them. */
        if (!m->explicit_flush) memcpy(m->host, PTR(m->guest), m->size);
        gfree(m->guest);
        m->host = NULL;
    }
    RET(glUnmapBufferARB(ARG(0)));
}

/* ---- strings ---- */
SHIM(_glGetString) {
    GLenum name = ARG(0);
    static struct { GLenum name; uint32_t g; } cache[8];
    for (int i = 0; i < 8; i++)
        if (cache[i].g && cache[i].name == name) { RET(cache[i].g); return; }
    const char *s = (const char *)glGetString(name);
    if (!s) { RET(0); return; }
    uint32_t g = gstrdup(s);
    for (int i = 0; i < 8; i++)
        if (!cache[i].g) { cache[i].name = name; cache[i].g = g; break; }
    RET(g);
}

SHIM(_glShaderSourceARB) {
    /* (handle, count, const char **strings, const GLint *lengths) */
    GLsizei n = (GLsizei)ARG(1);
    const GLcharARB *strs[64];
    if (n > 64) rt_fatal("glShaderSourceARB: too many strings");
    for (GLsizei i = 0; i < n; i++) strs[i] = CPTR(R32_(g_mem, ARG(2) + 4 * (uint32_t)i));
    glShaderSourceARB((GLhandleARB)(uintptr_t)ARG(0), n, strs, PTR(ARG(3)));
}

SHIM(_gluCheckExtension) {
    /* Whole-word search of extName in the space-separated extString. */
    const char *name = CPTR(ARG(0)), *list = CPTR(ARG(1));
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ' ');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !memcmp(p, name, n)) { RET(1); return; }
        p = e ? e + 1 : NULL;
    }
    RET(0);
}

const ShimDef shims_gl_special[] = {
    SHIM_ENTRY(_glBindFramebufferEXT), SHIM_ENTRY(_glDrawBuffer), SHIM_ENTRY(_glReadBuffer),
    SHIM_ENTRY(_glBindBufferARB), SHIM_ENTRY(_glVertexPointer), SHIM_ENTRY(_glNormalPointer),
    SHIM_ENTRY(_glColorPointer), SHIM_ENTRY(_glTexCoordPointer), SHIM_ENTRY(_glSecondaryColorPointer),
    SHIM_ENTRY(_glVertexAttribPointerARB), SHIM_ENTRY(_glDrawRangeElements), SHIM_ENTRY(_glMapBufferARB),
    SHIM_ENTRY(_glFlushMappedBufferRangeAPPLE), SHIM_ENTRY(_glUnmapBufferARB), SHIM_ENTRY(_glGetString),
    SHIM_ENTRY(_glShaderSourceARB), SHIM_ENTRY(_gluCheckExtension), SHIM_ENTRY(_glViewport), SHIM_ENTRY(_glScissor),
    SHIM_ENTRY(_glGetIntegerv), SHIM_ENTRY(_glGetFloatv), SHIM_ENTRY(_glWindowPos2i), SHIM_ENTRY(_glCopyPixels),
    SHIM_ENTRY(_glReadPixels), SHIM_ENTRY(_glCopyTexSubImage2D), SHIM_ENTRY(_glCopyTexImage2D),
    {0, 0},
};
