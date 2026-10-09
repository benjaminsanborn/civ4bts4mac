/* Cocoa platform layer: the application, Carbon WindowRefs backed by
 * NSWindows, and OS input translated into Carbon events.
 *
 * Coordinates handed to the guest are QuickDraw globals: origin at the top
 * left of the main display, y down, in points. */
#import <Cocoa/Cocoa.h>

#include "cf.h"
#include "events.h"
#include "handles.h"
#include "platform.h"

#define noErr 0
#define kWindowStructureRgn 32
#define kWindowContentRgn 33

/* Carbon modifier bits */
#define cmdKey 0x0100
#define shiftKey 0x0200
#define alphaLock 0x0400
#define optionKey 0x0800
#define controlKey 0x1000

extern uint8_t g_keymap[16];
void ev_set_focus_target(uint32_t window_target);

static NSMutableArray *g_windows; /* HWindow*, front to back via NSApp order */
static HWindow *g_focus;
static uint32_t g_buttons; /* bit 0 left, 1 right, 2 middle */
static NSPoint g_mouse_global;

@interface GameView : NSView
@end
@implementation GameView
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)isFlipped { return YES; }
@end

@interface GameWindow : NSWindow
@end
@implementation GameWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

/* ---- application ---- */
static void ensure_app(void) {
    static int done;
    if (done) return;
    done = 1;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    [NSApp activateIgnoringOtherApps:YES];
    g_windows = [NSMutableArray new];
}

static double main_screen_height(void) { return NSMaxY([[NSScreen screens][0] frame]); }

static NSRect qd_to_cocoa(int top, int left, int bottom, int right) {
    double h = main_screen_height();
    return NSMakeRect(left, h - bottom, right - left, bottom - top);
}

static void cocoa_to_qd(NSRect r, int16_t out[4]) {
    double h = main_screen_height();
    out[0] = (int16_t)(h - NSMaxY(r)); /* top */
    out[1] = (int16_t)NSMinX(r);       /* left */
    out[2] = (int16_t)(h - NSMinY(r)); /* bottom */
    out[3] = (int16_t)NSMaxX(r);       /* right */
}

HWindow *win_of(uint32_t g) {
    HWindow *w = gh_get(g, TAG_WINDOW);
    if (!w) rt_fatal("bad WindowRef %08x", g);
    return w;
}

uint32_t win_event_target(uint32_t window) {
    HWindow *w = gh_get(window, TAG_WINDOW);
    return w ? w->target : 0;
}

/* ---- input translation ---- */
static uint32_t carbon_modifiers(NSEventModifierFlags f) {
    uint32_t m = 0;
    if (f & NSEventModifierFlagCommand) m |= cmdKey;
    if (f & NSEventModifierFlagShift) m |= shiftKey;
    if (f & NSEventModifierFlagCapsLock) m |= alphaLock;
    if (f & NSEventModifierFlagOption) m |= optionKey;
    if (f & NSEventModifierFlagControl) m |= controlKey;
    return m;
}

static HWindow *hwindow_for(NSWindow *nw) {
    for (NSValue *v in g_windows) {
        HWindow *w = [v pointerValue];
        if (w->ns == (__bridge void *)nw) return w;
    }
    return NULL;
}

static void add_mouse_params(HEvent *e, NSEvent *ev, HWindow *w) {
    NSPoint p = [NSEvent mouseLocation];
    float global[2] = {(float)p.x, (float)(main_screen_height() - p.y)};
    g_mouse_global = NSMakePoint(global[0], global[1]);
    ev_set_param(e, kEventParamMouseLocation, typeHIPoint, global, 8);
    if (w) {
        NSRect content = [(__bridge NSWindow *)w->ns contentRectForFrameRect:[(__bridge NSWindow *)w->ns frame]];
        int16_t qd[4];
        cocoa_to_qd(content, qd);
        float local[2] = {global[0] - qd[1], global[1] - qd[0]};
        ev_set_param(e, kEventParamWindowMouseLocation, typeHIPoint, local, 8);
        ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    }
    uint32_t mods = carbon_modifiers([ev modifierFlags]);
    ev_set_param(e, kEventParamKeyModifiers, typeUInt32, &mods, 4);
    float delta[2] = {(float)[ev deltaX], (float)[ev deltaY]};
    ev_set_param(e, kEventParamMouseDelta, typeHIPoint, delta, 8);
}

static void post_mouse(NSEvent *ev, uint32_t kind, uint16_t button) {
    HWindow *w = [ev window] ? hwindow_for([ev window]) : g_focus;
    HEvent *e = ev_create(kEventClassMouse, kind);
    add_mouse_params(e, ev, w);
    if (button) {
        ev_set_param(e, kEventParamMouseButton, typeMouseButton, &button, 2);
        uint32_t clicks = (uint32_t)[ev clickCount];
        ev_set_param(e, kEventParamClickCount, typeUInt32, &clicks, 4);
    }
    ev_post(e);
    ev_release(e);
}

static void post_key(NSEvent *ev, uint32_t kind) {
    uint32_t code = [ev keyCode], mods = carbon_modifiers([ev modifierFlags]), kbd = 40;
    if (code < 128) {
        if (kind == kEventRawKeyUp) g_keymap[code >> 3] &= ~(1u << (code & 7));
        else g_keymap[code >> 3] |= 1u << (code & 7);
    }
    NSString *chars = kind == kEventRawKeyModifiersChanged ? @"" : [ev characters];
    HEvent *e = ev_create(kEventClassKeyboard, kind);
    ev_set_param(e, kEventParamKeyCode, typeUInt32, &code, 4);
    ev_set_param(e, kEventParamKeyModifiers, typeUInt32, &mods, 4);
    ev_set_param(e, kEventParamKeyboardType, typeUInt32, &kbd, 4);
    if (kind != kEventRawKeyModifiersChanged) {
        char mac = 0;
        NSString *raw = [ev charactersIgnoringModifiers];
        if ([chars length]) mac = (char)[chars characterAtIndex:0];
        else if ([raw length]) mac = (char)[raw characterAtIndex:0];
        ev_set_param(e, kEventParamKeyMacCharCodes, typeChar, &mac, 1);
        unichar u[16];
        NSUInteger n = MIN([chars length], 16u);
        [chars getCharacters:u range:NSMakeRange(0, n)];
        ev_set_param(e, kEventParamKeyUnicodes, typeUnicodeText, u, (uint32_t)(2 * n));
    }
    if (g_focus) ev_set_param(e, kEventParamWindowRef, typeWindowRef, &g_focus->g, 4);
    ev_post(e);
    ev_release(e);
}

static void translate(NSEvent *ev) {
    switch ([ev type]) {
    case NSEventTypeLeftMouseDown: g_buttons |= 1; post_mouse(ev, kEventMouseDown, 1); break;
    case NSEventTypeLeftMouseUp: g_buttons &= ~1u; post_mouse(ev, kEventMouseUp, 1); break;
    case NSEventTypeRightMouseDown: g_buttons |= 2; post_mouse(ev, kEventMouseDown, 2); break;
    case NSEventTypeRightMouseUp: g_buttons &= ~2u; post_mouse(ev, kEventMouseUp, 2); break;
    case NSEventTypeOtherMouseDown: g_buttons |= 4; post_mouse(ev, kEventMouseDown, 3); break;
    case NSEventTypeOtherMouseUp: g_buttons &= ~4u; post_mouse(ev, kEventMouseUp, 3); break;
    case NSEventTypeMouseMoved: post_mouse(ev, kEventMouseMoved, 0); break;
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged: post_mouse(ev, kEventMouseDragged, 0); break;
    case NSEventTypeScrollWheel: {
        HWindow *w = [ev window] ? hwindow_for([ev window]) : g_focus;
        HEvent *e = ev_create(kEventClassMouse, kEventMouseWheelMoved);
        add_mouse_params(e, ev, w);
        uint16_t axis = 1; /* kEventMouseWheelAxisY */
        int32_t delta = (int32_t)lround([ev scrollingDeltaY] * ([ev hasPreciseScrollingDeltas] ? 0.1 : 1.0));
        if (delta) {
            ev_set_param(e, kEventParamMouseWheelAxis, typeMouseWheelAxis, &axis, 2);
            ev_set_param(e, kEventParamMouseWheelDelta, typeSInt32, &delta, 4);
            ev_post(e);
        }
        ev_release(e);
        break;
    }
    case NSEventTypeKeyDown: post_key(ev, [ev isARepeat] ? kEventRawKeyRepeat : kEventRawKeyDown); break;
    case NSEventTypeKeyUp: post_key(ev, kEventRawKeyUp); break;
    case NSEventTypeFlagsChanged: post_key(ev, kEventRawKeyModifiersChanged); break;
    default: break;
    }
}

void plat_pump(double timeout) {
    ensure_app();
    @autoreleasepool {
        NSDate *until = timeout > 0 ? [NSDate dateWithTimeIntervalSinceNow:timeout] : [NSDate distantPast];
        for (;;) {
            NSEvent *ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
            if (!ev) break;
            translate(ev);
            /* Window management still goes through AppKit; key events would only beep. */
            NSEventType t = [ev type];
            if (t != NSEventTypeKeyDown && t != NSEventTypeKeyUp) [NSApp sendEvent:ev];
            until = [NSDate distantPast];
        }
    }
}

uint32_t plat_mouse_buttons(void) { return g_buttons; }
void plat_window_content_size(HWindow *w, int *width, int *height) {
    NSRect b = [(__bridge NSView *)w->view bounds];
    *width = (int)b.size.width;
    *height = (int)b.size.height;
}
void plat_mouse_global(int16_t *v, int16_t *h) {
    NSPoint p = [NSEvent mouseLocation];
    *h = (int16_t)p.x;
    *v = (int16_t)(main_screen_height() - p.y);
}

/* ---- window delegate: focus and close ---- */
@interface WinDelegate : NSObject <NSWindowDelegate>
@end
@implementation WinDelegate
- (void)windowDidBecomeKey:(NSNotification *)n {
    HWindow *w = hwindow_for([n object]);
    if (!w) return;
    g_focus = w;
    ev_set_focus_target(w->target);
    HEvent *e = ev_create(kEventClassWindow, kEventWindowActivated);
    ev_set_param(e, kEventParamDirectObject, typeWindowRef, &w->g, 4);
    ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    ev_post(e);
    ev_release(e);
}
- (void)windowDidResignKey:(NSNotification *)n {
    HWindow *w = hwindow_for([n object]);
    if (!w) return;
    HEvent *e = ev_create(kEventClassWindow, kEventWindowDeactivated);
    ev_set_param(e, kEventParamDirectObject, typeWindowRef, &w->g, 4);
    ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    ev_post(e);
    ev_release(e);
}
- (BOOL)windowShouldClose:(NSWindow *)nw {
    HWindow *w = hwindow_for(nw);
    if (w) {
        HEvent *e = ev_create(kEventClassWindow, kEventWindowClose);
        ev_set_param(e, kEventParamDirectObject, typeWindowRef, &w->g, 4);
        ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
        ev_post(e);
        ev_release(e);
    }
    return NO; /* the guest decides */
}
@end

/* ---- Window Manager shims ---- */
SHIM(_CreateNewWindow) {
    /* (WindowClass, WindowAttributes, const Rect *bounds, WindowRef *out) */
    ensure_app();
    uint32_t r = ARG(2);
    int top = (int16_t)R16_(g_mem, r), left = (int16_t)R16_(g_mem, r + 2);
    int bottom = (int16_t)R16_(g_mem, r + 4), right = (int16_t)R16_(g_mem, r + 6);
    HWindow *w = calloc(1, sizeof *w);
    w->wclass = ARG(0);
    w->attrs = ARG(1);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (w->wclass == 4 /* kPlainWindowClass */ || w->wclass == 6 || w->wclass == 13 /* overlay */ || w->wclass == 11)
        style = NSWindowStyleMaskBorderless;
    GameWindow *nw = [[GameWindow alloc] initWithContentRect:qd_to_cocoa(top, left, bottom, right)
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    [nw setReleasedWhenClosed:NO];
    [nw setAcceptsMouseMovedEvents:YES];
    static WinDelegate *delegate;
    if (!delegate) delegate = [WinDelegate new];
    [nw setDelegate:delegate];
    GameView *view = [[GameView alloc] initWithFrame:[[nw contentView] bounds]];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [nw setContentView:view];
    [nw makeFirstResponder:view];
    w->ns = (__bridge_retained void *)nw;
    w->view = (__bridge void *)view;
    w->target = ev_new_target(ev_app_target());
    w->g = gh_new(TAG_WINDOW, w);
    w->port = gh_new(TAG_PORT, w);
    [g_windows addObject:[NSValue valueWithPointer:w]];
    W32_(g_mem, ARG(3), w->g);
    rt_log("CreateNewWindow class %u attrs %08x bounds %d,%d %dx%d", w->wclass, w->attrs, left, top, right - left,
           bottom - top);
    RET(noErr);
}

SHIM(_DisposeWindow) {
    HWindow *w = win_of(ARG(0));
    NSWindow *nw = (__bridge_transfer NSWindow *)w->ns;
    [nw orderOut:nil];
    [nw close];
    for (NSUInteger i = 0; i < [g_windows count]; i++)
        if ([g_windows[i] pointerValue] == w) { [g_windows removeObjectAtIndex:i]; break; }
    if (g_focus == w) g_focus = NULL;
    gh_free(w->port);
    gh_free(w->g);
    free(w);
}

SHIM(_ShowWindow) {
    HWindow *w = win_of(ARG(0));
    w->visible = 1;
    [(__bridge NSWindow *)w->ns makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
SHIM(_HideWindow) {
    HWindow *w = win_of(ARG(0));
    w->visible = 0;
    [(__bridge NSWindow *)w->ns orderOut:nil];
}
SHIM(_SelectWindow) { [(__bridge NSWindow *)win_of(ARG(0))->ns makeKeyAndOrderFront:nil]; }
SHIM(_ActivateWindow) { if ((uint8_t)ARG(1)) [(__bridge NSWindow *)win_of(ARG(0))->ns makeKeyWindow]; RET(noErr); }
SHIM(_IsWindowVisible) { RET(win_of(ARG(0))->visible); }
SHIM(_IsWindowCollapsed) { RET([(__bridge NSWindow *)win_of(ARG(0))->ns isMiniaturized]); }
SHIM(_FrontWindow) {
    for (NSWindow *nw in [NSApp orderedWindows]) {
        HWindow *w = hwindow_for(nw);
        if (w) { RET(w->g); return; }
    }
    RET(0);
}
SHIM(_GetNextWindow) {
    BOOL found = NO;
    for (NSWindow *nw in [NSApp orderedWindows]) {
        HWindow *w = hwindow_for(nw);
        if (!w) continue;
        if (found) { RET(w->g); return; }
        if (w->g == ARG(0)) found = YES;
    }
    RET(0);
}

SHIM(_GetWindowBounds) {
    /* (window, WindowRegionCode, Rect *out) */
    NSWindow *nw = (__bridge NSWindow *)win_of(ARG(0))->ns;
    NSRect r = ARG(1) == kWindowStructureRgn ? [nw frame] : [nw contentRectForFrameRect:[nw frame]];
    int16_t qd[4];
    cocoa_to_qd(r, qd);
    memcpy(PTR(ARG(2)), qd, 8);
    RET(noErr);
}
SHIM(_SetWindowBounds) {
    NSWindow *nw = (__bridge NSWindow *)win_of(ARG(0))->ns;
    uint32_t r = ARG(2);
    NSRect cr = qd_to_cocoa((int16_t)R16_(g_mem, r), (int16_t)R16_(g_mem, r + 2), (int16_t)R16_(g_mem, r + 4),
                            (int16_t)R16_(g_mem, r + 6));
    NSRect fr = ARG(1) == kWindowStructureRgn ? cr : [nw frameRectForContentRect:cr];
    [nw setFrame:fr display:YES];
    RET(noErr);
}
SHIM(_RepositionWindow) { [(__bridge NSWindow *)win_of(ARG(0))->ns center]; RET(noErr); }
SHIM(_GetWindowRegion) {
    /* (window, regionCode, RgnHandle): regions are rectangles here; see regions in shims_qd.c */
    extern void qd_rgn_set_rect(uint32_t rgn, const int16_t r[4]);
    NSWindow *nw = (__bridge NSWindow *)win_of(ARG(0))->ns;
    NSRect r = ARG(1) == kWindowStructureRgn ? [nw frame] : [nw contentRectForFrameRect:[nw frame]];
    int16_t qd[4];
    cocoa_to_qd(r, qd);
    qd_rgn_set_rect(ARG(2), qd);
    RET(noErr);
}

SHIM(_SetWindowTitleWithCFString) {
    NSString *t = (__bridge NSString *)CFI(1);
    [(__bridge NSWindow *)win_of(ARG(0))->ns setTitle:t ? t : @""];
    RET(noErr);
}
SHIM(_CopyWindowTitleAsCFString) {
    NSString *t = [(__bridge NSWindow *)win_of(ARG(0))->ns title];
    W32_(g_mem, ARG(1), cf_out_owned(CFBridgingRetain([t copy])));
    RET(noErr);
}
SHIM(_GetWRefCon) { RET(win_of(ARG(0))->refcon); }
SHIM(_SetWindowProperty) {
    /* (window, creator, tag, size, data*) */
    HWindow *w = win_of(ARG(0));
    for (int i = 0; i < w->nprops; i++) {
        if (w->props[i].creator == ARG(1) && w->props[i].tag == ARG(2)) {
            free(w->props[i].data);
            w->props[i].size = ARG(3);
            w->props[i].data = malloc(ARG(3) ? ARG(3) : 1);
            memcpy(w->props[i].data, PTR(ARG(4)), ARG(3));
            RET(noErr);
            return;
        }
    }
    w->props = realloc(w->props, sizeof *w->props * (size_t)(w->nprops + 1));
    w->props[w->nprops] = (WinProp){ARG(1), ARG(2), ARG(3), malloc(ARG(3) ? ARG(3) : 1)};
    memcpy(w->props[w->nprops].data, PTR(ARG(4)), ARG(3));
    w->nprops++;
    RET(noErr);
}
SHIM(_GetWindowProperty) {
    /* (window, creator, tag, bufferSize, actualSize*, buffer*) */
    HWindow *w = win_of(ARG(0));
    for (int i = 0; i < w->nprops; i++) {
        if (w->props[i].creator == ARG(1) && w->props[i].tag == ARG(2)) {
            uint32_t n = w->props[i].size < ARG(3) ? w->props[i].size : ARG(3);
            if (ARG(4)) W32_(g_mem, ARG(4), w->props[i].size);
            if (ARG(5)) memcpy(PTR(ARG(5)), w->props[i].data, n);
            RET(noErr);
            return;
        }
    }
    RET(-5603); /* errWindowPropertyNotFound */
}
SHIM(_GetWindowEventTarget) { RET(win_of(ARG(0))->target); }
SHIM(_GetWindowPort) { RET(win_of(ARG(0))->port); }
SHIM(_GetWindowFromPort) {
    HWindow *w = gh_get(ARG(0), TAG_PORT);
    RET(w ? w->g : 0);
}
SHIM(_SetPortWindowPort) {}
SHIM(_InvalWindowRect) { [(__bridge NSView *)win_of(ARG(0))->view setNeedsDisplay:YES]; RET(noErr); }
SHIM(_ValidWindowRect) { RET(noErr); }
SHIM(_BeginUpdate) {}
SHIM(_EndUpdate) {}
SHIM(_SetWindowContentColor) { RET(noErr); }
SHIM(_SetWindowModified) { RET(noErr); }
SHIM(_SetWindowProxyCreatorAndType) { RET(noErr); }
SHIM(_CreateWindowGroup) { W32_(g_mem, ARG(1), 0x6770); RET(noErr); }
SHIM(_SetWindowGroup) { RET(noErr); }
SHIM(_SetWindowGroupLevel) { RET(noErr); }
SHIM(_FindWindow) {
    /* (Point by value {v,h}, WindowRef *out) -> WindowPartCode; inContent = 3 */
    int16_t v = (int16_t)(ARG(0) & 0xffff), h = (int16_t)(ARG(0) >> 16);
    NSPoint p = NSMakePoint(h, main_screen_height() - v);
    for (NSWindow *nw in [NSApp orderedWindows]) {
        HWindow *w = hwindow_for(nw);
        if (w && NSPointInRect(p, [nw frame])) {
            if (ARG(1)) W32_(g_mem, ARG(1), w->g);
            RET(NSPointInRect(p, [nw contentRectForFrameRect:[nw frame]]) ? 3 : 4);
            return;
        }
    }
    if (ARG(1)) W32_(g_mem, ARG(1), 0);
    RET(0);
}

/* ---- mouse / cursor ---- */
SHIM(_Button) { RET(g_buttons & 1); }
SHIM(_GetCurrentEventButtonState) { RET(g_buttons); }
SHIM(_GetGlobalMouse) {
    int16_t v, h;
    plat_mouse_global(&v, &h);
    W16_(g_mem, ARG(0), (uint16_t)v);
    W16_(g_mem, ARG(0) + 2, (uint16_t)h);
}
SHIM(_GetMouse) {
    /* Local to the current port: approximate with the focused window's content. */
    int16_t v, h;
    plat_mouse_global(&v, &h);
    if (g_focus) {
        NSWindow *nw = (__bridge NSWindow *)g_focus->ns;
        int16_t qd[4];
        cocoa_to_qd([nw contentRectForFrameRect:[nw frame]], qd);
        v -= qd[0];
        h -= qd[1];
    }
    W16_(g_mem, ARG(0), (uint16_t)v);
    W16_(g_mem, ARG(0) + 2, (uint16_t)h);
}
SHIM(_HideCursor) { [NSCursor hide]; }
SHIM(_ShowCursor) { [NSCursor unhide]; }
SHIM(_ObscureCursor) { [NSCursor setHiddenUntilMouseMoves:YES]; }
SHIM(_SetThemeCursor) { [[NSCursor arrowCursor] set]; RET(noErr); }
SHIM(_CGCursorIsVisible) { RET(1); }
SHIM(_CGWarpMouseCursorPosition) {
    /* CGPoint by value (two floats on i386) */
    CGWarpMouseCursorPosition(CGPointMake(ARGF32(0), ARGF32(1)));
    RET(0);
}
SHIM(_CGDisplayMoveCursorToPoint) { CGWarpMouseCursorPosition(CGPointMake(ARGF32(1), ARGF32(2))); RET(0); }
SHIM(_CGAssociateMouseAndMouseCursorPosition) { RET(CGAssociateMouseAndMouseCursorPosition((boolean_t)ARG(0))); }
SHIM(_CGSetLocalEventsSuppressionInterval) { RET(0); }

/* ---- menu bar ---- */
SHIM(_HideMenuBar) { [NSMenu setMenuBarVisible:NO]; }
SHIM(_ShowMenuBar) { [NSMenu setMenuBarVisible:YES]; }
SHIM(_IsMenuBarVisible) { RET([NSMenu menuBarVisible]); }
SHIM(_GetMBarHeight) { RET(24); }

const ShimDef shims_platform[] = {
    SHIM_ENTRY(_CreateNewWindow), SHIM_ENTRY(_DisposeWindow), SHIM_ENTRY(_ShowWindow), SHIM_ENTRY(_HideWindow),
    SHIM_ENTRY(_SelectWindow), SHIM_ENTRY(_ActivateWindow), SHIM_ENTRY(_IsWindowVisible),
    SHIM_ENTRY(_IsWindowCollapsed), SHIM_ENTRY(_FrontWindow), SHIM_ENTRY(_GetNextWindow), SHIM_ENTRY(_GetWindowBounds),
    SHIM_ENTRY(_SetWindowBounds), SHIM_ENTRY(_RepositionWindow), SHIM_ENTRY(_GetWindowRegion),
    SHIM_ENTRY(_SetWindowTitleWithCFString), SHIM_ENTRY(_CopyWindowTitleAsCFString), SHIM_ENTRY(_GetWRefCon),
    SHIM_ENTRY(_SetWindowProperty), SHIM_ENTRY(_GetWindowProperty), SHIM_ENTRY(_GetWindowEventTarget),
    SHIM_ENTRY(_GetWindowPort), SHIM_ENTRY(_GetWindowFromPort), SHIM_ENTRY(_SetPortWindowPort),
    SHIM_ENTRY(_InvalWindowRect), SHIM_ENTRY(_ValidWindowRect), SHIM_ENTRY(_BeginUpdate), SHIM_ENTRY(_EndUpdate),
    SHIM_ENTRY(_SetWindowContentColor), SHIM_ENTRY(_SetWindowModified), SHIM_ENTRY(_SetWindowProxyCreatorAndType),
    SHIM_ENTRY(_CreateWindowGroup), SHIM_ENTRY(_SetWindowGroup), SHIM_ENTRY(_SetWindowGroupLevel),
    SHIM_ENTRY(_FindWindow), SHIM_ENTRY(_Button), SHIM_ENTRY(_GetCurrentEventButtonState),
    SHIM_ENTRY(_GetGlobalMouse), SHIM_ENTRY(_GetMouse), SHIM_ENTRY(_HideCursor), SHIM_ENTRY(_ShowCursor),
    SHIM_ENTRY(_ObscureCursor), SHIM_ENTRY(_SetThemeCursor), SHIM_ENTRY(_CGCursorIsVisible),
    SHIM_ENTRY(_CGWarpMouseCursorPosition), SHIM_ENTRY(_CGDisplayMoveCursorToPoint),
    SHIM_ENTRY(_CGAssociateMouseAndMouseCursorPosition), SHIM_ENTRY(_CGSetLocalEventsSuppressionInterval),
    SHIM_ENTRY(_HideMenuBar), SHIM_ENTRY(_ShowMenuBar), SHIM_ENTRY(_IsMenuBarVisible), SHIM_ENTRY(_GetMBarHeight),
    {0, 0},
};
