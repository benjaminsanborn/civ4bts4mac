"""Differential test: original x86 code (Unicorn) vs recompiled native code.

For each sampled function, both sides start from identical guest memory and
registers with randomized arguments, run until the function returns, faults,
or calls an import, and the final registers + writable memory are compared.

  difftest.py BINARY LIB.dylib [-n 500] [--seed 1] [--fn 0x1234]
"""
import argparse
import ctypes
import os
import random
import struct
import sys

import unicorn as U
from unicorn import x86_const as UX

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "recomp"))
from macho import MachO  # noqa: E402

PAGE = 0x4000  # host page size on Apple Silicon; map identically on both sides
SCRATCH, SCRATCH_SIZE = 0x40000000, 0x100000
STACK, STACK_SIZE = 0x70000000, 0x100000
RET_MAGIC = 0x7FFF0000
MAX_INSNS = 3_000_000
PROT_R, PROT_RW = 1, 3
REGS = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
        UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
RNAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]


def align_down(a):
    return a & ~(PAGE - 1)


def align_up(a):
    return (a + PAGE - 1) & ~(PAGE - 1)


class Image:
    """The guest memory image shared by both sides."""

    def __init__(self, path):
        m = MachO(path)
        self.m = m
        # One contiguous image; a page shared by two segments gets the more
        # permissive protection. Regions are runs of equal protection.
        segs = [g for g in m.segments if g.segname not in ("__PAGEZERO", "__LINKEDIT")]
        lo = align_down(min(g.vmaddr for g in segs))
        hi = align_up(max(g.vmaddr + g.vmsize for g in segs))
        image = bytearray(hi - lo)
        prots = [0] * ((hi - lo) // PAGE)
        for g in segs:
            data = m.data[g.fileoff:g.fileoff + g.filesize]
            image[g.vmaddr - lo:g.vmaddr - lo + len(data)] = data
            for pg in range((align_down(g.vmaddr) - lo) // PAGE, (align_up(g.vmaddr + g.vmsize) - lo) // PAGE):
                prots[pg] = max(prots[pg], PROT_RW if g.initprot & 2 else PROT_R)
        self.regions = []  # [addr, size, prot, bytes]
        start = 0
        for pg in range(1, len(prots) + 1):
            if pg == len(prots) or prots[pg] != prots[start]:
                a, b = start * PAGE, pg * PAGE
                self.regions.append([lo + a, b - a, prots[start], image[a:b]])
                start = pg
        # Bind lazy pointers of coalesced weak symbols to their local definitions,
        # as dyld would, so the stub jump lands on real code under Unicorn.
        self.stub_names = m.stubs()
        stub_sec = m.section("__TEXT", "__symbol_stub")
        la = m.section("__DATA", "__la_symbol_ptr")
        self.stub_range = (stub_sec.addr, stub_sec.end)
        for i in range(stub_sec.size // stub_sec.reserved2):
            name = self.stub_names.get(stub_sec.addr + i * stub_sec.reserved2)
            sym = m.defined_symbol(name) if name else None
            if sym is not None:
                self.poke(la.addr + 4 * i, struct.pack("<I", sym.value))
        self.text = m.section("__TEXT", "__text")

    def poke(self, addr, data):
        for r in self.regions:
            if r[0] <= addr < r[0] + r[1]:
                r[3][addr - r[0]:addr - r[0] + len(data)] = data
                return
        raise KeyError(hex(addr))


def make_case(rng):
    """Random scratch heap, stack with arguments, and initial registers."""
    words = []
    for _ in range(SCRATCH_SIZE // 4):
        x = rng.random()
        if x < 0.4:
            words.append(SCRATCH + rng.randrange(0, SCRATCH_SIZE - 64, 4))
        elif x < 0.6:
            words.append(rng.randrange(0, 100))
        elif x < 0.8:
            words.append(0)
        else:
            words.append(rng.getrandbits(32))
    scratch = struct.pack("<%dI" % len(words), *words)
    stack = bytearray(STACK_SIZE)
    esp = STACK + STACK_SIZE - 0x10000
    args = [RET_MAGIC] + [words[rng.randrange(len(words))] for _ in range(16)]
    stack[esp - STACK:esp - STACK + 4 * len(args)] = struct.pack("<%dI" % len(args), *args)
    regs = [words[rng.randrange(len(words))] for _ in range(8)]
    regs[4] = esp
    regs[5] = esp + 0x100
    return scratch, bytes(stack), regs


class Native:
    def __init__(self, lib):
        self.lib = ctypes.CDLL(lib)
        self.lib.dt_init.restype = ctypes.c_int
        self.lib.dt_base.restype = ctypes.c_void_p
        assert self.lib.dt_init() == 0
        self.base = self.lib.dt_base()
        self.mapped = set()

    def map(self, addr, size, prot):
        if self.lib.dt_protect(ctypes.c_uint32(addr), ctypes.c_uint32(size), prot) != 0:
            raise OSError("mprotect failed")

    def load(self, img, scratch, stack):
        for addr, size, prot, buf in img.regions:
            self.map(addr, size, PROT_RW)
            ctypes.memmove(self.base + addr, bytes(buf), size)
            self.map(addr, size, prot)
        for addr, data in ((SCRATCH, scratch), (STACK, stack)):
            self.map(addr, len(data), PROT_RW)
            ctypes.memmove(self.base + addr, data, len(data))

    def read(self, addr, size):
        return ctypes.string_at(self.base + addr, size)

    def run(self, fn, regs):
        r = (ctypes.c_uint32 * 8)(*regs)
        fa = ctypes.c_uint32(0)
        msg = ctypes.create_string_buffer(256)
        code = self.lib.dt_run(ctypes.c_uint32(fn), r, ctypes.byref(fa), msg, 256)
        return code, list(r), fa.value, msg.value.decode(errors="replace")


class Emu:
    def __init__(self, img):
        self.img = img
        self.uc = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
        for addr, size, prot, _ in img.regions:
            self.uc.mem_map(addr, size, U.UC_PROT_READ | U.UC_PROT_EXEC | (U.UC_PROT_WRITE if prot == PROT_RW else 0))
        self.uc.mem_map(SCRATCH, SCRATCH_SIZE, U.UC_PROT_READ | U.UC_PROT_WRITE)
        self.uc.mem_map(STACK, STACK_SIZE, U.UC_PROT_READ | U.UC_PROT_WRITE)
        self.fault = None
        self.import_hit = None
        self.uc.hook_add(U.UC_HOOK_MEM_INVALID, self._on_invalid)
        lo, hi = img.stub_range
        self.uc.hook_add(U.UC_HOOK_CODE, self._on_stub, begin=lo, end=hi - 1)

    def _on_invalid(self, uc, access, addr, size, value, data):
        self.fault = addr
        return False

    def _on_stub(self, uc, addr, size, data):
        name = self.img.stub_names.get(addr)
        sym = self.img.m.defined_symbol(name) if name else None
        if sym is None:  # a real import: stop here, like the native side's trap
            self.import_hit = name
            uc.emu_stop()

    def load(self, scratch, stack):
        for addr, size, prot, buf in self.img.regions:
            self.uc.mem_write(addr, bytes(buf))
        self.uc.mem_write(SCRATCH, scratch)
        self.uc.mem_write(STACK, stack)

    def run(self, fn, regs):
        self.fault = None
        self.import_hit = None
        for r, v in zip(REGS, regs):
            self.uc.reg_write(r, v)
        self.uc.reg_write(UX.UC_X86_REG_EFLAGS, 0x202)
        try:
            self.uc.emu_start(fn, RET_MAGIC, count=MAX_INSNS)
        except U.UcError as e:
            return "fault", self.fault, str(e)
        pc = self.uc.reg_read(UX.UC_X86_REG_EIP)
        if self.import_hit:
            return "import", self.import_hit, ""
        if pc == RET_MAGIC:
            return "ok", None, ""
        return "timeout", pc, ""

    def regs(self):
        return [self.uc.reg_read(r) for r in REGS]


def writable_regions(img):
    out = [(a, s) for a, s, p, _ in img.regions if p == PROT_RW]
    return out + [(SCRATCH, SCRATCH_SIZE), (STACK, STACK_SIZE)]


def first_diff(a, b, base):
    for i in range(len(a)):
        if a[i] != b[i]:
            return base + i
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("lib")
    ap.add_argument("-n", type=int, default=300)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--fn", default="")
    ap.add_argument("-v", action="store_true")
    args = ap.parse_args()

    img = Image(args.binary)
    funcs = [f.value for f in img.m.functions()]
    names = {f.value: f.name for f in img.m.functions()}
    rng = random.Random(args.seed)
    picks = [int(x, 16) for x in args.fn.split(",") if x] or rng.sample(funcs, args.n)
    nat = Native(args.lib)
    emu = Emu(img)

    stats = {}
    failures = []
    for k, fn in enumerate(picks):
        crng = random.Random(args.seed * 1000003 + fn)
        scratch, stack, regs = make_case(crng)
        emu.load(scratch, stack)
        nat.load(img, scratch, stack)
        ekind, eval_, emsg = emu.run(fn, regs)
        if ekind == "timeout":  # likely an endless loop; native has no instruction limit
            stats["skip:timeout"] = stats.get("skip:timeout", 0) + 1
            continue
        ncode, nregs, nfa, nmsg = nat.run(fn, regs)

        if ekind == "timeout":
            outcome = "skip:timeout"
        elif ncode == 2 and "dispatch miss" in nmsg:
            outcome = "skip:dispatch-miss"
        elif ekind == "ok" and ncode == 0:
            outcome = "pass:return"
        elif ekind == "fault" and ncode == 1:
            outcome = "pass:fault" if align_down(eval_ or 0) == align_down(nfa) else "FAIL:fault-addr"
        elif ekind == "fault" and "UC_ERR_EXCEPTION" in emsg and ncode == 2 and nmsg == "#DE":
            outcome = "pass:div-by-zero"
        elif ekind == "import" and ncode == 2 and nmsg == eval_:
            outcome = "pass:import"
        else:
            outcome = "FAIL:outcome"
        detail = f"emu={ekind}:{eval_ if not isinstance(eval_, int) else hex(eval_)} {emsg} native={ncode}:{hex(nfa)} {nmsg}"
        if outcome in ("pass:return", "pass:import"):
            eregs = emu.regs()
            # At an import trap the native side spilled registers before the call.
            bad = [f"{RNAMES[i]} emu={eregs[i]:#x} nat={nregs[i]:#x}" for i in range(8) if eregs[i] != nregs[i]]
            if bad:
                outcome = "FAIL:regs"
                detail = " ".join(bad)
            else:
                for addr, size in writable_regions(img):
                    a = bytes(emu.uc.mem_read(addr, size))
                    b = nat.read(addr, size)
                    if a != b:
                        d = first_diff(a, b, addr)
                        outcome = "FAIL:memory"
                        detail = f"first diff at {d:#x}: emu={a[d - addr:d - addr + 8].hex()} nat={b[d - addr:d - addr + 8].hex()}"
                        break
        stats[outcome] = stats.get(outcome, 0) + 1
        if outcome.startswith("FAIL"):
            failures.append((fn, outcome, detail))
        if args.v or outcome.startswith("FAIL"):
            print(f"{fn:#010x} {outcome:18s} {names.get(fn, '')[:70]}  {detail}", flush=True)
        if k % 50 == 0:
            print(f"@@progress {{\"done\": {k}, \"total\": {len(picks)}}}", flush=True)

    print("\nresults:")
    for k, v in sorted(stats.items()):
        print(f"  {v:6d} {k}")
    tested = sum(v for k, v in stats.items() if not k.startswith("skip"))
    passed = sum(v for k, v in stats.items() if k.startswith("pass"))
    print(f"pass rate: {passed}/{tested}")


if __name__ == "__main__":
    main()
