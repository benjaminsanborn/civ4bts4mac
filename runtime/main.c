/* Host entry point: load the statically linked guest world into the arena,
 * run the guest's static initializers, then call the game's main(). */
#include "functab.h"
#include "shim.h"
#include "cf.h"

#include <dlfcn.h>
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

static void load_symbols(void) {
    if (g_sym_n) return;
    const char *path = getenv("CIV4_SYMBOLS");
    FILE *f = fopen(path ? path : "build/gen/symbols.txt", "r");
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

static void on_trap(CPU *cpu, uint32_t addr, const char *what) {
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

static void on_crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *a = si->si_addr;
    if (a >= g_mem && a < g_mem + ((size_t)1 << 32))
        fprintf(stderr, "\n[rt] CRASH: signal %d, guest address %08x\n", sig, (uint32_t)(a - g_mem));
    else
        fprintf(stderr, "\n[rt] CRASH: signal %d, host address %p\n", sig, (void *)a);
    backtrace_host();
    _exit(71);
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
}

_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\n[rt] FATAL: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
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

extern const ShimDef shims_libc[], shims_stdio[], shims_pthread[], shims_cf[], shims_files[], shims_posix[], shims_carbon[], shims_dyld[];

static void register_all_shims(void) {
    rt_register_shims(shims_libc);
    rt_register_shims(shims_stdio);
    rt_register_shims(shims_pthread);
    rt_register_shims(shims_cf);
    rt_register_shims(shims_files);
    rt_register_shims(shims_posix);
    rt_register_shims(shims_carbon);
    rt_register_shims(shims_dyld);
    uint32_t have = 0;
    for (uint32_t i = 0; i < g_import_count; i++) have += g_import_shims[i] != NULL;
    rt_log("%u/%u host imports implemented", have, g_import_count);
}

void rt_libc_init(CPU *cpu, int argc, char **argv);
void rt_cf_init(void);

int main(int argc, char **argv) {
    const char *world = getenv("CIV4_WORLD");
    /* The guest's Python standard library (the old system Python 2.6 is gone). */
    if (!getenv("PYTHONHOME")) {
        char home[PATH_MAX];
        if (realpath("build/pylib", home)) setenv("PYTHONHOME", home, 1);
    }
    if (rt_arena_init()) rt_fatal("cannot reserve guest arena");
    rt_trap_hook = on_trap;
    install_crash_handler();
    load_world(world ? world : "build/gen/world.img");
    if (rt_heap_init()) rt_fatal("cannot create guest heap");
    if (mprotect(g_mem + STACK_TOP - STACK_SIZE, STACK_SIZE, PROT_READ | PROT_WRITE)) rt_fatal("stack");
    register_all_shims();

    static CPU cpu;
    t_cpu = &cpu;
    cpu.fcw = 0x37f;
    cpu.r[4] = STACK_TOP - 64;
    rt_libc_init(&cpu, argc, argv);
    rt_cf_init();

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
