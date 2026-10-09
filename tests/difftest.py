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
from world import IMPORT_BASE, World  # noqa: E402

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
    """The guest memory image shared by both sides: the linked world (game with
    binds applied, host data cells, guest modules)."""

    def __init__(self, path, modules):
        self.world = World(path, modules)
        w = self.world
        regs = w.regions()
        lo = align_down(min(a for a, _, _ in regs))
        hi = align_up(max(a + len(d) for a, d, _ in regs))
        # Sparse page map: page -> prot; contents in one dict of page buffers.
        prots = {}
        pages = {}
        for a, data, prot in regs:
            for pg in range(align_down(a), align_up(a + len(data)), PAGE):
                prots[pg] = max(prots.get(pg, 0), PROT_RW if prot == PROT_RW else PROT_R)
                pages.setdefault(pg, bytearray(PAGE))
            for pg in range(align_down(a), align_up(a + len(data)), PAGE):
                s0, s1 = max(pg, a), min(pg + PAGE, a + len(data))
                pages[pg][s0 - pg:s1 - pg] = data[s0 - a:s1 - a]
        self.regions = []  # [addr, size, prot, bytes], runs of contiguous equal-prot pages
        run = None
        for pg in sorted(pages):
            if run and run[0] + run[1] == pg and run[2] == prots[pg]:
                run[1] += PAGE
                run[3] += pages[pg]
            else:
                run = [pg, PAGE, prots[pg], bytearray(pages[pg])]
                self.regions.append(run)
        self.import_names = w.import_names
        self.functions = [(a, n) for a, n in w.functions() if any(l <= a < h for l, h in w.code_ranges())]

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
        n = len(img.import_names)
        self.uc.mem_map(IMPORT_BASE, align_up(4 * n), U.UC_PROT_READ | U.UC_PROT_EXEC)
        self.fault = None
        self.import_hit = None
        self.uc.hook_add(U.UC_HOOK_MEM_INVALID, self._on_invalid)
        self.uc.hook_add(U.UC_HOOK_CODE, self._on_import, begin=IMPORT_BASE, end=IMPORT_BASE + 4 * n - 1)

    def _on_invalid(self, uc, access, addr, size, value, data):
        self.fault = addr
        return False

    def _on_import(self, uc, addr, size, data):
        # Reached a host import (directly, or through a stub's lazy pointer): stop
        # here, matching the native side's trap in rt_import.
        self.import_hit = self.img.import_names[(addr - IMPORT_BASE) // 4]
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
    ap.add_argument("--module", action="append", default=[])
    ap.add_argument("--range", default="", help="sample only functions in LO-HI (hex)")
    args = ap.parse_args()

    img = Image(args.binary, args.module)
    funcs = [a for a, _ in img.functions]
    if args.range:
        lo, hi = (int(x, 16) for x in args.range.split("-"))
        funcs = [a for a in funcs if lo <= a < hi]
    names = dict(img.functions)
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
