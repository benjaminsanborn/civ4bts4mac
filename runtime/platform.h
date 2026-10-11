/* Shared state between the Cocoa platform layer, AGL, and QuickDraw shims. */
#ifndef CIV4_PLATFORM_H
#define CIV4_PLATFORM_H

#include "shim.h"

typedef struct {
    uint32_t creator, tag, size;
    void *data;
} WinProp;

typedef struct HWindow {
    uint32_t g;      /* guest WindowRef */
    uint32_t port;   /* guest CGrafPtr for this window */
    uint32_t target; /* EventTargetRef */
    uint32_t wclass, attrs, refcon;
    int visible;
    void *ns;   /* NSWindow (retained) */
    void *view; /* content NSView */
    WinProp *props;
    int nprops;
    int mode_w, mode_h; /* nonzero for a display port: logical (rendered) size */
} HWindow;

HWindow *win_of(uint32_t g);
uint32_t win_event_target(uint32_t window);
uint32_t plat_mouse_buttons(void);
void plat_mouse_global(int16_t *v, int16_t *h);
void plat_display_mode(int *w, int *h); /* current virtual display mode (agl.m) */
void plat_default_resolution(int *w, int *h); /* first-run resolution (agl.m) */
int plat_fix_saved_height(int w, int h);      /* notch-free height for a full-screen size */
int plat_mode_offered(int w, int h);          /* in the resolution list (agl.m) */

#endif
