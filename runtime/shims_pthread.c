/* pthread shims. Guest threads run on real host threads, each with its own
 * CPU and a guest stack in the arena. Guest sync objects are backed by host
 * objects whose pointers are kept inside the guest struct's opaque bytes. */
#include "shim.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

void rt_set_cpu(CPU *cpu);

#define GUEST_STACK_SIZE (2u << 20)
#define MUTEX_LIVE 0x4d555458u /* 'MUTX': opaque holds a host pointer */
#define ONCE_DONE 0x4f4e4345u  /* 'ONCE' */

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- threads ---- */
typedef struct {
    uint32_t start, arg;
    CPU *cpu;
} ThreadStart;

static void *thread_main(void *p) {
    ThreadStart ts = *(ThreadStart *)p;
    free(p);
    rt_set_cpu(ts.cpu);
    rt_call(ts.cpu, ts.start, 1, ts.arg);
    return NULL;
}

SHIM(_pthread_create) {
    uint32_t stack = gmemalign(0x4000, GUEST_STACK_SIZE);
    CPU *c = calloc(1, sizeof *c);
    c->fcw = 0x37f;
    c->r[4] = stack + GUEST_STACK_SIZE - 64;
    ThreadStart *ts = malloc(sizeof *ts);
    *ts = (ThreadStart){ARG(2), ARG(3), c};
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20); /* host stack: recompiled frames nest deeply */
    int r = pthread_create(&th, &at, thread_main, ts);
    if (ARG(0)) W32_(g_mem, ARG(0), (uint32_t)(uintptr_t)th); /* opaque handle; only compared */
    RET(r);
}
SHIM(_pthread_cancel$UNIX2003) { RET(0); }
SHIM(_pthread_testcancel$UNIX2003) {}

/* ---- mutexes: guest pthread_mutex_t = {long sig; char opaque[40]} ---- */
typedef struct {
    pthread_mutex_t m;
} HostMutex;

static HostMutex *mutex_of(uint32_t g, int type) {
    uint32_t sig = R32_(g_mem, g);
    if (sig == MUTEX_LIVE) {
        HostMutex *hm;
        memcpy(&hm, PTR(g + 4), sizeof hm);
        return hm;
    }
    pthread_mutex_lock(&g_init_lock);
    HostMutex *hm;
    if (R32_(g_mem, g) == MUTEX_LIVE) {
        memcpy(&hm, PTR(g + 4), sizeof hm);
    } else {
        hm = malloc(sizeof *hm);
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        /* Static initializers: 0x32AAABA7 normal, 0x32AAABA2 recursive (Darwin). */
        if (type == 2 || sig == 0x32AAABA2u) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&hm->m, &a);
        pthread_mutexattr_destroy(&a);
        memcpy(PTR(g + 4), &hm, sizeof hm);
        __atomic_store_n((uint32_t *)PTR(g), MUTEX_LIVE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_init_lock);
    return hm;
}

pthread_mutex_t *rt_host_mutex(uint32_t g) { return &mutex_of(g, 0)->m; }

/* guest pthread_mutexattr_t = {long sig; char opaque[8]}: opaque[0] = type */
SHIM(_pthread_mutexattr_init) { W32_(g_mem, ARG(0), 0x4d545841u); W32_(g_mem, ARG(0) + 4, 0); RET(0); }
SHIM(_pthread_mutexattr_destroy$UNIX2003) { RET(0); }
SHIM(_pthread_mutexattr_settype) { W32_(g_mem, ARG(0) + 4, ARG(1)); RET(0); }
SHIM(_pthread_mutex_init) {
    W32_(g_mem, ARG(0), 0);
    mutex_of(ARG(0), ARG(1) ? (int)R32_(g_mem, ARG(1) + 4) : 0);
    RET(0);
}
SHIM(_pthread_mutex_destroy) {
    if (R32_(g_mem, ARG(0)) == MUTEX_LIVE) {
        HostMutex *hm = mutex_of(ARG(0), 0);
        pthread_mutex_destroy(&hm->m);
        free(hm);
        W32_(g_mem, ARG(0), 0);
    }
    RET(0);
}
SHIM(_pthread_mutex_lock) { RET(pthread_mutex_lock(&mutex_of(ARG(0), 0)->m)); }
SHIM(_pthread_mutex_trylock) { RET(pthread_mutex_trylock(&mutex_of(ARG(0), 0)->m)); }
SHIM(_pthread_mutex_unlock) { RET(pthread_mutex_unlock(&mutex_of(ARG(0), 0)->m)); }

/* ---- once: guest pthread_once_t = {long sig; char opaque[4]} ---- */
SHIM(_pthread_once) {
    uint32_t ctl = ARG(0);
    if (__atomic_load_n((uint32_t *)PTR(ctl + 4), __ATOMIC_ACQUIRE) == ONCE_DONE) { RET(0); return; }
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once_lock);
    if (R32_(g_mem, ctl + 4) != ONCE_DONE) {
        rt_call(cpu, ARG(1), 0);
        __atomic_store_n((uint32_t *)PTR(ctl + 4), ONCE_DONE, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    RET(0);
}

/* ---- thread-specific data ---- */
#define MAX_KEYS 512
static uint32_t g_nkeys = 1; /* key 0 reserved so 0 never looks valid */
static __thread uint32_t t_specific[MAX_KEYS];

SHIM(_pthread_key_create) {
    uint32_t k = __atomic_fetch_add(&g_nkeys, 1, __ATOMIC_SEQ_CST);
    if (k >= MAX_KEYS) rt_fatal("out of pthread keys");
    W32_(g_mem, ARG(0), k);
    RET(0);
}
SHIM(_pthread_key_delete) { RET(0); }
SHIM(_pthread_getspecific) { RET(ARG(0) < MAX_KEYS ? t_specific[ARG(0)] : 0); }
SHIM(_pthread_setspecific) {
    if (ARG(0) >= MAX_KEYS) { RET(22); return; }
    t_specific[ARG(0)] = ARG(1);
    RET(0);
}

const ShimDef shims_pthread[] = {
    SHIM_ENTRY(_pthread_create), SHIM_ENTRY(_pthread_cancel$UNIX2003), SHIM_ENTRY(_pthread_testcancel$UNIX2003),
    SHIM_ENTRY(_pthread_mutexattr_init), SHIM_ENTRY(_pthread_mutexattr_destroy$UNIX2003),
    SHIM_ENTRY(_pthread_mutexattr_settype), SHIM_ENTRY(_pthread_mutex_init), SHIM_ENTRY(_pthread_mutex_destroy),
    SHIM_ENTRY(_pthread_mutex_lock), SHIM_ENTRY(_pthread_mutex_trylock), SHIM_ENTRY(_pthread_mutex_unlock),
    SHIM_ENTRY(_pthread_once), SHIM_ENTRY(_pthread_key_create), SHIM_ENTRY(_pthread_key_delete),
    SHIM_ENTRY(_pthread_getspecific), SHIM_ENTRY(_pthread_setspecific),
    {0, 0},
};
