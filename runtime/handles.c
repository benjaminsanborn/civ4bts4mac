/* Opaque guest handles for host objects: a 16-byte guest block
 * {tag, index, 0, 0} whose address is what the guest sees (EventRef,
 * WindowRef, EventLoopTimerRef, ...). Guest code only passes them around. */
#include "handles.h"

#include <pthread.h>
#include <stdlib.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void **g_objs;
static uint32_t g_n, g_cap;
static uint32_t *g_free;
static uint32_t g_nfree, g_freecap;

uint32_t gh_new(uint32_t tag, void *host) {
    pthread_mutex_lock(&g_lock);
    uint32_t idx;
    if (g_nfree) {
        idx = g_free[--g_nfree];
    } else {
        if (g_n == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 1024;
            g_objs = realloc(g_objs, g_cap * sizeof *g_objs);
        }
        idx = g_n++;
    }
    g_objs[idx] = host;
    pthread_mutex_unlock(&g_lock);
    uint32_t g = gcalloc(1, 16);
    W32_(g_mem, g, tag);
    W32_(g_mem, g + 4, idx);
    return g;
}

/* Handles live in the guest heap, 8-byte aligned. Anything else (a stale or
 * garbage pointer from a guest race) is rejected before it is read: outside
 * the heap the read itself would fault. */
static int plausible(uint32_t g) { return g >= 0x10000000u && g < 0xA0000000u && !(g & 7); }

void *gh_get(uint32_t g, uint32_t tag) {
    if (!plausible(g)) return NULL;
    if (R32_(g_mem, g) != tag) return NULL;
    uint32_t idx = R32_(g_mem, g + 4);
    return idx < g_n ? g_objs[idx] : NULL;
}

void gh_free(uint32_t g) {
    if (!plausible(g)) return;
    pthread_mutex_lock(&g_lock);
    uint32_t idx = R32_(g_mem, g + 4);
    g_objs[idx] = NULL;
    if (g_nfree == g_freecap) {
        g_freecap = g_freecap ? g_freecap * 2 : 256;
        g_free = realloc(g_free, g_freecap * sizeof *g_free);
    }
    g_free[g_nfree++] = idx;
    pthread_mutex_unlock(&g_lock);
    W32_(g_mem, g, 0xdeaddeadu);
    gfree(g);
}
