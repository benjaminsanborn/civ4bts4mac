/* Old dyld symbol lookup (NSLookupAndBindSymbol & co.), used by the game's
 * Win32 GetProcAddress emulation. Resolves against every exported guest
 * symbol, then against host shims. */
#include "shim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name; uint32_t addr; } Export;
static Export *g_exports;
static size_t g_nexports;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int cmp_export(const void *a, const void *b) {
    return strcmp(((const Export *)a)->name, ((const Export *)b)->name);
}

static void load_exports(void) {
    const char *path = getenv("CIV4_EXPORTS");
    FILE *f = fopen(path ? path : "build/gen/exports.txt", "r");
    if (!f) rt_fatal("cannot open exports table");
    size_t cap = 1 << 17;
    g_exports = malloc(cap * sizeof *g_exports);
    char line[4096], name[4000];
    unsigned a;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%x %3999s", &a, name) != 2) continue;
        if (g_nexports == cap) g_exports = realloc(g_exports, (cap *= 2) * sizeof *g_exports);
        g_exports[g_nexports++] = (Export){strdup(name), a};
    }
    fclose(f);
    qsort(g_exports, g_nexports, sizeof *g_exports, cmp_export);
}

/* Guest address for a Mach-O symbol name (with leading underscore), or 0. */
uint32_t rt_lookup_symbol(const char *name) {
    pthread_once(&g_once, load_exports);
    Export key = {(char *)name, 0};
    Export *e = bsearch(&key, g_exports, g_nexports, sizeof *g_exports, cmp_export);
    if (e) return e->addr;
    return rt_dynamic_import(name);
}

SHIM(_NSIsSymbolNameDefined) {
    uint32_t a = rt_lookup_symbol(CPTR(ARG(0)));
    if (!a) rt_log("NSIsSymbolNameDefined: %s not found", CPTR(ARG(0)));
    RET(a != 0);
}
/* NSSymbol handles are simply the symbol's guest address. */
SHIM(_NSLookupAndBindSymbol) { RET(rt_lookup_symbol(CPTR(ARG(0)))); }
SHIM(_NSAddressOfSymbol) { RET(ARG(0)); }

const ShimDef shims_dyld[] = {
    SHIM_ENTRY(_NSIsSymbolNameDefined), SHIM_ENTRY(_NSLookupAndBindSymbol), SHIM_ENTRY(_NSAddressOfSymbol),
    {0, 0},
};
