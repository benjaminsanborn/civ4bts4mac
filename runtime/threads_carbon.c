/* Carbon Thread Manager: cooperative threads.
 *
 * Aspyr's Win32 layer builds CreateThread on NewThread and relies on the
 * threads being cooperative (EnterCriticalSection is a no-op). Each Carbon
 * thread runs on its own host thread, but a baton lets only one of them run
 * guest code at a time; it changes hands at YieldToAnyThread (the game's
 * Sleep() spins on it), SetThreadState, thread exit, and whenever the main
 * thread pumps events (as Carbon's event loop does). Used by LAN/online play,
 * replays and the asset file-cache task.
 *
 * Host threads that are not Carbon threads (guest pthreads such as the
 * QuickTime music thread) get their own thread IDs and never hold the baton. */
#include "shim.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#define noErr 0
#define threadNotFoundErr (-618)
#define kReadyThreadState 0
#define kStoppedThreadState 1
#define kRunningThreadState 2
#define kNewSuspend 1
#define kApplicationThreadID 1
#define GUEST_STACK_SIZE (1u << 20)

uint32_t gmemalign(uint32_t align, uint32_t size);
void rt_set_cpu(CPU *c);

typedef struct CThread {
    uint32_t id;
    int state, disposed, coop;
    uint32_t entry, param, result_ptr;
    uint32_t terminator, term_param;
    CPU *cpu;
    struct CThread *next;
} CThread;

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static CThread g_main = {kApplicationThreadID, kRunningThreadState, 0, 1};
static CThread *g_threads = &g_main; /* cooperative threads, main first */
static uint32_t g_baton = kApplicationThreadID, g_next_id = 2;
static __thread CThread *t_self;

/* The calling host thread's record: main, a Carbon thread, or (lazily) a
 * non-cooperative host thread with an ID of its own. */
static CThread *self(void) {
    if (t_self) return t_self;
    if (pthread_main_np()) return t_self = &g_main;
    CThread *t = calloc(1, sizeof *t);
    pthread_mutex_lock(&g_mx);
    t->id = g_next_id++;
    pthread_mutex_unlock(&g_mx);
    t->state = kRunningThreadState;
    return t_self = t;
}

static CThread *find(uint32_t id) {
    for (CThread *t = g_threads; t; t = t->next)
        if (t->id == id) return t;
    return NULL;
}

/* Next ready cooperative thread after `from` (round robin), or NULL. */
static CThread *next_ready(CThread *from) {
    CThread *start = from && from->coop ? from->next : g_threads;
    for (int pass = 0; pass < 2; pass++)
        for (CThread *t = pass ? g_threads : start; t; t = t->next) {
            if (pass && t == start) return NULL;
            if (t != from && !t->disposed && t->state == kReadyThreadState) return t;
        }
    return NULL;
}

/* Hand the baton to `to` and wait (lock held) until it comes back to `me`. */
static void switch_to(CThread *me, CThread *to) {
    if (me->state == kRunningThreadState) me->state = kReadyThreadState;
    to->state = kRunningThreadState;
    g_baton = to->id;
    pthread_cond_broadcast(&g_cv);
    while (g_baton != me->id) {
        pthread_cond_wait(&g_cv, &g_mx);
        if (me->disposed) {
            pthread_mutex_unlock(&g_mx);
            pthread_exit(NULL);
        }
    }
    me->state = kRunningThreadState;
}

/* Give other cooperative threads a turn; returns whether anyone ran. */
static int yield_coop(CThread *me) {
    pthread_mutex_lock(&g_mx);
    CThread *n = me->coop && g_baton == me->id ? next_ready(me) : NULL;
    if (n) switch_to(me, n);
    pthread_mutex_unlock(&g_mx);
    return n != NULL;
}

/* Called by the event loop on the main thread (events.c). */
void cthread_event_yield(void) {
    if (g_threads->next) yield_coop(self());
}

static void finish(CThread *me) {
    /* lock held */
    me->disposed = 1;
    for (CThread **pp = &g_threads; *pp; pp = &(*pp)->next)
        if (*pp == me) { *pp = me->next; break; }
    CThread *n = next_ready(NULL);
    if (!n) n = &g_main;
    n->state = kRunningThreadState;
    g_baton = n->id;
    pthread_cond_broadcast(&g_cv);
}

static void *thread_body(void *p) {
    CThread *me = p;
    t_self = me;
    rt_set_cpu(me->cpu);
    pthread_mutex_lock(&g_mx);
    while (g_baton != me->id) {
        pthread_cond_wait(&g_cv, &g_mx);
        if (me->disposed) { pthread_mutex_unlock(&g_mx); return NULL; }
    }
    me->state = kRunningThreadState;
    pthread_mutex_unlock(&g_mx);
    uint32_t r = rt_call(me->cpu, me->entry, 1, me->param);
    if (me->result_ptr) W32_(g_mem, me->result_ptr, r);
    if (me->terminator) rt_call(me->cpu, me->terminator, 2, me->id, me->term_param);
    pthread_mutex_lock(&g_mx);
    finish(me);
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

SHIM(_NewThread) {
    /* (ThreadStyle, ThreadEntryTPP entry, void *param, Size stackSize,
     *  ThreadOptions options, void **threadResult, ThreadID *threadMade) */
    self();
    CThread *t = calloc(1, sizeof *t);
    t->coop = 1;
    t->entry = ARG(1);
    t->param = ARG(2);
    t->result_ptr = ARG(5);
    t->state = (ARG(4) & kNewSuspend) ? kStoppedThreadState : kReadyThreadState;
    uint32_t stack = gmemalign(0x4000, GUEST_STACK_SIZE);
    t->cpu = calloc(1, sizeof *t->cpu);
    t->cpu->fcw = 0x37f;
    t->cpu->r[4] = stack + GUEST_STACK_SIZE - 64;
    pthread_mutex_lock(&g_mx);
    t->id = g_next_id++;
    CThread **pp = &g_threads;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    pthread_mutex_unlock(&g_mx);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20); /* recompiled frames nest deeply */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &at, thread_body, t);
    if (ARG(6)) W32_(g_mem, ARG(6), t->id);
    RET(noErr);
}

SHIM(_YieldToAnyThread) {
    /* Also where the game's Sleep() spins: give the time back if nobody else is ready. */
    if (!yield_coop(self())) usleep(250);
    RET(noErr);
}

SHIM(_GetCurrentThread) {
    W32_(g_mem, ARG(0), self()->id);
    RET(noErr);
}

SHIM(_GetThreadState) {
    /* (ThreadID, ThreadState *) */
    pthread_mutex_lock(&g_mx);
    CThread *t = find(ARG(0));
    int st = t ? (t->id == g_baton ? kRunningThreadState : t->state) : -1;
    pthread_mutex_unlock(&g_mx);
    if (st < 0) {
        if (ARG(0) == self()->id) st = kRunningThreadState;
        else { RET(threadNotFoundErr); return; }
    }
    if (ARG(1)) W16_(g_mem, ARG(1), (uint16_t)st);
    RET(noErr);
}

SHIM(_SetThreadState) {
    /* (ThreadID, ThreadState newState, ThreadID suggestedThread) */
    CThread *me = self();
    pthread_mutex_lock(&g_mx);
    CThread *t = find(ARG(0));
    if (!t) { pthread_mutex_unlock(&g_mx); RET(ARG(0) == me->id ? noErr : threadNotFoundErr); return; }
    uint16_t ns = (uint16_t)ARG(1);
    if (t == me) {
        if (ns == kStoppedThreadState && me->coop && g_baton == me->id) {
            CThread *s = find(ARG(2));
            CThread *n = s && s != me && !s->disposed && s->state == kReadyThreadState ? s : next_ready(me);
            if (n) {
                me->state = kStoppedThreadState;
                switch_to(me, n); /* returns when someone makes us ready and hands back the baton */
            }
        }
    } else if (t->state != kRunningThreadState) {
        t->state = ns == kStoppedThreadState ? kStoppedThreadState : kReadyThreadState;
        if (ns == kRunningThreadState && me->coop && g_baton == me->id) switch_to(me, t);
    }
    pthread_mutex_unlock(&g_mx);
    RET(noErr);
}

SHIM(_SetThreadTerminator) {
    /* (ThreadID, ThreadTerminationUPP, void *param) */
    pthread_mutex_lock(&g_mx);
    CThread *t = find(ARG(0));
    if (t) { t->terminator = ARG(1); t->term_param = ARG(2); }
    pthread_mutex_unlock(&g_mx);
    RET(noErr);
}

SHIM(_DisposeThread) {
    /* (ThreadID, void *threadResult, Boolean recycleThread) */
    CThread *me = self();
    pthread_mutex_lock(&g_mx);
    CThread *t = find(ARG(0));
    if (!t || t == &g_main) { pthread_mutex_unlock(&g_mx); RET(threadNotFoundErr); return; }
    uint32_t term = t->terminator, tparam = t->term_param, id = t->id;
    pthread_mutex_unlock(&g_mx);
    if (term) rt_call(cpu, term, 2, id, tparam);
    pthread_mutex_lock(&g_mx);
    if (t->result_ptr) W32_(g_mem, t->result_ptr, ARG(1));
    if (t == me) {
        finish(me);
        pthread_mutex_unlock(&g_mx);
        pthread_exit(NULL);
    }
    t->disposed = 1; /* its host thread exits when it next wakes */
    for (CThread **pp = &g_threads; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mx);
    RET(noErr);
}

const ShimDef shims_threads_carbon[] = {
    SHIM_ENTRY(_NewThread), SHIM_ENTRY(_YieldToAnyThread), SHIM_ENTRY(_GetCurrentThread),
    SHIM_ENTRY(_GetThreadState), SHIM_ENTRY(_SetThreadState), SHIM_ENTRY(_SetThreadTerminator),
    SHIM_ENTRY(_DisposeThread),
    {0, 0},
};
