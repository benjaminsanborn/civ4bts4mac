/* Guest C++ exceptions: a host implementation of the _Unwind_* API that the
 * guest's libsupc++ (__cxa_throw, __gxx_personality_v0) calls.
 *
 * Frames are walked on the guest stack using the guest's DWARF CFI
 * (__eh_frame of the game and the guest modules). The guest personality
 * routine decides where to land, exactly as on real hardware. To install a
 * landing pad we throw GuestUnwind{target, esp0} up the host stack; the lifted
 * function owning that guest frame (matched by its entry esp) catches it,
 * reloads registers and jumps to the pad. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "functab.h"
#include "shim.h"

namespace {

enum { URC_NO_REASON = 0, URC_FATAL_PHASE2_ERROR = 2, URC_FATAL_PHASE1_ERROR = 3, URC_END_OF_STACK = 5,
       URC_HANDLER_FOUND = 6, URC_INSTALL_CONTEXT = 7, URC_CONTINUE_UNWIND = 8 };
enum { UA_SEARCH_PHASE = 1, UA_CLEANUP_PHASE = 2, UA_HANDLER_FRAME = 4 };

const uint32_t CALL_RET_MAGIC = 0xFFFF0000u;

/* Darwin i386 eh_frame register numbers (4 and 5 are ebp/esp, swapped vs. SysV)
 * -> CPU register index (eax ecx edx ebx esp ebp esi edi). 8 = return address. */
const int kDwarfToCpu[9] = {0, 1, 2, 3, 5, 4, 6, 7, -1};
const int kRA = 8;

uint8_t rd8(uint32_t a) { return R8_(g_mem, a); }
uint32_t rd32(uint32_t a) { return R32_(g_mem, a); }

uint32_t uleb(uint32_t &a) {
    uint32_t v = 0, shift = 0;
    for (;;) {
        uint8_t b = rd8(a++);
        v |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if (b < 0x80) return v;
    }
}
int32_t sleb(uint32_t &a) {
    int32_t v = 0;
    uint32_t shift = 0;
    uint8_t b;
    do {
        b = rd8(a++);
        v |= (int32_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b >= 0x80);
    if (shift < 32 && (b & 0x40)) v |= -(1 << shift);
    return v;
}

uint32_t encoded(uint32_t &a, uint8_t enc) {
    if (enc == 0xff) return 0;
    uint32_t start = a, v = 0;
    switch (enc & 0x0f) {
    case 0x00: case 0x03: case 0x0b: v = rd32(a); a += 4; break;
    case 0x02: case 0x0a: v = R16_(g_mem, a); if ((enc & 0x0f) == 0x0a) v = (uint32_t)(int32_t)(int16_t)v; a += 2; break;
    case 0x01: v = uleb(a); break;
    case 0x09: v = (uint32_t)sleb(a); break;
    default: rt_fatal("unwind: pointer encoding %02x", enc);
    }
    if ((enc & 0x70) == 0x10) v += start;
    if (enc & 0x80) v = rd32(v);
    return v;
}

struct Cie {
    uint32_t code_align;
    int32_t data_align;
    uint32_t ra;
    uint32_t insns, insns_end;
    uint8_t fde_enc, lsda_enc;
    bool has_z; /* augmentation data present in FDEs */
    uint32_t personality;
};

struct Fde {
    uint32_t pc_begin, pc_end;
    uint32_t insns, insns_end;
    uint32_t lsda;
    uint32_t cie; /* index into g_cies */
};

std::vector<Cie> g_cies;
std::vector<Fde> g_fdes;
bool g_indexed;

void index_eh_frame(uint32_t base, uint32_t size) {
    std::vector<std::pair<uint32_t, uint32_t>> cie_at; /* guest addr -> index */
    uint32_t p = base, end = base + size;
    while (p + 4 <= end) {
        uint32_t len = rd32(p);
        if (!len) break;
        uint32_t rec = p + 4, next = rec + len;
        uint32_t id = rd32(rec);
        if (id == 0) {
            Cie c = {};
            uint32_t a = rec + 5; /* id, version */
            char aug[16] = {0};
            for (int i = 0; i < 15; i++) {
                aug[i] = (char)rd8(a++);
                if (!aug[i]) break;
            }
            c.code_align = uleb(a);
            c.data_align = sleb(a);
            c.ra = rd8(a++);
            c.lsda_enc = 0xff;
            c.has_z = aug[0] == 'z';
            if (c.has_z) {
                uint32_t alen = uleb(a), aend = a + alen;
                for (const char *s = aug + 1; *s; s++) {
                    if (*s == 'R') c.fde_enc = rd8(a++);
                    else if (*s == 'L') c.lsda_enc = rd8(a++);
                    else if (*s == 'P') { uint8_t pe = rd8(a++); c.personality = encoded(a, pe); }
                }
                a = aend;
            }
            c.insns = a;
            c.insns_end = next;
            cie_at.push_back({p, (uint32_t)g_cies.size()});
            g_cies.push_back(c);
        } else {
            uint32_t cie_addr = rec - id;
            uint32_t ci = UINT32_MAX;
            for (auto &kv : cie_at)
                if (kv.first == cie_addr) ci = kv.second;
            if (ci == UINT32_MAX) { p = next; continue; }
            const Cie &c = g_cies[ci];
            Fde f = {};
            uint32_t a = rec + 4;
            f.pc_begin = encoded(a, c.fde_enc);
            f.pc_end = f.pc_begin + encoded(a, c.fde_enc & 0x0f);
            f.cie = ci;
            if (c.has_z) {
                /* 'z' augmentation: data length, then the LSDA pointer if the CIE has 'L'. */
                uint32_t alen = uleb(a), aend = a + alen;
                if (c.lsda_enc != 0xff) {
                    uint32_t la = a;
                    f.lsda = encoded(la, c.lsda_enc);
                }
                a = aend;
            }
            f.insns = a;
            f.insns_end = next;
            g_fdes.push_back(f);
        }
        p = next;
    }
}

void build_index() {
    if (g_indexed) return;
    g_indexed = true;
    for (uint32_t i = 0; i < g_eh_frame_count; i++) index_eh_frame(g_eh_frames[i][0], g_eh_frames[i][1]);
    std::sort(g_fdes.begin(), g_fdes.end(), [](const Fde &a, const Fde &b) { return a.pc_begin < b.pc_begin; });
}

const Fde *find_fde(uint32_t pc) {
    build_index();
    auto it = std::upper_bound(g_fdes.begin(), g_fdes.end(), pc,
                               [](uint32_t v, const Fde &f) { return v < f.pc_begin; });
    if (it == g_fdes.begin()) return nullptr;
    --it;
    return pc < it->pc_end ? &*it : nullptr;
}

/* ---- CFI ---- */
enum RuleKind : uint8_t { R_SAME, R_UNDEF, R_OFFSET, R_REG };
struct Rule {
    RuleKind kind;
    int32_t val;
};
struct CfiState {
    uint32_t cfa_reg;
    int32_t cfa_off;
    Rule rules[9];
};

void run_cfi(const Cie &c, uint32_t a, uint32_t end, uint32_t pc_begin, uint32_t target, CfiState &st,
             const CfiState *initial) {
    uint32_t loc = pc_begin;
    std::vector<CfiState> stack;
    while (a < end && loc <= target) {
        uint8_t op = rd8(a++);
        uint8_t hi = op & 0xc0, lo = op & 0x3f;
        if (hi == 0x40) { loc += lo * c.code_align; continue; }
        if (hi == 0x80) { st.rules[lo < 9 ? lo : 8] = {R_OFFSET, (int32_t)uleb(a) * c.data_align}; continue; }
        if (hi == 0xc0) { if (initial && lo < 9) st.rules[lo] = initial->rules[lo]; continue; }
        switch (op) {
        case 0x00: break;
        case 0x01: loc = encoded(a, c.fde_enc); break;
        case 0x02: loc += rd8(a) * c.code_align; a += 1; break;
        case 0x03: loc += R16_(g_mem, a) * c.code_align; a += 2; break;
        case 0x04: loc += rd32(a) * c.code_align; a += 4; break;
        case 0x05: { uint32_t r = uleb(a); int32_t o = (int32_t)uleb(a) * c.data_align; if (r < 9) st.rules[r] = {R_OFFSET, o}; break; }
        case 0x06: { uint32_t r = uleb(a); if (initial && r < 9) st.rules[r] = initial->rules[r]; break; }
        case 0x07: { uint32_t r = uleb(a); if (r < 9) st.rules[r] = {R_UNDEF, 0}; break; }
        case 0x08: { uint32_t r = uleb(a); if (r < 9) st.rules[r] = {R_SAME, 0}; break; }
        case 0x09: { uint32_t r = uleb(a), r2 = uleb(a); if (r < 9) st.rules[r] = {R_REG, (int32_t)r2}; break; }
        case 0x0a: stack.push_back(st); break;
        case 0x0b: if (!stack.empty()) { st = stack.back(); stack.pop_back(); } break;
        case 0x0c: st.cfa_reg = uleb(a); st.cfa_off = (int32_t)uleb(a); break;
        case 0x0d: st.cfa_reg = uleb(a); break;
        case 0x0e: st.cfa_off = (int32_t)uleb(a); break;
        case 0x11: { uint32_t r = uleb(a); int32_t o = sleb(a) * c.data_align; if (r < 9) st.rules[r] = {R_OFFSET, o}; break; }
        case 0x12: st.cfa_reg = uleb(a); st.cfa_off = sleb(a) * c.data_align; break;
        case 0x13: st.cfa_off = sleb(a) * c.data_align; break;
        case 0x2e: uleb(a); break; /* GNU_args_size */
        case 0x2f: { uint32_t r = uleb(a); int32_t o = -(int32_t)uleb(a) * c.data_align; if (r < 9) st.rules[r] = {R_OFFSET, o}; break; }
        default: rt_fatal("unwind: unsupported CFA op %02x", op);
        }
    }
}

struct Frame {
    uint32_t r[8]; /* CPU register order */
    uint32_t ip;   /* return address into this frame (or landing pad when installing) */
    uint32_t cfa;
    const Fde *fde;
};

/* Compute this frame's CFA and the caller's register state. False at the end of the guest stack. */
bool step(Frame &f, Frame &caller) {
    const Fde *fde = f.fde;
    const Cie &c = g_cies[fde->cie];
    CfiState init = {};
    for (auto &r : init.rules) r = {R_SAME, 0};
    run_cfi(c, c.insns, c.insns_end, fde->pc_begin, UINT32_MAX, init, nullptr);
    CfiState st = init;
    run_cfi(c, fde->insns, fde->insns_end, fde->pc_begin, f.ip - 1, st, &init);
    int cr = st.cfa_reg < 9 ? kDwarfToCpu[st.cfa_reg] : -1;
    if (cr < 0) rt_fatal("unwind: CFA register %u", st.cfa_reg);
    f.cfa = f.r[cr] + (uint32_t)st.cfa_off;
    caller = f;
    for (int dr = 0; dr < 9; dr++) {
        const Rule &rule = st.rules[dr];
        uint32_t v;
        if (rule.kind == R_OFFSET) v = rd32(f.cfa + (uint32_t)rule.val);
        else if (rule.kind == R_REG) v = rule.val < 8 ? f.r[kDwarfToCpu[rule.val]] : 0;
        else continue;
        if (dr == kRA) caller.ip = v;
        else caller.r[kDwarfToCpu[dr]] = v;
    }
    if (st.rules[kRA].kind != R_OFFSET && st.rules[kRA].kind != R_REG) return false;
    caller.r[4] = f.cfa; /* esp of the caller = CFA */
    if (caller.ip == CALL_RET_MAGIC || caller.ip == 0) return false;
    caller.fde = find_fde(caller.ip - 1);
    return caller.fde != nullptr;
}

/* The context handed to the personality: one guest block per thread; the
 * host frame it refers to is the thread's current frame. */
thread_local Frame *t_ctx_frame;
thread_local uint32_t t_ctx_guest;
thread_local bool t_ip_set;

uint32_t ctx_handle() {
    if (!t_ctx_guest) t_ctx_guest = gcalloc(1, 16);
    return t_ctx_guest;
}

int call_personality(CPU *cpu, Frame &f, int actions, uint32_t ue) {
    uint32_t pers = g_cies[f.fde->cie].personality;
    if (!pers) return URC_CONTINUE_UNWIND;
    t_ctx_frame = &f;
    t_ip_set = false;
    uint32_t cls_lo = rd32(ue), cls_hi = rd32(ue + 4);
    return (int)rt_call(cpu, pers, 6, 1, actions, cls_lo, cls_hi, ue, ctx_handle());
}

/* Initial frame: the guest function that called the _Unwind_* import. */
Frame caller_of_import(CPU *cpu) {
    Frame f = {};
    memcpy(f.r, cpu->r, sizeof f.r);
    f.ip = rd32(cpu->r[4]);
    f.r[4] = cpu->r[4] + 4;
    f.fde = find_fde(f.ip - 1);
    return f;
}

bool debug_unwind();
[[noreturn]] void install(CPU *cpu, const Frame &f) {
    if (debug_unwind()) rt_log("unwind: install ip %08x esp %08x esp0 %08x", f.ip, f.r[4], f.cfa - 4);
    memcpy(cpu->r, f.r, sizeof f.r);
    /* The landing pad's frame: its CFA is computed from the same CFI at the pad. */
    throw GuestUnwind{f.ip, f.cfa - 4};
}

bool debug_unwind() {
    static int d = -1;
    if (d < 0) d = getenv("CIV4_DEBUG_UNWIND") != nullptr;
    return d;
}

int phase2(CPU *cpu, Frame f, uint32_t ue) {
    uint32_t handler_cfa = rd32(ue + 16);
    for (;;) {
        if (!f.fde) {
            if (debug_unwind()) rt_log("unwind2: no FDE for ip %08x", f.ip);
            return URC_END_OF_STACK;
        }
        Frame caller;
        bool more = step(f, caller); /* fills f.cfa */
        int actions = UA_CLEANUP_PHASE | (f.cfa == handler_cfa ? UA_HANDLER_FRAME : 0);
        int r = call_personality(cpu, f, actions, ue);
        if (debug_unwind())
            rt_log("unwind2: ip %08x cfa %08x handler_cfa %08x actions %d -> %d (more %d, caller ip %08x)", f.ip, f.cfa,
                   handler_cfa, actions, r, more, caller.ip);
        if (r == URC_INSTALL_CONTEXT) install(cpu, f);
        if (r != URC_CONTINUE_UNWIND) return URC_FATAL_PHASE2_ERROR;
        if (actions & UA_HANDLER_FRAME) return URC_FATAL_PHASE2_ERROR; /* handler frame must install */
        if (!more) return URC_END_OF_STACK;
        f = caller;
    }
}

bool debug_unwind();
int raise_exception(CPU *cpu, uint32_t ue) {
    Frame start = caller_of_import(cpu);
    /* Phase 1: search for a handler. */
    Frame f = start;
    for (;;) {
        if (!f.fde) {
            if (debug_unwind()) rt_log("unwind: no FDE for ip %08x", f.ip);
            return URC_END_OF_STACK;
        }
        Frame caller;
        bool more = step(f, caller);
        int r = call_personality(cpu, f, UA_SEARCH_PHASE, ue);
        if (debug_unwind())
            rt_log("unwind: ip %08x fde [%08x,%08x) cfa %08x esp %08x ebp %08x -> caller ip %08x (more %d) pers %d", f.ip,
                   f.fde->pc_begin, f.fde->pc_end, f.cfa, f.r[4], f.r[5], caller.ip, more, r);
        if (r == URC_HANDLER_FOUND) {
            W32_(g_mem, ue + 12, 0);     /* private_1: not a forced unwind */
            W32_(g_mem, ue + 16, f.cfa); /* private_2: handler frame */
            break;
        }
        if (r != URC_CONTINUE_UNWIND) return URC_FATAL_PHASE1_ERROR;
        if (!more) return URC_END_OF_STACK;
        f = caller;
    }
    if (debug_unwind()) {
        char buf[256] = "";
        for (int i = 12; i >= 1; i--) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " %08x", rd32(ue - 4 * i));
        rt_log("unwind: exception header before ue %08x:%s", ue, buf);
    }
    /* Phase 2: run cleanups up to and including the handler frame. */
    return phase2(cpu, start, ue);
}

} // namespace

/* ---- the _Unwind_* API (struct _Unwind_Exception: class 0, cleanup 8, private_1 12, private_2 16) ---- */
extern "C" {

SHIM(__Unwind_RaiseException) { RET(raise_exception(cpu, ARG(0))); }
SHIM(__Unwind_Resume) {
    /* Called at the end of a landing pad's cleanup: continue phase 2 from its frame. */
    int r = phase2(cpu, caller_of_import(cpu), ARG(0));
    rt_fatal("_Unwind_Resume failed (%d)", r);
}
SHIM(__Unwind_Resume_or_Rethrow) {
    uint32_t ue = ARG(0);
    if (rd32(ue + 12) == 0) { RET(raise_exception(cpu, ue)); return; }
    int r = phase2(cpu, caller_of_import(cpu), ue);
    RET(r);
}
SHIM(__Unwind_DeleteException) {
    uint32_t ue = ARG(0), cleanup = rd32(ue + 8);
    if (cleanup) rt_call(cpu, cleanup, 2, 1 /* _URC_FOREIGN_EXCEPTION_CAUGHT */, ue);
}
SHIM(__Unwind_GetIP) { RET(t_ctx_frame->ip); }
SHIM(__Unwind_SetIP) { t_ctx_frame->ip = ARG(1); t_ip_set = true; }
SHIM(__Unwind_GetGR) {
    uint32_t r = ARG(1);
    RET(r < 8 ? t_ctx_frame->r[kDwarfToCpu[r]] : 0);
}
SHIM(__Unwind_SetGR) {
    uint32_t r = ARG(1);
    if (r < 8) t_ctx_frame->r[kDwarfToCpu[r]] = ARG(2);
}
SHIM(__Unwind_GetLanguageSpecificData) { RET(t_ctx_frame->fde->lsda); }
SHIM(__Unwind_GetRegionStart) { RET(t_ctx_frame->fde->pc_begin); }
SHIM(__Unwind_GetDataRelBase) { RET(0); }
SHIM(__Unwind_GetTextRelBase) { RET(0); }

extern const ShimDef shims_unwind[];
const ShimDef shims_unwind[] = {
    SHIM_ENTRY(__Unwind_RaiseException), SHIM_ENTRY(__Unwind_Resume), SHIM_ENTRY(__Unwind_Resume_or_Rethrow),
    SHIM_ENTRY(__Unwind_DeleteException), SHIM_ENTRY(__Unwind_GetIP), SHIM_ENTRY(__Unwind_SetIP),
    SHIM_ENTRY(__Unwind_GetGR), SHIM_ENTRY(__Unwind_SetGR), SHIM_ENTRY(__Unwind_GetLanguageSpecificData),
    SHIM_ENTRY(__Unwind_GetRegionStart), SHIM_ENTRY(__Unwind_GetDataRelBase), SHIM_ENTRY(__Unwind_GetTextRelBase),
    {0, 0},
};

} // extern "C"
