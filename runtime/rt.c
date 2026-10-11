/* Core runtime: guest arena, indirect-call dispatch, import routing. */
#include "rt.h"
#include "events.h"
#include "functab.h"

#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

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

/* Whether the guest byte at addr can be read (for shims given untrusted
 * pointers). Pages only ever become readable, so readable pages are cached. */
int rt_guest_readable(uint32_t addr) {
    enum { PAGE_SHIFT_ = 14, SLOTS = 64 };
    static _Atomic uint32_t cache[SLOTS]; /* page number + 1 */
    uint32_t page = addr >> PAGE_SHIFT_;
    if (atomic_load_explicit(&cache[page % SLOTS], memory_order_relaxed) == page + 1) return 1;
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)(g_mem + ((uint64_t)page << PAGE_SHIFT_));
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &n, &obj))
        return 0;
    if (a > (mach_vm_address_t)(uintptr_t)(g_mem + ((uint64_t)page << PAGE_SHIFT_)) || !(info.protection & VM_PROT_READ))
        return 0;
    atomic_store_explicit(&cache[page % SLOTS], page + 1, memory_order_relaxed);
    return 1;
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

/* Imports created at runtime (rt_dynamic_import) get ids after the static ones. */
#define MAX_DYN_IMPORTS 1024
GuestFn g_dyn_shims[MAX_DYN_IMPORTS];
const char *g_dyn_names[MAX_DYN_IMPORTS];
uint32_t g_dyn_count;

static const char *import_name(uint32_t id) {
    return id < g_import_count ? g_import_names[id]
         : id - g_import_count < g_dyn_count ? g_dyn_names[id - g_import_count] : "?";
}

/* CIV4_WATCH=addr[,addr...]: report when any watched guest dword changes,
 * checked at every host call (so the culprit ran since the previous one). */
static uint32_t g_watch[8], g_watch_val[8];
static int g_nwatch = -1;
static uint32_t g_last_import;
/* CIV4_WATCH_ARM=value:n: only start watching after an import has returned `value` n times. */
static uint32_t g_arm_value, g_arm_count;
/* Breakpoint target for debuggers: called when the watch arms. */
__attribute__((noinline)) void rt_watch_armed(uint32_t guest_addr) { __asm__ volatile("" ::"r"(guest_addr)); }
static int g_armed = 1;
void rt_report_watch(CPU *cpu, int i, uint32_t old, uint32_t now, uint32_t last_import);
static void check_watch(CPU *cpu) {
    if (g_nwatch < 0) {
        g_nwatch = 0;
        const char *arm = getenv("CIV4_WATCH_ARM");
        if (arm) {
            char *e;
            g_arm_value = (uint32_t)strtoul(arm, &e, 16);
            g_arm_count = *e == ':' ? (uint32_t)strtoul(e + 1, NULL, 10) : 1;
            g_armed = 0;
        }
        const char *w = getenv("CIV4_WATCH");
        while (w && *w && g_nwatch < 8) {
            g_watch[g_nwatch] = (uint32_t)strtoul(w, (char **)&w, 16);
            g_watch_val[g_nwatch] = R32_(g_mem, g_watch[g_nwatch]);
            g_nwatch++;
            if (*w == ',') w++;
        }
    }
    for (int i = 0; i < g_nwatch && g_armed; i++) {
        uint32_t v = R32_(g_mem, g_watch[i]);
        if (v != g_watch_val[i]) {
            rt_report_watch(cpu, i, g_watch_val[i], v, g_last_import);
            g_watch_val[i] = v;
        }
    }
}

/* CIV4_HEAPCHECK=start:every: full heap check after every `every`-th host call
 * from call number `start` on (needs a CIV4_HEAP_DEBUG build of heap.c). */
uint64_t g_import_calls;
static uint64_t g_hc_start = UINT64_MAX, g_hc_every;
void rt_heap_check(void);

/* Flight recorder: the last host calls, dumped on crashes and traps. */
#define FR_N 64
static struct { uint64_t n; uint32_t id, ret_addr, a[4], result; } g_fr[FR_N];

void rt_dump_recent_calls(void) {
    fprintf(stderr, "  recent host calls (oldest first):\n");
    for (uint64_t k = g_import_calls >= FR_N ? g_import_calls - FR_N + 1 : 1; k <= g_import_calls; k++) {
        typeof(g_fr[0]) *e = &g_fr[k % FR_N];
        if (e->n != k) continue;
        fprintf(stderr, "    #%llu %s(%08x, %08x, %08x, %08x) from %08x -> %08x\n", (unsigned long long)k,
                import_name(e->id), e->a[0], e->a[1], e->a[2], e->a[3], e->ret_addr, e->result);
    }
}

static void rt_import_debug(CPU *cpu, uint32_t id) {
    if (g_nwatch) check_watch(cpu);
    g_import_calls++;
    if (g_hc_start == UINT64_MAX) {
        const char *h = getenv("CIV4_HEAPCHECK");
        g_hc_start = UINT64_MAX - 1;
        if (h) {
            char *e;
            g_hc_start = strtoull(h, &e, 10);
            g_hc_every = *e == ':' ? strtoull(e + 1, NULL, 10) : 1;
        }
    }
    static uint64_t break_at = UINT64_MAX;
    if (break_at == UINT64_MAX) {
        const char *b = getenv("CIV4_BREAK_AT");
        break_at = b ? strtoull(b, NULL, 10) : UINT64_MAX - 1;
    }
    if (g_import_calls == break_at) rt_watch_armed(0); /* debugger hook */
    static uint64_t peek_at = UINT64_MAX;
    static uint32_t peek_addr;
    if (peek_at == UINT64_MAX) {
        const char *pk = getenv("CIV4_PEEK"); /* addr@call: print a guest qword at that call */
        peek_at = UINT64_MAX - 1;
        if (pk) {
            char *e;
            peek_addr = (uint32_t)strtoul(pk, &e, 16);
            peek_at = strtoull(e + 1, NULL, 10);
        }
    }
    if (g_import_calls == peek_at)
        fprintf(stderr, "[peek] %08x = %016llx at call %llu\n", peek_addr, (unsigned long long)R64_(g_mem, peek_addr),
                (unsigned long long)g_import_calls);
    extern void rt_hw_watch_at_call(uint64_t call);
    rt_hw_watch_at_call(g_import_calls);
    {
        static int sweep_every = -1;
        if (sweep_every < 0) {
            const char *e = getenv("CIV4_REDZONE_SWEEP");
            sweep_every = e ? atoi(e) : 0;
        }
        extern void rt_redzone_sweep(void);
        if (sweep_every && g_import_calls % (uint64_t)sweep_every == 0) rt_redzone_sweep();
    }
    if (g_import_calls >= g_hc_start && (g_import_calls - g_hc_start) % g_hc_every == 0) {
        rt_heap_check();
        if (g_hc_every == 1 || (g_import_calls % 100000) == 0)
            fprintf(stderr, "[heapcheck] ok at call %llu (%s)\n", (unsigned long long)g_import_calls, import_name(id));
    }
    /* CIV4_TRACE=1 traces every import; CIV4_TRACE=_a,_b only those names. */
    static int trace = -1;
    static const char *only;
    if (trace < 0) {
        const char *t = getenv("CIV4_TRACE");
        trace = t != NULL;
        only = t && strcmp(t, "1") ? t : NULL;
    }
    int tr = trace;
    if (tr && only) {
        const char *n = import_name(id), *hit = strstr(only, n);
        size_t len = strlen(n);
        tr = hit && (hit == only || hit[-1] == ',') && (hit[len] == ',' || !hit[len]);
    }
    if (tr) {
        uint32_t sp = cpu->r[4];
        fprintf(stderr, "[trace] %s(%08x, %08x, %08x, %08x) from %08x\n", import_name(id), R32_(g_mem, sp + 4),
                R32_(g_mem, sp + 8), R32_(g_mem, sp + 12), R32_(g_mem, sp + 16), R32_(g_mem, sp));
    }
    {
        typeof(g_fr[0]) *e = &g_fr[g_import_calls % FR_N];
        uint32_t sp = cpu->r[4];
        e->n = g_import_calls;
        e->id = id;
        e->ret_addr = R32_(g_mem, sp);
        for (int k = 0; k < 4; k++) e->a[k] = R32_(g_mem, sp + 4 + 4 * k);
        e->result = 0xdddddddd;
    }
    GuestFn shim = id < g_import_count ? g_import_shims[id]
                 : id - g_import_count < g_dyn_count ? g_dyn_shims[id - g_import_count] : NULL;
    if (!shim) {
        /* Exploration mode: log each missing import once and return 0. */
        static int lenient = -1;
        if (lenient < 0) lenient = getenv("CIV4_LENIENT") != NULL;
        if (lenient && id < g_import_count) {
            static uint8_t seen[8192];
            if (!seen[id]) {
                seen[id] = 1;
                fprintf(stderr, "[rt] lenient: %s -> 0\n", g_import_names[id]);
            }
            cpu->r[0] = cpu->r[2] = 0;
            cpu->r[4] += 4;
            return;
        }
        rt_unimpl(cpu, id, id < g_import_count ? g_import_names[id] : "bad import id");
    }
    shim(cpu);
    cpu->r[4] += 4; /* pop the return address, as the callee's ret would */
    g_fr[g_import_calls % FR_N].result = cpu->r[0];
    if (g_nwatch) {
        /* Changes made by the shim are normally legitimate: resync (and report if asked). */
        static int report_shims = -1;
        if (report_shims < 0) report_shims = getenv("CIV4_WATCH_SHIMS") != NULL;
        for (int i = 0; i < g_nwatch; i++) {
            uint32_t v = R32_(g_mem, g_watch[i]);
            if (report_shims && g_armed && v != g_watch_val[i])
                fprintf(stderr, "[watch] #%d %08x -> %08x by host call %s (call %llu, args %08x %08x %08x)\n", i,
                        g_watch_val[i], v, import_name(id), (unsigned long long)g_import_calls,
                        R32_(g_mem, cpu->r[4]), R32_(g_mem, cpu->r[4] + 4), R32_(g_mem, cpu->r[4] + 8));
            g_watch_val[i] = v;
        }
        g_last_import = id;
        if (!g_armed && cpu->r[0] == g_arm_value && --g_arm_count == 0) {
            g_armed = 1;
            fprintf(stderr, "[rt] watch armed at call %llu:", (unsigned long long)g_import_calls);
            for (int i = 0; i < g_nwatch; i++) fprintf(stderr, " %08x=%08x", g_watch[i], g_watch_val[i]);
            fprintf(stderr, "\n");
            rt_watch_armed(g_arm_value);
        }
    }
    if (tr) fprintf(stderr, "[trace]   -> %08x\n", cpu->r[0]);
}

/* Debug features (watch, trace, heap checks, ...) are all behind this flag so
 * the common path stays short: hundreds of millions of calls go through here. */
static int g_import_debug = -1;

static int import_debug_enabled(void) {
    static const char *const vars[] = {"CIV4_WATCH", "CIV4_HEAPCHECK", "CIV4_BREAK_AT", "CIV4_PEEK", "CIV4_HWWATCH",
                                       "CIV4_REDZONE_SWEEP", "CIV4_TRACE"};
    for (size_t i = 0; i < sizeof vars / sizeof vars[0]; i++)
        if (getenv(vars[i])) return 1;
    return 0;
}

/* A ticker thread sets g_pump_due every 50 ms; the main thread then reads the
 * window queue at its next import or indirect call (plat_pump_busy), so long
 * game frames don't leave the app unresponsive to the window server. */
static volatile int g_pump_due;
static void *pump_ticker(void *unused) {
    (void)unused;
    for (;;) {
        usleep(50000);
        g_pump_due = 1;
    }
    return NULL;
}
static void pump_if_main(void) {
    if (!pthread_main_np()) return;
    g_pump_due = 0;
    plat_pump_busy();
}
void rt_start_pump_ticker(void) {
    pthread_t t;
    pthread_create(&t, NULL, pump_ticker, NULL);
    pthread_detach(t);
}

void rt_import(CPU *cpu, uint32_t id) {
    if (__builtin_expect(g_import_debug != 0, 0)) {
        if (g_import_debug < 0) g_import_debug = import_debug_enabled();
        if (g_import_debug) {
            rt_import_debug(cpu, id);
            return;
        }
    }
    g_import_calls++;
    if (__builtin_expect(g_pump_due, 0)) pump_if_main();
    typeof(g_fr[0]) *e = &g_fr[g_import_calls % FR_N];
    uint32_t sp = cpu->r[4];
    e->n = g_import_calls;
    e->id = id;
    e->ret_addr = R32_(g_mem, sp);
    for (int k = 0; k < 4; k++) e->a[k] = R32_(g_mem, sp + 4 + 4 * k);
    e->result = 0xdddddddd;
    GuestFn shim = id < g_import_count ? g_import_shims[id]
                 : id - g_import_count < g_dyn_count ? g_dyn_shims[id - g_import_count] : NULL;
    if (__builtin_expect(!shim, 0)) {
        /* Exploration mode: log each missing import once and return 0. */
        static int lenient = -1;
        if (lenient < 0) lenient = getenv("CIV4_LENIENT") != NULL;
        if (!lenient || id >= g_import_count) rt_unimpl(cpu, id, id < g_import_count ? g_import_names[id] : "bad import id");
        static uint8_t seen[8192];
        if (!seen[id]) {
            seen[id] = 1;
            fprintf(stderr, "[rt] lenient: %s -> 0\n", g_import_names[id]);
        }
        cpu->r[0] = cpu->r[2] = 0;
        cpu->r[4] += 4;
        return;
    }
    shim(cpu);
    cpu->r[4] += 4; /* pop the return address, as the callee's ret would */
    e->result = cpu->r[0];
}

/* Indirect calls (virtual methods, function pointers) are hot in the AI, so a
 * direct-mapped cache sits in front of the binary search. Each entry packs
 * (guest address << 32 | table index) into one word, so concurrent readers
 * never see a torn entry. Address 0 is never a function, so 0 means empty. */
static _Atomic uint64_t g_lookup_cache[1u << 16];

LiftedFn rt_lookup(uint32_t target) {
    _Atomic uint64_t *slot = &g_lookup_cache[(target * 2654435761u) >> 16];
    uint64_t v = atomic_load_explicit(slot, memory_order_relaxed);
    if ((uint32_t)(v >> 32) == target && target) return g_functab[(uint32_t)v].fn;
    uint32_t lo = 0, hi = g_functab_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_functab[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= g_functab_len || g_functab[lo].addr != target) return NULL;
    atomic_store_explicit(slot, (uint64_t)target << 32 | lo, memory_order_relaxed);
    return g_functab[lo].fn;
}

static const StubEntry *find_stub(uint32_t target) {
    uint32_t lo = 0, hi = g_stubs_len;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_stubs[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    return (lo < g_stubs_len && g_stubs[lo].addr == target) ? &g_stubs[lo] : NULL;
}

/* Where an indirect call lands: a recompiled function (table index), or an
 * import (id | DISPATCH_IMPORT), whether reached directly through the import
 * table or through a dyld stub. Resolutions are cached like rt_lookup's, so
 * calls into imports (libc, libstdc++) skip both binary searches. */
#define DISPATCH_IMPORT 0x80000000u
static _Atomic uint64_t g_dispatch_cache[1u << 16];

static int resolve(uint32_t target, uint32_t *out) {
    for (int depth = 0; depth < 4; depth++) {
        uint32_t lo = 0, hi = g_functab_len;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (g_functab[mid].addr < target) lo = mid + 1;
            else hi = mid;
        }
        if (lo < g_functab_len && g_functab[lo].addr == target) { *out = lo; return 1; }
        if (target >= IMPORT_BASE && target < IMPORT_BASE + 4 * (g_import_count + g_dyn_count) && !(target & 3)) {
            *out = (target - IMPORT_BASE) / 4 | DISPATCH_IMPORT;
            return 1;
        }
        const StubEntry *st = find_stub(target);
        if (!st) return 0;
        if (st->is_import) { *out = st->target | DISPATCH_IMPORT; return 1; }
        target = st->target;
    }
    return 0;
}

static uint32_t dispatch_resolve(CPU *cpu, uint32_t target) {
    _Atomic uint64_t *slot = &g_dispatch_cache[(target * 2654435761u) >> 16];
    uint64_t v = atomic_load_explicit(slot, memory_order_relaxed);
    if ((uint32_t)(v >> 32) == target && target) return (uint32_t)v;
    uint32_t r;
    if (!resolve(target, &r)) rt_unimpl(cpu, target, "dispatch miss");
    atomic_store_explicit(slot, (uint64_t)target << 32 | r, memory_order_relaxed);
    return r;
}

/* Call a lifted function with the register state in cpu->r, and put its
 * results back there (ebx/esi/edi are preserved by the callee). */
void rt_enter(CPU *cpu, LiftedFn fn) {
    uint32_t *r = cpu->r;
    GRet g = fn(cpu, r[0], r[1], r[2], r[3], r[6], r[7], r[4] | (uint64_t)r[5] << 32);
    r[0] = (uint32_t)g.ad;
    r[2] = (uint32_t)(g.ad >> 32);
    r[4] = (uint32_t)g.sb;
    r[5] = (uint32_t)(g.sb >> 32);
}

/* Host code calling guest code (or an import) at `target`, state in cpu->r. */
void rt_dispatch(CPU *cpu, uint32_t target) {
    if (__builtin_expect(g_pump_due, 0)) pump_if_main();
    uint32_t r = dispatch_resolve(cpu, target);
    if (r & DISPATCH_IMPORT) rt_import(cpu, r & ~DISPATCH_IMPORT);
    else rt_enter(cpu, g_functab[r].fn);
}

/* Lifted code calling (or jumping) through a pointer: target in cpu->tgt. */
GRet rt_dispatch_l(LARGS) {
    if (__builtin_expect(g_pump_due, 0)) pump_if_main();
    uint32_t r = dispatch_resolve(cpu, cpu->tgt);
    if (!(r & DISPATCH_IMPORT)) return g_functab[r].fn(cpu, eax, ecx, edx, ebx, esi, edi, spbp_);
    uint32_t esp = (uint32_t)spbp_, ebp = (uint32_t)(spbp_ >> 32);
    SPILL;
    rt_import(cpu, r & ~DISPATCH_IMPORT);
    return gret(cpu->r[0], cpu->r[2], cpu->r[4], cpu->r[5]);
}
