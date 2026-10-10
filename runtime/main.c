/* Host entry point: load the statically linked guest world into the arena,
 * run the guest's static initializers, then call the game's main(). */
#include "functab.h"
#include "shim.h"
#include "cf.h"

#include <dlfcn.h>
#include <sys/ucontext.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int rt_arena_init(void);
int rt_heap_init(void);
extern GuestFn g_import_shims[];
extern void (*rt_trap_hook)(CPU *, uint32_t, const char *);

#define PAGE 0x4000u
#define STACK_TOP 0xF0000000u
#define STACK_SIZE (16u << 20)
#define CALL_RET_MAGIC 0xFFFF0000u

static __thread CPU *t_cpu;
CPU *rt_cpu(void) { return t_cpu; }
void rt_set_cpu(CPU *cpu) { t_cpu = cpu; }

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[rt] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- symbolization for crash reports ---- */
static uint32_t *g_sym_addr;
static char **g_sym_name;
static size_t g_sym_n;

/* Absolute path of a file in the build directory (the game chdirs away). */
static char g_build_dir[PATH_MAX];
const char *rt_build_path(const char *rel, char *buf, size_t n) {
    snprintf(buf, n, "%s/%s", g_build_dir, rel);
    return buf;
}

static void load_symbols(void) {
    if (g_sym_n) return;
    char pbuf[PATH_MAX];
    const char *path = getenv("CIV4_SYMBOLS");
    FILE *f = fopen(path ? path : rt_build_path("gen/symbols.txt", pbuf, sizeof pbuf), "r");
    if (!f) return;
    size_t cap = 1 << 17;
    g_sym_addr = malloc(cap * sizeof *g_sym_addr);
    g_sym_name = malloc(cap * sizeof *g_sym_name);
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        unsigned a;
        char name[4000];
        if (sscanf(line, "%x %3999s", &a, name) != 2) continue;
        if (g_sym_n == cap) {
            cap *= 2;
            g_sym_addr = realloc(g_sym_addr, cap * sizeof *g_sym_addr);
            g_sym_name = realloc(g_sym_name, cap * sizeof *g_sym_name);
        }
        g_sym_addr[g_sym_n] = a;
        g_sym_name[g_sym_n++] = strdup(name);
    }
    fclose(f);
}

static const char *symbolize(uint32_t a, uint32_t *off) {
    load_symbols();
    size_t lo = 0, hi = g_sym_n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_sym_addr[mid] <= a) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return "?";
    *off = a - g_sym_addr[lo - 1];
    return g_sym_name[lo - 1];
}

static void backtrace_host(void);

static void backtrace_guest(CPU *cpu) {
    uint32_t ebp = cpu->r[5], off = 0;
    /* The return address at esp is the caller of the trapping import. */
    uint32_t ret = R32_(g_mem, cpu->r[4]);
    fprintf(stderr, "  guest backtrace:\n");
    if (ret != CALL_RET_MAGIC) fprintf(stderr, "    %08x %s+%u\n", ret, symbolize(ret, &off), off);
    for (int i = 0; i < 40 && ebp && ebp < 0xFFFFFFF0u; i++) {
        uint32_t r = R32_(g_mem, ebp + 4);
        if (!r || r == CALL_RET_MAGIC) break;
        fprintf(stderr, "    %08x %s+%u\n", r, symbolize(r, &off), off);
        uint32_t next = R32_(g_mem, ebp);
        if (next <= ebp) break;
        ebp = next;
    }
}

void rt_report_watch(CPU *cpu, int i, uint32_t old, uint32_t now, uint32_t last_import) {
    extern const char *const g_import_names[];
    fprintf(stderr, "\n[rt] WATCH #%d: %08x -> %08x (since host call %s)\n", i, old, now,
            last_import < g_import_count ? g_import_names[last_import] : "?");
    backtrace_guest(cpu);
}

void rt_dump_recent_calls(void);
static void on_trap(CPU *cpu, uint32_t addr, const char *what) {
    rt_dump_recent_calls();
    fprintf(stderr, "\n[rt] TRAP: %s (0x%08x)\n", what, addr);
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7]);
    backtrace_guest(cpu);
    backtrace_host();
    exit(70);
}

/* Host stack walk: recompiled frames are named f_<guest address>. */
static void backtrace_host(void) {
    void *frames[128];
    int n = backtrace(frames, 128);
    fprintf(stderr, "  host backtrace (recompiled frames symbolized):\n");
    for (int i = 0; i < n; i++) {
        Dl_info di;
        if (!dladdr(frames[i], &di) || !di.dli_sname) {
            fprintf(stderr, "    %p\n", frames[i]);
            continue;
        }
        unsigned ga;
        if (sscanf(di.dli_sname, "f_%8x", &ga) == 1) {
            uint32_t off = 0;
            const char *name = symbolize(ga, &off);
            fprintf(stderr, "    %08x %s+%u\n", ga, name, off);
        } else {
            fprintf(stderr, "    %s+%ld\n", di.dli_sname, (long)((char *)frames[i] - (char *)di.dli_saddr));
        }
    }
}
void rt_backtrace_host(void) { backtrace_host(); }

void rt_dump_recent_calls(void);
static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    rt_dump_recent_calls();
    extern uint64_t g_import_calls;
    fprintf(stderr, "\n[rt] host call count at crash: %llu\n", (unsigned long long)g_import_calls);
    if (t_cpu)
        fprintf(stderr, "[rt] guest esp %08x args %08x %08x %08x\n", t_cpu->r[4], R32_(g_mem, t_cpu->r[4] + 4),
                R32_(g_mem, t_cpu->r[4] + 8), R32_(g_mem, t_cpu->r[4] + 12));
    uint8_t *a = si->si_addr;
    if (uc)
        fprintf(stderr, "[rt] faulting host pc %llx (slide %lx) -- tools/guestpc.sh maps it to x86\n",
                (unsigned long long)((ucontext_t *)uc)->uc_mcontext->__ss.__pc, (long)_dyld_get_image_vmaddr_slide(0));
    if (t_cpu)
        fprintf(stderr, "[rt] last spilled guest regs: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
                t_cpu->r[0], t_cpu->r[1], t_cpu->r[2], t_cpu->r[3], t_cpu->r[4], t_cpu->r[5], t_cpu->r[6], t_cpu->r[7]);
    if (t_cpu && getenv("CIV4_CRASH_DEREF")) {
        /* Debug aid: dump the object passed as the first stack argument, and the
         * table it points to at the given offset (CIV4_CRASH_DEREF=offset). */
        uint32_t obj = R32_(g_mem, t_cpu->r[4] + 4), off = (uint32_t)strtoul(getenv("CIV4_CRASH_DEREF"), NULL, 16);
        uint32_t tab = R32_(g_mem, obj + off);
        fprintf(stderr, "[rt] obj %08x:", obj);
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", R32_(g_mem, obj + 4 * i));
        fprintf(stderr, "\n[rt] table %08x:", tab);
        for (int i = 0; i < 20; i++) fprintf(stderr, " %08x", R32_(g_mem, tab + 4 * i));
        fprintf(stderr, "\n");
    }
    if (a >= g_mem && a < g_mem + ((size_t)1 << 32))
        fprintf(stderr, "\n[rt] CRASH: signal %d, guest address %08x\n", sig, (uint32_t)(a - g_mem));
    else
        fprintf(stderr, "\n[rt] CRASH: signal %d, host address %p\n", sig, (void *)a);
    backtrace_host();
    _exit(71);
}

/* ---- in-process hardware watchpoints (ARM64 debug registers) ----
 * CIV4_HWWATCH=AAAAAAAA@NNNNNNNNNNN: at host call N, watch stores to guest
 * address A (8 bytes). Each hit is reported and the store is allowed to proceed. */
#include <mach/mach.h>
static uint64_t g_hw_addr;
static int g_hw_hits, g_hw_rearm;

static void hw_set(int enable) {
    arm_debug_state64_t ds;
    memset(&ds, 0, sizeof ds);
    if (enable) {
        ds.__wvr[0] = g_hw_addr & ~7ull;
        ds.__wcr[0] = (0xFFu << 5) | (2u << 3) | (2u << 1) | 1u; /* 8 bytes, stores, EL0, enabled */
    }
    kern_return_t kr = thread_set_state(mach_thread_self(), ARM_DEBUG_STATE64, (thread_state_t)&ds,
                                        ARM_DEBUG_STATE64_COUNT);
    if (kr != KERN_SUCCESS) fprintf(stderr, "[rt] hw watch: thread_set_state failed (%d)\n", kr);
}

void rt_hw_watch_at_call(uint64_t call) {
    static uint32_t addr;
    static uint64_t at = UINT64_MAX;
    if (at == UINT64_MAX) {
        const char *w = getenv("CIV4_HWWATCH");
        at = UINT64_MAX - 1;
        if (w) {
            char *e;
            addr = (uint32_t)strtoul(w, &e, 16);
            at = *e == '@' ? strtoull(e + 1, NULL, 10) : 0;
        }
    }
    if (g_hw_rearm) {
        g_hw_rearm = 0;
        hw_set(1);
    }
    if (call == at) {
        g_hw_addr = (uint64_t)(g_mem + addr);
        hw_set(1);
        fprintf(stderr, "[rt] hw watch armed on %08x at call %llu\n", addr, (unsigned long long)call);
    }
}

static void on_trap_signal(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    Dl_info di;
    /* Ignore the allocator's own bookkeeping writes (static dlmalloc helpers
     * resolve to the nearest exported mspace_* symbol). */
    if (getenv("CIV4_HWWATCH_SKIP_HEAP") && dladdr((void *)pc, &di) && di.dli_sname &&
        (!strncmp(di.dli_sname, "mspace_", 7) || !strncmp(di.dli_sname, "create_mspace", 13))) {
        hw_set(0);
        g_hw_rearm = 1;
        return;
    }
    extern uint64_t g_import_calls;
    fprintf(stderr, "\n[rt] HW WATCH hit #%d at call %llu, host pc %llx", ++g_hw_hits, (unsigned long long)g_import_calls,
            (unsigned long long)pc);
    if (dladdr((void *)pc, &di) && di.dli_sname)
        fprintf(stderr, " (%s+%ld)", di.dli_sname, (long)(pc - (uint64_t)di.dli_saddr));
    fprintf(stderr, "; value before store: %016llx\n", *(unsigned long long *)(g_hw_addr & ~7ull));
    {
        void *frames[24];
        int n = backtrace(frames, 24);
        fprintf(stderr, "  raw pcs (slide %lx): %llx", (long)_dyld_get_image_vmaddr_slide(0), (unsigned long long)pc);
        for (int i = 0; i < n; i++) fprintf(stderr, " %p", frames[i]);
        fprintf(stderr, "\n");
    }
    if (t_cpu)
        fprintf(stderr, "  guest esp %08x ebp %08x args %08x %08x %08x\n", t_cpu->r[4], t_cpu->r[5],
                R32_(g_mem, t_cpu->r[4] + 4), R32_(g_mem, t_cpu->r[4] + 8), R32_(g_mem, t_cpu->r[4] + 12));
    backtrace_host();
    /* Let the store happen; re-arm at the next host call. */
    hw_set(0);
    g_hw_rearm = g_hw_hits < 12;
}

static void install_crash_handler(void) {
    static uint8_t altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    struct sigaction st;
    memset(&st, 0, sizeof st);
    st.sa_sigaction = on_trap_signal;
    st.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGTRAP, &st, NULL);
}

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n[rt] FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    rt_dump_recent_calls();
    if (t_cpu) backtrace_guest(t_cpu);
    backtrace_host();
    exit(70);
}

/* ---- shims ---- */
#define MAX_TABLES 64
static const ShimDef *g_tables[MAX_TABLES];
static int g_ntables;
extern GuestFn g_dyn_shims[];
extern const char *g_dyn_names[];
extern uint32_t g_dyn_count;

void rt_register_shims(const ShimDef *defs) {
    if (g_ntables < MAX_TABLES) g_tables[g_ntables++] = defs;
    for (; defs->name; defs++) {
        for (uint32_t i = 0; i < g_import_count; i++) {
            if (!strcmp(g_import_names[i], defs->name)) g_import_shims[i] = defs->fn;
        }
    }
}

uint32_t rt_dynamic_import(const char *name) {
    for (uint32_t i = 0; i < g_import_count; i++)
        if (!strcmp(g_import_names[i], name)) return IMPORT_BASE + 4 * i;
    for (uint32_t i = 0; i < g_dyn_count; i++)
        if (!strcmp(g_dyn_names[i], name)) return IMPORT_BASE + 4 * (g_import_count + i);
    for (int t = 0; t < g_ntables; t++) {
        for (const ShimDef *d = g_tables[t]; d->name; d++) {
            if (!strcmp(d->name, name)) {
                g_dyn_shims[g_dyn_count] = d->fn;
                g_dyn_names[g_dyn_count] = d->name;
                return IMPORT_BASE + 4 * (g_import_count + g_dyn_count++);
            }
        }
    }
    return 0;
}

uint32_t rt_hostdata(const char *name) {
    for (uint32_t i = 0; i < g_hostdata_count; i++)
        if (!strcmp(g_hostdata[i].name, name)) return g_hostdata[i].addr;
    return 0;
}

uint32_t rt_call(CPU *cpu, uint32_t fn, int nargs, ...) {
    va_list ap;
    va_start(ap, nargs);
    uint32_t args[8];
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t saved[8];
    memcpy(saved, cpu->r, sizeof saved);
    uint32_t esp = (cpu->r[4] - 4 * nargs) & ~15u;
    for (int i = 0; i < nargs; i++) W32_(g_mem, esp + 4 * i, args[i]);
    esp -= 4;
    W32_(g_mem, esp, CALL_RET_MAGIC);
    cpu->r[4] = esp;
    rt_dispatch(cpu, fn);
    uint32_t ret = cpu->r[0];
    /* Callee-saved registers are the callee's business; restore the rest of
     * the caller-visible state so the shim's guest frame is untouched. */
    uint32_t edx = cpu->r[2];
    memcpy(cpu->r, saved, sizeof saved);
    cpu->r[2] = edx;
    return ret;
}

/* ---- loading ---- */
static void load_world(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) rt_fatal("cannot open %s: %s", path, strerror(errno));
    char magic[4];
    uint32_t count;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "C4WD", 4) || fread(&count, 4, 1, f) != 1)
        rt_fatal("%s: bad world image", path);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t hdr[3];
        if (fread(hdr, 4, 3, f) != 3) rt_fatal("%s: truncated", path);
        uint32_t addr = hdr[0], size = hdr[1];
        uint32_t lo = addr & ~(PAGE - 1), hi = (addr + size + PAGE - 1) & ~(PAGE - 1);
        /* Everything RW for now: pages are shared between segments at 16K granularity. */
        if (mprotect(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE)) rt_fatal("mprotect %08x", lo);
        if (fread(g_mem + addr, 1, size, f) != size) rt_fatal("%s: truncated region", path);
    }
    fclose(f);
}

extern const ShimDef shims_libc[], shims_stdio[], shims_pthread[], shims_cf[], shims_files[], shims_posix[], shims_carbon[], shims_dyld[], shims_events[],
    shims_platform[], shims_agl[], shims_gl_gen[], shims_gl_special[], shims_qd[], shims_cf_xml[], shims_iokit[], shims_quickdraw[], shims_movies[], shims_unwind[], shims_openal[], shims_net[], shims_cg_image[], shims_sound_converter[], shims_threads_carbon[];

static void register_all_shims(void) {
    rt_register_shims(shims_libc);
    rt_register_shims(shims_stdio);
    rt_register_shims(shims_pthread);
    rt_register_shims(shims_cf);
    rt_register_shims(shims_files);
    rt_register_shims(shims_posix);
    rt_register_shims(shims_carbon);
    rt_register_shims(shims_dyld);
    rt_register_shims(shims_events);
    rt_register_shims(shims_platform);
    rt_register_shims(shims_agl);
    rt_register_shims(shims_gl_gen);
    rt_register_shims(shims_gl_special);
    rt_register_shims(shims_qd);
    rt_register_shims(shims_cf_xml);
    rt_register_shims(shims_iokit);
    rt_register_shims(shims_quickdraw);
    rt_register_shims(shims_movies);
    rt_register_shims(shims_openal);
    rt_register_shims(shims_net);
    rt_register_shims(shims_cg_image);
    rt_register_shims(shims_sound_converter);
    rt_register_shims(shims_threads_carbon);
    rt_register_shims(shims_unwind);
    uint32_t have = 0;
    for (uint32_t i = 0; i < g_import_count; i++) have += g_import_shims[i] != NULL;
    rt_log("%u/%u host imports implemented", have, g_import_count);
    if (getenv("CIV4_LIST_MISSING"))
        for (uint32_t i = 0; i < g_import_count; i++)
            if (!g_import_shims[i]) rt_log("missing import %s", g_import_names[i]);
}

void rt_libc_init(CPU *cpu, int argc, char **argv);
void rt_cf_init(void);
void rt_cg_image_init(void);

int main(int argc, char **argv) {
    const char *world = getenv("CIV4_WORLD");
    /* Everything we load lives next to this executable (build/). */
    {
        char exe[PATH_MAX], real[PATH_MAX];
        uint32_t n = sizeof exe;
        if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) rt_fatal("cannot locate executable");
        *strrchr(real, '/') = 0;
        strlcpy(g_build_dir, real, sizeof g_build_dir);
    }
    char pbuf[PATH_MAX];
    /* Debug output directories are relative to where we were started (the game chdirs). */
    if (getenv("CIV4_FRAMES") && getenv("CIV4_FRAMES")[0] != '/') {
        char cwd[PATH_MAX], abs[PATH_MAX];
        snprintf(abs, sizeof abs, "%s/%s", getcwd(cwd, sizeof cwd), getenv("CIV4_FRAMES"));
        setenv("CIV4_FRAMES", abs, 1);
    }
    /* The guest's Python standard library (the old system Python 2.6 is gone). */
    if (!getenv("PYTHONHOME")) setenv("PYTHONHOME", rt_build_path("pylib", pbuf, sizeof pbuf), 1);
    if (!getenv("CIV4_APP")) {
        char app[PATH_MAX];
        if (realpath(rt_build_path("game/Beyond the Sword/Civilization IV Beyond the Sword.app", pbuf, sizeof pbuf), app))
            setenv("CIV4_APP", app, 1);
    }
    if (rt_arena_init()) rt_fatal("cannot reserve guest arena");
    rt_trap_hook = on_trap;
    install_crash_handler();
    load_world(world ? world : rt_build_path("gen/world.img", pbuf, sizeof pbuf));
    if (rt_heap_init()) rt_fatal("cannot create guest heap");
    if (mprotect(g_mem + STACK_TOP - STACK_SIZE, STACK_SIZE, PROT_READ | PROT_WRITE)) rt_fatal("stack");
    register_all_shims();

    static CPU cpu;
    t_cpu = &cpu;
    cpu.fcw = 0x37f;
    cpu.r[4] = STACK_TOP - 64;
    rt_libc_init(&cpu, argc, argv);
    rt_cf_init();
    rt_cg_image_init();

    /* Guest argv/envp. */
    uint32_t gargv = gcalloc(argc + 1, 4);
    for (int i = 0; i < argc; i++) W32_(g_mem, gargv + 4 * i, gstrdup(argv[i]));
    uint32_t genvp = gcalloc(1, 4);
    uint32_t gapple = gcalloc(1, 4);

    rt_log("running %u static initializers", g_init_count);
    for (uint32_t i = 0; i < g_init_count; i++) rt_call(&cpu, g_init_funcs[i], 4, argc, gargv, genvp, gapple);
    rt_log("calling main at %08x", g_main);
    uint32_t rc = rt_call(&cpu, g_main, 3, argc, gargv, genvp);
    rt_log("main returned %u", rc);
    return (int)rc;
}
