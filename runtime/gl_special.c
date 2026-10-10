/* OpenGL calls that need more than pointer translation:
 * - array/element pointers are buffer offsets while a VBO/IBO is bound,
 * - mapped buffers get a guest-visible shadow copy,
 * - strings and string arrays cross the guest/host boundary. */
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <string.h>

#include "shim.h"

static GLuint bound(GLenum target);

/* While a buffer object is bound, "pointers" are offsets into it. The live
 * binding is queried because glPopClientAttrib can restore it behind our back. */
static const void *array_ptr(uint32_t a) { return bound(GL_ARRAY_BUFFER_ARB) ? (const void *)(uintptr_t)a : PTR(a); }
static const void *element_ptr(uint32_t a) {
    return bound(GL_ELEMENT_ARRAY_BUFFER_ARB) ? (const void *)(uintptr_t)a : PTR(a);
}

SHIM(_glBindBufferARB) { glBindBufferARB(ARG(0), ARG(1)); }

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
SHIM(_glBindFramebufferEXT) { glBindFramebufferEXT(ARG(0), ARG(1) ? ARG(1) : agl_default_fbo()); }
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
    if (access != GL_WRITE_ONLY_ARB) memcpy(PTR(g), h, (size_t)size);
    for (int i = 0; i < MAX_MAPS; i++) {
        if (!g_maps[i].host) {
            g_maps[i] = (Mapping){target, bound(target), h, g, (uint32_t)size};
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
        /* With FLUSHING_UNMAP disabled the guest flushes ranges itself; copying the
         * whole shadow is still correct since unflushed ranges are undefined. */
        memcpy(m->host, PTR(m->guest), m->size);
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
    SHIM_ENTRY(_glShaderSourceARB), SHIM_ENTRY(_gluCheckExtension),
    {0, 0},
};
