/* libc shims: memory, strings, wide strings, conversions, math, ctype, time,
 * process, 64-bit arithmetic helpers. stdio and pthreads live in their own files. */
#include "shim.h"

#include <ctype.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <runetype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* ---- errno: one guest int per host thread ---- */
static __thread uint32_t t_errno_cell;

uint32_t rt_errno_addr(void) {
    if (!t_errno_cell) t_errno_cell = gcalloc(1, 4);
    return t_errno_cell;
}
void rt_set_errno(int e) { W32_(g_mem, rt_errno_addr(), (uint32_t)e); }

SHIM(___error) { RET(rt_errno_addr()); }

/* ---- memory ---- */
SHIM(_malloc) { RET(gmalloc(ARG(0))); }
SHIM(_calloc) { RET(gcalloc(ARG(0), ARG(1))); }
SHIM(_realloc) { RET(grealloc(ARG(0), ARG(1))); }
SHIM(_free) { gfree(ARG(0)); }
SHIM(_memcpy) { memcpy(PTR(ARG(0)), PTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_memmove) { memmove(PTR(ARG(0)), PTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_memset) { memset(PTR(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
SHIM(___memcpy_chk) { memcpy(PTR(ARG(0)), PTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(___memmove_chk) { memmove(PTR(ARG(0)), PTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(___memset_chk) { memset(PTR(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
SHIM(___bzero) { memset(PTR(ARG(0)), 0, ARG(1)); }
SHIM(_memcmp) { RET(memcmp(PTR(ARG(0)), PTR(ARG(1)), ARG(2))); }
SHIM(_memchr) { RET(GADDR(memchr(PTR(ARG(0)), (int)ARG(1), ARG(2)))); }

/* ---- narrow strings ---- */
SHIM(_strlen) { RET(strlen(CPTR(ARG(0)))); }
SHIM(_strcmp) { RET(strcmp(CPTR(ARG(0)), CPTR(ARG(1)))); }
SHIM(_strncmp) { RET(strncmp(CPTR(ARG(0)), CPTR(ARG(1)), ARG(2))); }
SHIM(_strcasecmp) { RET(strcasecmp(CPTR(ARG(0)), CPTR(ARG(1)))); }
SHIM(_strcoll) { RET(strcoll(CPTR(ARG(0)), CPTR(ARG(1)))); }
SHIM(_strcpy) { strcpy(PTR(ARG(0)), CPTR(ARG(1))); RET(ARG(0)); }
SHIM(___strcpy_chk) { strcpy(PTR(ARG(0)), CPTR(ARG(1))); RET(ARG(0)); }
SHIM(_strncpy) { strncpy(PTR(ARG(0)), CPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_strlcpy) { RET(strlcpy(PTR(ARG(0)), CPTR(ARG(1)), ARG(2))); }
SHIM(_strcat) { strcat(PTR(ARG(0)), CPTR(ARG(1))); RET(ARG(0)); }
SHIM(___strcat_chk) { strcat(PTR(ARG(0)), CPTR(ARG(1))); RET(ARG(0)); }
SHIM(_strncat) { strncat(PTR(ARG(0)), CPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(___strncat_chk) { strncat(PTR(ARG(0)), CPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_strchr) { RET(GADDR(strchr(CPTR(ARG(0)), (int)ARG(1)))); }
SHIM(_strrchr) { RET(GADDR(strrchr(CPTR(ARG(0)), (int)ARG(1)))); }
SHIM(_strstr) { RET(GADDR(strstr(CPTR(ARG(0)), CPTR(ARG(1))))); }
SHIM(_strcspn) { RET(strcspn(CPTR(ARG(0)), CPTR(ARG(1)))); }
SHIM(_strdup) { RET(gstrdup(CPTR(ARG(0)))); }
SHIM(_strxfrm) { RET(strxfrm(PTR(ARG(0)), CPTR(ARG(1)), ARG(2))); }
SHIM(_strtok) { RET(GADDR(strtok(PTR(ARG(0)), CPTR(ARG(1))))); }
SHIM(_c2pstrcpy) {
    /* Pascal string: length byte then chars. */
    const char *s = CPTR(ARG(1));
    uint8_t *d = PTR(ARG(0));
    size_t n = strlen(s);
    if (n > 255) n = 255;
    d[0] = (uint8_t)n;
    memcpy(d + 1, s, n);
}

/* ---- wide strings (wchar_t is 4 bytes on both Darwin i386 and the host) ---- */
#define WPTR(a) ((wchar_t *)PTR(a))
SHIM(_wcslen) { RET(wcslen(WPTR(ARG(0)))); }
SHIM(_wcscmp) { RET(wcscmp(WPTR(ARG(0)), WPTR(ARG(1)))); }
SHIM(_wcsncmp) { RET(wcsncmp(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2))); }
SHIM(_wcscoll) { RET(wcscoll(WPTR(ARG(0)), WPTR(ARG(1)))); }
SHIM(_wcscpy) { wcscpy(WPTR(ARG(0)), WPTR(ARG(1))); RET(ARG(0)); }
SHIM(_wcsncpy) { wcsncpy(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_wcscat) { wcscat(WPTR(ARG(0)), WPTR(ARG(1))); RET(ARG(0)); }
SHIM(_wcschr) { RET(GADDR(wcschr(WPTR(ARG(0)), (wchar_t)ARG(1)))); }
SHIM(_wcsrchr) { RET(GADDR(wcsrchr(WPTR(ARG(0)), (wchar_t)ARG(1)))); }
SHIM(_wcsstr) { RET(GADDR(wcsstr(WPTR(ARG(0)), WPTR(ARG(1))))); }
SHIM(_wcsxfrm) { RET(wcsxfrm(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2))); }
SHIM(_wcstok) {
    wchar_t *last = PTR(ARG(2)) ? WPTR(R32_(g_mem, ARG(2))) : NULL;
    wchar_t *r = wcstok(WPTR(ARG(0)), WPTR(ARG(1)), &last);
    if (ARG(2)) W32_(g_mem, ARG(2), GADDR(last));
    RET(GADDR(r));
}
SHIM(_wmemchr) { RET(GADDR(wmemchr(WPTR(ARG(0)), (wchar_t)ARG(1), ARG(2)))); }
SHIM(_wmemcmp) { RET(wmemcmp(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2))); }
SHIM(_wmemcpy) { wmemcpy(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_wmemmove) { wmemmove(WPTR(ARG(0)), WPTR(ARG(1)), ARG(2)); RET(ARG(0)); }
SHIM(_wmemset) { wmemset(WPTR(ARG(0)), (wchar_t)ARG(1), ARG(2)); RET(ARG(0)); }
SHIM(_btowc) { RET(btowc((int)ARG(0))); }
SHIM(_wctob) { RET(wctob((wint_t)ARG(0))); }

/* mbstate_t is 128 bytes on both sides; pass it through. */
SHIM(_mbrtowc) { RET(mbrtowc(WPTR(ARG(0)), CPTR(ARG(1)), ARG(2), PTR(ARG(3)))); }
SHIM(_wcrtomb) { RET(wcrtomb(PTR(ARG(0)), (wchar_t)ARG(1), PTR(ARG(2)))); }
SHIM(_mbstowcs) { RET(mbstowcs(WPTR(ARG(0)), CPTR(ARG(1)), ARG(2))); }
SHIM(_wcstombs) { RET(wcstombs(PTR(ARG(0)), WPTR(ARG(1)), ARG(2))); }

/* ---- conversions (end pointers are guest pointers to guest pointers) ---- */
#define SET_END(slot, hp) do { if (slot) W32_(g_mem, (slot), GADDR(hp)); } while (0)
SHIM(_atoi) { RET(atoi(CPTR(ARG(0)))); }
SHIM(_atof) { RETF(atof(CPTR(ARG(0)))); }
SHIM(_strtol) {
    char *e; long v = strtol(CPTR(ARG(0)), &e, (int)ARG(2));
    if (v > INT32_MAX) v = INT32_MAX, rt_set_errno(ERANGE);
    if (v < INT32_MIN) v = INT32_MIN, rt_set_errno(ERANGE);
    SET_END(ARG(1), e); RET(v);
}
SHIM(_strtoul) {
    /* i386 unsigned long is 32 bits: "-1" wraps to 0xffffffff like the original. */
    char *e; unsigned long v = strtoul(CPTR(ARG(0)), &e, (int)ARG(2));
    SET_END(ARG(1), e); RET((uint32_t)v);
}
SHIM(_strtod$UNIX2003) { char *e; double v = strtod(CPTR(ARG(0)), &e); SET_END(ARG(1), e); RETF(v); }
SHIM(_strtof$UNIX2003) { char *e; float v = strtof(CPTR(ARG(0)), &e); SET_END(ARG(1), e); RETF(v); }
SHIM(_strtold) { char *e; double v = strtod(CPTR(ARG(0)), &e); SET_END(ARG(1), e); RETF(v); }
SHIM(_wcstol) {
    wchar_t *e; long v = wcstol(WPTR(ARG(0)), &e, (int)ARG(2));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    SET_END(ARG(1), e); RET(v);
}
SHIM(_wcstoll) { wchar_t *e; long long v = wcstoll(WPTR(ARG(0)), &e, (int)ARG(2)); SET_END(ARG(1), e); RET64(v); }
SHIM(_wcstod) { wchar_t *e; double v = wcstod(WPTR(ARG(0)), &e); SET_END(ARG(1), e); RETF(v); }
SHIM(_wcstof) { wchar_t *e; float v = wcstof(WPTR(ARG(0)), &e); SET_END(ARG(1), e); RETF(v); }

/* ---- ctype (the guest's inline ctype macros read _DefaultRuneLocale directly) ---- */
SHIM(___maskrune) { RET(___runetype((int)ARG(0)) & ARG(1)); }
SHIM(___tolower) { RET(tolower((int)ARG(0))); }
SHIM(___toupper) { RET(toupper((int)ARG(0))); }

/* ---- math: float/double args on the stack, results in st(0) ---- */
SHIM(_sin) { RETF(sin(ARGF64(0))); }
SHIM(_cos) { RETF(cos(ARGF64(0))); }
SHIM(_tan) { RETF(tan(ARGF64(0))); }
SHIM(_atan) { RETF(atan(ARGF64(0))); }
SHIM(_acos) { RETF(acos(ARGF64(0))); }
SHIM(_atan2) { RETF(atan2(ARGF64(0), ARGF64(2))); }
SHIM(_exp) { RETF(exp(ARGF64(0))); }
SHIM(_pow) { RETF(pow(ARGF64(0), ARGF64(2))); }
SHIM(_fmod) { RETF(fmod(ARGF64(0), ARGF64(2))); }
SHIM(_floor) { RETF(floor(ARGF64(0))); }
SHIM(_ceil) { RETF(ceil(ARGF64(0))); }
SHIM(_modf) { double ip; double f = modf(ARGF64(0), &ip); WF64_(g_mem, ARG(2), ip); RETF(f); }
SHIM(_sinf) { RETF(sinf(ARGF32(0))); }
SHIM(_cosf) { RETF(cosf(ARGF32(0))); }
SHIM(_tanf) { RETF(tanf(ARGF32(0))); }
SHIM(_asinf) { RETF(asinf(ARGF32(0))); }
SHIM(_acosf) { RETF(acosf(ARGF32(0))); }
SHIM(_atanf) { RETF(atanf(ARGF32(0))); }
SHIM(_atan2f) { RETF(atan2f(ARGF32(0), ARGF32(1))); }
SHIM(_expf) { RETF(expf(ARGF32(0))); }
SHIM(_logf) { RETF(logf(ARGF32(0))); }
SHIM(_powf) { RETF(powf(ARGF32(0), ARGF32(1))); }
SHIM(_fmodf) { RETF(fmodf(ARGF32(0), ARGF32(1))); }
SHIM(_floorf) { RETF(floorf(ARGF32(0))); }
SHIM(_ceilf) { RETF(ceilf(ARGF32(0))); }
SHIM(_modff) { float ip; float f = modff(ARGF32(0), &ip); WF32_(g_mem, ARG(1), ip); RETF(f); }

/* ---- 64-bit arithmetic helpers (libgcc) ---- */
SHIM(___udivdi3) { uint64_t b = ARG64(2); if (!b) rt_fatal("udivdi3 by zero"); RET64(ARG64(0) / b); }
SHIM(___umoddi3) { uint64_t b = ARG64(2); if (!b) rt_fatal("umoddi3 by zero"); RET64(ARG64(0) % b); }
SHIM(___divdi3) { int64_t b = (int64_t)ARG64(2); if (!b) rt_fatal("divdi3 by zero"); RET64((int64_t)ARG64(0) / b); }
SHIM(___moddi3) { int64_t b = (int64_t)ARG64(2); if (!b) rt_fatal("moddi3 by zero"); RET64((int64_t)ARG64(0) % b); }
SHIM(___popcountsi2) { RET(__builtin_popcount(ARG(0))); }

/* ---- sorting / searching with guest comparators ---- */
typedef struct { CPU *cpu; uint32_t fn; uint8_t *base; } SortCtx;
static int sort_cmp(void *c, const void *a, const void *b) {
    SortCtx *s = c;
    return (int)rt_call(s->cpu, s->fn, 2, GADDR(a), GADDR(b));
}
SHIM(_qsort) {
    SortCtx c = {cpu, ARG(3), PTR(ARG(0))};
    qsort_r(PTR(ARG(0)), ARG(1), ARG(2), &c, sort_cmp);
}
SHIM(_bsearch) {
    uint32_t key = ARG(0), base = ARG(1), n = ARG(2), size = ARG(3), fn = ARG(4);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2, elt = base + mid * size;
        int r = (int)rt_call(cpu, fn, 2, key, elt);
        if (!r) { RET(elt); return; }
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    RET(0);
}

/* ---- random ---- */
SHIM(_rand) { RET(rand()); }
SHIM(_srand) { srand(ARG(0)); }

/* ---- time ---- */
SHIM(_time) {
    uint32_t t = (uint32_t)time(NULL);
    if (ARG(0)) W32_(g_mem, ARG(0), t);
    RET(t);
}
SHIM(_clock$UNIX2003) { RET((uint32_t)clock()); }
SHIM(_mach_absolute_time) { RET64(mach_absolute_time()); }
SHIM(_mach_timebase_info) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    W32_(g_mem, ARG(0), tb.numer);
    W32_(g_mem, ARG(0) + 4, tb.denom);
    RET(0);
}
SHIM(_usleep$UNIX2003) { RET(usleep(ARG(0))); }
SHIM(_sleep$UNIX2003) { RET(sleep(ARG(0))); }

/* struct tm on i386: 9 ints, long tm_gmtoff, char *tm_zone (44 bytes). */
void rt_tm_from_guest(struct tm *t, uint32_t g) {
    memset(t, 0, sizeof *t);
    int *f = &t->tm_sec;
    for (int i = 0; i < 9; i++) f[i] = (int)R32_(g_mem, g + 4 * i);
    t->tm_gmtoff = (int32_t)R32_(g_mem, g + 36);
}
uint32_t rt_tm_to_guest(const struct tm *t, uint32_t g) {
    const int *f = &t->tm_sec;
    for (int i = 0; i < 9; i++) W32_(g_mem, g + 4 * i, (uint32_t)f[i]);
    W32_(g_mem, g + 36, (uint32_t)t->tm_gmtoff);
    static uint32_t zone;
    if (!zone) zone = gcalloc(1, 16);
    strlcpy(PTR(zone), t->tm_zone ? t->tm_zone : "", 16);
    W32_(g_mem, g + 40, zone);
    return g;
}
SHIM(_localtime) {
    static uint32_t gtm;
    if (!gtm) gtm = gcalloc(1, 44);
    time_t tt = (time_t)(int32_t)R32_(g_mem, ARG(0));
    struct tm t;
    localtime_r(&tt, &t);
    RET(rt_tm_to_guest(&t, gtm));
}
SHIM(_asctime) {
    static uint32_t buf;
    if (!buf) buf = gcalloc(1, 32);
    struct tm t;
    rt_tm_from_guest(&t, ARG(0));
    asctime_r(&t, PTR(buf));
    RET(buf);
}
SHIM(_strftime$UNIX2003) {
    struct tm t;
    rt_tm_from_guest(&t, ARG(3));
    RET(strftime(PTR(ARG(0)), ARG(1), CPTR(ARG(2)), &t));
}
SHIM(_wcsftime$UNIX2003) {
    struct tm t;
    rt_tm_from_guest(&t, ARG(3));
    RET(wcsftime(WPTR(ARG(0)), ARG(1), WPTR(ARG(2)), &t));
}

/* ---- process ---- */
#define MAX_ATEXIT 256
static struct { uint32_t fn, arg; int has_arg; } g_atexit[MAX_ATEXIT];
static int g_natexit;

SHIM(___cxa_atexit) {
    if (g_natexit < MAX_ATEXIT) g_atexit[g_natexit++] = (typeof(g_atexit[0])){ARG(0), ARG(1), 1};
    RET(0);
}
SHIM(_atexit) {
    if (g_natexit < MAX_ATEXIT) g_atexit[g_natexit++] = (typeof(g_atexit[0])){ARG(0), 0, 0};
    RET(0);
}
void rt_run_atexit(CPU *cpu) {
    while (g_natexit > 0) {
        g_natexit--;
        if (g_atexit[g_natexit].has_arg) rt_call(cpu, g_atexit[g_natexit].fn, 1, g_atexit[g_natexit].arg);
        else rt_call(cpu, g_atexit[g_natexit].fn, 0);
    }
}
SHIM(_exit) {
    rt_log("guest exit(%d)", (int)ARG(0));
    rt_run_atexit(cpu);
    fflush(NULL);
    exit((int)ARG(0));
}
SHIM(__exit) { fflush(NULL); _exit((int)ARG(0)); }
SHIM(_abort) { rt_fatal("guest abort()"); }
SHIM(___stack_chk_fail) { rt_fatal("guest stack protector tripped"); }

SHIM(_getenv) {
    /* Environment strings are copied into the guest once and kept. */
    const char *v = getenv(CPTR(ARG(0)));
    RET(v ? gstrdup(v) : 0);
}
SHIM(_setlocale) {
    static uint32_t c;
    if (!c) c = gstrdup("C");
    RET(c);
}
SHIM(_gethostname) { RET(gethostname(PTR(ARG(0)), ARG(1))); }
SHIM(_getrlimit$UNIX2003) {
    /* struct rlimit: two 64-bit values. */
    W64_(g_mem, ARG(1), 0x7fffffffffffffffull);
    W64_(g_mem, ARG(1) + 8, 0x7fffffffffffffffull);
    RET(0);
}
SHIM(_setrlimit$UNIX2003) { RET(0); }
SHIM(_setpriority) { RET(0); }

/* mmap: anonymous mappings come from the guest heap. */
SHIM(_mmap$UNIX2003) {
    uint32_t len = ARG(1), flags = ARG(3);
    if (!(flags & 0x1000 /* MAP_ANON */)) rt_fatal("file-backed mmap not supported");
    uint32_t p = gmemalign(0x4000, len);
    if (!p) { rt_set_errno(ENOMEM); RET(0xffffffffu); return; }
    memset(PTR(p), 0, len);
    RET(p);
}
SHIM(_munmap$UNIX2003) { gfree(ARG(0)); RET(0); }
SHIM(_mprotect$UNIX2003) { RET(0); }

SHIM(_OSAtomicAnd32Barrier) { RET(__atomic_and_fetch((uint32_t *)PTR(ARG(1)), ARG(0), __ATOMIC_SEQ_CST)); }

const ShimDef shims_libc[] = {
    SHIM_ENTRY(___error), SHIM_ENTRY(_malloc), SHIM_ENTRY(_calloc), SHIM_ENTRY(_realloc), SHIM_ENTRY(_free),
    SHIM_ENTRY(_memcpy), SHIM_ENTRY(_memmove), SHIM_ENTRY(_memset), SHIM_ENTRY(___memcpy_chk),
    SHIM_ENTRY(___memmove_chk), SHIM_ENTRY(___memset_chk), SHIM_ENTRY(___bzero), SHIM_ENTRY(_memcmp),
    SHIM_ENTRY(_memchr), SHIM_ENTRY(_strlen), SHIM_ENTRY(_strcmp), SHIM_ENTRY(_strncmp), SHIM_ENTRY(_strcasecmp),
    SHIM_ENTRY(_strcoll), SHIM_ENTRY(_strcpy), SHIM_ENTRY(___strcpy_chk), SHIM_ENTRY(_strncpy),
    SHIM_ENTRY(_strlcpy), SHIM_ENTRY(_strcat), SHIM_ENTRY(___strcat_chk), SHIM_ENTRY(_strncat),
    SHIM_ENTRY(___strncat_chk), SHIM_ENTRY(_strchr), SHIM_ENTRY(_strrchr), SHIM_ENTRY(_strstr),
    SHIM_ENTRY(_strcspn), SHIM_ENTRY(_strdup), SHIM_ENTRY(_strxfrm), SHIM_ENTRY(_strtok), SHIM_ENTRY(_c2pstrcpy),
    SHIM_ENTRY(_wcslen), SHIM_ENTRY(_wcscmp), SHIM_ENTRY(_wcsncmp), SHIM_ENTRY(_wcscoll), SHIM_ENTRY(_wcscpy),
    SHIM_ENTRY(_wcsncpy), SHIM_ENTRY(_wcscat), SHIM_ENTRY(_wcschr), SHIM_ENTRY(_wcsrchr), SHIM_ENTRY(_wcsstr),
    SHIM_ENTRY(_wcsxfrm), SHIM_ENTRY(_wcstok), SHIM_ENTRY(_wmemchr), SHIM_ENTRY(_wmemcmp), SHIM_ENTRY(_wmemcpy),
    SHIM_ENTRY(_wmemmove), SHIM_ENTRY(_wmemset), SHIM_ENTRY(_btowc), SHIM_ENTRY(_wctob), SHIM_ENTRY(_mbrtowc),
    SHIM_ENTRY(_wcrtomb), SHIM_ENTRY(_mbstowcs), SHIM_ENTRY(_wcstombs), SHIM_ENTRY(_atoi), SHIM_ENTRY(_atof),
    SHIM_ENTRY(_strtol), SHIM_ENTRY(_strtoul), SHIM_ENTRY(_strtod$UNIX2003), SHIM_ENTRY(_strtof$UNIX2003),
    SHIM_ENTRY(_strtold), SHIM_ENTRY(_wcstol), SHIM_ENTRY(_wcstoll), SHIM_ENTRY(_wcstod), SHIM_ENTRY(_wcstof),
    SHIM_ENTRY(___maskrune), SHIM_ENTRY(___tolower), SHIM_ENTRY(___toupper), SHIM_ENTRY(_sin), SHIM_ENTRY(_cos),
    SHIM_ENTRY(_tan), SHIM_ENTRY(_atan), SHIM_ENTRY(_acos), SHIM_ENTRY(_atan2), SHIM_ENTRY(_exp), SHIM_ENTRY(_pow),
    SHIM_ENTRY(_fmod), SHIM_ENTRY(_floor), SHIM_ENTRY(_ceil), SHIM_ENTRY(_modf), SHIM_ENTRY(_sinf),
    SHIM_ENTRY(_cosf), SHIM_ENTRY(_tanf), SHIM_ENTRY(_asinf), SHIM_ENTRY(_acosf), SHIM_ENTRY(_atanf),
    SHIM_ENTRY(_atan2f), SHIM_ENTRY(_expf), SHIM_ENTRY(_logf), SHIM_ENTRY(_powf), SHIM_ENTRY(_fmodf),
    SHIM_ENTRY(_floorf), SHIM_ENTRY(_ceilf), SHIM_ENTRY(_modff), SHIM_ENTRY(___udivdi3), SHIM_ENTRY(___umoddi3),
    SHIM_ENTRY(___divdi3), SHIM_ENTRY(___moddi3), SHIM_ENTRY(___popcountsi2), SHIM_ENTRY(_qsort),
    SHIM_ENTRY(_bsearch), SHIM_ENTRY(_rand), SHIM_ENTRY(_srand), SHIM_ENTRY(_time), SHIM_ENTRY(_clock$UNIX2003),
    SHIM_ENTRY(_mach_absolute_time), SHIM_ENTRY(_mach_timebase_info), SHIM_ENTRY(_usleep$UNIX2003),
    SHIM_ENTRY(_sleep$UNIX2003), SHIM_ENTRY(_localtime), SHIM_ENTRY(_asctime), SHIM_ENTRY(_strftime$UNIX2003),
    SHIM_ENTRY(_wcsftime$UNIX2003), SHIM_ENTRY(___cxa_atexit), SHIM_ENTRY(_atexit), SHIM_ENTRY(_exit),
    SHIM_ENTRY(__exit), SHIM_ENTRY(_abort), SHIM_ENTRY(___stack_chk_fail), SHIM_ENTRY(_getenv),
    SHIM_ENTRY(_setlocale), SHIM_ENTRY(_gethostname), SHIM_ENTRY(_getrlimit$UNIX2003),
    SHIM_ENTRY(_setrlimit$UNIX2003), SHIM_ENTRY(_setpriority), SHIM_ENTRY(_mmap$UNIX2003),
    SHIM_ENTRY(_munmap$UNIX2003), SHIM_ENTRY(_mprotect$UNIX2003), SHIM_ENTRY(_OSAtomicAnd32Barrier),
    {0, 0},
};

/* ---- libc data the guest reads directly ---- */
void rt_stdio_init(void);

void rt_libc_init(CPU *cpu, int argc, char **argv) {
    (void)cpu; (void)argv;
    uint32_t a;
    if ((a = rt_hostdata("___mb_cur_max"))) W32_(g_mem, a, 1);
    if ((a = rt_hostdata("___stack_chk_guard"))) W32_(g_mem, a, 0x00e2e4ffu);
    if ((a = rt_hostdata("_NXArgc"))) W32_(g_mem, a, (uint32_t)argc);
    if ((a = rt_hostdata("__DefaultRuneLocale"))) {
        /* i386 _RuneLocale: magic[8], encoding[32], sgetrune, sputrune, invalid_rune,
         * then __runetype[256], __maplower[256], __mapupper[256]. */
        memcpy(PTR(a), _DefaultRuneLocale.__magic, 8);
        memcpy(PTR(a) + 8, _DefaultRuneLocale.__encoding, 32);
        W32_(g_mem, a + 48, (uint32_t)_DefaultRuneLocale.__invalid_rune);
        for (int i = 0; i < 256; i++) {
            W32_(g_mem, a + 52 + 4 * i, _DefaultRuneLocale.__runetype[i]);
            W32_(g_mem, a + 52 + 1024 + 4 * i, (uint32_t)_DefaultRuneLocale.__maplower[i]);
            W32_(g_mem, a + 52 + 2048 + 4 * i, (uint32_t)_DefaultRuneLocale.__mapupper[i]);
        }
    }
    rt_stdio_init();
}
