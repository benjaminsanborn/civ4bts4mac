/* Carbon shims: Multiprocessing Services, Gestalt, timing, process manager,
 * memory manager. (Windowing, events and QuickTime live elsewhere.) */
#include "cf.h"

#include <mach/mach_time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define noErr 0
#define paramErr (-50)
#define gestaltUndefSelectorErr (-5551)

/* ---- Multiprocessing Services: task storage = thread-specific data ---- */
#define MAX_TLS 256
static uint32_t g_tls_used[MAX_TLS];
static __thread uint32_t t_tls[MAX_TLS];

SHIM(_MPAllocateTaskStorageIndex) {
    for (uint32_t i = 1; i < MAX_TLS; i++) {
        uint32_t z = 0;
        if (__atomic_compare_exchange_n(&g_tls_used[i], &z, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            W32_(g_mem, ARG(0), i);
            RET(noErr);
            return;
        }
    }
    RET(-29296); /* kMPInsufficientResourcesErr */
}
SHIM(_MPDeallocateTaskStorageIndex) { if (ARG(0) < MAX_TLS) g_tls_used[ARG(0)] = 0; RET(noErr); }
SHIM(_MPSetTaskStorageValue) { if (ARG(0) >= MAX_TLS) { RET(paramErr); return; } t_tls[ARG(0)] = ARG(1); RET(noErr); }
SHIM(_MPGetTaskStorageValue) { RET(ARG(0) < MAX_TLS ? t_tls[ARG(0)] : 0); }
SHIM(_MPProcessors) {
    int n = 1;
    size_t len = sizeof n;
    sysctlbyname("hw.ncpu", &n, &len, NULL, 0);
    RET(n);
}
SHIM(_MPAllocateAligned) {
    /* (size, alignment log2, options) */
    uint32_t align = 1u << (ARG(1) & 31);
    uint32_t p = gmemalign(align < 16 ? 16 : align, ARG(0));
    if (p && (ARG(2) & 1 /* kMPAllocateClearMask */)) memset(PTR(p), 0, ARG(0));
    RET(p);
}
SHIM(_MPFree) { gfree(ARG(0)); }

/* ---- Memory Manager (pointers only; handles are rare) ---- */
SHIM(_NewPtrClear) { RET(gcalloc(1, ARG(0))); }
SHIM(_DisposePtr) { gfree(ARG(0)); }

/* ---- Gestalt ---- */
SHIM(_Gestalt) {
    uint32_t sel = ARG(0), out = ARG(1), v;
    switch (sel) {
    case 'sysv': v = 0x1069; break;           /* report 10.6.9-ish: the newest the game knows */
    case 'sys1': v = 10; break;
    case 'sys2': v = 6; break;
    case 'sys3': v = 8; break;
    case 'ram ': v = 0x7fffffff; break;
    case 'cput': v = 0x0108; break;           /* gestaltCPUPentium4-class x86 */
    case 'sysa': v = 10; break;               /* gestaltIntel */
    case 'pclk': case 'bclk': v = 2000000000u; break;
    case 'mcpu': v = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN); break;
    case 'qtim': v = 0x07600000; break;       /* QuickTime 7.6 */
    default:
        rt_log("Gestalt: unknown selector '%c%c%c%c'", sel >> 24, sel >> 16 & 255, sel >> 8 & 255, sel & 255);
        RET(gestaltUndefSelectorErr);
        return;
    }
    if (out) W32_(g_mem, out, v);
    RET(noErr);
}

/* ---- timing ---- */
static uint64_t now_us(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}
SHIM(_Microseconds) {
    /* UnsignedWide on little-endian targets is {lo, hi}. */
    W64_(g_mem, ARG(0), now_us());
}
SHIM(_TickCount) { RET((uint32_t)(now_us() * 60 / 1000000)); }
SHIM(_GetDblTime) { RET(30); }
SHIM(_GetDateTime) {
    /* Seconds since 1904-01-01 */
    W32_(g_mem, ARG(0), (uint32_t)(time(NULL) + 2082844800u));
}

/* ---- Process Manager ---- */
SHIM(_GetCurrentProcess) {
    /* ProcessSerialNumber {hi, lo}: kCurrentProcess = {0, 2} */
    W32_(g_mem, ARG(0), 0);
    W32_(g_mem, ARG(0) + 4, 2);
    RET(noErr);
}
SHIM(_GetProcessPID) { W32_(g_mem, ARG(1), (uint32_t)getpid()); RET(noErr); }
SHIM(_CSCopyUserName) {
    const char *u = getenv("USER");
    RETCF_OWNED(CFStringCreateWithCString(NULL, u ? u : "Player", kCFStringEncodingUTF8));
}

/* ---- Debugger: log instead of trapping ---- */
SHIM(_Debugger) { rt_log("guest called Debugger()"); }

const ShimDef shims_carbon[] = {
    SHIM_ENTRY(_MPAllocateTaskStorageIndex), SHIM_ENTRY(_MPDeallocateTaskStorageIndex),
    SHIM_ENTRY(_MPSetTaskStorageValue), SHIM_ENTRY(_MPGetTaskStorageValue), SHIM_ENTRY(_MPProcessors),
    SHIM_ENTRY(_MPAllocateAligned), SHIM_ENTRY(_MPFree), SHIM_ENTRY(_NewPtrClear), SHIM_ENTRY(_DisposePtr),
    SHIM_ENTRY(_Gestalt), SHIM_ENTRY(_Microseconds), SHIM_ENTRY(_TickCount), SHIM_ENTRY(_GetDblTime),
    SHIM_ENTRY(_GetDateTime), SHIM_ENTRY(_GetCurrentProcess), SHIM_ENTRY(_GetProcessPID),
    SHIM_ENTRY(_CSCopyUserName), SHIM_ENTRY(_Debugger),
    {0, 0},
};
