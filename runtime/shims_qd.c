/* Minimal QuickDraw: rectangles, rectangular regions, ports, devices.
 *
 * Regions are real guest Handles (pointer to a master pointer to
 * {UInt16 rgnSize; Rect rgnBBox}) because QuickDraw-era code dereferences
 * them directly. Only rectangular regions exist here. */
#include "handles.h"
#include "platform.h"

#include <CoreGraphics/CoreGraphics.h>
#include <string.h>

/* Rect = {top, left, bottom, right} as SInt16 */
static void get_rect(uint32_t g, int16_t r[4]) { memcpy(r, PTR(g), 8); }
static void put_rect(uint32_t g, const int16_t r[4]) { memcpy(PTR(g), r, 8); }

static uint32_t rgn_data(uint32_t rgn) { return R32_(g_mem, rgn); }

void qd_rgn_set_rect(uint32_t rgn, const int16_t r[4]) {
    uint32_t d = rgn_data(rgn);
    W16_(g_mem, d, 10);
    int16_t e[4] = {0, 0, 0, 0};
    put_rect(d + 2, (r[2] <= r[0] || r[3] <= r[1]) ? e : r);
}

SHIM(_NewRgn) {
    uint32_t h = gmalloc(4), d = gcalloc(1, 16);
    W32_(g_mem, h, d);
    W16_(g_mem, d, 10);
    RET(h);
}
SHIM(_DisposeRgn) { gfree(rgn_data(ARG(0))); gfree(ARG(0)); }
SHIM(_RectRgn) { int16_t r[4]; get_rect(ARG(1), r); qd_rgn_set_rect(ARG(0), r); }
SHIM(_CopyRgn) { memcpy(PTR(rgn_data(ARG(1))), PTR(rgn_data(ARG(0))), 10); }
SHIM(_EmptyRgn) {
    int16_t r[4];
    get_rect(rgn_data(ARG(0)) + 2, r);
    RET(r[2] <= r[0] || r[3] <= r[1]);
}
SHIM(_IsRegionRectangular) { RET(1); }
SHIM(_GetRegionBounds) { memcpy(PTR(ARG(1)), PTR(rgn_data(ARG(0)) + 2), 8); RET(ARG(1)); }
SHIM(_GetGrayRgn) {
    static uint32_t gray;
    if (!gray) {
        gray = gmalloc(4);
        W32_(g_mem, gray, gcalloc(1, 16));
        int16_t r[4] = {0, 0, 4096, 8192};
        qd_rgn_set_rect(gray, r);
    }
    RET(gray);
}

/* ---- rect utilities ---- */
SHIM(_OffsetRect) {
    int16_t r[4];
    get_rect(ARG(0), r);
    int16_t dh = (int16_t)ARG(1), dv = (int16_t)ARG(2);
    r[0] += dv; r[2] += dv; r[1] += dh; r[3] += dh;
    put_rect(ARG(0), r);
}
SHIM(_InsetRect) {
    int16_t r[4];
    get_rect(ARG(0), r);
    int16_t dh = (int16_t)ARG(1), dv = (int16_t)ARG(2);
    r[0] += dv; r[2] -= dv; r[1] += dh; r[3] -= dh;
    put_rect(ARG(0), r);
}
SHIM(_EqualRect) { RET(!memcmp(PTR(ARG(0)), PTR(ARG(1)), 8)); }
SHIM(_EmptyRect) { int16_t r[4]; get_rect(ARG(0), r); RET(r[2] <= r[0] || r[3] <= r[1]); }
SHIM(_PtInRect) {
    /* (Point by value {v,h}, const Rect *) */
    int16_t v = (int16_t)(ARG(0) & 0xffff), h = (int16_t)(ARG(0) >> 16), r[4];
    get_rect(ARG(1), r);
    RET(v >= r[0] && v < r[2] && h >= r[1] && h < r[3]);
}

SHIM(_SetRect) {
    int16_t r[4] = {(int16_t)ARG(2), (int16_t)ARG(1), (int16_t)ARG(4), (int16_t)ARG(3)}; /* (rect, l, t, r, b) */
    put_rect(ARG(0), r);
}
SHIM(_GetQDGlobalsWhite) { memset(PTR(ARG(0)), 0, 8); RET(ARG(0)); }
SHIM(_GetQDGlobalsBlack) { memset(PTR(ARG(0)), 0xff, 8); RET(ARG(0)); }
SHIM(_DMGetGDeviceByDisplayID) { if (ARG(1)) W32_(g_mem, ARG(1), 0x6764); RET(noErr); }
SHIM(_DMGetDisplayIDByGDevice) { if (ARG(1)) W32_(g_mem, ARG(1), 0x04272c40u /* GUEST_DISPLAY_ID */); RET(noErr); }

/* ---- CGGeometry (i386: CGFloat is float; CGRect = 4 floats passed by value) ---- */
static CGRect arg_rect(CPU *cpu, int slot) {
    return CGRectMake(RF32_(g_mem, cpu->r[4] + 4 + 4 * slot), RF32_(g_mem, cpu->r[4] + 8 + 4 * slot),
                      RF32_(g_mem, cpu->r[4] + 12 + 4 * slot), RF32_(g_mem, cpu->r[4] + 16 + 4 * slot));
}
static void put_cgrect(uint32_t g, CGRect r) {
    float f[4] = {(float)r.origin.x, (float)r.origin.y, (float)r.size.width, (float)r.size.height};
    memcpy(PTR(g), f, 16);
}
SHIM(_CGRectGetMinX) { RETF(CGRectGetMinX(arg_rect(cpu, 0))); }
SHIM(_CGRectGetMaxX) { RETF(CGRectGetMaxX(arg_rect(cpu, 0))); }
SHIM(_CGRectGetMinY) { RETF(CGRectGetMinY(arg_rect(cpu, 0))); }
SHIM(_CGRectGetMaxY) { RETF(CGRectGetMaxY(arg_rect(cpu, 0))); }
SHIM(_CGRectContainsPoint) { RET(CGRectContainsPoint(arg_rect(cpu, 0), CGPointMake(ARGF32(4), ARGF32(5)))); }
SHIM(_CGRectContainsRect) { RET(CGRectContainsRect(arg_rect(cpu, 0), arg_rect(cpu, 4))); }
/* Struct results come back through a hidden pointer that the callee pops. */
SHIM(_CGRectInset) {
    put_cgrect(ARG(0), CGRectInset(arg_rect(cpu, 1), ARGF32(5), ARGF32(6)));
    RET(ARG(0));
    cpu->r[4] += 4;
}
SHIM(_CGRectOffset) {
    put_cgrect(ARG(0), CGRectOffset(arg_rect(cpu, 1), ARGF32(5), ARGF32(6)));
    RET(ARG(0));
    cpu->r[4] += 4;
}
SHIM(_CGAffineTransformScale) {
    /* (sret, CGAffineTransform t = 6 floats, sx, sy) */
    float t[6];
    memcpy(t, PTR(ARGADDR(1)), 24);
    CGAffineTransform r = CGAffineTransformScale(CGAffineTransformMake(t[0], t[1], t[2], t[3], t[4], t[5]), ARGF32(7), ARGF32(8));
    float o[6] = {(float)r.a, (float)r.b, (float)r.c, (float)r.d, (float)r.tx, (float)r.ty};
    memcpy(PTR(ARG(0)), o, 24);
    RET(ARG(0));
    cpu->r[4] += 4;
}

/* ---- ports: window ports are TAG_PORT handles; port bounds are window-local.
 * The current port and GWorlds live in quickdraw.c. ---- */
int qd_gworld_bounds(uint32_t port, int16_t r[4]);
SHIM(_GetMainDevice) { RET(0x6764); }
SHIM(_GetDeviceList) { RET(0x6764); }
SHIM(_GetNextDevice) { RET(0); }
SHIM(_SetGDevice) {}
SHIM(_TestDeviceAttribute) { RET(0); }

extern void plat_window_content_size(HWindow *w, int *width, int *height);
SHIM(_GetPortBounds) {
    int16_t gr[4];
    if (qd_gworld_bounds(ARG(0), gr)) { put_rect(ARG(1), gr); RET(ARG(1)); return; }
    HWindow *w = gh_get(ARG(0), TAG_PORT);
    int width = 640, height = 480;
    if (w) plat_window_content_size(w, &width, &height);
    int16_t r[4] = {0, 0, (int16_t)height, (int16_t)width};
    put_rect(ARG(1), r);
    RET(ARG(1));
}
SHIM(_SetPortBounds) {}
SHIM(_GetPortClipRegion) {
    int16_t r[4] = {-32768, -32768, 32767, 32767};
    qd_rgn_set_rect(ARG(1), r);
    RET(ARG(1));
}
SHIM(_GetPortVisibleRegion) {
    HWindow *w = gh_get(ARG(0), TAG_PORT);
    int width = 640, height = 480;
    if (w) plat_window_content_size(w, &width, &height);
    int16_t r[4] = {0, 0, (int16_t)height, (int16_t)width};
    qd_rgn_set_rect(ARG(1), r);
    RET(ARG(1));
}
SHIM(_ClipRect) {}
SHIM(_CreateNewPort) { RET(gh_new(TAG_PORT, NULL)); }
SHIM(_DisposePort) {}

const ShimDef shims_qd[] = {
    SHIM_ENTRY(_NewRgn), SHIM_ENTRY(_DisposeRgn), SHIM_ENTRY(_RectRgn), SHIM_ENTRY(_CopyRgn), SHIM_ENTRY(_EmptyRgn),
    SHIM_ENTRY(_IsRegionRectangular), SHIM_ENTRY(_GetRegionBounds), SHIM_ENTRY(_GetGrayRgn), SHIM_ENTRY(_OffsetRect),
    SHIM_ENTRY(_InsetRect), SHIM_ENTRY(_EqualRect), SHIM_ENTRY(_EmptyRect), SHIM_ENTRY(_PtInRect), SHIM_ENTRY(_GetMainDevice),
    SHIM_ENTRY(_GetDeviceList), SHIM_ENTRY(_GetNextDevice), SHIM_ENTRY(_SetGDevice), SHIM_ENTRY(_TestDeviceAttribute),
    SHIM_ENTRY(_GetPortBounds), SHIM_ENTRY(_SetPortBounds), SHIM_ENTRY(_GetPortClipRegion),
    SHIM_ENTRY(_GetPortVisibleRegion), SHIM_ENTRY(_ClipRect), SHIM_ENTRY(_CreateNewPort), SHIM_ENTRY(_DisposePort),
    SHIM_ENTRY(_SetRect), SHIM_ENTRY(_GetQDGlobalsWhite), SHIM_ENTRY(_GetQDGlobalsBlack),
    SHIM_ENTRY(_DMGetGDeviceByDisplayID), SHIM_ENTRY(_DMGetDisplayIDByGDevice), SHIM_ENTRY(_CGRectGetMinX),
    SHIM_ENTRY(_CGRectGetMaxX), SHIM_ENTRY(_CGRectGetMinY), SHIM_ENTRY(_CGRectGetMaxY), SHIM_ENTRY(_CGRectContainsPoint),
    SHIM_ENTRY(_CGRectContainsRect), SHIM_ENTRY(_CGRectInset), SHIM_ENTRY(_CGRectOffset),
    SHIM_ENTRY(_CGAffineTransformScale),
    {0, 0},
};
