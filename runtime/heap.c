/* Guest heap: a dlmalloc mspace placed inside the guest arena, so allocations
 * are native-speed but every pointer handed to the guest is a 32-bit guest
 * address. */
#include "shim.h"

#include <string.h>
#include <sys/mman.h>

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_DL_PREFIX 1
#define MALLOC_ALIGNMENT 16
#define FOOTERS 0
#ifdef CIV4_HEAP_DEBUG
#define DEBUG 1
#endif
#define DLMALLOC_EXPORT static
/* Explain allocator corruption before aborting. */
struct malloc_state;
static void heap_usage_error(struct malloc_state *m, void *mem);
#define USAGE_ERROR_ACTION(m, p) heap_usage_error((struct malloc_state *)(m), (void *)(p))
#define CORRUPTION_ERROR_ACTION(m) heap_usage_error((struct malloc_state *)(m), 0)
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#endif
#include <execinfo.h>
#include <stdio.h>
#include "../third_party/dlmalloc/malloc.c"

#define HEAP_BASE 0x10000000u
#define HEAP_SIZE 0x90000000u /* up to 0xA0000000 */

static mspace g_heap;
static uint32_t g_trace_addr;

static void heap_usage_error(struct malloc_state *m, void *mem) {
    extern uint64_t g_import_calls;
    fprintf(stderr, "[heap] at call %llu\n", (unsigned long long)g_import_calls);
    fprintf(stderr, "[heap] allocator error: mem=%08x least_addr=%08x top=%08x topsize=%llx dv=%08x dvsize=%llx\n",
            mem ? GADDR(mem) : 0, GADDR(m->least_addr), GADDR(m->top), (unsigned long long)m->topsize, GADDR(m->dv),
            (unsigned long long)m->dvsize);
    if (mem) {
        mchunkptr p = mem2chunk(mem);
        size_t sz = chunksize(p);
        mchunkptr n = chunk_plus_offset(p, sz);
        fprintf(stderr, "[heap]   chunk %08x head %llx (size %zx cinuse %d pinuse %d); next %08x head %llx (pinuse %d)\n",
                GADDR(p), (unsigned long long)p->head, sz, (int)!!cinuse(p), (int)!!pinuse(p), GADDR(n),
                (unsigned long long)n->head, (int)!!pinuse(n));
    }
    void *bt[16];
    int n = backtrace(bt, 16);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}
static void heap_trace(const char *op, uint32_t p, uint32_t r, void *caller);

int rt_heap_init(void) {
    if (mprotect(g_mem + HEAP_BASE, HEAP_SIZE, PROT_READ | PROT_WRITE)) return -1;
    g_heap = create_mspace_with_base(g_mem + HEAP_BASE, HEAP_SIZE, 1);
    if (getenv("CIV4_HEAP_LAYOUT"))
        fprintf(stderr, "[heap] mstate at guest %08x size %zu; top field %08x, smallbins %08x, treebins %08x\n",
                GADDR(g_heap), sizeof(struct malloc_state), GADDR(&((mstate)g_heap)->top),
                GADDR(&((mstate)g_heap)->smallbins), GADDR(&((mstate)g_heap)->treebins));
    return g_heap ? 0 : -1;
}

static uint32_t raw_malloc(uint32_t size) {
    uint32_t r = GADDR(mspace_malloc(g_heap, size ? size : 1));
    heap_trace("malloc", 0, r, __builtin_return_address(0));
    return r;
}
static uint32_t raw_calloc(uint32_t n, uint32_t size) {
    uint32_t r = GADDR(mspace_calloc(g_heap, n, size));
    heap_trace("calloc", 0, r, __builtin_return_address(0));
    return r;
}
static uint32_t raw_realloc(uint32_t p, uint32_t size) {
    if (g_trace_addr && p == g_trace_addr) {
        extern uint64_t g_import_calls;
        fprintf(stderr, "[heap] enter realloc(%08x, %u) head=%llx at call %llu\n", p, size,
                (unsigned long long)R64_(g_mem, p - 8), (unsigned long long)g_import_calls);
    }
    uint32_t r = GADDR(mspace_realloc(g_heap, PTR(p), size));
    heap_trace("realloc", p, r, __builtin_return_address(0));
    return r;
}
static uint32_t raw_memalign(uint32_t align, uint32_t size) { return GADDR(mspace_memalign(g_heap, align, size)); }
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
/* CIV4_TRACE_HEAP=addr: log every heap operation touching that guest address, with the host caller. */
static int g_trace_init;
static void heap_trace(const char *op, uint32_t p, uint32_t r, void *caller) {
    if (!g_trace_init) {
        g_trace_init = 1;
        const char *t = getenv("CIV4_TRACE_HEAP");
        g_trace_addr = t ? (uint32_t)strtoul(t, NULL, 16) : 0;
    }
    if (!g_trace_addr || (p != g_trace_addr && r != g_trace_addr)) return;
    Dl_info di;
    const char *who = dladdr(caller, &di) && di.dli_sname ? di.dli_sname : "?";
    extern uint64_t g_import_calls;
    fprintf(stderr, "[heap] %s(%08x) -> %08x from %s at call %llu\n", op, p, r, who, (unsigned long long)g_import_calls);
}

static void raw_free(uint32_t p) {
    heap_trace("free", p, 0, __builtin_return_address(0));
    if (p) mspace_free(g_heap, PTR(p));
}

/* ---- debug heap (CIV4_REDZONE=1) ----
 * Block layout: [size u32][guest call site u32][magic u32][alloc id u32] user... [32-byte redzone 0xAB]
 * The user pointer stays 16-aligned. Checked on free/realloc and by rt_redzone_sweep(). */
#define RZ 32
#define HDR 16
#define RZ_MAGIC 0x52454421u /* live */
#define RZ_DEAD 0x44454144u  /* freed */
static int g_rz = -1;
static uint32_t g_rz_id;
static uint32_t *g_live;
static size_t g_nlive, g_caplive;

static int rz_on(void) {
    if (g_rz < 0) g_rz = getenv("CIV4_REDZONE") != NULL;
    return g_rz;
}
static uint32_t guest_caller(void) {
    CPU *c = rt_cpu();
    return c ? R32_(g_mem, c->r[4]) : 0; /* return address of the guest call into the shim */
}
static void rz_fail(uint32_t user, const char *what) {
    uint32_t b = user - HDR;
    fprintf(stderr, "\n[redzone] %s: block %08x size %u allocated by guest code at %08x (alloc #%u)\n", what, user,
            R32_(g_mem, b), R32_(g_mem, b + 4), R32_(g_mem, b + 12));
    rt_fatal("heap corruption detected");
}
static void rz_check(uint32_t user, const char *ctx) {
    uint32_t b = user - HDR, size = R32_(g_mem, b);
    if (R32_(g_mem, b + 8) == RZ_DEAD) rz_fail(user, "double free / use of freed block");
    if (R32_(g_mem, b + 8) != RZ_MAGIC) rz_fail(user, "header overwritten (underflow?)");
    for (uint32_t i = 0; i < RZ; i++)
        if (R8_(g_mem, user + size + i) != 0xAB) {
            fprintf(stderr, "[redzone] %s: overflow at +%u (byte %02x)\n", ctx, size + i, R8_(g_mem, user + size + i));
            rz_fail(user, "buffer overflow past end");
        }
}
static void live_add(uint32_t u) {
    if (g_nlive == g_caplive) {
        g_caplive = g_caplive ? g_caplive * 2 : 1 << 16;
        g_live = realloc(g_live, g_caplive * sizeof *g_live);
    }
    g_live[g_nlive++] = u;
}
static void live_del(uint32_t u) {
    for (size_t i = g_nlive; i-- > 0;)
        if (g_live[i] == u) { g_live[i] = g_live[--g_nlive]; return; }
}
static uint32_t rz_wrap(uint32_t b, uint32_t size) {
    if (!b) return 0;
    uint32_t u = b + HDR;
    W32_(g_mem, b, size);
    W32_(g_mem, b + 4, guest_caller());
    W32_(g_mem, b + 8, RZ_MAGIC);
    W32_(g_mem, b + 12, ++g_rz_id);
    memset(PTR(u + size), 0xAB, RZ);
    live_add(u);
    return u;
}
void rt_quarantine_sweep(void);
void rt_redzone_sweep(void) {
    if (!rz_on()) return;
    for (size_t i = 0; i < g_nlive; i++) rz_check(g_live[i], "sweep");
    rt_quarantine_sweep();
}

uint32_t gmalloc(uint32_t size) {
    if (!rz_on()) return raw_malloc(size);
    return rz_wrap(raw_malloc(size + HDR + RZ), size);
}
uint32_t gcalloc(uint32_t n, uint32_t size) {
    if (!rz_on()) return raw_calloc(n, size);
    uint32_t total = n * size;
    return rz_wrap(raw_calloc(1, total + HDR + RZ), total);
}
uint32_t gmemalign(uint32_t align, uint32_t size) {
    if (!rz_on()) return raw_memalign(align, size);
    /* Over-allocate so the user pointer (block + HDR) can be aligned. */
    uint32_t a = align < 16 ? 16 : align;
    uint32_t b = raw_memalign(a, size + a + RZ);
    if (!b) return 0;
    uint32_t u = b + a, hdr = u - HDR;
    W32_(g_mem, hdr, size);
    W32_(g_mem, hdr + 4, guest_caller());
    W32_(g_mem, hdr + 8, RZ_MAGIC);
    W32_(g_mem, hdr + 12, ++g_rz_id);
    W32_(g_mem, b, 0x414c4e44u); /* 'ALND': marks an aligned block */
    W32_(g_mem, b + 4, u);
    memset(PTR(u + size), 0xAB, RZ);
    live_add(u);
    return u;
}
static uint32_t base_of(uint32_t u) {
    /* Aligned blocks keep their real base below the header. */
    for (uint32_t a = 16; a <= 4096; a <<= 1)
        if (u >= a && R32_(g_mem, u - a) == 0x414c4e44u && R32_(g_mem, u - a + 4) == u) return u - a;
    return u - HDR;
}
/* Quarantine: freed blocks are poisoned with 0xDD and only released after
 * QUAR_BYTES more have been freed; any change means a write after free. */
#define QUAR_BYTES (64u << 20)
static uint32_t *g_quar;
static size_t g_qhead, g_qtail, g_qcap;
static uint64_t g_qbytes;

static void quar_release(uint32_t u) {
    uint32_t size = R32_(g_mem, u - HDR);
    const uint8_t *d = PTR(u);
    for (uint32_t i = 0; i < size; i++) {
        if (d[i] != 0xDD) {
            fprintf(stderr, "\n[redzone] write after free: block %08x size %u, byte +%u = %02x; allocated at %08x, "
                            "freed at %08x\n", u, size, i, d[i], R32_(g_mem, u - HDR + 4), R32_(g_mem, u - HDR + 12));
            rt_fatal("heap corruption detected");
        }
    }
    g_qbytes -= size;
    raw_free(base_of(u));
}

void rt_quarantine_sweep(void) {
    for (size_t i = g_qhead; i != g_qtail; i = (i + 1) % g_qcap) {
        uint32_t u = g_quar[i], size = R32_(g_mem, u - HDR);
        const uint8_t *d = PTR(u);
        for (uint32_t k = 0; k < size; k++)
            if (d[k] != 0xDD) {
                fprintf(stderr, "\n[redzone] write after free (sweep): block %08x size %u, byte +%u = %02x; allocated "
                                "at %08x, freed at %08x\n", u, size, k, d[k], R32_(g_mem, u - HDR + 4),
                        R32_(g_mem, u - HDR + 12));
                rt_fatal("heap corruption detected");
            }
    }
}

void gfree(uint32_t u) {
    if (!rz_on()) { raw_free(u); return; }
    if (!u) return;
    rz_check(u, "free");
    uint32_t size = R32_(g_mem, u - HDR);
    W32_(g_mem, u - HDR + 8, RZ_DEAD);
    W32_(g_mem, u - HDR + 12, guest_caller()); /* free site replaces the alloc id */
    live_del(u);
    memset(PTR(u), 0xDD, size);
    if (!g_qcap) {
        g_qcap = 1 << 20;
        g_quar = malloc(g_qcap * sizeof *g_quar);
    }
    if ((g_qtail + 1) % g_qcap == g_qhead) {
        quar_release(g_quar[g_qhead]);
        g_qhead = (g_qhead + 1) % g_qcap;
    }
    g_quar[g_qtail] = u;
    g_qtail = (g_qtail + 1) % g_qcap;
    g_qbytes += size;
    while (g_qbytes > QUAR_BYTES && g_qhead != g_qtail) {
        quar_release(g_quar[g_qhead]);
        g_qhead = (g_qhead + 1) % g_qcap;
    }
}
uint32_t grealloc(uint32_t u, uint32_t size) {
    if (!rz_on()) return raw_realloc(u, size);
    if (!u) return gmalloc(size);
    rz_check(u, "realloc");
    uint32_t old = R32_(g_mem, u - HDR);
    uint32_t n = gmalloc(size);
    if (n) memcpy(PTR(n), PTR(u), old < size ? old : size);
    gfree(u);
    return n;
}

uint32_t gstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    uint32_t g = gmalloc((uint32_t)n);
    memcpy(PTR(g), s, n);
    return g;
}

#if DEBUG
/* Full walk of the heap; aborts (via dlmalloc's assert) on any inconsistency. */
void rt_heap_check(void) { do_check_malloc_state((mstate)g_heap); }
#else
void rt_heap_check(void) {}
#endif

size_t rt_heap_usable(uint32_t p) { return p ? (rz_on() ? R32_(g_mem, p - HDR) : mspace_usable_size(PTR(p))) : 0; }

/* Guest heap bytes in use (for CIV4_MEMSTATS). */
size_t rt_guest_heap_used(void) {
    struct mallinfo mi = mspace_mallinfo(g_heap);
    return mi.uordblks;
}
