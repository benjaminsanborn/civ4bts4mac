/* CoreFoundation shims.
 *
 * A guest CF object is an 8-byte proxy {CF_PROXY_ISA, index} allocated in the
 * guest heap; the index selects a retained host CFTypeRef. The proxy for a
 * given host object is unique, so pointer comparisons in guest code still
 * work. The table holds one reference to each host object; a periodic sweep
 * drops entries whose host object has no other references (the guest has
 * released it, so under real CF it would already be gone) and frees their
 * proxies. Without that, every XML node and string the game ever touched
 * stayed alive (millions within minutes).
 */
#include "cf.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CF_PROXY_ISA 0x017FFF00u
#define CFINDEX(n) ((CFIndex)(int32_t)ARG(n))

static pthread_mutex_t g_cf_lock = PTHREAD_MUTEX_INITIALIZER;
static CFTypeRef *g_tab;
static uint32_t *g_tab_guest; /* guest address for each entry (proxy or constant string) */
static uint32_t g_tab_n, g_tab_cap;
static uint32_t *g_tab_free, g_nfree; /* recycled indices */
static uint32_t g_live, g_sweep_at = 1u << 17;

/* Open-addressing map: host pointer -> guest proxy, and guest constant string -> host. */
typedef struct { uintptr_t k; uint32_t v; } Slot;
typedef struct { Slot *s; size_t cap, n; } Map;

static uint32_t *map_find(Map *m, uintptr_t k) {
    if (!m->cap) return NULL;
    for (size_t i = (k * 0x9E3779B97F4A7C15ull) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->s[i].k) return NULL;
        if (m->s[i].k == k) return &m->s[i].v;
    }
}

static void map_put(Map *m, uintptr_t k, uint32_t v) {
    if ((m->n + 1) * 2 > m->cap) {
        Map nm = {calloc(m->cap ? m->cap * 2 : 1024, sizeof(Slot)), m->cap ? m->cap * 2 : 1024, 0};
        for (size_t i = 0; i < m->cap; i++)
            if (m->s[i].k) map_put(&nm, m->s[i].k, m->s[i].v);
        free(m->s);
        *m = nm;
    }
    size_t i = (k * 0x9E3779B97F4A7C15ull) & (m->cap - 1);
    while (m->s[i].k && m->s[i].k != k) i = (i + 1) & (m->cap - 1);
    if (!m->s[i].k) m->n++;
    m->s[i].k = k;
    m->s[i].v = v;
}

/* Delete with backward shift (linear probing). */
static void map_del(Map *m, uintptr_t k) {
    if (!m->cap) return;
    size_t mask = m->cap - 1, i = (k * 0x9E3779B97F4A7C15ull) & mask;
    while (m->s[i].k && m->s[i].k != k) i = (i + 1) & mask;
    if (!m->s[i].k) return;
    m->n--;
    for (size_t j = i;;) {
        j = (j + 1) & mask;
        if (!m->s[j].k) break;
        size_t h = (m->s[j].k * 0x9E3779B97F4A7C15ull) & mask;
        if (i <= j ? (h > i && h <= j) : (h > i || h <= j)) continue;
        m->s[i] = m->s[j];
        i = j;
    }
    m->s[i].k = 0;
    m->s[i].v = 0;
}

static Map g_host2guest, g_conststr;
static uint32_t g_cfstr_isa;

static uint32_t tab_add(CFTypeRef h, uint32_t guest) {
    g_live++;
    if (g_nfree) {
        uint32_t i = g_tab_free[--g_nfree];
        g_tab[i] = h;
        g_tab_guest[i] = guest;
        return i;
    }
    if (g_tab_n == g_tab_cap) {
        g_tab_cap = g_tab_cap ? g_tab_cap * 2 : 4096;
        g_tab = realloc(g_tab, g_tab_cap * sizeof *g_tab);
        g_tab_guest = realloc(g_tab_guest, g_tab_cap * sizeof *g_tab_guest);
        g_tab_free = realloc(g_tab_free, g_tab_cap * sizeof *g_tab_free);
    }
    g_tab[g_tab_n] = h;
    g_tab_guest[g_tab_n] = guest;
    return g_tab_n++;
}

/* CIV4_SWEEP_DEBUG: poison swept proxies instead of freeing them, and keep
 * per-object guest copies, to find guest uses of released objects. */
#define SWEPT_ISA 0x5EE7CF00u
static int sweep_debug(void) {
    static int d = -1;
    if (d < 0) d = getenv("CIV4_SWEEP_DEBUG") != NULL;
    return d;
}
int cf_sweep_debug(void) { return sweep_debug(); }

static void (*g_sweep_hooks[8])(CFTypeRef);
static int g_nsweep_hooks;
void cf_on_sweep(void (*fn)(CFTypeRef)) { g_sweep_hooks[g_nsweep_hooks++] = fn; }
static void forget_host(CFTypeRef h);

/* Drop proxies whose host object is referenced only by this table. Tagged
 * pointers (no real retain count) and constant strings are kept. */
static void sweep(void) {
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < g_tab_n; i++) {
        CFTypeRef h = g_tab[i];
        if (!h || (intptr_t)h < 0) continue;
        uint32_t g = g_tab_guest[i];
        if (R32_(g_mem, g) != CF_PROXY_ISA) continue;
        if (CFGetRetainCount(h) != 1) continue;
        map_del(&g_host2guest, (uintptr_t)h);
        forget_host(h);
        for (int k = 0; k < g_nsweep_hooks; k++) g_sweep_hooks[k](h);
        if (sweep_debug()) {
            W32_(g_mem, g, SWEPT_ISA); /* keep the proxy: cf_in reports any later use */
        } else {
            W32_(g_mem, g, 0); /* a stale guest use then fails loudly in cf_in */
            gfree(g);
        }
        CFRelease(h);
        g_tab[i] = NULL;
        g_tab_free[g_nfree++] = i;
        g_live--;
        dropped++;
    }
    g_sweep_at = g_live * 2 + (1u << 17);
    static int debug = -1;
    if (debug < 0) debug = getenv("CIV4_MEMSTATS") != NULL;
    if (debug) rt_log("cf sweep: dropped %u, %u live", dropped, g_live);
}

CFTypeRef cf_in(uint32_t g) {
    if (!g) return NULL;
    uint32_t isa = R32_(g_mem, g);
    if (isa == CF_PROXY_ISA) return g_tab[R32_(g_mem, g + 4)];
    if (isa == g_cfstr_isa) {
        pthread_mutex_lock(&g_cf_lock);
        uint32_t *idx = map_find(&g_conststr, g);
        CFTypeRef h;
        if (idx) {
            h = g_tab[*idx];
        } else {
            /* __cfstring: {isa, flags, chars, length}; flag 0x10 = UTF-16 storage */
            uint32_t flags = R32_(g_mem, g + 4), chars = R32_(g_mem, g + 8), len = R32_(g_mem, g + 12);
            if (flags & 0x10)
                h = CFStringCreateWithBytes(NULL, PTR(chars), (CFIndex)len * 2, kCFStringEncodingUTF16LE, false);
            else
                h = CFStringCreateWithBytes(NULL, PTR(chars), (CFIndex)len, kCFStringEncodingMacRoman, false);
            map_put(&g_conststr, g, tab_add(h, g));
            /* Constant strings map back to themselves so identity holds. */
            map_put(&g_host2guest, (uintptr_t)h, g);
        }
        pthread_mutex_unlock(&g_cf_lock);
        return h;
    }
    if (isa == SWEPT_ISA) rt_fatal("guest used a swept CF proxy %08x", g);
    rt_fatal("not a CF object: guest %08x (isa %08x)", g, isa);
}

uint32_t cf_out(CFTypeRef h) {
    if (!h) return 0;
    pthread_mutex_lock(&g_cf_lock);
    uint32_t *g = map_find(&g_host2guest, (uintptr_t)h);
    uint32_t r;
    if (g) {
        r = *g;
    } else {
        static int no_sweep = -1;
        if (no_sweep < 0) no_sweep = getenv("CIV4_NO_SWEEP") != NULL;
        if (!g_nfree && g_live >= g_sweep_at && !no_sweep) sweep();
        CFRetain(h);
        r = gmalloc(8);
        W32_(g_mem, r, CF_PROXY_ISA);
        W32_(g_mem, r + 4, tab_add(h, r));
        map_put(&g_host2guest, (uintptr_t)h, r);
    }
    pthread_mutex_unlock(&g_cf_lock);
    return r;
}

uint32_t cf_out_owned(CFTypeRef h) {
    /* The guest owns the +1 from the host call; the proxy table holds its own. */
    return cf_out(h);
}

/* ---- callbacks / constants ---- */
static CFBundleRef g_main_bundle;
static uint32_t g_arr_cb, g_dict_key_cb, g_dict_val_cb, g_copystr_key_cb;

static const CFArrayCallBacks *arr_cb(uint32_t g) {
    if (!g) return NULL;
    if (g == g_arr_cb) return &kCFTypeArrayCallBacks;
    rt_fatal("CFArray with custom guest callbacks (%08x)", g);
}
static const CFDictionaryKeyCallBacks *dict_key_cb(uint32_t g) {
    if (!g) return NULL;
    if (g == g_dict_key_cb) return &kCFTypeDictionaryKeyCallBacks;
    if (g == g_copystr_key_cb) return &kCFCopyStringDictionaryKeyCallBacks;
    rt_fatal("CFDictionary with custom guest key callbacks (%08x)", g);
}
static const CFDictionaryValueCallBacks *dict_val_cb(uint32_t g) {
    if (!g) return NULL;
    if (g == g_dict_val_cb) return &kCFTypeDictionaryValueCallBacks;
    rt_fatal("CFDictionary with custom guest value callbacks (%08x)", g);
}

/* Collections created without callbacks hold raw guest values, not CF objects. */
static Map g_raw_collections;
static int is_raw(CFTypeRef c) { return map_find(&g_raw_collections, (uintptr_t)c) != NULL; }
static const void *val_in(CFTypeRef coll, uint32_t g) { return is_raw(coll) ? (const void *)(uintptr_t)g : cf_in(g); }
static uint32_t val_out(CFTypeRef coll, const void *h) { return is_raw(coll) ? (uint32_t)(uintptr_t)h : cf_out(h); }

void rt_cf_init(void) {
    g_cfstr_isa = rt_hostdata("___CFConstantStringClassReference");
    g_arr_cb = rt_hostdata("_kCFTypeArrayCallBacks");
    g_dict_key_cb = rt_hostdata("_kCFTypeDictionaryKeyCallBacks");
    g_dict_val_cb = rt_hostdata("_kCFTypeDictionaryValueCallBacks");
    g_copystr_key_cb = rt_hostdata("_kCFCopyStringDictionaryKeyCallBacks");
    uint32_t a;
    if ((a = rt_hostdata("_kCFBooleanTrue"))) W32_(g_mem, a, cf_out(kCFBooleanTrue));
    if ((a = rt_hostdata("_kCFBooleanFalse"))) W32_(g_mem, a, cf_out(kCFBooleanFalse));
    if ((a = rt_hostdata("_kCFBundleNameKey"))) W32_(g_mem, a, cf_out(kCFBundleNameKey));
    if ((a = rt_hostdata("_kCFPreferencesCurrentApplication"))) W32_(g_mem, a, cf_out(kCFPreferencesCurrentApplication));
    if ((a = rt_hostdata("_kCFAbsoluteTimeIntervalSince1970"))) WF64_(g_mem, a, kCFAbsoluteTimeIntervalSince1970);
    /* Allocators: guest NULL / these cells all mean "default". */

    /* The guest's main bundle is the original game app, not this host binary. */
    const char *app = getenv("CIV4_APP");
    if (!app) app = "build/game/Beyond the Sword/Civilization IV Beyond the Sword.app";
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)app, (CFIndex)strlen(app), true);
    g_main_bundle = CFBundleCreate(NULL, u);
    CFRelease(u);
    if (!g_main_bundle) rt_fatal("cannot open game bundle %s (set CIV4_APP)", app);
}

/* Allocator arguments are ignored: everything uses the default allocator. */

/* ---- generic ---- */
SHIM(_CFRetain) { if (ARG(0)) CFRetain(CFI(0)); RET(ARG(0)); }
SHIM(_CFRelease) { if (ARG(0)) CFRelease(CFI(0)); }
SHIM(_CFGetTypeID) { RET(CFGetTypeID(CFI(0))); }
SHIM(_CFShow) { CFShow(CFI(0)); }
SHIM(_CFAllocatorGetDefault) { RET(0); }

#define TYPEID(T) SHIM(_##T##GetTypeID) { RET(T##GetTypeID()); }
TYPEID(CFArray) TYPEID(CFData) TYPEID(CFDate) TYPEID(CFDictionary) TYPEID(CFNumber) TYPEID(CFString)

/* ---- strings ---- */
SHIM(_CFStringCreateWithCString) { RETCF_OWNED(CFStringCreateWithCString(NULL, CPTR(ARG(1)), ARG(2))); }
SHIM(_CFStringCreateWithPascalString) {
    const uint8_t *p = PTR(ARG(1));
    RETCF_OWNED(CFStringCreateWithBytes(NULL, p + 1, p[0], ARG(2), false));
}
SHIM(_CFStringCreateWithBytes) {
    RETCF_OWNED(CFStringCreateWithBytes(NULL, PTR(ARG(1)), CFINDEX(2), ARG(3), (Boolean)ARG(4)));
}
SHIM(_CFStringCreateWithCharacters) { RETCF_OWNED(CFStringCreateWithCharacters(NULL, PTR(ARG(1)), CFINDEX(2))); }
SHIM(_CFStringCreateCopy) { RETCF_OWNED(CFStringCreateCopy(NULL, CFI(1))); }
SHIM(_CFStringCreateMutable) { RETCF_OWNED(CFStringCreateMutable(NULL, CFINDEX(1))); }
SHIM(_CFStringCreateMutableCopy) { RETCF_OWNED(CFStringCreateMutableCopy(NULL, CFINDEX(1), CFI(2))); }
SHIM(_CFStringGetLength) { RET(CFStringGetLength(CFI(0))); }
SHIM(_CFStringGetCharacterAtIndex) { RET(CFStringGetCharacterAtIndex(CFI(0), CFINDEX(1))); }
SHIM(_CFStringGetCharacters) {
    /* (str, CFRange{loc,len}, UniChar *buf) */
    /* Old CF tolerated ranges past the end (the game asks for length + 1); clamp and zero-fill. */
    CFStringRef str = CFI(0);
    CFIndex len = CFStringGetLength(str), loc = CFINDEX(1), n = CFINDEX(2), ok = n;
    if (loc > len) loc = len;
    if (loc + ok > len) ok = len - loc;
    CFStringGetCharacters(str, CFRangeMake(loc, ok), PTR(ARG(3)));
    if (n > ok) memset((uint8_t *)PTR(ARG(3)) + 2 * ok, 0, (size_t)(n - ok) * 2);
}
SHIM(_CFStringGetCString) { RET(CFStringGetCString(CFI(0), PTR(ARG(1)), CFINDEX(2), ARG(3))); }
SHIM(_CFStringGetCStringPtr) { RET(0); /* host storage is not guest-addressable; NULL is allowed */ }
SHIM(_CFStringGetBytes) {
    /* (str, range, encoding, lossByte, isExternal, buffer, maxBufLen, CFIndex *usedBufLen) */
    CFIndex used = 0;
    CFIndex r = CFStringGetBytes(CFI(0), CFRangeMake(CFINDEX(1), CFINDEX(2)), ARG(3), (UInt8)ARG(4),
                                 (Boolean)ARG(5), PTR(ARG(6)), CFINDEX(7), &used);
    if (ARG(8)) W32_(g_mem, ARG(8), (uint32_t)used);
    RET(r);
}
SHIM(_CFStringGetMaximumSizeForEncoding) { RET(CFStringGetMaximumSizeForEncoding(CFINDEX(0), ARG(1))); }
SHIM(_CFStringGetSystemEncoding) { RET(CFStringGetSystemEncoding()); }
SHIM(_CFStringGetIntValue) { RET(CFStringGetIntValue(CFI(0))); }
SHIM(_CFStringCompare) { RET(CFStringCompare(CFI(0), CFI(1), ARG(2))); }
SHIM(_CFStringAppend) { CFStringAppend((CFMutableStringRef)CFI(0), CFI(1)); }
SHIM(_CFStringDelete) { CFStringDelete((CFMutableStringRef)CFI(0), CFRangeMake(CFINDEX(1), CFINDEX(2))); }
SHIM(_CFStringReplace) { CFStringReplace((CFMutableStringRef)CFI(0), CFRangeMake(CFINDEX(1), CFINDEX(2)), CFI(3)); }
SHIM(_CFStringLowercase) { CFStringLowercase((CFMutableStringRef)CFI(0), NULL); }
SHIM(_CFStringFind) {
    CFRange r = CFStringFind(CFI(0), CFI(1), ARG(2));
    RET64(((uint64_t)(uint32_t)r.length << 32) | (uint32_t)r.location);
}
SHIM(_CFStringFindCharacterFromSet) {
    /* (str, set, CFRange rangeToSearch, options, CFRange *result) */
    CFRange res;
    Boolean ok = CFStringFindCharacterFromSet(CFI(0), CFI(1), CFRangeMake(CFINDEX(2), CFINDEX(3)), ARG(4), &res);
    if (ok && ARG(5)) {
        W32_(g_mem, ARG(5), (uint32_t)res.location);
        W32_(g_mem, ARG(5) + 4, (uint32_t)res.length);
    }
    RET(ok);
}
/* ---- formatting (CFString formats with %@) ---- */
#include "format.h"
#include <stdlib.h>

static void format_object(WOut *o, uint32_t g) {
    CFTypeRef obj = cf_in(g);
    CFStringRef d = !obj ? CFSTR("(null)")
                  : CFGetTypeID(obj) == CFStringGetTypeID() ? (CFStringRef)CFRetain(obj) : CFCopyDescription(obj);
    CFIndex n = CFStringGetLength(d);
    for (CFIndex i = 0; i < n; i++) rt_wout_put(o, CFStringGetCharacterAtIndex(d, i));
    CFRelease(d);
}

static CFStringRef format_cf(CFStringRef fmt, uint32_t va) {
    rt_format_object = format_object;
    CFIndex n = CFStringGetLength(fmt);
    wchar_t *wf = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    for (CFIndex i = 0; i < n; i++) wf[i] = CFStringGetCharacterAtIndex(fmt, i);
    wf[n] = 0;
    size_t len;
    wchar_t *out = rt_format_host(wf, va, &len);
    free(wf);
    UniChar *u = malloc(sizeof(UniChar) * (len ? len : 1));
    for (size_t i = 0; i < len; i++) u[i] = (UniChar)out[i];
    free(out);
    CFStringRef r = CFStringCreateWithCharacters(NULL, u, (CFIndex)len);
    free(u);
    return r;
}

SHIM(_CFStringCreateWithFormat) {
    /* (allocator, formatOptions, format, ...) */
    RETCF_OWNED(format_cf(CFI(2), ARGADDR(3)));
}
SHIM(_CFStringAppendFormatAndArguments) {
    /* (mutable string, formatOptions, format, va_list) */
    CFStringRef r = format_cf(CFI(2), ARG(3));
    CFStringAppend((CFMutableStringRef)CFI(0), r);
    CFRelease(r);
}

SHIM(_CFStringCreateArrayBySeparatingStrings) { RETCF_OWNED(CFStringCreateArrayBySeparatingStrings(NULL, CFI(1), CFI(2))); }

/* ---- character sets ---- */
SHIM(_CFCharacterSetGetPredefined) { RETCF(CFCharacterSetGetPredefined(ARG(0))); }
SHIM(_CFCharacterSetCreateWithCharactersInString) { RETCF_OWNED(CFCharacterSetCreateWithCharactersInString(NULL, CFI(1))); }
SHIM(_CFCharacterSetIsCharacterMember) { RET(CFCharacterSetIsCharacterMember(CFI(0), (UniChar)ARG(1))); }
SHIM(_CFCharacterSetIsLongCharacterMember) { RET(CFCharacterSetIsLongCharacterMember(CFI(0), ARG(1))); }

/* ---- numbers, booleans, dates ---- */
SHIM(_CFNumberCreate) {
    /* CFNumberType values are the same; the guest's long/CFIndex types are 32-bit. */
    CFNumberType t = (CFNumberType)ARG(1);
    void *p = PTR(ARG(2));
    int32_t v32;
    if (t == kCFNumberLongType || t == kCFNumberCFIndexType || t == kCFNumberNSIntegerType) {
        memcpy(&v32, p, 4);
        RETCF_OWNED(CFNumberCreate(NULL, kCFNumberSInt32Type, &v32));
        return;
    }
    RETCF_OWNED(CFNumberCreate(NULL, t, p));
}
SHIM(_CFNumberGetValue) {
    CFNumberType t = (CFNumberType)ARG(1);
    if (t == kCFNumberLongType || t == kCFNumberCFIndexType || t == kCFNumberNSIntegerType) t = kCFNumberSInt32Type;
    RET(CFNumberGetValue(CFI(0), t, PTR(ARG(2))));
}
SHIM(_CFBooleanGetValue) { RET(CFBooleanGetValue(CFI(0))); }
SHIM(_CFAbsoluteTimeGetCurrent) { RETF(CFAbsoluteTimeGetCurrent()); }
SHIM(_CFDateCreate) { RETCF_OWNED(CFDateCreate(NULL, ARGF64(1))); }
SHIM(_CFDateCompare) { RET(CFDateCompare(CFI(0), CFI(1), NULL)); }
SHIM(_CFTimeZoneCopyDefault) { RETCF_OWNED(CFTimeZoneCopyDefault()); }
SHIM(_CFTimeZoneGetSecondsFromGMT) { RETF(CFTimeZoneGetSecondsFromGMT(CFI(0), ARGF64(1))); }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
/* CFGregorianDate (i386): {SInt32 year; SInt8 month, day, hour, minute; double second} = 16 bytes */
static CFGregorianDate gdate_in(uint32_t g) {
    CFGregorianDate d;
    d.year = (SInt32)R32_(g_mem, g);
    d.month = (SInt8)R8_(g_mem, g + 4);
    d.day = (SInt8)R8_(g_mem, g + 5);
    d.hour = (SInt8)R8_(g_mem, g + 6);
    d.minute = (SInt8)R8_(g_mem, g + 7);
    d.second = RF64_(g_mem, g + 8);
    return d;
}
SHIM(_CFAbsoluteTimeGetGregorianDate) {
    /* Struct return through a hidden pointer, which the callee pops (ret $4). */
    uint32_t out = ARG(0);
    CFGregorianDate d = CFAbsoluteTimeGetGregorianDate(ARGF64(1), CFI(3));
    W32_(g_mem, out, (uint32_t)d.year);
    W8_(g_mem, out + 4, (uint8_t)d.month);
    W8_(g_mem, out + 5, (uint8_t)d.day);
    W8_(g_mem, out + 6, (uint8_t)d.hour);
    W8_(g_mem, out + 7, (uint8_t)d.minute);
    WF64_(g_mem, out + 8, d.second);
    RET(out);
    cpu->r[4] += 4;
}
SHIM(_CFGregorianDateGetAbsoluteTime) { RETF(CFGregorianDateGetAbsoluteTime(gdate_in(ARGADDR(0)), CFI(4))); }
SHIM(_CFAbsoluteTimeGetDayOfWeek) { RET(CFAbsoluteTimeGetDayOfWeek(ARGF64(0), CFI(2))); }
SHIM(_CFAbsoluteTimeAddGregorianUnits) {
    /* (double at, tz, CFGregorianUnits {SInt32 y, mo, d, h, mi; double s} by value) */
    uint32_t u = ARGADDR(3);
    CFGregorianUnits units = {(SInt32)R32_(g_mem, u), (SInt32)R32_(g_mem, u + 4), (SInt32)R32_(g_mem, u + 8),
                              (SInt32)R32_(g_mem, u + 12), (SInt32)R32_(g_mem, u + 16), RF64_(g_mem, u + 20)};
    RETF(CFAbsoluteTimeAddGregorianUnits(ARGF64(0), CFI(2), units));
}
#pragma clang diagnostic pop

/* ---- data ---- */
static Map g_data_bytes; /* host CFData -> guest copy of its bytes */
SHIM(_CFDataCreate) { RETCF_OWNED(CFDataCreate(NULL, PTR(ARG(1)), CFINDEX(2))); }
SHIM(_CFDataGetLength) { RET(CFDataGetLength(CFI(0))); }
SHIM(_CFDataGetBytes) { CFDataGetBytes(CFI(0), CFRangeMake(CFINDEX(1), CFINDEX(2)), PTR(ARG(3))); }
SHIM(_CFDataGetBytePtr) {
    CFDataRef d = CFI(0);
    pthread_mutex_lock(&g_cf_lock);
    uint32_t *g = map_find(&g_data_bytes, (uintptr_t)d);
    uint32_t r;
    if (g) {
        r = *g;
    } else {
        CFIndex n = CFDataGetLength(d);
        r = gmalloc((uint32_t)n + 1);
        memcpy(PTR(r), CFDataGetBytePtr(d), (size_t)n);
        map_put(&g_data_bytes, (uintptr_t)d, r);
    }
    pthread_mutex_unlock(&g_cf_lock);
    RET(r);
}

/* ---- arrays ---- */
SHIM(_CFArrayCreateMutable) {
    CFMutableArrayRef a = CFArrayCreateMutable(NULL, CFINDEX(1), arr_cb(ARG(2)));
    if (!ARG(2)) {
        pthread_mutex_lock(&g_cf_lock);
        map_put(&g_raw_collections, (uintptr_t)a, 1);
        pthread_mutex_unlock(&g_cf_lock);
    }
    RETCF_OWNED(a);
}
SHIM(_CFArrayGetCount) { RET(CFArrayGetCount(CFI(0))); }
SHIM(_CFArrayGetValueAtIndex) { CFArrayRef a = CFI(0); RET(val_out(a, CFArrayGetValueAtIndex(a, CFINDEX(1)))); }
SHIM(_CFArrayAppendValue) { CFMutableArrayRef a = (CFMutableArrayRef)CFI(0); CFArrayAppendValue(a, val_in(a, ARG(1))); }
SHIM(_CFArraySetValueAtIndex) {
    CFMutableArrayRef a = (CFMutableArrayRef)CFI(0);
    CFArraySetValueAtIndex(a, CFINDEX(1), val_in(a, ARG(2)));
}
SHIM(_CFArrayGetFirstIndexOfValue) {
    CFArrayRef a = CFI(0);
    RET(CFArrayGetFirstIndexOfValue(a, CFRangeMake(CFINDEX(1), CFINDEX(2)), val_in(a, ARG(3))));
}

/* ---- dictionaries (keys/values are CF objects unless created without callbacks) ---- */
SHIM(_CFDictionaryCreateMutable) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, CFINDEX(1), dict_key_cb(ARG(2)), dict_val_cb(ARG(3)));
    if (!ARG(2) && !ARG(3)) {
        pthread_mutex_lock(&g_cf_lock);
        map_put(&g_raw_collections, (uintptr_t)d, 1);
        pthread_mutex_unlock(&g_cf_lock);
    } else if (!ARG(2) || !ARG(3)) {
        rt_fatal("CFDictionary with mixed raw/CF callbacks");
    }
    RETCF_OWNED(d);
}
SHIM(_CFDictionaryCreateMutableCopy) { RETCF_OWNED(CFDictionaryCreateMutableCopy(NULL, CFINDEX(1), CFI(2))); }
SHIM(_CFDictionaryGetCount) { RET(CFDictionaryGetCount(CFI(0))); }
SHIM(_CFDictionaryGetValue) { CFDictionaryRef d = CFI(0); RET(val_out(d, CFDictionaryGetValue(d, val_in(d, ARG(1))))); }
SHIM(_CFDictionaryGetValueIfPresent) {
    CFDictionaryRef d = CFI(0);
    const void *v = NULL;
    Boolean ok = CFDictionaryGetValueIfPresent(d, val_in(d, ARG(1)), &v);
    if (ok && ARG(2)) W32_(g_mem, ARG(2), val_out(d, v));
    RET(ok);
}
SHIM(_CFDictionarySetValue) {
    CFMutableDictionaryRef d = (CFMutableDictionaryRef)CFI(0);
    CFDictionarySetValue(d, val_in(d, ARG(1)), val_in(d, ARG(2)));
}
SHIM(_CFDictionaryGetKeysAndValues) {
    CFDictionaryRef d = CFI(0);
    CFIndex n = CFDictionaryGetCount(d);
    const void **k = malloc(sizeof(void *) * (size_t)(n ? n : 1)), **v = malloc(sizeof(void *) * (size_t)(n ? n : 1));
    CFDictionaryGetKeysAndValues(d, k, v);
    for (CFIndex i = 0; i < n; i++) {
        if (ARG(1)) W32_(g_mem, ARG(1) + 4 * (uint32_t)i, val_out(d, k[i]));
        if (ARG(2)) W32_(g_mem, ARG(2) + 4 * (uint32_t)i, val_out(d, v[i]));
    }
    free(k);
    free(v);
}

/* ---- URLs and paths ---- */
SHIM(_CFURLCreateWithFileSystemPath) { RETCF_OWNED(CFURLCreateWithFileSystemPath(NULL, CFI(1), ARG(2), (Boolean)ARG(3))); }
SHIM(_CFURLCreateWithFileSystemPathRelativeToBase) {
    RETCF_OWNED(CFURLCreateWithFileSystemPathRelativeToBase(NULL, CFI(1), ARG(2), (Boolean)ARG(3), CFI(4)));
}
SHIM(_CFURLCreateFromFileSystemRepresentation) {
    RETCF_OWNED(CFURLCreateFromFileSystemRepresentation(NULL, PTR(ARG(1)), CFINDEX(2), (Boolean)ARG(3)));
}
SHIM(_CFURLCreateWithString) { RETCF_OWNED(CFURLCreateWithString(NULL, CFI(1), CFI(2))); }
SHIM(_CFURLCopyFileSystemPath) { RETCF_OWNED(CFURLCopyFileSystemPath(CFI(0), ARG(1))); }
SHIM(_CFURLGetString) { RETCF(CFURLGetString(CFI(0))); }
SHIM(_CFURLGetFileSystemRepresentation) {
    RET(CFURLGetFileSystemRepresentation(CFI(0), (Boolean)ARG(1), PTR(ARG(2)), CFINDEX(3)));
}
SHIM(_CFURLCreateCopyAppendingPathComponent) {
    RETCF_OWNED(CFURLCreateCopyAppendingPathComponent(NULL, CFI(1), CFI(2), (Boolean)ARG(3)));
}
SHIM(_CFURLCreateCopyDeletingLastPathComponent) { RETCF_OWNED(CFURLCreateCopyDeletingLastPathComponent(NULL, CFI(1))); }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
SHIM(_CFURLCreateDataAndPropertiesFromResource) {
    /* (alloc, url, CFDataRef *data, CFDictionaryRef *props, CFArrayRef desired, SInt32 *err) */
    CFDataRef data = NULL;
    CFDictionaryRef props = NULL;
    SInt32 err = 0;
    Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL, CFI(1), ARG(2) ? &data : NULL, ARG(3) ? &props : NULL,
                                                         CFI(4), &err);
    if (ARG(2)) W32_(g_mem, ARG(2), cf_out_owned(data));
    if (ARG(3)) W32_(g_mem, ARG(3), cf_out_owned(props));
    if (ARG(5)) W32_(g_mem, ARG(5), (uint32_t)err);
    RET(ok);
}
SHIM(_CFURLWriteDataAndPropertiesToResource) {
    /* (url, data, props, SInt32 *err) */
    SInt32 err = 0;
    Boolean ok = CFURLWriteDataAndPropertiesToResource(CFI(0), CFI(1), CFI(2), &err);
    if (ARG(3)) W32_(g_mem, ARG(3), (uint32_t)err);
    RET(ok);
}
#pragma clang diagnostic pop

/* ---- bundles ---- */
SHIM(_CFBundleGetMainBundle) { RETCF(g_main_bundle); }
SHIM(_CFBundleCreate) {
    CFURLRef u = CFI(1);
    CFBundleRef b = CFBundleCreate(NULL, u);
    if (!b && u) {
        /* Old CF accepted bundle URLs not marked as directories; retry as one. */
        CFStringRef p = CFURLCopyFileSystemPath(u, kCFURLPOSIXPathStyle);
        CFURLRef d = CFURLCreateWithFileSystemPath(NULL, p, kCFURLPOSIXPathStyle, true);
        b = CFBundleCreate(NULL, d);
        CFRelease(d);
        if (!b) {
            char buf[1024];
            CFStringGetCString(p, buf, sizeof buf, kCFStringEncodingUTF8);
            rt_log("CFBundleCreate failed for %s", buf);
        }
        CFRelease(p);
    }
    RETCF_OWNED(b);
}
SHIM(_CFBundleCopyBundleURL) { RETCF_OWNED(CFBundleCopyBundleURL(CFI(0))); }
SHIM(_CFBundleCopyResourcesDirectoryURL) { RETCF_OWNED(CFBundleCopyResourcesDirectoryURL(CFI(0))); }
SHIM(_CFBundleCopyResourceURL) { RETCF_OWNED(CFBundleCopyResourceURL(CFI(0), CFI(1), CFI(2), CFI(3))); }
SHIM(_CFBundleCopyLocalizedString) { RETCF_OWNED(CFBundleCopyLocalizedString(CFI(0), CFI(1), CFI(2), CFI(3))); }
SHIM(_CFBundleGetValueForInfoDictionaryKey) { RETCF(CFBundleGetValueForInfoDictionaryKey(CFI(0), CFI(1))); }
SHIM(_CFBundleGetPackageInfo) {
    UInt32 type = 0, creator = 0;
    CFBundleGetPackageInfo(CFI(0), &type, &creator);
    if (ARG(1)) W32_(g_mem, ARG(1), type);
    if (ARG(2)) W32_(g_mem, ARG(2), creator);
}
SHIM(_CFBundleIsExecutableLoaded) { RET(1); }
SHIM(_CFBundleLoadExecutable) { RET(1); }
SHIM(_CFBundleGetFunctionPointerForName) {
    /* Any bundle: hand back a guest-callable thunk for the host shim of that name. */
    char name[256] = "_";
    CFStringGetCString(CFI(1), name + 1, sizeof name - 1, kCFStringEncodingUTF8);
    uint32_t a = rt_dynamic_import(name);
    if (!a) rt_log("CFBundleGetFunctionPointerForName: no shim for %s", name);
    RET(a);
}

/* ---- preferences (passed straight through to the host preference store) ---- */
/* Test runs (tools/try.sh) set CIV4_PREFS_DOMAIN so they never touch the
 * player's real preferences (the game keeps its display mode there). */
static CFStringRef prefs_app(CFStringRef app) {
    static CFStringRef override;
    static int checked;
    if (!checked) {
        checked = 1;
        const char *d = getenv("CIV4_PREFS_DOMAIN");
        if (d && *d) override = CFStringCreateWithCString(NULL, d, kCFStringEncodingUTF8);
    }
    return override && app && app != kCFPreferencesAnyApplication ? override : app;
}
SHIM(_CFPreferencesCopyAppValue) { RETCF_OWNED(CFPreferencesCopyAppValue(CFI(0), prefs_app(CFI(1)))); }
SHIM(_CFPreferencesSetAppValue) { CFPreferencesSetAppValue(CFI(0), CFI(1), prefs_app(CFI(2))); }
SHIM(_CFPreferencesAppSynchronize) { RET(CFPreferencesAppSynchronize(prefs_app(CFI(0)))); }
SHIM(_CFPreferencesGetAppBooleanValue) {
    Boolean valid = 0;
    Boolean r = CFPreferencesGetAppBooleanValue(CFI(0), prefs_app(CFI(1)), &valid);
    if (ARG(2)) W8_(g_mem, ARG(2), valid);
    RET(r);
}
SHIM(_CFPreferencesGetAppIntegerValue) {
    Boolean valid = 0;
    CFIndex r = CFPreferencesGetAppIntegerValue(CFI(0), prefs_app(CFI(1)), &valid);
    if (ARG(2)) W8_(g_mem, ARG(2), valid);
    RET(r);
}

/* ---- UUID ---- */
SHIM(_CFUUIDCreate) { RETCF_OWNED(CFUUIDCreate(NULL)); }
SHIM(_CFUUIDGetUUIDBytes) {
    CFUUIDBytes b = CFUUIDGetUUIDBytes(CFI(1));
    memcpy(PTR(ARG(0)), &b, 16); /* 16-byte struct return through a hidden pointer */
    RET(ARG(0));
    cpu->r[4] += 4;
}

const ShimDef shims_cf[] = {
    SHIM_ENTRY(_CFRetain), SHIM_ENTRY(_CFRelease), SHIM_ENTRY(_CFGetTypeID), SHIM_ENTRY(_CFShow),
    SHIM_ENTRY(_CFAllocatorGetDefault), SHIM_ENTRY(_CFArrayGetTypeID), SHIM_ENTRY(_CFDataGetTypeID),
    SHIM_ENTRY(_CFDateGetTypeID), SHIM_ENTRY(_CFDictionaryGetTypeID), SHIM_ENTRY(_CFNumberGetTypeID),
    SHIM_ENTRY(_CFStringGetTypeID), SHIM_ENTRY(_CFStringCreateWithCString), SHIM_ENTRY(_CFStringCreateWithPascalString),
    SHIM_ENTRY(_CFStringCreateWithBytes), SHIM_ENTRY(_CFStringCreateWithCharacters), SHIM_ENTRY(_CFStringCreateCopy),
    SHIM_ENTRY(_CFStringCreateMutable), SHIM_ENTRY(_CFStringCreateMutableCopy), SHIM_ENTRY(_CFStringGetLength),
    SHIM_ENTRY(_CFStringGetCharacterAtIndex), SHIM_ENTRY(_CFStringGetCharacters), SHIM_ENTRY(_CFStringGetCString),
    SHIM_ENTRY(_CFStringGetCStringPtr), SHIM_ENTRY(_CFStringGetBytes), SHIM_ENTRY(_CFStringGetMaximumSizeForEncoding),
    SHIM_ENTRY(_CFStringGetSystemEncoding), SHIM_ENTRY(_CFStringGetIntValue), SHIM_ENTRY(_CFStringCompare),
    SHIM_ENTRY(_CFStringAppend), SHIM_ENTRY(_CFStringDelete), SHIM_ENTRY(_CFStringReplace),
    SHIM_ENTRY(_CFStringLowercase), SHIM_ENTRY(_CFStringFind), SHIM_ENTRY(_CFStringFindCharacterFromSet),
    SHIM_ENTRY(_CFStringCreateArrayBySeparatingStrings), SHIM_ENTRY(_CFStringCreateWithFormat),
    SHIM_ENTRY(_CFStringAppendFormatAndArguments), SHIM_ENTRY(_CFCharacterSetGetPredefined),
    SHIM_ENTRY(_CFCharacterSetCreateWithCharactersInString), SHIM_ENTRY(_CFCharacterSetIsCharacterMember),
    SHIM_ENTRY(_CFCharacterSetIsLongCharacterMember), SHIM_ENTRY(_CFNumberCreate), SHIM_ENTRY(_CFNumberGetValue),
    SHIM_ENTRY(_CFBooleanGetValue), SHIM_ENTRY(_CFAbsoluteTimeGetCurrent), SHIM_ENTRY(_CFDateCreate),
    SHIM_ENTRY(_CFDateCompare), SHIM_ENTRY(_CFTimeZoneCopyDefault), SHIM_ENTRY(_CFTimeZoneGetSecondsFromGMT),
    SHIM_ENTRY(_CFAbsoluteTimeGetGregorianDate), SHIM_ENTRY(_CFGregorianDateGetAbsoluteTime),
    SHIM_ENTRY(_CFAbsoluteTimeGetDayOfWeek), SHIM_ENTRY(_CFAbsoluteTimeAddGregorianUnits), SHIM_ENTRY(_CFDataCreate),
    SHIM_ENTRY(_CFDataGetLength), SHIM_ENTRY(_CFDataGetBytes), SHIM_ENTRY(_CFDataGetBytePtr),
    SHIM_ENTRY(_CFArrayCreateMutable), SHIM_ENTRY(_CFArrayGetCount), SHIM_ENTRY(_CFArrayGetValueAtIndex),
    SHIM_ENTRY(_CFArrayAppendValue), SHIM_ENTRY(_CFArraySetValueAtIndex), SHIM_ENTRY(_CFArrayGetFirstIndexOfValue),
    SHIM_ENTRY(_CFDictionaryCreateMutable), SHIM_ENTRY(_CFDictionaryCreateMutableCopy),
    SHIM_ENTRY(_CFDictionaryGetCount), SHIM_ENTRY(_CFDictionaryGetValue), SHIM_ENTRY(_CFDictionaryGetValueIfPresent),
    SHIM_ENTRY(_CFDictionarySetValue), SHIM_ENTRY(_CFDictionaryGetKeysAndValues),
    SHIM_ENTRY(_CFURLCreateWithFileSystemPath), SHIM_ENTRY(_CFURLCreateWithFileSystemPathRelativeToBase),
    SHIM_ENTRY(_CFURLCreateFromFileSystemRepresentation), SHIM_ENTRY(_CFURLCreateWithString),
    SHIM_ENTRY(_CFURLCopyFileSystemPath), SHIM_ENTRY(_CFURLGetString), SHIM_ENTRY(_CFURLGetFileSystemRepresentation),
    SHIM_ENTRY(_CFURLCreateCopyAppendingPathComponent), SHIM_ENTRY(_CFURLCreateCopyDeletingLastPathComponent),
    SHIM_ENTRY(_CFURLCreateDataAndPropertiesFromResource), SHIM_ENTRY(_CFURLWriteDataAndPropertiesToResource),
    SHIM_ENTRY(_CFBundleGetMainBundle), SHIM_ENTRY(_CFBundleCreate), SHIM_ENTRY(_CFBundleCopyBundleURL),
    SHIM_ENTRY(_CFBundleCopyResourcesDirectoryURL), SHIM_ENTRY(_CFBundleCopyResourceURL),
    SHIM_ENTRY(_CFBundleCopyLocalizedString), SHIM_ENTRY(_CFBundleGetValueForInfoDictionaryKey),
    SHIM_ENTRY(_CFBundleGetPackageInfo), SHIM_ENTRY(_CFBundleIsExecutableLoaded), SHIM_ENTRY(_CFBundleLoadExecutable),
    SHIM_ENTRY(_CFBundleGetFunctionPointerForName), SHIM_ENTRY(_CFPreferencesCopyAppValue),
    SHIM_ENTRY(_CFPreferencesSetAppValue), SHIM_ENTRY(_CFPreferencesAppSynchronize),
    SHIM_ENTRY(_CFPreferencesGetAppBooleanValue), SHIM_ENTRY(_CFPreferencesGetAppIntegerValue),
    SHIM_ENTRY(_CFUUIDCreate), SHIM_ENTRY(_CFUUIDGetUUIDBytes),
    {0, 0},
};

uint32_t rt_cf_proxy_count(void) { return g_live; }

/* Per-object state kept by this file (called by the sweep, lock held). */
static void forget_host(CFTypeRef h) {
    uint32_t *bytes = map_find(&g_data_bytes, (uintptr_t)h);
    if (bytes) {
        if (!sweep_debug()) gfree(*bytes);
        map_del(&g_data_bytes, (uintptr_t)h);
    }
    map_del(&g_raw_collections, (uintptr_t)h);
}
