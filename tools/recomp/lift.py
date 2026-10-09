"""i386 -> C static recompiler.

Each guest function becomes `void f_XXXXXXXX(CPU *cpu)`. Control flow inside a
function is recovered by recursive descent from its entry; flags are kept in C
locals and only computed where a later instruction can read them (backward
liveness over the instruction-level CFG).
"""
import struct
from typing import Dict, List, Optional, Set, Tuple

import capstone
from capstone import x86 as X

from world import IMPORT_BASE, World

OP_REG, OP_IMM, OP_MEM = X.X86_OP_REG, X.X86_OP_IMM, X.X86_OP_MEM

FC, FP, FZ, FS, FO = 1, 2, 4, 8, 16
ALLF = FC | FP | FZ | FS | FO
FLAG_NAMES = [(FC, "CF"), (FP, "PF"), (FZ, "ZF"), (FS, "SF"), (FO, "OF")]

COND_USES = {
    "o": FO, "no": FO, "b": FC, "ae": FC, "e": FZ, "ne": FZ, "be": FC | FZ, "a": FC | FZ,
    "s": FS, "ns": FS, "p": FP, "np": FP, "l": FS | FO, "ge": FS | FO, "le": FZ | FS | FO,
    "g": FZ | FS | FO,
}
COND_EXPR = {
    "o": "OF", "no": "!OF", "b": "CF", "ae": "!CF", "e": "ZF", "ne": "!ZF",
    "be": "(CF | ZF)", "a": "(!CF && !ZF)", "s": "SF", "ns": "!SF", "p": "PF", "np": "!PF",
    "l": "(SF != OF)", "ge": "(SF == OF)", "le": "(ZF || SF != OF)", "g": "(!ZF && SF == OF)",
}

GPR32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
GPR16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
GPR8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
GPR8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

NORETURN_IMPORTS = {
    "_abort", "_exit", "__exit", "___cxa_throw", "___cxa_rethrow", "__Unwind_Resume",
    "___stack_chk_fail", "___assert_rtn", "__ZSt9terminatev", "_longjmp", "_siglongjmp",
    "___cxa_call_unexpected", "_err", "_errx", "___cxa_bad_cast", "___cxa_bad_typeid",
    "___cxa_pure_virtual",
}

MASK = {8: "0xffu", 16: "0xffffu", 32: "0xffffffffu"}


def cond_of(mnem: str, prefix: str) -> Optional[str]:
    if mnem.startswith(prefix):
        c = mnem[len(prefix):]
        if c in COND_USES:
            return c
    return None


class Unsupported(Exception):
    pass


class Program:
    """The code to lift: game executable plus guest modules, as resolved by a World."""

    def __init__(self, world: World):
        self.w = world
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.ranges = sorted(world.code_ranges())
        self.code = [(lo, hi, world.read(lo, hi - lo)) for lo, hi in self.ranges]
        funcs = [(a, n) for a, n in world.functions() if self.in_text(a)]
        self.names = dict(funcs)
        self.starts = [a for a, _ in funcs]
        self.ends = {}
        for a, b in zip(self.starts, self.starts[1:] + [None]):
            hi = self.range_end(a)
            self.ends[a] = min(b, hi) if b is not None else hi
        # Stubs resolve either to guest code (a direct call) or to a host import.
        self.stub_import: Dict[int, int] = {}
        self.stub_local: Dict[int, int] = {}
        for addr, (kind, v) in world.stub_targets().items():
            if kind == "guest":
                self.stub_local[addr] = v
            elif kind == "host":
                self.stub_import[addr] = v
        self.import_names = world.import_names
        self.import_id = world.import_id

    # ---- address helpers ----
    def in_text(self, a: int) -> bool:
        return any(lo <= a < hi for lo, hi in self.ranges)

    def range_end(self, a: int) -> int:
        for lo, hi in self.ranges:
            if lo <= a < hi:
                return hi
        return a

    def func_end(self, entry: int) -> int:
        if entry in self.ends:
            return self.ends[entry]
        import bisect
        i = bisect.bisect_right(self.starts, entry)
        hi = self.range_end(entry)
        return min(self.starts[i], hi) if i < len(self.starts) else hi

    def resolve_call(self, target: int) -> Tuple[str, int]:
        """Classify a direct call/jump target: ('import', id) or ('func', addr)."""
        if target in self.stub_import:
            return "import", self.stub_import[target]
        if target in self.stub_local:
            return "func", self.stub_local[target]
        if IMPORT_BASE <= target < IMPORT_BASE + 4 * len(self.import_names):
            return "import", (target - IMPORT_BASE) // 4
        return "func", target

    def import_name(self, i: int) -> str:
        return self.import_names[i]

    def decode(self, addr: int):
        for lo, hi, code in self.code:
            if lo <= addr < hi:
                off = addr - lo
                for insn in self.md.disasm(code[off:off + 16], addr, 1):
                    return insn
        return None

    def read32(self, addr: int) -> Optional[int]:
        try:
            return struct.unpack("<I", self.w.read(addr, 4))[0]
        except (KeyError, struct.error):
            return None


class Func:
    """One lifted function: discovery, flag liveness, C emission."""

    def __init__(self, prog: Program, entry: int):
        self.p = prog
        self.entry = entry
        self.end = prog.func_end(entry)
        self.insns: Dict[int, object] = {}
        self.tables: Dict[int, List[int]] = {}
        self.labels: Set[int] = set()
        self.calls: Set[int] = set()  # guest functions referenced directly
        self.unsupported: List[str] = []
        self.ext_targets: Set[int] = set()

    # ---- discovery ----
    def discover(self) -> None:
        work = [self.entry]
        while work:
            a = work.pop()
            while a not in self.insns:
                if not (self.entry <= a < self.end):
                    break
                insn = self.p.decode(a)
                if insn is None:
                    break
                self.insns[a] = insn
                mn = insn.mnemonic
                nxt = a + insn.size
                if mn.startswith("j"):
                    op = insn.operands[0]
                    if mn == "jmp":
                        if op.type == OP_IMM:
                            t = op.imm & 0xffffffff
                            if self.entry <= t < self.end:
                                work.append(t)
                                self.labels.add(t)
                        else:
                            tbl = self.jump_table(insn)
                            if tbl:
                                self.tables[a] = tbl
                                for t in tbl:
                                    work.append(t)
                                    self.labels.add(t)
                        break
                    t = op.imm & 0xffffffff
                    if self.entry <= t < self.end:
                        work.append(t)
                        self.labels.add(t)
                    a = nxt
                    continue
                if mn in ("ret", "hlt", "int", "ud2"):
                    break
                if mn == "call" and insn.operands[0].type == OP_IMM:
                    kind, val = self.p.resolve_call(insn.operands[0].imm & 0xffffffff)
                    if kind == "import" and _noreturn(self.p.import_name(val)):
                        break
                    if kind == "func" and _noreturn(self.p.names.get(val, "")):
                        break
                a = nxt

    def jump_table(self, insn) -> List[int]:
        op = insn.operands[0]
        if op.type != OP_MEM or op.mem.scale != 4 or op.mem.base != 0 or op.mem.index == 0:
            return []
        base = op.mem.disp & 0xffffffff
        idx_reg = insn.reg_name(op.mem.index)
        # Look for the bounds check `cmp idx, N; ja default` just before the jump.
        count = None
        prev = sorted(k for k in self.insns if k < insn.address)[-8:]
        for k in reversed(prev):
            pi = self.insns[k]
            if pi.mnemonic == "cmp" and len(pi.operands) == 2 and pi.operands[1].type == OP_IMM:
                o0 = pi.operands[0]
                if o0.type == OP_REG and _reg32(pi.reg_name(o0.reg)) == idx_reg:
                    count = (pi.operands[1].imm & 0xffffffff) + 1
                    break
        out = []
        for i in range(count if count is not None else 4096):
            v = self.p.read32(base + 4 * i)
            if v is None or not (self.entry <= v < self.end):
                if count is None:
                    break
                return []
            out.append(v)
        return out

    # ---- flag liveness ----
    def succs(self, a: int) -> List[int]:
        insn = self.insns[a]
        mn = insn.mnemonic
        nxt = a + insn.size
        if mn in ("ret", "hlt", "int", "ud2"):
            return []
        if mn == "jmp":
            op = insn.operands[0]
            if op.type == OP_IMM:
                t = op.imm & 0xffffffff
                return [t] if t in self.insns else []
            return [t for t in self.tables.get(a, []) if t in self.insns]
        out = [nxt] if nxt in self.insns else []
        if mn.startswith("j"):
            t = insn.operands[0].imm & 0xffffffff
            if t in self.insns:
                out.append(t)
        return out

    def flag_effects(self, insn) -> Tuple[int, int]:
        """(uses, defs) as flag masks. defs are must-defs."""
        mn = base_mnemonic(insn)
        for pre in ("j", "set", "cmov"):
            c = cond_of(mn, pre)
            if c:
                return COND_USES[c], 0
        if mn in ("add", "sub", "cmp", "neg", "and", "or", "xor", "test", "shld", "shrd",
                  "cmpsb", "cmpsd", "cmpsw", "scasb", "scasd", "scasw"):
            if mn in ("shld", "shrd") and insn.operands[2].type != OP_IMM:
                return ALLF, 0
            if mn in ("shld", "shrd") and (insn.operands[2].imm & 31) == 0:
                return 0, 0
            if insn.prefix[0] in (0xF3, 0xF2) and mn.startswith(("cmps", "scas")):
                return ALLF, 0  # zero-count rep leaves flags untouched
            return 0, ALLF
        if mn in ("adc", "sbb"):
            return FC, ALLF
        if mn in ("inc", "dec"):
            return 0, FP | FZ | FS | FO
        if mn in ("shl", "sal", "shr", "sar", "rol", "ror"):
            cnt = insn.operands[1] if len(insn.operands) > 1 else None
            if cnt is not None and cnt.type != OP_IMM:
                return ALLF, 0  # count may be zero: flags conditionally unchanged
            n = (cnt.imm if cnt is not None else 1) & 31
            if n == 0:
                return 0, 0
            if mn in ("rol", "ror"):
                return 0, FC | FO
            return 0, ALLF
        if mn in ("imul", "mul", "div", "idiv", "bsr", "bsf", "ucomiss", "ucomisd",
                  "comiss", "comisd", "call"):
            return 0, ALLF
        if mn in ("bt", "bts", "btr", "btc"):
            return 0, FC
        if mn == "jecxz":
            return 0, 0
        return 0, 0

    def liveness(self) -> Dict[int, int]:
        addrs = sorted(self.insns)
        eff = {a: self.flag_effects(self.insns[a]) for a in addrs}
        succ = {a: self.succs(a) for a in addrs}
        live_in = {a: 0 for a in addrs}
        live_out = {a: 0 for a in addrs}
        changed = True
        while changed:
            changed = False
            for a in reversed(addrs):
                out = 0
                for s in succ[a]:
                    out |= live_in[s]
                uses, defs = eff[a]
                inn = uses | (out & ~defs)
                if out != live_out[a] or inn != live_in[a]:
                    live_out[a] = out
                    live_in[a] = inn
                    changed = True
        return live_out

    # ---- emission ----
    def lift(self) -> str:
        self.discover()
        live = self.liveness()
        addrs = sorted(self.insns)
        body: List[str] = []
        for i, a in enumerate(addrs):
            insn = self.insns[a]
            if a in self.labels:
                body.append(f"L_{a:x}:;")
            body.append(f"    /* {a:x}: {insn.mnemonic} {insn.op_str} */")
            try:
                stmts = Emit(self, insn, live[a]).run()
            except Unsupported as e:
                self.unsupported.append(f"{insn.mnemonic} {insn.op_str} ({e})")
                stmts = [f'rt_unimpl(cpu, 0x{a:x}u, "{insn.mnemonic} {insn.op_str}");']
            body.extend("    " + s for s in stmts)
            # Fallthrough to an address that is not emitted next.
            nxt = a + insn.size
            if _falls_through(insn) and (i + 1 >= len(addrs) or addrs[i + 1] != nxt):
                if nxt in self.insns:
                    self.labels.add(nxt)
                    body.append(f"    goto L_{nxt:x};")
                elif nxt in self.p.ends:
                    body.extend("    " + s for s in self.tail_call(nxt))
                else:  # after a no-return call: padding or inline data follows
                    body.append(f'    rt_unimpl(cpu, 0x{nxt:x}u, "fell off function end");')
        # Labels can be added late (fallthrough gotos); re-emit them.
        out = [f"void f_{self.entry:08x}(CPU *cpu) {{", "    PROLOGUE;"]
        for line in body:
            out.append(line)
        text = "\n".join(out)
        for a in self.labels:
            if f"L_{a:x}:;" not in text and a in self.insns:
                text = text.replace(f"    /* {a:x}: ", f"L_{a:x}:;\n    /* {a:x}: ", 1)
        return text + "\n}\n"

    def tail_call(self, target: int) -> List[str]:
        kind, val = self.p.resolve_call(target)
        if kind == "import":
            return [f"SPILL; rt_import(cpu, {val}); /* {self.p.import_name(val)} */ return;"]
        if not self.p.in_text(val):
            return [f'rt_unimpl(cpu, 0x{target:x}u, "jump outside text");']
        self.calls.add(val)
        return [f"SPILL; f_{val:08x}(cpu); return;"]


def _noreturn(name: str) -> bool:
    return name in NORETURN_IMPORTS or "__throw_" in name


def base_mnemonic(insn) -> str:
    """Mnemonic without rep/repe/repne (the prefix byte is checked separately)."""
    mn = insn.mnemonic
    return mn.split(" ", 1)[1] if " " in mn else mn


def _reg32(name: str) -> str:
    if name in GPR32:
        return name
    return GPR16.get(name) or GPR8L.get(name) or GPR8H.get(name) or name


def _falls_through(insn) -> bool:
    mn = insn.mnemonic
    return mn not in ("jmp", "ret", "hlt", "int", "ud2")


class Emit:
    """Translate one instruction into C statements."""

    def __init__(self, fn: Func, insn, live_out: int):
        self.f = fn
        self.i = insn
        self.ops = insn.operands
        self.live = live_out
        self.next = insn.address + insn.size

    # ---- operand access ----
    def rn(self, reg: int) -> str:
        return self.i.reg_name(reg)

    def ea(self, op) -> str:
        m = op.mem
        if m.segment != 0:
            raise Unsupported("segment override")
        parts = []
        if m.base:
            parts.append(_reg32(self.rn(m.base)))
        if m.index:
            idx = _reg32(self.rn(m.index))
            parts.append(idx if m.scale == 1 else f"{idx} * {m.scale}u")
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xffffffff:x}u")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def bits(self, op) -> int:
        return op.size * 8

    def rd(self, op, size: Optional[int] = None) -> str:
        size = size or self.bits(op)
        if op.type == OP_IMM:
            return f"0x{op.imm & ((1 << size) - 1):x}u"
        if op.type == OP_REG:
            name = self.rn(op.reg)
            if name in GPR32:
                return name
            if name in GPR16:
                return f"({GPR16[name]} & 0xffffu)"
            if name in GPR8L:
                return f"({GPR8L[name]} & 0xffu)"
            if name in GPR8H:
                return f"(({GPR8H[name]} >> 8) & 0xffu)"
            raise Unsupported("reg " + name)
        if op.type == OP_MEM:
            return {8: "R8", 16: "R16", 32: "R32"}[size] + "(" + self.ea(op) + ")"
        raise Unsupported("operand")

    def wr(self, op, val: str) -> str:
        if op.type == OP_REG:
            name = self.rn(op.reg)
            if name in GPR32:
                return f"{name} = (uint32_t)({val});"
            if name in GPR16:
                r = GPR16[name]
                return f"{r} = ({r} & 0xffff0000u) | (({val}) & 0xffffu);"
            if name in GPR8L:
                r = GPR8L[name]
                return f"{r} = ({r} & 0xffffff00u) | (({val}) & 0xffu);"
            if name in GPR8H:
                r = GPR8H[name]
                return f"{r} = ({r} & 0xffff00ffu) | ((({val}) & 0xffu) << 8);"
            raise Unsupported("reg " + name)
        if op.type == OP_MEM:
            return {8: "W8", 16: "W16", 32: "W32"}[self.bits(op)] + f"({self.ea(op)}, {val});"
        raise Unsupported("write operand")

    def xmm(self, op) -> Optional[int]:
        if op.type == OP_REG:
            name = self.rn(op.reg)
            if name.startswith("xmm"):
                return int(name[3:])
        return None

    # ---- flags ----
    def want(self, f: int) -> bool:
        return bool(self.live & f)

    def szp(self, n: int, r: str = "r_") -> List[str]:
        out = []
        if self.want(FZ):
            out.append(f"ZF = ({r} == 0);")
        if self.want(FS):
            out.append(f"SF = ({r} >> {n - 1}) & 1;")
        if self.want(FP):
            out.append(f"PF = parity8({r});")
        return out

    def arith(self, kind: str, n: int, a: str, b: str, dst=None, carry_in: bool = False) -> List[str]:
        """add/sub/adc/sbb/cmp with masked n-bit results; writes dst if given."""
        mk = MASK[n]
        s = [f"{{ uint32_t a_ = ({a}) & {mk}, b_ = ({b}) & {mk};"]
        cin = " + CF" if carry_in and kind == "add" else (" - CF" if carry_in else "")
        if kind == "add":
            s.append(f"uint32_t r_ = (a_ + b_{cin}) & {mk};")
            if self.want(FC):
                s.append("CF = CF ? (r_ <= a_) : (r_ < a_);" if carry_in else "CF = r_ < a_;")
            if self.want(FO):
                s.append(f"OF = (((a_ ^ r_) & (b_ ^ r_)) >> {n - 1}) & 1;")
        else:
            s.append(f"uint32_t r_ = (a_ - b_{cin}) & {mk};")
            if self.want(FO):
                s.append(f"OF = (((a_ ^ b_) & (a_ ^ r_)) >> {n - 1}) & 1;")
            if self.want(FC):
                s.append("CF = CF ? (a_ <= b_) : (a_ < b_);" if carry_in else "CF = a_ < b_;")
        s.extend(self.szp(n))
        if dst is not None:
            s.append(self.wr(dst, "r_"))
        s.append("}")
        return [" ".join(s)]

    def logic(self, n: int, expr: str, dst=None) -> List[str]:
        s = [f"{{ uint32_t r_ = ({expr}) & {MASK[n]};"]
        if self.want(FC):
            s.append("CF = 0;")
        if self.want(FO):
            s.append("OF = 0;")
        s.extend(self.szp(n))
        if dst is not None:
            s.append(self.wr(dst, "r_"))
        s.append("}")
        return [" ".join(s)]

    # ---- dispatch ----
    def run(self) -> List[str]:
        mn = base_mnemonic(self.i)
        for pre, h in (("j", self.do_jcc), ("set", self.do_setcc), ("cmov", self.do_cmov)):
            c = cond_of(mn, pre)
            if c:
                return h(c)
        h = getattr(self, "i_" + mn.replace(" ", "_"), None)
        if h is None:
            raise Unsupported("no handler")
        return h()

    # ---- control flow ----
    def do_jcc(self, c: str) -> List[str]:
        t = self.ops[0].imm & 0xffffffff
        return [f"if ({COND_EXPR[c]}) " + self.jump_to(t)]

    def jump_to(self, t: int) -> str:
        if t in self.f.insns:
            return f"goto L_{t:x};"
        return "{ " + " ".join(self.f.tail_call(t)) + " }"

    def i_jecxz(self):
        return [f"if (ecx == 0) " + self.jump_to(self.ops[0].imm & 0xffffffff)]

    def i_jmp(self):
        op = self.ops[0]
        if op.type == OP_IMM:
            return [self.jump_to(op.imm & 0xffffffff)]
        tbl = self.f.tables.get(self.i.address)
        s = [f"{{ uint32_t t_ = {self.rd(op)};"]
        if tbl:
            s.append("switch (t_) {")
            for t in sorted(set(tbl)):
                s.append(f"case 0x{t:x}u: goto L_{t:x};")
            s.append("}")
        s.append("SPILL; rt_dispatch(cpu, t_); return; }")
        return [" ".join(s)]

    def i_call(self):
        op = self.ops[0]
        if op.type == OP_IMM:
            t = op.imm & 0xffffffff
            if t == self.next:
                return [f"PUSH32(0x{self.next:x}u);"]
            kind, val = self.f.p.resolve_call(t)
            if kind == "import":
                return [f"PUSH32(0x{self.next:x}u); SPILL; rt_import(cpu, {val}); RELOAD; /* {self.f.p.import_name(val)} */"]
            if not self.f.p.in_text(val):
                raise Unsupported("call outside text")
            self.f.calls.add(val)
            return [f"PUSH32(0x{self.next:x}u); SPILL; f_{val:08x}(cpu); RELOAD;"]
        return [f"{{ uint32_t t_ = {self.rd(op)}; PUSH32(0x{self.next:x}u); SPILL; rt_dispatch(cpu, t_); RELOAD; }}"]

    def i_ret(self):
        n = 4 + (self.ops[0].imm if self.ops else 0)
        return [f"esp += {n}u; SPILL; return;"]

    def i_leave(self):
        return ["esp = ebp; ebp = R32(esp); esp += 4;"]

    def i_push(self):
        return [f"PUSH32({self.rd(self.ops[0])});"]

    def i_pop(self):
        op = self.ops[0]
        if op.type == OP_REG and self.rn(op.reg) == "esp":
            return ["esp = R32(esp);"]
        return [f"{{ uint32_t v_ = R32(esp); esp += 4; {self.wr(op, 'v_')} }}"]

    def i_nop(self):
        return []

    i_cld = i_nop

    def i_hlt(self):
        return [f'rt_unimpl(cpu, 0x{self.i.address:x}u, "hlt");']

    i_int = i_hlt
    i_ud2 = i_hlt

    # ---- data movement ----
    def i_mov(self):
        d, s = self.ops
        return [self.wr(d, self.rd(s, self.bits(d)))]

    def i_lea(self):
        return [self.wr(self.ops[0], self.ea(self.ops[1]))]

    def i_movzx(self):
        return [self.wr(self.ops[0], self.rd(self.ops[1]))]

    def i_movsx(self):
        d, s = self.ops
        t = {8: "int8_t", 16: "int16_t"}[self.bits(s)]
        return [self.wr(d, f"(uint32_t)(int32_t)({t})({self.rd(s)})")]

    def i_xchg(self):
        a, b = self.ops
        return [f"{{ uint32_t t_ = {self.rd(a)}; {self.wr(a, self.rd(b))} {self.wr(b, 't_')} }}"]

    def i_bswap(self):
        r = self.rn(self.ops[0].reg)
        return [f"{r} = __builtin_bswap32({r});"]

    def i_cdq(self):
        return ["edx = (uint32_t)((int32_t)eax >> 31);"]

    def i_cwde(self):
        return ["eax = (uint32_t)(int32_t)(int16_t)eax;"]

    def i_cbw(self):
        return ["eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);"]

    def do_setcc(self, c):
        return [self.wr(self.ops[0], f"({COND_EXPR[c]}) ? 1u : 0u")]

    def do_cmov(self, c):
        d, s = self.ops
        return [f"if ({COND_EXPR[c]}) {self.wr(d, self.rd(s))}"]

    # ---- integer arithmetic ----
    def binop(self, kind):
        d, s = self.ops
        n = self.bits(d)
        return self.arith(kind, n, self.rd(d), self.rd(s, n), dst=d)

    def i_add(self):
        return self.binop("add")

    def i_sub(self):
        d, s = self.ops
        return self.binop("sub")

    def i_cmp(self):
        d, s = self.ops
        n = self.bits(d)
        return self.arith("sub", n, self.rd(d), self.rd(s, n))

    def i_adc(self):
        d, s = self.ops
        n = self.bits(d)
        return self.arith("add", n, self.rd(d), self.rd(s, n), dst=d, carry_in=True)

    def i_sbb(self):
        d, s = self.ops
        n = self.bits(d)
        return self.arith("sub", n, self.rd(d), self.rd(s, n), dst=d, carry_in=True)

    def i_neg(self):
        d = self.ops[0]
        n = self.bits(d)
        return self.arith("sub", n, "0", self.rd(d), dst=d)

    def i_inc(self):
        return self.incdec("+")

    def i_dec(self):
        return self.incdec("-")

    def incdec(self, sign):
        d = self.ops[0]
        n = self.bits(d)
        s = [f"{{ uint32_t r_ = ({self.rd(d)} {sign} 1u) & {MASK[n]};"]
        if self.want(FO):
            edge = (1 << (n - 1)) if sign == "+" else (1 << (n - 1)) - 1
            s.append(f"OF = (r_ == 0x{edge:x}u);")
        s.extend(self.szp(n))
        s.append(self.wr(d, "r_"))
        s.append("}")
        return [" ".join(s)]

    def i_and(self):
        d, s = self.ops
        return self.logic(self.bits(d), f"{self.rd(d)} & {self.rd(s, self.bits(d))}", dst=d)

    def i_or(self):
        d, s = self.ops
        return self.logic(self.bits(d), f"{self.rd(d)} | {self.rd(s, self.bits(d))}", dst=d)

    def i_xor(self):
        d, s = self.ops
        if d.type == OP_REG and s.type == OP_REG and d.reg == s.reg:
            return self.logic(self.bits(d), "0", dst=d)
        return self.logic(self.bits(d), f"{self.rd(d)} ^ {self.rd(s, self.bits(d))}", dst=d)

    def i_test(self):
        d, s = self.ops
        return self.logic(self.bits(d), f"{self.rd(d)} & {self.rd(s, self.bits(d))}")

    def i_not(self):
        d = self.ops[0]
        return [self.wr(d, f"~{self.rd(d)}")]

    def shift(self, kind):
        d = self.ops[0]
        n = self.bits(d)
        mk = MASK[n]
        cnt_op = self.ops[1] if len(self.ops) > 1 else None
        imm = None
        if cnt_op is None:
            imm = 1
        elif cnt_op.type == OP_IMM:
            imm = cnt_op.imm & 31
        if imm == 0:
            return []
        cnt = str(imm) if imm is not None else "(ecx & 31u)"
        s = [f"{{ uint32_t a_ = {self.rd(d)} & {mk}, c_ = {cnt};"]
        if imm is None:
            s.append("if (c_) {")
        if kind == "shl":
            s.append(f"uint32_t r_ = (uint32_t)((uint64_t)a_ << c_) & {mk};")
            cf = f"(uint32_t)(((uint64_t)a_ >> ({n} - c_)) & 1)"
            of = f"((r_ >> {n - 1}) & 1) ^ CF"
        elif kind == "shr":
            s.append(f"uint32_t r_ = (uint32_t)((uint64_t)a_ >> c_);")
            cf = "(uint32_t)(((uint64_t)a_ >> (c_ - 1)) & 1)"
            of = f"(a_ >> {n - 1}) & 1"
        else:  # sar
            st = {8: "int8_t", 16: "int16_t", 32: "int32_t"}[n]
            s.append(f"int32_t sa_ = ({st})a_; uint32_t r_ = (uint32_t)(sa_ >> (c_ > {n - 1} ? {n - 1} : c_)) & {mk};")
            cf = f"(uint32_t)((sa_ >> (c_ > {n} ? {n - 1} : c_ - 1)) & 1)"
            of = "0"
        if self.want(FC) or self.want(FO) or imm is None:
            s.append(f"CF = {cf};")
        if self.want(FO) or imm is None:
            s.append(f"OF = {of};")
        if imm is None:
            s.extend([f"ZF = (r_ == 0);", f"SF = (r_ >> {n - 1}) & 1;", "PF = parity8(r_);"])
        else:
            s.extend(self.szp(n))
        s.append(self.wr(d, "r_"))
        if imm is None:
            s.append("}")
        s.append("}")
        return [" ".join(s)]

    def i_shl(self):
        return self.shift("shl")

    i_sal = i_shl

    def i_shr(self):
        return self.shift("shr")

    def i_sar(self):
        return self.shift("sar")

    def rotate(self, left):
        d = self.ops[0]
        n = self.bits(d)
        mk = MASK[n]
        c = self.ops[1]
        cnt = f"{c.imm & 31}u" if c.type == OP_IMM else "(ecx & 31u)"
        s = [f"{{ uint32_t a_ = {self.rd(d)} & {mk}, c_ = {cnt} % {n}u;"]
        if left:
            s.append(f"uint32_t r_ = c_ ? ((a_ << c_) | (a_ >> ({n}u - c_))) & {mk} : a_;")
        else:
            s.append(f"uint32_t r_ = c_ ? ((a_ >> c_) | (a_ << ({n}u - c_))) & {mk} : a_;")
        s.append(self.wr(d, "r_"))
        if c.type != OP_IMM or (c.imm & 31):
            guard = f"if ({cnt}) " if c.type != OP_IMM else ""
            if left:
                s.append(f"{guard}{{ CF = r_ & 1; OF = ((r_ >> {n - 1}) & 1) ^ CF; }}")
            else:
                s.append(f"{guard}{{ CF = (r_ >> {n - 1}) & 1; OF = CF ^ ((r_ >> {n - 2}) & 1); }}")
        s.append("}")
        return [" ".join(s)]

    def i_rol(self):
        return self.rotate(True)

    def i_ror(self):
        return self.rotate(False)

    def dshift(self, left):
        d, src, c = self.ops
        if self.bits(d) != 32:
            raise Unsupported("16-bit shld/shrd")
        cnt = f"{c.imm & 31}u" if c.type == OP_IMM else "(ecx & 31u)"
        s = [f"{{ uint32_t a_ = {self.rd(d)}, b_ = {self.rd(src)}, c_ = {cnt}; if (c_) {{"]
        if left:
            s.append("uint32_t r_ = (a_ << c_) | (uint32_t)((uint64_t)b_ >> (32 - c_)); CF = (a_ >> (32 - c_)) & 1;")
        else:
            s.append("uint32_t r_ = (a_ >> c_) | (uint32_t)((uint64_t)b_ << (32 - c_)); CF = (a_ >> (c_ - 1)) & 1;")
        s.append("OF = ((r_ ^ a_) >> 31) & 1; ZF = (r_ == 0); SF = r_ >> 31; PF = parity8(r_);")
        s.append(self.wr(d, "r_"))
        s.append("} }")
        return [" ".join(s)]

    def i_shld(self):
        return self.dshift(True)

    def i_shrd(self):
        return self.dshift(False)

    def i_imul(self):
        ops = self.ops
        if len(ops) == 1:
            n = self.bits(ops[0])
            if n != 32:
                raise Unsupported("imul r/m8/16")
            return [f"{{ int64_t p_ = (int64_t)(int32_t)eax * (int32_t){self.rd(ops[0])}; eax = (uint32_t)p_; "
                    f"edx = (uint32_t)((uint64_t)p_ >> 32); CF = OF = (p_ != (int32_t)p_); }}"]
        if len(ops) == 2:
            d, a, b = ops[0], ops[0], ops[1]
        else:
            d, a, b = ops
        n = self.bits(d)
        if n == 16:
            return [f"{{ int32_t p_ = (int32_t)(int16_t){self.rd(a)} * (int32_t)(int16_t){self.rd(b, 16)}; "
                    f"CF = OF = (p_ != (int16_t)p_); {self.wr(d, '(uint32_t)p_')} }}"]
        if n != 32:
            raise Unsupported("imul 8")
        return [f"{{ int64_t p_ = (int64_t)(int32_t){self.rd(a)} * (int32_t){self.rd(b, 32)}; "
                f"CF = OF = (p_ != (int32_t)p_); {self.wr(d, '(uint32_t)p_')} }}"]

    def i_mul(self):
        op = self.ops[0]
        if self.bits(op) != 32:
            raise Unsupported("mul r/m8/16")
        return [f"{{ uint64_t p_ = (uint64_t)eax * {self.rd(op)}; eax = (uint32_t)p_; edx = (uint32_t)(p_ >> 32); "
                f"CF = OF = (edx != 0); }}"]

    def i_div(self):
        op = self.ops[0]
        if self.bits(op) != 32:
            raise Unsupported("div r/m8/16")
        return [f"{{ uint64_t n_ = ((uint64_t)edx << 32) | eax; uint32_t d_ = {self.rd(op)}; "
                f"if (!d_) rt_unimpl(cpu, 0x{self.i.address:x}u, \"#DE\"); "
                f"eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ % d_); }}"]

    def i_idiv(self):
        op = self.ops[0]
        if self.bits(op) != 32:
            raise Unsupported("idiv r/m8/16")
        return [f"{{ int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d_ = (int32_t){self.rd(op)}; "
                f"if (!d_) rt_unimpl(cpu, 0x{self.i.address:x}u, \"#DE\"); "
                f"eax = (uint32_t)(int32_t)(n_ / d_); edx = (uint32_t)(int32_t)(n_ % d_); }}"]

    def i_bsr(self):
        d, s = self.ops
        return [f"{{ uint32_t v_ = {self.rd(s)}; ZF = (v_ == 0); if (v_) {{ {self.wr(d, '31u - __builtin_clz(v_)')} }} }}"]

    def i_bsf(self):
        d, s = self.ops
        return [f"{{ uint32_t v_ = {self.rd(s)}; ZF = (v_ == 0); if (v_) {{ {self.wr(d, '__builtin_ctz(v_)')} }} }}"]

    def bitop(self, op_expr):
        d, b = self.ops
        if d.type == OP_MEM and b.type == OP_REG:
            if self.bits(d) != 32:
                raise Unsupported("bt bitstring size")
            # Bit-string form: the register is a signed bit offset from the operand.
            s = [f"{{ int32_t o_ = (int32_t){self.rd(b)}; uint32_t a_ = {self.ea(d)} + (uint32_t)((o_ >> 5) * 4);",
                 "uint32_t v_ = R32(a_), k_ = (uint32_t)o_ & 31u; CF = (v_ >> k_) & 1;"]
            if op_expr:
                s.append(f"W32(a_, {op_expr});")
            s.append("}")
            return [" ".join(s)]
        n = self.bits(d)
        s = [f"{{ uint32_t v_ = {self.rd(d)}, k_ = ({self.rd(b)}) & {n - 1}u; CF = (v_ >> k_) & 1;"]
        if op_expr:
            s.append(self.wr(d, op_expr))
        s.append("}")
        return [" ".join(s)]

    def i_bt(self):
        return self.bitop(None)

    def i_bts(self):
        return self.bitop("v_ | (1u << k_)")

    def i_btr(self):
        return self.bitop("v_ & ~(1u << k_)")

    # ---- string ops (DF assumed clear) ----
    def strop(self, kind, n):
        rep = self.i.prefix[0]
        b = n // 8
        R = {8: "R8", 16: "R16", 32: "R32"}[n]
        W = {8: "W8", 16: "W16", 32: "W32"}[n]
        acc = {8: "(eax & 0xffu)", 16: "(eax & 0xffffu)", 32: "eax"}[n]
        if kind == "movs":
            one = f"{W}(edi, {R}(esi)); esi += {b}; edi += {b};"
        elif kind == "stos":
            one = f"{W}(edi, {acc}); edi += {b};"
        elif kind == "cmps":
            one = (f"{{ uint32_t a_ = {R}(esi), b_ = {R}(edi), r_ = (a_ - b_) & {MASK[n]}; esi += {b}; edi += {b}; "
                   f"CF = a_ < b_; ZF = r_ == 0; SF = (r_ >> {n - 1}) & 1; OF = (((a_ ^ b_) & (a_ ^ r_)) >> {n - 1}) & 1; PF = parity8(r_); }}")
        elif kind == "scas":
            one = (f"{{ uint32_t a_ = {acc}, b_ = {R}(edi), r_ = (a_ - b_) & {MASK[n]}; edi += {b}; "
                   f"CF = a_ < b_; ZF = r_ == 0; SF = (r_ >> {n - 1}) & 1; OF = (((a_ ^ b_) & (a_ ^ r_)) >> {n - 1}) & 1; PF = parity8(r_); }}")
        else:
            raise Unsupported(kind)
        if rep == 0:
            return [one]
        if kind in ("movs", "stos"):
            if kind == "stos" and n == 32:
                return ["while (ecx) { W32(edi, eax); edi += 4; ecx--; }"]
            return [f"while (ecx) {{ {one} ecx--; }}"]
        brk = "if (!ZF) break;" if rep == 0xF3 else "if (ZF) break;"
        return [f"while (ecx) {{ {one} ecx--; {brk} }}"]

    def string_size(self):
        return {"b": 8, "w": 16, "d": 32}[self.i.mnemonic[-1]]

    def i_movsb(self):
        return self.strop("movs", 8)

    def i_movsw(self):
        return self.strop("movs", 16)

    def i_stosb(self):
        return self.strop("stos", 8)

    def i_stosw(self):
        return self.strop("stos", 16)

    def i_stosd(self):
        return self.strop("stos", 32)

    def i_cmpsb(self):
        return self.strop("cmps", 8)

    def i_cmpsd(self):
        if any(self.xmm(o) is not None for o in self.ops):
            return self.sse_cmp("eq", "sd")
        return self.strop("cmps", 32)

    def i_scasb(self):
        return self.strop("scas", 8)

    def i_scasd(self):
        return self.strop("scas", 32)

    # ---- SSE ----
    def sread(self, op, kind):
        """Read a scalar float ('f' 32-bit, 'g' 64-bit) or bits ('d','q') from xmm/mem."""
        x = self.xmm(op)
        if x is not None:
            return f"XMM({x}).{kind}[0]"
        a = self.ea(op)
        return {"f": f"RF32({a})", "g": f"RF64({a})", "d": f"R32({a})", "q": f"R64({a})"}[kind]

    def vread(self, op):
        """Read a full 128-bit value into a local XMM named s_."""
        x = self.xmm(op)
        if x is not None:
            return f"XMM s_ = XMM({x});"
        return f"XMM s_; memcpy(&s_, M + {self.ea(op)}, 16);"

    def i_movss(self):
        return self.movscalar("f", 4)

    def i_movsd(self):
        if any(self.xmm(o) is not None for o in self.ops):
            return self.movscalar("g", 8)
        return self.strop("movs", 32)

    def movscalar(self, kind, size):
        d, s = self.ops
        dx, sx = self.xmm(d), self.xmm(s)
        if dx is not None and sx is not None:
            return [f"XMM({dx}).{kind}[0] = XMM({sx}).{kind}[0];"]
        if dx is not None:
            return [f"{{ {'float' if kind == 'f' else 'double'} v_ = {self.sread(s, kind)}; memset(&XMM({dx}), 0, 16); XMM({dx}).{kind}[0] = v_; }}"]
        w = "WF32" if kind == "f" else "WF64"
        return [f"{w}({self.ea(d)}, XMM({sx}).{kind}[0]);"]

    def mov128(self):
        d, s = self.ops
        dx = self.xmm(d)
        if dx is not None:
            return [f"{{ {self.vread(s)} XMM({dx}) = s_; }}"]
        return [f"memcpy(M + {self.ea(d)}, &XMM({self.xmm(s)}), 16);"]

    i_movaps = mov128
    i_movapd = mov128
    i_movups = mov128
    i_movupd = mov128
    i_movdqa = mov128
    i_movdqu = mov128

    def i_movd(self):
        d, s = self.ops
        dx, sx = self.xmm(d), self.xmm(s)
        if dx is not None:
            return [f"{{ uint32_t v_ = {self.rd(s)}; memset(&XMM({dx}), 0, 16); XMM({dx}).d[0] = v_; }}"]
        return [self.wr(d, f"XMM({sx}).d[0]")]

    def i_movq(self):
        d, s = self.ops
        dx, sx = self.xmm(d), self.xmm(s)
        if dx is not None:
            return [f"{{ uint64_t v_ = {self.sread(s, 'q')}; memset(&XMM({dx}), 0, 16); XMM({dx}).q[0] = v_; }}"]
        return [f"W64({self.ea(d)}, XMM({sx}).q[0]);"]

    def i_movhpd(self):
        d, s = self.ops
        dx = self.xmm(d)
        if dx is not None:
            return [f"XMM({dx}).q[1] = R64({self.ea(s)});"]
        return [f"W64({self.ea(d)}, XMM({self.xmm(s)}).q[1]);"]

    i_movhps = i_movhpd

    def i_movlpd(self):
        d, s = self.ops
        dx = self.xmm(d)
        if dx is not None:
            return [f"XMM({dx}).q[0] = R64({self.ea(s)});"]
        return [f"W64({self.ea(d)}, XMM({self.xmm(s)}).q[0]);"]

    i_movlps = i_movlpd

    def scalar(self, kind, expr):
        d, s = self.ops
        dx = self.xmm(d)
        t = "float" if kind == "f" else "double"
        return [f"{{ {t} a_ = XMM({dx}).{kind}[0], b_ = {self.sread(s, kind)}; XMM({dx}).{kind}[0] = {expr}; }}"]

    def i_addss(self): return self.scalar("f", "a_ + b_")
    def i_subss(self): return self.scalar("f", "a_ - b_")
    def i_mulss(self): return self.scalar("f", "a_ * b_")
    def i_divss(self): return self.scalar("f", "a_ / b_")
    def i_minss(self): return self.scalar("f", "minf_(a_, b_)")
    def i_maxss(self): return self.scalar("f", "maxf_(a_, b_)")
    def i_sqrtss(self): return self.scalar("f", "sqrtf(b_)")
    def i_addsd(self): return self.scalar("g", "a_ + b_")
    def i_subsd(self): return self.scalar("g", "a_ - b_")
    def i_mulsd(self): return self.scalar("g", "a_ * b_")
    def i_divsd(self): return self.scalar("g", "a_ / b_")
    def i_minsd(self): return self.scalar("g", "mind_(a_, b_)")
    def i_maxsd(self): return self.scalar("g", "maxd_(a_, b_)")
    def i_sqrtsd(self): return self.scalar("g", "sqrt(b_)")

    def packed(self, lanes, kind, expr):
        d, s = self.ops
        dx = self.xmm(d)
        return [f"{{ {self.vread(s)} for (int k_ = 0; k_ < {lanes}; k_++) {{ XMM({dx}).{kind}[k_] = {expr.replace('A', f'XMM({dx}).{kind}[k_]').replace('B', f's_.{kind}[k_]')}; }} }}"]

    def i_addps(self): return self.packed(4, "f", "A + B")
    def i_subps(self): return self.packed(4, "f", "A - B")
    def i_mulps(self): return self.packed(4, "f", "A * B")
    def i_addpd(self): return self.packed(2, "g", "A + B")
    def i_subpd(self): return self.packed(2, "g", "A - B")
    def i_mulpd(self): return self.packed(2, "g", "A * B")
    def i_andps(self): return self.packed(4, "d", "A & B")
    def i_andpd(self): return self.packed(4, "d", "A & B")
    def i_orps(self): return self.packed(4, "d", "A | B")
    def i_orpd(self): return self.packed(4, "d", "A | B")
    def i_por(self): return self.packed(4, "d", "A | B")
    def i_xorps(self): return self.packed(4, "d", "A ^ B")
    def i_xorpd(self): return self.packed(4, "d", "A ^ B")
    def i_pxor(self): return self.packed(4, "d", "A ^ B")
    def i_pand(self): return self.packed(4, "d", "A & B")
    def i_andnps(self): return self.packed(4, "d", "~A & B")
    def i_andnpd(self): return self.packed(4, "d", "~A & B")
    def i_pandn(self): return self.packed(4, "d", "~A & B")
    def i_paddd(self): return self.packed(4, "d", "A + B")
    def i_psubd(self): return self.packed(4, "d", "A - B")

    def pshift(self, kind, lanes, op):
        d, c = self.ops
        dx = self.xmm(d)
        w = 32 if kind == "d" else 64
        cnt = f"{c.imm}u" if c.type == OP_IMM else f"(uint32_t){self.sread(c, 'q')}"
        return [f"{{ uint64_t c_ = {cnt}; for (int k_ = 0; k_ < {lanes}; k_++) "
                f"XMM({dx}).{kind}[k_] = c_ >= {w} ? 0 : XMM({dx}).{kind}[k_] {op} c_; }}"]

    def i_pslld(self): return self.pshift("d", 4, "<<")
    def i_psrld(self): return self.pshift("d", 4, ">>")
    def i_psllq(self): return self.pshift("q", 2, "<<")
    def i_psrlq(self): return self.pshift("q", 2, ">>")

    def i_punpckldq(self):
        d, s = self.ops
        dx = self.xmm(d)
        return [f"{{ {self.vread(s)} XMM r_; r_.d[0] = XMM({dx}).d[0]; r_.d[1] = s_.d[0]; "
                f"r_.d[2] = XMM({dx}).d[1]; r_.d[3] = s_.d[1]; XMM({dx}) = r_; }}"]

    def ucomi(self, kind):
        a, b = self.ops
        t = "float" if kind == "f" else "double"
        s = [f"{{ {t} a_ = XMM({self.xmm(a)}).{kind}[0], b_ = {self.sread(b, kind)};",
             "if (a_ != a_ || b_ != b_) { ZF = PF = CF = 1; } else { PF = 0; ZF = (a_ == b_); CF = (a_ < b_); }",
             "OF = SF = 0; }"]
        return [" ".join(s)]

    def i_ucomiss(self): return self.ucomi("f")
    def i_comiss(self): return self.ucomi("f")
    def i_ucomisd(self): return self.ucomi("g")
    def i_comisd(self): return self.ucomi("g")

    SSE_CMP = {"eq": "a_ == b_", "lt": "a_ < b_", "le": "a_ <= b_", "unord": "a_ != a_ || b_ != b_",
               "neq": "!(a_ == b_)", "nlt": "!(a_ < b_)", "nle": "!(a_ <= b_)", "ord": "a_ == a_ && b_ == b_"}

    def sse_cmp(self, pred, sfx):
        kind, t, lane = ("f", "float", "d") if sfx == "ss" else ("g", "double", "q")
        d, s = self.ops[0], self.ops[1]
        dx = self.xmm(d)
        ones = "0xffffffffu" if sfx == "ss" else "~0ull"
        return [f"{{ {t} a_ = XMM({dx}).{kind}[0], b_ = {self.sread(s, kind)}; "
                f"XMM({dx}).{lane}[0] = ({self.SSE_CMP[pred]}) ? {ones} : 0; }}"]

    def __getattr__(self, name):
        # cmp<pred>ss / cmp<pred>sd
        if name.startswith("i_cmp") and name[-2:] in ("ss", "sd"):
            pred = name[5:-2]
            if pred in self.SSE_CMP:
                sfx = name[-2:]
                return lambda: self.sse_cmp(pred, sfx)
        raise AttributeError(name)

    def i_cvtsi2ss(self):
        d, s = self.ops
        return [f"XMM({self.xmm(d)}).f[0] = (float)(int32_t){self.rd(s)};"]

    def i_cvtsi2sd(self):
        d, s = self.ops
        return [f"XMM({self.xmm(d)}).g[0] = (double)(int32_t){self.rd(s)};"]

    def i_cvttss2si(self):
        d, s = self.ops
        return [self.wr(d, f"cvtt32((double){self.sread(s, 'f')})")]

    def i_cvttsd2si(self):
        d, s = self.ops
        return [self.wr(d, f"cvtt32({self.sread(s, 'g')})")]

    def i_cvtss2sd(self):
        d, s = self.ops
        return [f"XMM({self.xmm(d)}).g[0] = (double){self.sread(s, 'f')};"]

    def i_cvtsd2ss(self):
        d, s = self.ops
        return [f"XMM({self.xmm(d)}).f[0] = (float){self.sread(s, 'g')};"]

    def i_cvttps2dq(self):
        d, s = self.ops
        dx = self.xmm(d)
        return [f"{{ {self.vread(s)} for (int k_ = 0; k_ < 4; k_++) XMM({dx}).d[k_] = cvtt32((double)s_.f[k_]); }}"]

    def i_cvttpd2dq(self):
        d, s = self.ops
        dx = self.xmm(d)
        return [f"{{ {self.vread(s)} XMM({dx}).d[0] = cvtt32(s_.g[0]); XMM({dx}).d[1] = cvtt32(s_.g[1]); "
                f"XMM({dx}).q[1] = 0; }}"]

    def i_cvtdq2pd(self):
        d, s = self.ops
        dx = self.xmm(d)
        src = f"XMM({self.xmm(s)})" if self.xmm(s) is not None else None
        if src:
            return [f"{{ int32_t a_ = (int32_t){src}.d[0], b_ = (int32_t){src}.d[1]; XMM({dx}).g[0] = a_; XMM({dx}).g[1] = b_; }}"]
        a = self.ea(s)
        return [f"{{ int32_t a_ = (int32_t)R32({a}), b_ = (int32_t)R32({a} + 4u); XMM({dx}).g[0] = a_; XMM({dx}).g[1] = b_; }}"]

    # ---- atomics (guest memory is shared between guest threads) ----
    def i_xadd(self):
        d, src = self.ops
        n = self.bits(d)
        if n != 32:
            raise Unsupported("xadd size")
        if d.type == OP_MEM:
            fetch = f"__atomic_fetch_add((uint32_t *)(M + {self.ea(d)}), b_, __ATOMIC_SEQ_CST)"
            store = ""
        else:
            fetch = self.rd(d)
            store = self.wr(d, "a_ + b_")
        s = [f"{{ uint32_t b_ = {self.rd(src)}; uint32_t a_ = {fetch}; {store} uint32_t r_ = a_ + b_;"]
        if self.want(FC):
            s.append("CF = r_ < a_;")
        if self.want(FO):
            s.append("OF = (((a_ ^ r_) & (b_ ^ r_)) >> 31) & 1;")
        s.extend(self.szp(32))
        s.append(self.wr(src, "a_"))
        s.append("}")
        return [" ".join(s)]

    def i_cmpxchg(self):
        d, src = self.ops
        if self.bits(d) != 32 or d.type != OP_MEM:
            raise Unsupported("cmpxchg form")
        return [f"{{ uint32_t e_ = eax; ZF = __atomic_compare_exchange_n((uint32_t *)(M + {self.ea(d)}), &e_, "
                f"{self.rd(src)}, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); eax = e_; }}"]

    def i_mfence(self):
        return ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"]

    i_lfence = i_mfence
    i_sfence = i_mfence

    def i_pause(self):
        return []

    def i_pshufd(self):
        d, s, imm = self.ops
        dx = self.xmm(d)
        k = imm.imm
        return [f"{{ {self.vread(s)} XMM r_; " + " ".join(f"r_.d[{i}] = s_.d[{(k >> (2 * i)) & 3}];" for i in range(4))
                + f" XMM({dx}) = r_; }}"]

    # ---- x87 (scalar only; 64-bit precision) ----
    def fsrc(self, op):
        if op.type == OP_MEM:
            return {4: f"(double)RF32({self.ea(op)})", 8: f"RF64({self.ea(op)})",
                    10: f"rt_rf80(M, {self.ea(op)})"}[op.size]
        name = self.rn(op.reg)
        return f"ST({int(name[3])})"

    def _bad(self, what):
        raise Unsupported(what)

    def fdst(self, op, val):
        if op.type == OP_MEM:
            if op.size == 4:
                return f"WF32({self.ea(op)}, (float)({val}));"
            if op.size == 8:
                return f"WF64({self.ea(op)}, {val});"
            if op.size == 10:
                return f"rt_wf80(M, {self.ea(op)}, {val});"
            raise Unsupported("fst size")
        return f"ST({int(self.rn(op.reg)[3])}) = {val};"

    def i_fld(self):
        return [f"FPUSH({self.fsrc(self.ops[0])});"]

    def i_fldz(self):
        return ["FPUSH(0.0);"]

    def i_fld1(self):
        return ["FPUSH(1.0);"]

    def i_fild(self):
        op = self.ops[0]
        t = {2: "(int16_t)R16", 4: "(int32_t)R32", 8: "(int64_t)R64"}[op.size]
        return [f"FPUSH((double){t}({self.ea(op)}));"]

    def i_fst(self):
        return [self.fdst(self.ops[0], "ST(0)")]

    def i_fstp(self):
        return [self.fdst(self.ops[0], "ST(0)"), "FPOP();"]

    def i_fistp(self):
        op = self.ops[0]
        w = {2: "W16", 4: "W32", 8: "W64"}[op.size]
        return [f"{w}({self.ea(op)}, rt_fround(cpu, ST(0))); FPOP();"]

    def i_fxch(self):
        i = int(self.rn(self.ops[0].reg)[3]) if self.ops else 1
        return [f"{{ double t_ = ST(0); ST(0) = ST({i}); ST({i}) = t_; }}"]

    def farith(self, op, pop, rev=False):
        ops = self.ops
        if len(ops) == 1:
            return [f"ST(0) = ST(0) {op} {self.fsrc(ops[0])};"]
        dn = int(self.rn(ops[0].reg)[3])
        sn = int(self.rn(ops[1].reg)[3])
        expr = f"ST({sn}) {op} ST({dn})" if rev else f"ST({dn}) {op} ST({sn})"
        return [f"ST({dn}) = {expr};"] + (["FPOP();"] if pop else [])

    def i_fadd(self): return self.farith("+", False)
    def i_faddp(self): return self.farith("+", True)
    def i_fmul(self): return self.farith("*", False)
    def i_fmulp(self): return self.farith("*", True)
    def i_fdivp(self): return self.farith("/", True)
    def i_fsubp(self): return self.farith("-", True)

    def i_fldcw(self):
        return [f"cpu->fcw = (uint16_t)R16({self.ea(self.ops[0])});"]

    def i_fnstcw(self):
        return [f"W16({self.ea(self.ops[0])}, cpu->fcw);"]
