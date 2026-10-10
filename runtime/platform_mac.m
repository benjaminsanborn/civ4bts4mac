/* Cocoa platform layer: the application, Carbon WindowRefs backed by
 * NSWindows, and OS input translated into Carbon events.
 *
 * Coordinates handed to the guest are QuickDraw globals: origin at the top
 * left of the main display, y down, in points. */
#import <Cocoa/Cocoa.h>

#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>
#import <QuartzCore/QuartzCore.h>
#include <malloc/malloc.h>
#include "cf.h"
#include <math.h>
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
/* Hang watchdog: if the main thread stops pumping events for 20 s, save a
 * stack sample of the whole process to build/hangs/ (once per stall). */
static _Atomic double g_heartbeat;
size_t rt_guest_heap_used(void);
uint32_t rt_cf_proxy_count(void);
static void *watchdog(void *unused) {
    (void)unused;
    int reported = 0, memstats = getenv("CIV4_MEMSTATS") != NULL, tick = 0;
    for (;;) {
        sleep(5);
        if (memstats && ++tick % 6 == 0) {
            malloc_statistics_t ms;
            malloc_zone_statistics(NULL, &ms);
            rt_log("mem: guest heap %zu MB, host malloc %zu MB, CF proxies %u", rt_guest_heap_used() >> 20,
                   ms.size_in_use >> 20, rt_cf_proxy_count());
        }
        double idle = CACurrentMediaTime() - g_heartbeat;
        if (idle < 20) { reported = 0; continue; }
        if (reported) continue;
        reported = 1;
        char dir[PATH_MAX], path[PATH_MAX], pid[16];
        rt_build_path("hangs", dir, sizeof dir);
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/hang_%ld.txt", dir, (long)time(NULL));
        snprintf(pid, sizeof pid, "%d", getpid());
        rt_log("main thread stalled for %.0f s; sampling to %s", idle, path);
        char *argv[] = {"/usr/bin/sample", pid, "5", "-file", path, NULL};
        pid_t child;
        if (posix_spawn(&child, argv[0], NULL, NULL, argv, NULL) == 0) waitpid(child, NULL, 0);
    }
    return NULL;
}

static void ensure_app(void) {
    static int done;
    if (done) return;
    done = 1;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    [NSApp finishLaunching];
    /* A game: no App Nap, no timer coalescing. */
    static id activity;
    activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                          reason:@"game"];
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

/* The fullscreen display port, if any: the game then sees a screen of mode size at the origin. */
static HWindow *display_window(void) {
    for (NSValue *v in g_windows) {
        HWindow *w = [v pointerValue];
        if (w->mode_w) return w;
    }
    return NULL;
}

/* Where the display port's picture is on screen (Cocoa coordinates): the mode
 * is scaled to fit the screen with its aspect ratio kept (letter/pillarboxed). */
static NSRect display_rect(HWindow *d) {
    NSWindow *nw = (__bridge NSWindow *)d->ns;
    NSRect f = [(__bridge NSView *)d->view frame];
    return NSOffsetRect(f, nw.frame.origin.x, nw.frame.origin.y);
}
static void layout_display(HWindow *d) {
    NSView *content = [(__bridge NSWindow *)d->ns contentView];
    NSRect b = [content bounds];
    /* Keep the picture out from under the camera notch. */
    if (@available(macOS 12.0, *)) b.size.height -= [[(__bridge NSWindow *)d->ns screen] safeAreaInsets].top;
    double scale = fmin(b.size.width / d->mode_w, b.size.height / d->mode_h);
    double w = round(d->mode_w * scale), h = round(d->mode_h * scale);
    [(__bridge NSView *)d->view setFrame:NSMakeRect(round((b.size.width - w) / 2), round((b.size.height - h) / 2), w, h)];
}

/* Scripted input (CIV4_INPUT) replaces the real mouse and keyboard. */
static int g_scripted;
static NSPoint g_script_mouse;

/* Mouse position in game (QuickDraw global) coordinates. */
static NSPoint game_mouse(void) {
    if (g_scripted) return g_script_mouse;
    NSPoint p = [NSEvent mouseLocation];
    HWindow *d = display_window();
    if (d) {
        NSRect r = display_rect(d);
        double x = (p.x - r.origin.x) * d->mode_w / r.size.width, y = (NSMaxY(r) - p.y) * d->mode_h / r.size.height;
        return NSMakePoint(fmin(fmax(x, 0), d->mode_w - 1), fmin(fmax(y, 0), d->mode_h - 1));
    }
    return NSMakePoint(p.x, main_screen_height() - p.y);
}

static void add_mouse_params(HEvent *e, NSPoint at, uint32_t mods, NSPoint delta, HWindow *w) {
    float global[2] = {(float)at.x, (float)at.y};
    g_mouse_global = at;
    ev_set_param(e, kEventParamMouseLocation, typeHIPoint, global, 8);
    if (w) {
        NSRect content = [(__bridge NSWindow *)w->ns contentRectForFrameRect:[(__bridge NSWindow *)w->ns frame]];
        int16_t qd[4];
        cocoa_to_qd(content, qd);
        float local[2] = {global[0] - qd[1], global[1] - qd[0]};
        if (w->mode_w) local[0] = global[0], local[1] = global[1];
        ev_set_param(e, kEventParamWindowMouseLocation, typeHIPoint, local, 8);
        ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    }
    ev_set_param(e, kEventParamKeyModifiers, typeUInt32, &mods, 4);
    float d[2] = {(float)delta.x, (float)delta.y};
    ev_set_param(e, kEventParamMouseDelta, typeHIPoint, d, 8);
}

/* Mouse events always carry a game window: Carbon handlers read
 * kEventParamWindowRef without checking for errors. */
static HWindow *event_window(NSEvent *ev) {
    HWindow *w = [ev window] ? hwindow_for([ev window]) : NULL;
    if (!w) w = g_focus;
    if (!w && [g_windows count]) w = [g_windows[0] pointerValue];
    return w;
}

static void post_mouse_at(HWindow *w, uint32_t kind, uint16_t button, NSPoint at, uint32_t mods, NSPoint delta,
                          uint32_t clicks) {
    HEvent *e = ev_create(kEventClassMouse, kind);
    add_mouse_params(e, at, mods, delta, w);
    if (button) {
        ev_set_param(e, kEventParamMouseButton, typeMouseButton, &button, 2);
        ev_set_param(e, kEventParamClickCount, typeUInt32, &clicks, 4);
    }
    ev_post(e);
    ev_release(e);
}

static void post_mouse(NSEvent *ev, uint32_t kind, uint16_t button) {
    post_mouse_at(event_window(ev), kind, button, game_mouse(), carbon_modifiers([ev modifierFlags]),
                  NSMakePoint([ev deltaX], [ev deltaY]), button ? (uint32_t)[ev clickCount] : 0);
}

static void post_key_raw(uint32_t kind, uint32_t code, uint32_t mods, NSString *chars, NSString *raw) {
    uint32_t kbd = 40;
    int down = kind != kEventRawKeyUp;
    if (kind == kEventRawKeyModifiersChanged) {
        /* A modifier key is down while its flag is set. */
        uint32_t flag = 0;
        switch (code) {
        case 56: case 60: flag = shiftKey; break;
        case 59: case 62: flag = controlKey; break;
        case 58: case 61: flag = optionKey; break;
        case 55: case 54: flag = cmdKey; break;
        case 57: flag = alphaLock; break;
        }
        down = (mods & flag) != 0;
    }
    if (code < 128) {
        if (down) g_keymap[code >> 3] |= 1u << (code & 7);
        else g_keymap[code >> 3] &= ~(1u << (code & 7));
    }
    if (kind == kEventRawKeyModifiersChanged) chars = @"";
    HEvent *e = ev_create(kEventClassKeyboard, kind);
    ev_set_param(e, kEventParamKeyCode, typeUInt32, &code, 4);
    ev_set_param(e, kEventParamKeyModifiers, typeUInt32, &mods, 4);
    ev_set_param(e, kEventParamKeyboardType, typeUInt32, &kbd, 4);
    if (kind != kEventRawKeyModifiersChanged) {
        char mac = 0;
        if ([chars length]) mac = (char)[chars characterAtIndex:0];
        else if ([raw length]) mac = (char)[raw characterAtIndex:0];
        ev_set_param(e, kEventParamKeyMacCharCodes, typeChar, &mac, 1);
        unichar u[16];
        NSUInteger n = MIN([chars length], 16u);
        [chars getCharacters:u range:NSMakeRange(0, n)];
        ev_set_param(e, kEventParamKeyUnicodes, typeUnicodeText, u, (uint32_t)(2 * n));
    }
    /* Like mouse events, always name a window: the game's handlers don't check. */
    HWindow *w = g_focus ? g_focus : display_window();
    if (!w && [g_windows count]) w = [g_windows[0] pointerValue];
    if (w) ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    ev_post(e);
    ev_release(e);
}

static void post_key(NSEvent *ev, uint32_t kind) {
    NSString *chars = kind == kEventRawKeyModifiersChanged ? @"" : [ev characters];
    NSString *raw = kind == kEventRawKeyModifiersChanged ? @"" : [ev charactersIgnoringModifiers];
    post_key_raw(kind, [ev keyCode], carbon_modifiers([ev modifierFlags]), chars, raw);
}

/* ---- scripted input: CIV4_INPUT="SECS:CMD ARGS;..." ----
 * Commands (coordinates in game screen space):
 *   move X,Y   click X,Y   rclick X,Y   key TEXT   keycode N   command HICM (e.g. quit)   py CODE   quit
 * Seconds count from the first event pump. Real input is ignored while a script runs. */
typedef struct { double t; char cmd[16]; char arg[256]; } ScriptStep;
static ScriptStep *g_script;
static int g_script_n, g_script_pos;
static double g_script_t0;
/* Typed keys are played out over time like real typing: the game also polls
 * the live key map (GetKeys), which must match the events it is processing. */
typedef struct { double t; uint32_t kind, code, mods; char ch, raw; } KeyStep;
static KeyStep g_keysteps[1024];
static int g_nkeysteps, g_keystep_pos;
static void queue_key(double t, uint32_t kind, uint32_t code, uint32_t mods, char ch, char raw) {
    if (g_nkeysteps == 1024) return;
    g_keysteps[g_nkeysteps++] = (KeyStep){t, kind, code, mods, ch, raw};
}
static uint16_t g_script_up; /* button to release at g_script_up_t */
static double g_script_up_t;

static void script_load(void) {
    const char *spec = getenv("CIV4_INPUT");
    if (!spec || !*spec) return;
    g_scripted = 1;
    g_script_mouse = NSMakePoint(0, 0);
    char *copy = strdup(spec), *save = NULL;
    for (char *tok = strtok_r(copy, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
        g_script = realloc(g_script, sizeof *g_script * (size_t)(g_script_n + 1));
        ScriptStep *st = &g_script[g_script_n];
        memset(st, 0, sizeof *st);
        if (sscanf(tok, " %lf:%15s %255[^\n]", &st->t, st->cmd, st->arg) >= 2) g_script_n++;
    }
    free(copy);
    g_script_t0 = ev_now();
}

/* US layout virtual key codes for the characters scripts type. */
static int keycode_for(unichar c) {
    static const char rows[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.";
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == ' ') return 49;
    if (c == '\r' || c == '\n') return 36;
    if (c == 27) return 53;
    if (c == '\t') return 48;
    if (c == '`') return 50;
    for (int k = 0; k < (int)sizeof rows - 1; k++)
        if (rows[k] == c && c) return k;
    return 0;
}

static void script_mouse(uint32_t kind, uint16_t button, NSPoint at) {
    NSPoint delta = NSMakePoint(at.x - g_script_mouse.x, at.y - g_script_mouse.y);
    g_script_mouse = at;
    HWindow *w = display_window();
    if (!w && [g_windows count]) w = [g_windows[0] pointerValue];
    if (!w) return;
    post_mouse_at(w, kind, button, at, 0, delta, button ? 1 : 0);
}

/* "py CODE": run Python in the game (the event loop calls it on the main thread). */
static char g_pending_python[256];
int plat_take_python(char *out, size_t n) {
    if (!g_pending_python[0]) return 0;
    strlcpy(out, g_pending_python, n);
    g_pending_python[0] = 0;
    return 1;
}

static void post_command(uint32_t command);
/* The game takes clicks only in its active window; a scripted run's app may
 * never become frontmost, so tell the game its window is active. */
static void script_activate(void) {
    HWindow *w = display_window();
    if (!w && [g_windows count]) w = [g_windows[0] pointerValue];
    if (!w) return;
    g_focus = w;
    ev_set_focus_target(w->target);
    HEvent *e = ev_create(kEventClassWindow, kEventWindowActivated);
    ev_set_param(e, kEventParamDirectObject, typeWindowRef, &w->g, 4);
    ev_set_param(e, kEventParamWindowRef, typeWindowRef, &w->g, 4);
    ev_post(e);
    ev_release(e);
}

static void script_step(ScriptStep *st) {
    rt_log("input script t=%.1f: %s %s", st->t, st->cmd, st->arg);
    if (strcmp(st->cmd, "move") && strcmp(st->cmd, "py") && strcmp(st->cmd, "quit")) script_activate();
    double x = 0, y = 0;
    sscanf(st->arg, "%lf,%lf", &x, &y);
    NSPoint at = NSMakePoint(x, y);
    if (!strcmp(st->cmd, "move")) {
        script_mouse(kEventMouseMoved, 0, at);
    } else if (!strcmp(st->cmd, "click") || !strcmp(st->cmd, "rclick")) {
        uint16_t b = st->cmd[0] == 'r' ? 2 : 1;
        script_mouse(kEventMouseMoved, 0, at);
        g_buttons |= b;
        script_mouse(kEventMouseDown, b, at);
        g_script_up = b;
        g_script_up_t = ev_now() + 0.15;
    } else if (!strcmp(st->cmd, "key")) {
        /* Shifted symbols type as shift + their unshifted key. */
        static const char shifted[] = "!1@2#3$4%5^6&7*8(9)0_-+={[}]|\\:;\"'<,>.?/~`";
        for (const char *c = st->arg; *c; c++) {
            char base = *c;
            uint32_t mods = 0;
            if (*c >= 'A' && *c <= 'Z') base = (char)(*c + 32), mods = shiftKey;
            for (int k = 0; shifted[k]; k += 2)
                if (shifted[k] == *c) base = shifted[k + 1], mods = shiftKey;
            uint32_t code = (uint32_t)keycode_for((unichar)base);
            double t = g_nkeysteps > g_keystep_pos ? g_keysteps[g_nkeysteps - 1].t : ev_now();
            if (mods) queue_key(t += 0.03, kEventRawKeyModifiersChanged, 56, mods, 0, 0);
            queue_key(t += 0.03, kEventRawKeyDown, code, mods, *c, base);
            queue_key(t += 0.03, kEventRawKeyUp, code, mods, *c, base);
            if (mods) queue_key(t += 0.03, kEventRawKeyModifiersChanged, 56, 0, 0, 0);
        }
    } else if (!strcmp(st->cmd, "keycode")) {
        int code = atoi(st->arg);
        NSString *ch = code == 36 ? @"\r" : code == 53 ? @"\x1b" : @"";
        post_key_raw(kEventRawKeyDown, (uint32_t)code, 0, ch, ch);
        post_key_raw(kEventRawKeyUp, (uint32_t)code, 0, ch, ch);
    } else if (!strcmp(st->cmd, "py")) {
        strlcpy(g_pending_python, st->arg, sizeof g_pending_python);
    } else if (!strcmp(st->cmd, "command") && strlen(st->arg) == 4) {
        post_command(FOURCC(st->arg[0], st->arg[1], st->arg[2], st->arg[3]));
    } else if (!strcmp(st->cmd, "quit")) {
        rt_log("input script: quit");
        exit(0);
    }
}

static void script_run(void) {
    while (g_keystep_pos < g_nkeysteps && ev_now() >= g_keysteps[g_keystep_pos].t) {
        KeyStep *k = &g_keysteps[g_keystep_pos++];
        NSString *ch = k->ch ? [NSString stringWithFormat:@"%c", k->ch] : @"";
        NSString *raw = k->raw ? [NSString stringWithFormat:@"%c", k->raw] : @"";
        post_key_raw(k->kind, k->code, k->mods, ch, raw);
    }
    if (g_script_up && ev_now() >= g_script_up_t) {
        g_buttons &= ~(uint32_t)g_script_up;
        script_mouse(kEventMouseUp, g_script_up, g_script_mouse);
        g_script_up = 0;
    }
    while (g_script_pos < g_script_n && ev_now() - g_script_t0 >= g_script[g_script_pos].t)
        script_step(&g_script[g_script_pos++]);
}

/* Standard Mac commands go to the game's command handler as HICommands. */
static void post_command(uint32_t command) {
    uint8_t hc[14] = {0}; /* HICommand {attributes, commandID, {menuRef, menuItemIndex}} */
    memcpy(hc + 4, &command, 4);
    HEvent *e = ev_create(kEventClassCommand, 1 /* kEventCommandProcess */);
    ev_set_param(e, kEventParamDirectObject, FOURCC('h', 'c', 'm', 'd'), hc, sizeof hc);
    ev_post(e);
    ev_release(e);
}

/* Cmd-Q quits through the game (it can confirm or save); Cmd-H hides. */
static int app_shortcut(NSEvent *ev) {
    if ([ev type] != NSEventTypeKeyDown || !([ev modifierFlags] & NSEventModifierFlagCommand)) return 0;
    NSString *k = [[ev charactersIgnoringModifiers] lowercaseString];
    if ([k isEqualToString:@"q"]) {
        if (![ev isARepeat]) post_command(FOURCC('q', 'u', 'i', 't'));
        return 1;
    }
    if ([k isEqualToString:@"h"]) {
        [NSApp hide:nil];
        return 1;
    }
    return 0;
}

static void translate(NSEvent *ev) {
    if (g_scripted) return;
    if (app_shortcut(ev)) return;
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
        HWindow *w = event_window(ev);
        HEvent *e = ev_create(kEventClassMouse, kEventMouseWheelMoved);
        add_mouse_params(e, game_mouse(), carbon_modifiers([ev modifierFlags]), NSMakePoint([ev deltaX], [ev deltaY]), w);
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

static void simulate_become_key(HWindow *w);
void plat_pump(double timeout) {
    ensure_app();
    g_heartbeat = CACurrentMediaTime();
    static int watchdog_started;
    if (!watchdog_started) {
        watchdog_started = 1;
        pthread_t t;
        pthread_create(&t, NULL, watchdog, NULL);
        pthread_detach(t);
    }
    static int stalled;
    if (!stalled && getenv("CIV4_TEST_STALL") && CACurrentMediaTime() - g_heartbeat < 1 && ev_now() > 10) {
        stalled = 1; /* test the watchdog: stall the main thread once */
        sleep(28);
    }
    static double focus_test = -1; /* CIV4_TEST_FOCUS=T: hide the app T s after launch, bring it back 5 s later */
    if (focus_test < 0)
        focus_test = getenv("CIV4_TEST_FOCUS") ? CACurrentMediaTime() + atof(getenv("CIV4_TEST_FOCUS")) : 0;
    if (focus_test > 0 && CACurrentMediaTime() > focus_test) {
        static int phase;
        if (phase == 0) {
            phase = 1;
            rt_log("focus test: hiding");
            [NSApp hide:nil];
            /* An app that never became active (terminal launch, locked screen)
             * gets no activation notifications; deliver what AppKit would. */
            if (![NSApp isActive])
                [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidResignActiveNotification
                                                                    object:NSApp];
        } else if (phase == 1 && CACurrentMediaTime() > focus_test + 5) {
            phase = 2;
            rt_log("focus test: unhiding");
            [NSApp unhide:nil];
            [NSApp activateIgnoringOtherApps:YES];
            HWindow *w = display_window();
            if (![NSApp isActive] && w)
                simulate_become_key(w);
        }
    }
    static int script_init;
    if (!script_init) {
        script_init = 1;
        script_load();
    }
    script_run();
    if ((g_script_pos < g_script_n || g_script_up || g_keystep_pos < g_nkeysteps) && timeout > 0.01) timeout = 0.01;
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
    if (w->mode_w) {
        *width = w->mode_w;
        *height = w->mode_h;
        return;
    }
    NSRect b = [(__bridge NSView *)w->view bounds];
    *width = (int)b.size.width;
    *height = (int)b.size.height;
}
void plat_mouse_global(int16_t *v, int16_t *h) {
    NSPoint p = game_mouse();
    *h = (int16_t)lround(p.x);
    *v = (int16_t)lround(p.y);
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
/* The game pauses while its window is inactive, so it is deactivated only
 * when the app loses focus, not when key focus moves between its own windows
 * (the fullscreen port takes it from the game's helper windows at startup). */
- (void)appDidResignActive:(NSNotification *)n {
    (void)n;
    HWindow *w = g_focus ? g_focus : display_window();
    if (!w || g_scripted) return; /* scripted runs keep the game window active */
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

static WinDelegate *win_delegate(void) {
    static WinDelegate *delegate;
    if (!delegate) {
        delegate = [WinDelegate new];
        [[NSNotificationCenter defaultCenter] addObserver:delegate
                                                 selector:@selector(appDidResignActive:)
                                                     name:NSApplicationDidResignActiveNotification
                                                   object:nil];
    }
    return delegate;
}
static void simulate_become_key(HWindow *w) {
    [win_delegate() windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification
                                                                     object:(__bridge NSWindow *)w->ns]];
}

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
    [nw setDelegate:win_delegate()];
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

/* Fullscreen: a borderless window over the main screen, standing in for the
 * captured display. Its logical size is the current virtual display mode. */
SHIM(_CreateNewPortForCGDisplayID) {
    ensure_app();
    static HWindow *display_port;
    if (!display_port) {
        HWindow *w = calloc(1, sizeof *w);
        NSScreen *screen = [NSScreen screens][0];
        GameWindow *nw = [[GameWindow alloc] initWithContentRect:[screen frame]
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        /* Normal level, with the Dock and menu bar hidden while the game is
         * frontmost: Cmd-Tab and other apps keep working. */
        [nw setLevel:NSNormalWindowLevel];
        [nw setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        [NSApp setPresentationOptions:NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar];
        [nw setReleasedWhenClosed:NO];
        [nw setAcceptsMouseMovedEvents:YES];
        [nw setDelegate:win_delegate()];
        [nw setBackgroundColor:[NSColor blackColor]];
        /* Black backdrop; the GL view inside it is sized to the mode's aspect ratio. */
        NSView *backdrop = [[NSView alloc] initWithFrame:[[nw contentView] bounds]];
        GameView *view = [[GameView alloc] initWithFrame:[backdrop bounds]];
        [backdrop addSubview:view];
        [nw setContentView:backdrop];
        [nw makeFirstResponder:view];
        w->ns = (__bridge_retained void *)nw;
        w->view = (__bridge void *)view;
        w->target = ev_new_target(ev_app_target());
        w->g = gh_new(TAG_WINDOW, w);
        w->port = gh_new(TAG_PORT, w);
        w->visible = 1;
        [g_windows addObject:[NSValue valueWithPointer:w]];
        display_port = w;
        [nw makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    plat_display_mode(&display_port->mode_w, &display_port->mode_h);
    layout_display(display_port);
    if (g_scripted || getenv("CIV4_INPUT")) {
        /* Scripted runs: the app may not be frontmost (macOS won't let it take
         * focus), but the game only takes clicks in an active window. */
        g_focus = display_port;
        ev_set_focus_target(display_port->target);
        HEvent *e = ev_create(kEventClassWindow, kEventWindowActivated);
        ev_set_param(e, kEventParamDirectObject, typeWindowRef, &display_port->g, 4);
        ev_set_param(e, kEventParamWindowRef, typeWindowRef, &display_port->g, 4);
        ev_post(e);
        ev_release(e);
    }
    rt_log("CreateNewPortForCGDisplayID: fullscreen %dx%d", display_port->mode_w, display_port->mode_h);
    RET(display_port->port);
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
    HWindow *w = win_of(ARG(0));
    if (w->mode_w) {
        /* The display port is the game's whole screen: its mode size at the
         * origin, like the mouse coordinates it reports. */
        int16_t qd[4] = {0, 0, (int16_t)w->mode_h, (int16_t)w->mode_w};
        memcpy(PTR(ARG(2)), qd, 8);
        RET(noErr);
        return;
    }
    NSWindow *nw = (__bridge NSWindow *)w->ns;
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
    HWindow *d = display_window();
    if (d && d->visible) {
        if (ARG(1)) W32_(g_mem, ARG(1), d->g);
        RET(3);
        return;
    }
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
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("CIV4_DEBUG_EVENTS") != NULL;
    if (dbg && (g_buttons || dbg++ % 600 == 1)) rt_log("GetGlobalMouse -> %d,%d (buttons %u, scripted %d)", h, v, g_buttons, g_scripted);
    W16_(g_mem, ARG(0), (uint16_t)v);
    W16_(g_mem, ARG(0) + 2, (uint16_t)h);
}
SHIM(_GetMouse) {
    /* Local to the current port: approximate with the focused window's content. */
    int16_t v, h;
    plat_mouse_global(&v, &h);
    if (g_focus && !g_focus->mode_w) {
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
/* Move the cursor to a point in game coordinates (scaled from the virtual display mode). */
static void warp_game(double x, double y) {
    if (g_scripted) {
        g_script_mouse = NSMakePoint(x, y);
        return;
    }
    HWindow *d = display_window();
    if (d) {
        NSRect r = display_rect(d);
        x = r.origin.x + x * r.size.width / d->mode_w;
        y = main_screen_height() - NSMaxY(r) + y * r.size.height / d->mode_h;
    }
    CGWarpMouseCursorPosition(CGPointMake(x, y));
    CGAssociateMouseAndMouseCursorPosition(true);
}
SHIM(_CGWarpMouseCursorPosition) {
    /* CGPoint by value (two floats on i386) */
    warp_game(ARGF32(0), ARGF32(1));
    RET(0);
}
SHIM(_CGDisplayMoveCursorToPoint) { warp_game(ARGF32(1), ARGF32(2)); RET(0); }
/* Synthesized mouse events: the game posts them to move the cursor. Keep them
 * as real CGEvents (the guest CFReleases them), but only ever warp on post. */
SHIM(_CGEventCreateMouseEvent) {
    /* (source, CGEventType, CGPoint {float x, y}, CGMouseButton) */
    CGEventRef e = CGEventCreateMouseEvent(NULL, (CGEventType)ARG(1), CGPointMake(ARGF32(2), ARGF32(3)),
                                           (CGMouseButton)ARG(4));
    RETCF_OWNED(e);
}
SHIM(_CGEventSetType) { CGEventSetType((CGEventRef)CFI(0), (CGEventType)ARG(1)); }
SHIM(_CGEventPost) {
    CGEventRef e = (CGEventRef)CFI(1);
    if (!e) return;
    CGEventType t = CGEventGetType(e);
    if (t == kCGEventMouseMoved || t == kCGEventLeftMouseDragged || t == kCGEventRightMouseDragged) {
        CGPoint p = CGEventGetLocation(e);
        warp_game(p.x, p.y);
    }
}

/* ---- named pixmap cursors (32-bit, byte-swapped to big-endian ARGB by the game) ---- */
static NSMutableDictionary<NSString *, NSCursor *> *g_cursors;
SHIM(_QDRegisterNamedPixMapCursor) {
    /* (PixMapHandle crsr, PixMapHandle mask, Point hotSpot, const char *name) */
    uint32_t pm = R32_(g_mem, ARG(0));
    uint32_t base = R32_(g_mem, pm), rb = R16_(g_mem, pm + 4) & 0x3fff;
    int16_t top = (int16_t)R16_(g_mem, pm + 6), left = (int16_t)R16_(g_mem, pm + 8);
    int16_t bottom = (int16_t)R16_(g_mem, pm + 10), right = (int16_t)R16_(g_mem, pm + 12);
    int w = right - left, h = bottom - top;
    if (w <= 0 || h <= 0 || R16_(g_mem, pm + 32) != 32) { RET(-50); return; }
    CFDataRef data = CFDataCreate(NULL, PTR(base), (CFIndex)rb * h);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
    static CGColorSpaceRef cs;
    if (!cs) cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, rb, cs,
                                   kCGImageAlphaFirst | kCGBitmapByteOrder32Big, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    NSImage *ni = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(w, h)];
    int16_t hv = (int16_t)(ARG(2) & 0xffff), hh = (int16_t)(ARG(2) >> 16);
    NSCursor *c = [[NSCursor alloc] initWithImage:ni hotSpot:NSMakePoint(hh, hv)];
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CFRelease(data);
    if (!g_cursors) g_cursors = [NSMutableDictionary new];
    g_cursors[[NSString stringWithUTF8String:CPTR(ARG(3))]] = c;
    RET(noErr);
}
SHIM(_QDIsNamedPixMapCursorRegistered) { RET(g_cursors[[NSString stringWithUTF8String:CPTR(ARG(0))]] != nil); }
SHIM(_QDSetNamedPixMapCursor) {
    NSCursor *c = g_cursors[[NSString stringWithUTF8String:CPTR(ARG(0))]];
    if (!c) { RET(-50); return; }
    [c set];
    RET(noErr);
}
/* (sic: Apple's API name is misspelled) */
SHIM(_QDUnregisterNamedPixMapCursur) {
    [g_cursors removeObjectForKey:[NSString stringWithUTF8String:CPTR(ARG(0))]];
    RET(noErr);
}

/* ---- port <-> global coordinates ---- */
uint32_t qd_current_port(void);
static void port_origin(int16_t *top, int16_t *left) {
    *top = *left = 0;
    /* Fullscreen: the game's coordinate space is the display; its other
     * (hidden) windows' host positions don't apply. */
    if (display_window()) return;
    HWindow *w = gh_get(qd_current_port(), TAG_PORT);
    if (!w || w->mode_w) return;
    NSWindow *nw = (__bridge NSWindow *)w->ns;
    int16_t qd[4];
    cocoa_to_qd([nw contentRectForFrameRect:[nw frame]], qd);
    *top = qd[0];
    *left = qd[1];
}
SHIM(_GlobalToLocal) {
    int16_t t, l;
    port_origin(&t, &l);
    W16_(g_mem, ARG(0), (uint16_t)((int16_t)R16_(g_mem, ARG(0)) - t));
    W16_(g_mem, ARG(0) + 2, (uint16_t)((int16_t)R16_(g_mem, ARG(0) + 2) - l));
}
SHIM(_LocalToGlobal) {
    int16_t t, l;
    port_origin(&t, &l);
    W16_(g_mem, ARG(0), (uint16_t)((int16_t)R16_(g_mem, ARG(0)) + t));
    W16_(g_mem, ARG(0) + 2, (uint16_t)((int16_t)R16_(g_mem, ARG(0) + 2) + l));
}

/* ---- KeyTranslate: (transData, UInt16 keycode | modifiers << 8, UInt32 *state) -> char code (US layout) ---- */
SHIM(_KeyTranslate) {
    static const char lower[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\rlj'k;\\,/nm.\t `\b";
    static const char upper[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\rLJ\"K:|<?NM>\t ~\b";
    uint32_t code = ARG(1) & 0x7f, mods = (ARG(1) >> 8) & 0xff;
    int shift = (mods & (shiftKey >> 8)) != 0, caps = (mods & (alphaLock >> 8)) != 0;
    uint32_t ch = 0;
    if (code < sizeof lower - 1) {
        ch = (uint8_t)(shift ? upper[code] : lower[code]);
        if (caps && ch >= 'a' && ch <= 'z') ch -= 32;
    } else if (code == 53) {
        ch = 27; /* escape */
    } else if (code == 76) {
        ch = 3; /* keypad enter */
    } else if (code == 117) {
        ch = 127; /* forward delete */
    } else if (code >= 123 && code <= 126) {
        static const uint8_t arrows[4] = {28, 29, 31, 30}; /* left right down up */
        ch = arrows[code - 123];
    }
    if (ARG(2)) W32_(g_mem, ARG(2), 0);
    RET(ch);
}

SHIM(_DisableMenuCommand) {}
SHIM(_EnableMenuCommand) {}
SHIM(_CGAssociateMouseAndMouseCursorPosition) { RET(CGAssociateMouseAndMouseCursorPosition((boolean_t)ARG(0))); }
SHIM(_CGSetLocalEventsSuppressionInterval) { RET(0); }

void NSBeep_c(void) { NSBeep(); }

/* LSOpenCFURLRef(CFURLRef, CFURLRef *launchedURL): web links (ShellExecute). */
SHIM(_LSOpenCFURLRef) {
    NSURL *u = (__bridge NSURL *)CFI(0);
    BOOL ok = u && [[NSWorkspace sharedWorkspace] openURL:u];
    if (ARG(1)) W32_(g_mem, ARG(1), 0);
    RET(ok ? 0 : -10814 /* kLSApplicationNotFoundErr */);
}

/* ---- menu bar ---- */
SHIM(_HideMenuBar) { [NSMenu setMenuBarVisible:NO]; }
SHIM(_ShowMenuBar) { [NSMenu setMenuBarVisible:YES]; }
SHIM(_IsMenuBarVisible) { RET([NSMenu menuBarVisible]); }
SHIM(_GetMBarHeight) { RET(24); }

const ShimDef shims_platform[] = {
    SHIM_ENTRY(_CreateNewWindow), SHIM_ENTRY(_CreateNewPortForCGDisplayID), SHIM_ENTRY(_DisposeWindow), SHIM_ENTRY(_ShowWindow), SHIM_ENTRY(_HideWindow),
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
    SHIM_ENTRY(_CGEventCreateMouseEvent), SHIM_ENTRY(_CGEventSetType), SHIM_ENTRY(_CGEventPost),
    SHIM_ENTRY(_QDRegisterNamedPixMapCursor), SHIM_ENTRY(_QDIsNamedPixMapCursorRegistered),
    SHIM_ENTRY(_QDSetNamedPixMapCursor), SHIM_ENTRY(_QDUnregisterNamedPixMapCursur), SHIM_ENTRY(_GlobalToLocal),
    SHIM_ENTRY(_LocalToGlobal), SHIM_ENTRY(_KeyTranslate), SHIM_ENTRY(_DisableMenuCommand), SHIM_ENTRY(_EnableMenuCommand),
    SHIM_ENTRY(_LSOpenCFURLRef), SHIM_ENTRY(_HideMenuBar), SHIM_ENTRY(_ShowMenuBar), SHIM_ENTRY(_IsMenuBarVisible), SHIM_ENTRY(_GetMBarHeight),
    {0, 0},
};
