"""Survey the binary: function count, import stubs, instruction histogram."""
import collections
import sys

import capstone

from macho import MachO


def main(path: str) -> None:
    m = MachO(path)
    text = m.section("__TEXT", "__text")
    funcs = m.functions()
    stubs = m.stubs()
    local_stubs = sum(1 for n in stubs.values() if m.defined_symbol(n))
    print(f"functions: {len(funcs)}  stubs: {len(stubs)} ({local_stubs} resolve to local weak defs)")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = m.read(text.addr, text.size)
    hist = collections.Counter()
    prefixes = collections.Counter()
    total = 0
    bounds = [f.value for f in funcs] + [text.end]
    for i, f in enumerate(funcs):
        start, end = bounds[i], bounds[i + 1]
        for insn in md.disasm_lite(code[start - text.addr:end - text.addr], start):
            _addr, _size, mnem, ops = insn
            hist[mnem] += 1
            if "gs:" in ops or "fs:" in ops:
                prefixes[mnem + " " + ops.split(",")[0]] += 1
            total += 1
    print(f"instructions: {total}  distinct mnemonics: {len(hist)}")
    for mnem, n in hist.most_common():
        print(f"{n:9d} {100.0 * n / total:6.3f}% {mnem}")
    print("segment-prefixed:", prefixes.most_common(10))


if __name__ == "__main__":
    main(sys.argv[1])
