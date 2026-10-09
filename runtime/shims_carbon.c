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
    case 'ram ': case 'lram': v = 0x7fffffff; break;
    case 'mclk': v = 2000; break;             /* gestaltProcClkSpeedMHz */
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
SHIM(_GetProcessInformation) {
    /* ProcessInfoRec (2-byte packed): length 0, name 4, psn 8, type 16, sig 20, mode 24,
     * location 28, size 32, freeMem 36, launcher 40, launchDate 48, activeTime 52, appSpec 56 */
    uint32_t r = ARG(1);
    if (R32_(g_mem, r + 4)) { /* processName: Pascal string buffer */
        const char *n = "Civilization IV Beyond the Sword";
        W8_(g_mem, R32_(g_mem, r + 4), (uint32_t)strlen(n));
        memcpy(PTR(R32_(g_mem, r + 4) + 1), n, strlen(n));
    }
    W32_(g_mem, r + 8, 0);
    W32_(g_mem, r + 12, 2);
    W32_(g_mem, r + 16, 'APPL');
    W32_(g_mem, r + 20, 'CIV4');
    W32_(g_mem, r + 24, 0);
    W32_(g_mem, r + 32, 0x7fffffff);
    W32_(g_mem, r + 36, 0x40000000);
    RET(noErr);
}
SHIM(_PBHGetVInfoSync) {
    /* HVolumeParam: ioResult 16, ioVAlBlkSiz 48, ioVFrBlk 62 (16-bit). Report lots of space. */
    uint32_t pb = ARG(0);
    W16_(g_mem, pb + 16, 0);
    W32_(g_mem, pb + 48, 0x10000);
    W16_(g_mem, pb + 62, 0xffff);
    W16_(g_mem, pb + 46, 0xffff);
    RET(noErr);
}
SHIM(_GetProcessPID) { W32_(g_mem, ARG(1), (uint32_t)getpid()); RET(noErr); }
SHIM(_CSCopyUserName) {
    const char *u = getenv("USER");
    RETCF_OWNED(CFStringCreateWithCString(NULL, u ? u : "Player", kCFStringEncodingUTF8));
}

/* ---- Thread Manager / toolbox init ---- */
SHIM(_GetCurrentThread) { W32_(g_mem, ARG(0), 1); RET(noErr); }
SHIM(_SetThreadTerminator) { RET(noErr); }
SHIM(_GetThreadState) { if (ARG(1)) W16_(g_mem, ARG(1), 0 /* kRunningThreadState */); RET(noErr); }
SHIM(_FlushEvents) {}
SHIM(_InitCursor) {}
SHIM(_SetQDGlobalsRandomSeed) {}

/* ---- keyboard state (KeyMap: 128 bits); filled by the event layer ---- */
uint8_t g_keymap[16];
SHIM(_GetKeys) { memcpy(PTR(ARG(0)), g_keymap, 16); }

/* ---- Apple Events: accepted, never delivered ---- */
SHIM(_AEInstallEventHandler) { RET(noErr); }

/* ---- Interface Builder nibs: not available; callers treat failure as "no nib" ---- */
SHIM(_CreateNibReferenceWithCFBundle) { W32_(g_mem, ARG(2), 0); RET(-43); }
SHIM(_CreateNibReference) { W32_(g_mem, ARG(1), 0); RET(-43); }

/* ---- QuickTime presence (the game checks for it; movies are handled separately) ---- */
SHIM(_EnterMovies) { RET(noErr); }
/* Pretend every requested component exists; the game only checks for presence here. */
SHIM(_FindNextComponent) { RET(1); }

/* ---- Memory Manager handles (movies are in movies.m) ---- */
SHIM(_DisposeHandle) { if (ARG(0)) { gfree(R32_(g_mem, ARG(0))); gfree(ARG(0)); } }

/* ---- Debugger: log instead of trapping ---- */
SHIM(_Debugger) { rt_log("guest called Debugger()"); }

const ShimDef shims_carbon[] = {
    SHIM_ENTRY(_MPAllocateTaskStorageIndex), SHIM_ENTRY(_MPDeallocateTaskStorageIndex),
    SHIM_ENTRY(_MPSetTaskStorageValue), SHIM_ENTRY(_MPGetTaskStorageValue), SHIM_ENTRY(_MPProcessors),
    SHIM_ENTRY(_MPAllocateAligned), SHIM_ENTRY(_MPFree), SHIM_ENTRY(_NewPtrClear), SHIM_ENTRY(_DisposePtr),
    SHIM_ENTRY(_Gestalt), SHIM_ENTRY(_Microseconds), SHIM_ENTRY(_TickCount), SHIM_ENTRY(_GetDblTime),
    SHIM_ENTRY(_GetDateTime), SHIM_ENTRY(_GetCurrentProcess), SHIM_ENTRY(_GetProcessPID), SHIM_ENTRY(_GetProcessInformation), SHIM_ENTRY(_PBHGetVInfoSync),
    SHIM_ENTRY(_CSCopyUserName), SHIM_ENTRY(_Debugger), SHIM_ENTRY(_GetCurrentThread), SHIM_ENTRY(_FlushEvents),
    SHIM_ENTRY(_SetThreadTerminator), SHIM_ENTRY(_GetThreadState),
    SHIM_ENTRY(_InitCursor), SHIM_ENTRY(_SetQDGlobalsRandomSeed), SHIM_ENTRY(_EnterMovies),
    SHIM_ENTRY(_FindNextComponent), SHIM_ENTRY(_DisposeHandle), SHIM_ENTRY(_CreateNibReferenceWithCFBundle), SHIM_ENTRY(_CreateNibReference), SHIM_ENTRY(_GetKeys),
    SHIM_ENTRY(_AEInstallEventHandler),
    {0, 0},
};
