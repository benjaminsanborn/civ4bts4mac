/* Writing host implementations ("shims") of functions the guest imports.
 *
 * A shim runs when guest code calls an import. At entry the guest esp points
 * at the return address, so argument n is at esp + 4 + 4n, exactly as the
 * real callee would see it. rt_import pops the return address afterwards.
 *
 *   SHIM(_strlen) { RET(strlen(PTR(ARG(0)))); }
 *
 * and list it in the file's ShimDef table.
 */
#ifndef CIV4_SHIM_H
#define CIV4_SHIM_H

#include "rt.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name; /* Mach-O symbol name, with leading underscore */
    GuestFn fn;
} ShimDef;

#define SHIM(sym) static void sh##sym(CPU *cpu)

/* Replace a guest function (by Mach-O symbol) with host code. Arguments and
 * results work like a SHIM; the generated stub performs the `ret`. */
#define OVERRIDE(sym) void ov_##sym(CPU *cpu)
#define SHIM_ENTRY(sym) {#sym, sh##sym}

#define ARG(n) R32_(g_mem, cpu->r[4] + 4 + 4 * (n))
#define ARG64(n) R64_(g_mem, cpu->r[4] + 4 + 4 * (n))
#define ARGF32(n) RF32_(g_mem, cpu->r[4] + 4 + 4 * (n))
#define ARGF64(n) RF64_(g_mem, cpu->r[4] + 4 + 4 * (n))
#define ARGADDR(n) (cpu->r[4] + 4 + 4 * (n)) /* guest address of argument n (for va_list) */

/* Guest address <-> host pointer. NULL maps to guest 0 and back.
 * Functions, not macros: arguments are often calls (GADDR(mspace_realloc(...)))
 * and must be evaluated exactly once. */
static inline void *PTR(uint32_t a) { return a ? g_mem + a : NULL; }
static inline const char *CPTR(uint32_t a) { return (const char *)PTR(a); }
static inline uint32_t GADDR(const void *p) { return p ? (uint32_t)((const uint8_t *)p - g_mem) : 0u; }

#define RET(v) (cpu->r[0] = (uint32_t)(v))
#define RET64(v) do { uint64_t v64__ = (uint64_t)(v); cpu->r[0] = (uint32_t)v64__; cpu->r[2] = (uint32_t)(v64__ >> 32); } while (0)
#define RETF(v) do { double v__ = (v); cpu->ftop = (cpu->ftop - 1) & 7; cpu->st[cpu->ftop] = v__; } while (0)

/* Guest heap (dlmalloc mspace inside the arena). Returns guest addresses. */
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t n, uint32_t size);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gmemalign(uint32_t align, uint32_t size);
void gfree(uint32_t p);
uint32_t gstrdup(const char *s);

/* Call a guest function from host code (callbacks). cdecl, up to 8 args. */
uint32_t rt_call(CPU *cpu, uint32_t fn, int nargs, ...);

/* Per-host-thread CPU of the running guest thread. */
CPU *rt_cpu(void);

/* Look up a host data cell's guest address by symbol name (0 if absent). */
uint32_t rt_hostdata(const char *name);

void rt_register_shims(const ShimDef *defs);
uint32_t rt_dynamic_import(const char *name);
uint32_t rt_lookup_symbol(const char *name);
const char *rt_build_path(const char *rel, char *buf, size_t n);
_Noreturn void rt_fatal(const char *fmt, ...);
void rt_log(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
