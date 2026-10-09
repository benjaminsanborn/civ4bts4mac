/* Native side of the differential tester, loaded from Python via ctypes.
 *
 * Runs one recompiled guest function on a large private thread stack and
 * turns guest faults (SIGSEGV/SIGBUS inside the arena) and runtime traps
 * into result codes instead of crashing the process.
 */
#include "rt.h"
#include "functab.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int rt_arena_init(void);
int rt_arena_protect(uint32_t addr, uint32_t size, int prot);
extern void (*rt_trap_hook)(CPU *, uint32_t, const char *);

enum { DT_OK = 0, DT_FAULT = 1, DT_TRAP = 2, DT_HOSTFAULT = 3, DT_NOFUNC = 4 };

static __thread sigjmp_buf t_env;
static __thread int t_active;

typedef struct {
    uint32_t fn;
    CPU cpu;
    int code;
    uint32_t fault_addr;
    char msg[256];
} Job;

static __thread Job *t_job;

static void on_signal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    if (!t_active) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    uint8_t *a = si->si_addr;
    if (a >= g_mem && a < g_mem + ((size_t)1 << 32) + (1 << 16)) {
        t_job->code = DT_FAULT;
        t_job->fault_addr = (uint32_t)(a - g_mem);
    } else {
        t_job->code = DT_HOSTFAULT;
        snprintf(t_job->msg, sizeof t_job->msg, "host fault at %p (sig %d)", a, sig);
    }
    siglongjmp(t_env, 1);
}

static void on_trap(CPU *cpu, uint32_t addr, const char *what) {
    (void)cpu;
    t_job->code = DT_TRAP;
    t_job->fault_addr = addr;
    snprintf(t_job->msg, sizeof t_job->msg, "%s", what);
    siglongjmp(t_env, 1);
}

static void *runner(void *arg) {
    Job *job = arg;
    t_job = job;
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    GuestFn fn = rt_lookup(job->fn);
    if (!fn) {
        job->code = DT_NOFUNC;
        return NULL;
    }
    if (sigsetjmp(t_env, 1) == 0) {
        t_active = 1;
        fn(&job->cpu);
        job->code = DT_OK;
    }
    t_active = 0;
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
    return NULL;
}

int dt_init(void) {
    if (rt_arena_init()) return -1;
    rt_trap_hook = on_trap;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    return 0;
}

uint8_t *dt_base(void) { return g_mem; }

int dt_protect(uint32_t addr, uint32_t size, int prot) { return rt_arena_protect(addr, size, prot); }

/* regs: in/out eax..edi. Returns a DT_* code; fault address / trap message out. */
int dt_run(uint32_t fn, uint32_t *regs, uint32_t *fault_addr, char *msg, int msglen) {
    static Job job;
    memset(&job, 0, sizeof job);
    job.fn = fn;
    memcpy(job.cpu.r, regs, 32);
    job.cpu.fcw = 0x37f;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, (size_t)512 << 20);
    pthread_t th;
    pthread_create(&th, &at, runner, &job);
    pthread_join(th, NULL);
    memcpy(regs, job.cpu.r, 32);
    *fault_addr = job.fault_addr;
    snprintf(msg, msglen, "%s", job.msg);
    return job.code;
}

uint32_t dt_functab_len(void) { return g_functab_len; }
