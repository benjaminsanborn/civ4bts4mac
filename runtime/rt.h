/* Runtime interface for statically recompiled i386 code.
 *
 * Guest code lives in a 4 GB arena (g_mem); a guest address is an offset into
 * it, so the original 32-bit pointers and data layouts stay valid. Each
 * recompiled function has the signature `void f(CPU *)` and operates on the
 * guest stack exactly like the original, so every calling convention works.
 */
#ifndef CIV4_RT_H
#define CIV4_RT_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
    uint8_t b[16];
    uint16_t w[8];
    uint32_t d[4];
    uint64_t q[2];
    float f[4];
    double g[2];
} XMM;

typedef struct CPU {
    uint32_t r[8]; /* eax ecx edx ebx esp ebp esi edi */
    XMM xmm[8];
    double st[8]; /* x87 stack, 64-bit precision only */
    uint32_t ftop;
    uint16_t fcw;
    uint32_t tgt; /* indirect call/jump target, for rt_dispatch_l */
} CPU;

typedef void (*GuestFn)(CPU *); /* host shims */

/* Lifted guest functions pass the integer registers as C arguments (esp and
 * ebp packed into one, so everything fits in arm64's eight argument
 * registers) and return eax:edx and esp:ebp in x0/x1. ebx/esi/edi are not
 * returned: compiled i386 code preserves them across calls (the i386 ABI;
 * the original callers rely on it too). cpu->r is only written around
 * imports and read at landing pads, where host code needs the full state. */
typedef struct { uint64_t ad, sb; } GRet; /* eax | edx << 32, esp | ebp << 32 */
#define LARGS CPU *cpu, uint32_t eax, uint32_t ecx, uint32_t edx, uint32_t ebx, uint32_t esi, uint32_t edi, uint64_t spbp_
typedef GRet (*LiftedFn)(LARGS);
static inline GRet gret(uint32_t eax, uint32_t edx, uint32_t esp, uint32_t ebp) {
    GRet r;
    r.ad = eax | (uint64_t)edx << 32;
    r.sb = esp | (uint64_t)ebp << 32;
    return r;
}

/* Guest address of host import `id` (for function pointers to host code). */
#define IMPORT_BASE 0x01C00000u

extern uint8_t *g_mem;

void rt_dispatch(CPU *cpu, uint32_t target);
LiftedFn rt_lookup(uint32_t target);
GRet rt_dispatch_l(LARGS);               /* indirect call to cpu->tgt from lifted code */
void rt_enter(CPU *cpu, LiftedFn fn);     /* call a lifted function with the state in cpu->r */
void rt_import(CPU *cpu, uint32_t id);
void rt_start_pump_ticker(void); /* see plat_pump_busy */
_Noreturn void rt_unimpl(CPU *cpu, uint32_t addr, const char *what);

/* ---- memory ---- */
static inline uint32_t R8_(uint8_t *M, uint32_t a) { return M[a]; }
static inline uint32_t R16_(uint8_t *M, uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t R32_(uint8_t *M, uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t R64_(uint8_t *M, uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void W8_(uint8_t *M, uint32_t a, uint32_t v) { M[a] = (uint8_t)v; }
static inline void W16_(uint8_t *M, uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(M + a, &x, 2); }
static inline void W32_(uint8_t *M, uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void W64_(uint8_t *M, uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline float RF32_(uint8_t *M, uint32_t a) { float v; memcpy(&v, M + a, 4); return v; }
static inline double RF64_(uint8_t *M, uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void WF32_(uint8_t *M, uint32_t a, float v) { memcpy(M + a, &v, 4); }
static inline void WF64_(uint8_t *M, uint32_t a, double v) { memcpy(M + a, &v, 8); }

#define R8(a) R8_(M, (uint32_t)(a))
#define R16(a) R16_(M, (uint32_t)(a))
#define R32(a) R32_(M, (uint32_t)(a))
#define R64(a) R64_(M, (uint32_t)(a))
#define W8(a, v) W8_(M, (uint32_t)(a), (v))
#define W16(a, v) W16_(M, (uint32_t)(a), (v))
#define W32(a, v) W32_(M, (uint32_t)(a), (v))
#define W64(a, v) W64_(M, (uint32_t)(a), (v))
#define RF32(a) RF32_(M, (uint32_t)(a))
#define RF64(a) RF64_(M, (uint32_t)(a))
#define WF32(a, v) WF32_(M, (uint32_t)(a), (v))
#define WF64(a, v) WF64_(M, (uint32_t)(a), (v))

#define PUSH32(v) do { uint32_t v_ = (v); esp -= 4; W32(esp, v_); } while (0)

/* ---- register file ---- */
#define PROLOGUE \
    uint8_t *const M = g_mem; \
    uint32_t eax = cpu->r[0], ecx = cpu->r[1], edx = cpu->r[2], ebx = cpu->r[3]; \
    uint32_t esp = cpu->r[4], ebp = cpu->r[5], esi = cpu->r[6], edi = cpu->r[7]; \
    uint32_t CF = 0, PF = 0, ZF = 0, SF = 0, OF = 0; \
    (void)M; (void)CF; (void)PF; (void)ZF; (void)SF; (void)OF
#define SPILL \
    (cpu->r[0] = eax, cpu->r[1] = ecx, cpu->r[2] = edx, cpu->r[3] = ebx, \
     cpu->r[4] = esp, cpu->r[5] = ebp, cpu->r[6] = esi, cpu->r[7] = edi)
#define RELOAD \
    (eax = cpu->r[0], ecx = cpu->r[1], edx = cpu->r[2], ebx = cpu->r[3], \
     esp = cpu->r[4], ebp = cpu->r[5], esi = cpu->r[6], edi = cpu->r[7])

/* Lifted-function framing (see LARGS). */
#define LPROLOGUE \
    uint8_t *const M = g_mem; \
    uint32_t esp = (uint32_t)spbp_, ebp = (uint32_t)(spbp_ >> 32); \
    uint32_t CF = 0, PF = 0, ZF = 0, SF = 0, OF = 0; \
    (void)M; (void)CF; (void)PF; (void)ZF; (void)SF; (void)OF
#define LRET return gret(eax, edx, esp, ebp)
#define LPASS cpu, eax, ecx, edx, ebx, esi, edi, esp | (uint64_t)ebp << 32
#define LTAKE(r) (eax = (uint32_t)(r).ad, edx = (uint32_t)((r).ad >> 32), esp = (uint32_t)(r).sb, ebp = (uint32_t)((r).sb >> 32))
#define LCALL(f) do { GRet r_ = f(LPASS); LTAKE(r_); } while (0)
#define LTAIL(f) return f(LPASS)

#define XMM(i) (cpu->xmm[i])

/* ---- x87 ---- */
#define ST(i) (cpu->st[(cpu->ftop + (i)) & 7])
#define FPUSH(v) do { double v_ = (v); cpu->ftop = (cpu->ftop - 1) & 7; cpu->st[cpu->ftop] = v_; } while (0)
#define FPOP() (cpu->ftop = (cpu->ftop + 1) & 7)

/* fistp: round per the control word; NaN / out of range gives the "integer
 * indefinite" value, as on x86 (a plain C cast would be undefined). */
static inline int64_t rt_fround(CPU *cpu, double v) {
    switch ((cpu->fcw >> 10) & 3) {
    case 0: v = nearbyint(v); break;
    case 1: v = floor(v); break;
    case 2: v = ceil(v); break;
    default: v = trunc(v); break;
    }
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
    return (int64_t)v;
}

/* 80-bit x87 extended values, converted to/from double (precision is lost). */
static inline double rt_rf80(uint8_t *M, uint32_t a) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, M + a, 8); memcpy(&se, M + a + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
static inline void rt_wf80(uint8_t *M, uint32_t a, double v) {
    uint64_t mant = 0; uint16_t se = signbit(v) ? 0x8000 : 0;
    double x = fabs(v);
    if (isnan(v)) { se |= 0x7fff; mant = 0xc000000000000000ull; }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (x != 0) {
        int e; double f = frexp(x, &e); /* x = f * 2^e, f in [0.5, 1) */
        mant = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(M + a, &mant, 8); memcpy(M + a + 8, &se, 2);
}

/* ---- helpers ---- */
static inline uint32_t parity8(uint32_t v) { return !__builtin_parity(v & 0xff); }

/* x86 cvtt* semantics: NaN / out of range gives the "integer indefinite" value. */
static inline uint32_t cvtt32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}

/* SSE min/max: return the second operand if either is NaN or both are zero. */
static inline float minf_(float a, float b) { return a < b ? a : b; }
static inline float maxf_(float a, float b) { return a > b ? a : b; }
static inline double mind_(double a, double b) { return a < b ? a : b; }
static inline double maxd_(double a, double b) { return a > b ? a : b; }

#ifdef __cplusplus
} /* extern "C" */

/* Thrown by the runtime unwinder to install a guest landing pad: the lifted
 * function whose invocation has entry esp `esp0` resumes at `target`. */
struct GuestUnwind {
    uint32_t target;
    uint32_t esp0;
};
#endif

#endif
