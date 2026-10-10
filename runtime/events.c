/* Carbon Event Manager emulation.
 *
 * Events, targets and handlers are host objects behind opaque guest handles.
 * Dispatch walks a target's handlers newest-first, then its parent target
 * (window -> application), exactly like Carbon; CallNextEventHandler resumes
 * the walk. The queue is fed by the platform layer (plat_pump) and by guest
 * PostEventToQueue calls; timers fire while the guest waits for events.
 * Everything here runs on the main thread.
 */
#include "events.h"
#include "handles.h"

#include <float.h>
#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define noErr 0
#define paramErr (-50)

typedef struct {
    uint32_t name, type, size;
    uint8_t *data;
} Param;

struct HEvent {
    uint32_t g;
    uint32_t cls, kind;
    double time;
    int refs;
    Param *p;
    int np;
};

typedef struct {
    uint32_t g;
    uint32_t upp, user;
    uint32_t *specs; /* class, kind pairs */
    int nspec;
    uint32_t target;
    int removed;
} Handler;

typedef struct {
    uint32_t g;
    uint32_t parent;
    Handler **h; /* oldest first; dispatch walks from the end */
    int nh, cap;
} Target;

typedef struct {
    HEvent *e;
    uint32_t target;
    int next; /* index of the next handler to try in `target`, walking down */
} CallCtx;

typedef struct {
    uint32_t g;
    uint32_t upp, user;
    double next, interval;
    int removed;
} Timer;

static uint32_t g_app_target, g_dispatcher_target;
static uint32_t g_focus_window_target; /* set by the platform layer */
static HEvent **g_queue;
static int g_nq, g_qcap;
static Timer **g_timers;
static int g_ntimers;
static volatile int g_quit;

double ev_now(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

/* ---- events ---- */
HEvent *ev_create(uint32_t cls, uint32_t kind) {
    HEvent *e = calloc(1, sizeof *e);
    e->cls = cls;
    e->kind = kind;
    e->time = ev_now();
    e->refs = 1;
    e->g = gh_new(TAG_EVENT, e);
    return e;
}

void ev_set_param(HEvent *e, uint32_t name, uint32_t type, const void *data, uint32_t size) {
    for (int i = 0; i < e->np; i++) {
        if (e->p[i].name == name) {
            free(e->p[i].data);
            e->p[i] = (Param){name, type, size, malloc(size ? size : 1)};
            memcpy(e->p[i].data, data, size);
            return;
        }
    }
    e->p = realloc(e->p, (size_t)(e->np + 1) * sizeof *e->p);
    e->p[e->np] = (Param){name, type, size, malloc(size ? size : 1)};
    memcpy(e->p[e->np].data, data, size);
    e->np++;
}

static Param *ev_param(HEvent *e, uint32_t name) {
    for (int i = 0; i < e->np; i++)
        if (e->p[i].name == name) return &e->p[i];
    return NULL;
}

void ev_release(HEvent *e) {
    if (!e || --e->refs > 0) return;
    for (int i = 0; i < e->np; i++) free(e->p[i].data);
    free(e->p);
    gh_free(e->g);
    free(e);
}

static HEvent *ev_of(uint32_t g) {
    HEvent *e = gh_get(g, TAG_EVENT);
    if (!e) rt_fatal("bad EventRef %08x", g);
    return e;
}

void ev_post(HEvent *e) {
    e->refs++;
    if (g_nq == g_qcap) {
        g_qcap = g_qcap ? g_qcap * 2 : 64;
        g_queue = realloc(g_queue, (size_t)g_qcap * sizeof *g_queue);
    }
    g_queue[g_nq++] = e;
}

/* ---- targets and handlers ---- */
uint32_t ev_new_target(uint32_t parent) {
    Target *t = calloc(1, sizeof *t);
    t->parent = parent;
    t->g = gh_new(TAG_TARGET, t);
    return t->g;
}

uint32_t ev_app_target(void) {
    if (!g_app_target) g_app_target = ev_new_target(0);
    return g_app_target;
}

void ev_set_focus_target(uint32_t window_target) { g_focus_window_target = window_target; }

static Target *target_of(uint32_t g) {
    Target *t = gh_get(g, TAG_TARGET);
    if (!t) rt_fatal("bad EventTargetRef %08x", g);
    return t;
}

static int handler_matches(Handler *h, HEvent *e) {
    for (int i = 0; i < h->nspec; i++)
        if (h->specs[2 * i] == e->cls && h->specs[2 * i + 1] == e->kind) return 1;
    return 0;
}

static int32_t dispatch_from(CPU *cpu, HEvent *e, uint32_t tg, int start);

/* Route an event sent to the dispatcher target to the right window or the app. */
static int32_t dispatcher_route(CPU *cpu, HEvent *e) {
    uint32_t dest = 0;
    Param *w = ev_param(e, kEventParamWindowRef);
    extern uint32_t win_event_target(uint32_t window);
    if (w && w->size == 4) dest = win_event_target(*(uint32_t *)w->data);
    if (!dest && (e->cls == kEventClassKeyboard || e->cls == kEventClassTextInput || e->cls == kEventClassMouse))
        dest = g_focus_window_target;
    if (!dest) dest = ev_app_target();
    Target *t = target_of(dest);
    return dispatch_from(cpu, e, dest, t->nh - 1);
}

static int32_t dispatch_from(CPU *cpu, HEvent *e, uint32_t tg, int start) {
    while (tg) {
        Target *t = target_of(tg);
        for (int i = start; i >= 0; i--) {
            if (i >= t->nh) continue;
            Handler *h = t->h[i];
            if (h->removed || !handler_matches(h, e)) continue;
            CallCtx ctx = {e, tg, i - 1};
            uint32_t callref = gh_new(TAG_CALLREF, &ctx);
            e->refs++;
            int32_t r = (int32_t)rt_call(cpu, h->upp, 3, callref, e->g, h->user);
            ev_release(e);
            gh_free(callref);
            if (r != eventNotHandledErr) return r;
        }
        if (tg == g_dispatcher_target) return dispatcher_route(cpu, e);
        tg = t->parent;
        if (tg) start = target_of(tg)->nh - 1;
    }
    return eventNotHandledErr;
}

static int32_t send_event(CPU *cpu, HEvent *e, uint32_t tg) {
    return dispatch_from(cpu, e, tg, target_of(tg)->nh - 1);
}

/* ---- queue / waiting ---- */
static int matches_list(HEvent *e, uint32_t n, uint32_t list) {
    if (!n) return 1;
    for (uint32_t i = 0; i < n; i++)
        if (R32_(g_mem, list + 8 * i) == e->cls && R32_(g_mem, list + 8 * i + 4) == e->kind) return 1;
    return 0;
}

static double fire_timers(CPU *cpu) {
    double now = ev_now(), next = DBL_MAX;
    for (int i = 0; i < g_ntimers; i++) {
        Timer *t = g_timers[i];
        if (t->removed || t->next < 0) continue;
        if (t->next <= now) {
            t->next = t->interval > 0 ? now + t->interval : -1;
            rt_call(cpu, t->upp, 2, t->g, t->user);
        }
        if (!t->removed && t->next >= 0 && t->next < next) next = t->next;
    }
    return next;
}

__attribute__((weak)) void plat_pump(double timeout) {
    if (timeout > 0) usleep((useconds_t)(timeout > 0.05 ? 50000 : timeout * 1e6));
}

/* Wait for a queued event matching the list. Returns it (still queued unless pulled). */
/* Input scripts can run Python in the game ("py CODE"; platform_mac.m). */
int plat_take_python(char *out, size_t n);
static void run_script_python(CPU *cpu) {
    char code[256];
    if (!plat_take_python(code, sizeof code)) return;
    static uint32_t fn;
    if (!fn) fn = rt_lookup_symbol("_PyRun_SimpleString");
    if (!fn) { rt_log("py: PyRun_SimpleString not found"); return; }
    uint32_t g = gstrdup(code);
    rt_log("py: %s -> %d", code, (int)rt_call(cpu, fn, 1, g));
    gfree(g);
}

static HEvent *receive(CPU *cpu, uint32_t n, uint32_t list, double timeout, int pull) {
    /* The game pumps events with a 1 ms timeout several times per frame and per
     * loading step (PeekMessage). Sleeping that long costs far more than 1 ms on a
     * modern system (timer coalescing), so very short timeouts just poll. */
    if (timeout > 0 && timeout <= 0.002) timeout = 0;
    double deadline = timeout < 0 ? DBL_MAX : ev_now() + timeout;
    for (;;) {
        double next_timer = fire_timers(cpu);
        plat_pump(0);
        run_script_python(cpu);
        for (int i = 0; i < g_nq; i++) {
            HEvent *e = g_queue[i];
            if (!matches_list(e, n, list)) continue;
            if (pull) {
                memmove(&g_queue[i], &g_queue[i + 1], (size_t)(g_nq - i - 1) * sizeof *g_queue);
                g_nq--;
            }
            return e;
        }
        double now = ev_now();
        if (now >= deadline || g_quit) return NULL;
        double wait = deadline - now;
        if (next_timer - now < wait) wait = next_timer - now;
        if (wait > 0.25) wait = 0.25;
        if (wait > 0) plat_pump(wait);
    }
}

/* ---- shims ---- */
SHIM(_GetApplicationEventTarget) { RET(ev_app_target()); }
SHIM(_GetEventDispatcherTarget) {
    if (!g_dispatcher_target) g_dispatcher_target = ev_new_target(0);
    RET(g_dispatcher_target);
}
SHIM(_GetCurrentEventLoop) { RET(0x10c1); }
SHIM(_GetMainEventLoop) { RET(0x10c1); }
SHIM(_GetCurrentEventQueue) { RET(0x10c2); }
SHIM(_GetMainEventQueue) { RET(0x10c2); }
SHIM(_GetCurrentEventTime) { RETF(ev_now()); }

SHIM(_CreateEvent) {
    /* (allocator, class, kind, EventTime when, attributes, EventRef *out) */
    HEvent *e = ev_create(ARG(1), ARG(2));
    double when = ARGF64(3);
    if (when > 0) e->time = when;
    W32_(g_mem, ARG(6), e->g);
    RET(noErr);
}
SHIM(_ReleaseEvent) { ev_release(ev_of(ARG(0))); }
SHIM(_GetEventRetainCount) { RET(ev_of(ARG(0))->refs); }
SHIM(_GetEventClass) { RET(ev_of(ARG(0))->cls); }
SHIM(_GetEventKind) { RET(ev_of(ARG(0))->kind); }

static int coerce(Param *p, uint32_t want, uint8_t *out, uint32_t *out_size) {
    /* The few coercions the game relies on: points between QD and HI forms. */
    if (want == typeQDPoint && p->type == typeHIPoint) {
        float f[2];
        memcpy(f, p->data, 8);
        int16_t qd[2] = {(int16_t)f[1], (int16_t)f[0]}; /* Point is {v, h} */
        memcpy(out, qd, 4);
        *out_size = 4;
        return 1;
    }
    if (want == typeHIPoint && p->type == typeQDPoint) {
        int16_t qd[2];
        memcpy(qd, p->data, 4);
        float f[2] = {qd[1], qd[0]};
        memcpy(out, f, 8);
        *out_size = 8;
        return 1;
    }
    if ((want == typeUInt32 || want == typeSInt32) && p->size == 4) {
        memcpy(out, p->data, 4);
        *out_size = 4;
        return 1;
    }
    return 0;
}

SHIM(_GetEventParameter) {
    /* (event, name, desiredType, outActualType*, bufferSize, outActualSize*, outData*) */
    HEvent *e = ev_of(ARG(0));
    Param *p = ev_param(e, ARG(1));
    if (!p) { RET(eventParameterNotFoundErr); return; }
    uint32_t want = ARG(2), cap = ARG(4);
    uint8_t tmp[64];
    const uint8_t *src = p->data;
    uint32_t size = p->size, type = p->type;
    if (want != typeWildCard && want != p->type) {
        if (!coerce(p, want, tmp, &size)) {
            RET(-9878); /* eventParameterTypeErr? use errAECoercionFail-like failure */
            return;
        }
        src = tmp;
        type = want;
    }
    if (ARG(3)) W32_(g_mem, ARG(3), type);
    if (ARG(5)) W32_(g_mem, ARG(5), size);
    if (ARG(6)) memcpy(PTR(ARG(6)), src, size < cap ? size : cap);
    RET(noErr);
}
SHIM(_SetEventParameter) {
    /* (event, name, type, size, data*) */
    ev_set_param(ev_of(ARG(0)), ARG(1), ARG(2), PTR(ARG(4)), ARG(3));
    RET(noErr);
}

SHIM(_InstallEventHandler) {
    /* (target, upp, numTypes, EventTypeSpec *list, userData, EventHandlerRef *out) */
    Target *t = target_of(ARG(0));
    Handler *h = calloc(1, sizeof *h);
    h->upp = ARG(1);
    h->user = ARG(4);
    h->target = ARG(0);
    h->nspec = (int)ARG(2);
    h->specs = malloc(sizeof(uint32_t) * 2 * (size_t)(h->nspec ? h->nspec : 1));
    memcpy(h->specs, PTR(ARG(3)), 8 * (size_t)h->nspec);
    h->g = gh_new(TAG_HANDLER, h);
    if (t->nh == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->h = realloc(t->h, (size_t)t->cap * sizeof *t->h);
    }
    t->h[t->nh++] = h;
    if (ARG(5)) W32_(g_mem, ARG(5), h->g);
    RET(noErr);
}
static Handler *handler_of(uint32_t g) {
    Handler *h = gh_get(g, TAG_HANDLER);
    if (!h) rt_fatal("bad EventHandlerRef %08x", g);
    return h;
}
SHIM(_AddEventTypesToHandler) {
    Handler *h = handler_of(ARG(0));
    int n = (int)ARG(1);
    h->specs = realloc(h->specs, sizeof(uint32_t) * 2 * (size_t)(h->nspec + n));
    memcpy(h->specs + 2 * h->nspec, PTR(ARG(2)), 8 * (size_t)n);
    h->nspec += n;
    RET(noErr);
}
SHIM(_RemoveEventTypesFromHandler) {
    Handler *h = handler_of(ARG(0));
    for (uint32_t k = 0; k < ARG(1); k++) {
        uint32_t c = R32_(g_mem, ARG(2) + 8 * k), kd = R32_(g_mem, ARG(2) + 8 * k + 4);
        for (int i = 0; i < h->nspec; i++)
            if (h->specs[2 * i] == c && h->specs[2 * i + 1] == kd) h->specs[2 * i] = 0;
    }
    RET(noErr);
}
SHIM(_RemoveEventHandler) {
    Handler *h = handler_of(ARG(0));
    h->removed = 1; /* left in place so an in-progress dispatch stays valid */
    RET(noErr);
}
SHIM(_CallNextEventHandler) {
    CallCtx *c = gh_get(ARG(0), TAG_CALLREF);
    if (!c) rt_fatal("bad EventHandlerCallRef");
    RET(dispatch_from(cpu, ev_of(ARG(1)), c->target, c->next));
}
SHIM(_SendEventToEventTarget) { RET(send_event(cpu, ev_of(ARG(0)), ARG(1))); }

SHIM(_PostEventToQueue) { ev_post(ev_of(ARG(1))); RET(noErr); }
SHIM(_FlushEventQueue) {
    for (int i = 0; i < g_nq; i++) ev_release(g_queue[i]);
    g_nq = 0;
    RET(noErr);
}
SHIM(_ReceiveNextEvent) {
    /* (numTypes, list*, EventTimeout timeout, Boolean pull, EventRef *out) */
    double timeout = ARGF64(2);
    int pull = (uint8_t)ARG(4);
    HEvent *e = receive(cpu, ARG(0), ARG(1), timeout, pull);
    if (!e) { RET(eventLoopTimedOutErr); return; }
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("CIV4_DEBUG_EVENTS") != NULL;
    if (dbg && e->cls == kEventClassMouse && e->kind != kEventMouseMoved)
        rt_log("ReceiveNextEvent: mouse kind %u (pull %d)", e->kind, pull);
    if (ARG(5)) W32_(g_mem, ARG(5), e->g);
    RET(noErr);
}
SHIM(_RunApplicationEventLoop) {
    if (!g_dispatcher_target) g_dispatcher_target = ev_new_target(0);
    g_quit = 0;
    while (!g_quit) {
        HEvent *e = receive(cpu, 0, 0, -1, 1);
        if (!e) continue;
        send_event(cpu, e, g_dispatcher_target);
        ev_release(e);
    }
    g_quit = 0;
}
SHIM(_QuitApplicationEventLoop) { g_quit = 1; }

/* Classic event API: no classic events are delivered; it just services the loop. */
SHIM(_WaitNextEvent) {
    /* (mask, EventRecord *out, sleep ticks, mouseRgn) */
    memset(PTR(ARG(1)), 0, 16);
    double t = ARG(2) / 60.0;
    receive(cpu, 1, 0, t > 0.05 ? 0.05 : t, 0); /* matches nothing: pumps and fires timers */
    RET(0);
}

/* ---- timers ---- */
SHIM(_InstallEventLoopTimer) {
    /* (loop, EventTimerInterval delay, EventTimerInterval interval, upp, userData, out*) */
    Timer *t = calloc(1, sizeof *t);
    double delay = ARGF64(1);
    t->interval = ARGF64(3);
    t->upp = ARG(5);
    t->user = ARG(6);
    t->next = ev_now() + (delay > 0 ? delay : 0);
    t->g = gh_new(TAG_TIMER, t);
    g_timers = realloc(g_timers, (size_t)(g_ntimers + 1) * sizeof *g_timers);
    g_timers[g_ntimers++] = t;
    if (ARG(7)) W32_(g_mem, ARG(7), t->g);
    RET(noErr);
}
SHIM(_RemoveEventLoopTimer) {
    Timer *t = gh_get(ARG(0), TAG_TIMER);
    if (t) t->removed = 1;
    RET(noErr);
}
SHIM(_SetEventLoopTimerNextFireTime) {
    Timer *t = gh_get(ARG(0), TAG_TIMER);
    if (t) {
        double d = ARGF64(1);
        t->next = d < 0 ? -1 : ev_now() + d; /* kEventDurationForever disables */
    }
    RET(noErr);
}

const ShimDef shims_events[] = {
    SHIM_ENTRY(_GetApplicationEventTarget), SHIM_ENTRY(_GetEventDispatcherTarget), SHIM_ENTRY(_GetCurrentEventLoop),
    SHIM_ENTRY(_GetMainEventLoop), SHIM_ENTRY(_GetCurrentEventQueue), SHIM_ENTRY(_GetMainEventQueue),
    SHIM_ENTRY(_GetCurrentEventTime), SHIM_ENTRY(_CreateEvent), SHIM_ENTRY(_ReleaseEvent),
    SHIM_ENTRY(_GetEventRetainCount), SHIM_ENTRY(_GetEventClass), SHIM_ENTRY(_GetEventKind),
    SHIM_ENTRY(_GetEventParameter), SHIM_ENTRY(_SetEventParameter), SHIM_ENTRY(_InstallEventHandler),
    SHIM_ENTRY(_AddEventTypesToHandler), SHIM_ENTRY(_RemoveEventTypesFromHandler), SHIM_ENTRY(_RemoveEventHandler),
    SHIM_ENTRY(_CallNextEventHandler), SHIM_ENTRY(_SendEventToEventTarget), SHIM_ENTRY(_PostEventToQueue),
    SHIM_ENTRY(_FlushEventQueue), SHIM_ENTRY(_ReceiveNextEvent), SHIM_ENTRY(_RunApplicationEventLoop),
    SHIM_ENTRY(_QuitApplicationEventLoop), SHIM_ENTRY(_WaitNextEvent), SHIM_ENTRY(_InstallEventLoopTimer),
    SHIM_ENTRY(_RemoveEventLoopTimer), SHIM_ENTRY(_SetEventLoopTimerNextFireTime),
    {0, 0},
};
