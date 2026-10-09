/* Core runtime: guest arena, indirect-call dispatch, import routing. */
#include "rt.h"
#include "functab.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

uint8_t *g_mem;

/* Guest arena: 4 GB plus a guard so accesses straddling 0xffffffff fault
 * instead of touching host memory. Everything starts PROT_NONE. */
#define ARENA_SIZE ((size_t)1 << 32)
#define ARENA_GUARD ((size_t)1 << 16)

int rt_arena_init(void) {
    void *p = mmap(NULL, ARENA_SIZE + ARENA_GUARD, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -1;
    g_mem = p;
    return 0;
}

int rt_arena_protect(uint32_t addr, uint32_t size, int prot) {
    return mprotect(g_mem + addr, size, prot);
}

/* Shims for imported (host-library) functions, indexed by import id.
 * A shim reads its arguments at esp+4 like the original callee would. */
GuestFn g_import_shims[4096];
void (*rt_trap_hook)(CPU *cpu, uint32_t addr, const char *what);

_Noreturn void rt_unimpl(CPU *cpu, uint32_t addr, const char *what) {
    if (rt_trap_hook) rt_trap_hook(cpu, addr, what);
    fprintf(stderr, "rt: trap at 0x%08x: %s\n", addr, what);
    abort();
}

void rt_import(CPU *cpu, uint32_t id) {
    GuestFn shim = id < g_import_count ? g_import_shims[id] : NULL;
    if (!shim) rt_unimpl(cpu, id, id < g_import_count ? g_import_names[id] : "bad import id");
    shim(cpu);
    cpu->r[4] += 4; /* pop the return address, as the callee's ret would */
}

static GuestFn lookup(uint32_t target) {
    uint32_t lo = 0, hi = g_functab_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_functab[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_functab_len && g_functab[lo].addr == target) ? g_functab[lo].fn : NULL;
}

void rt_dispatch(CPU *cpu, uint32_t target) {
    GuestFn fn = lookup(target);
    if (fn) {
        fn(cpu);
        return;
    }
    for (uint32_t i = 0; i < g_import_count; i++) {
        if (g_import_stub[i] == target) {
            rt_import(cpu, i);
            return;
        }
    }
    rt_unimpl(cpu, target, "dispatch miss");
}
