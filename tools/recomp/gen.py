"""Recompile guest functions to C.

  gen.py BINARY OUTDIR --all             lift every function in __text
  gen.py BINARY OUTDIR --roots A,B,...   lift these entries plus their direct-call closure
"""
import argparse
import collections
import json
import multiprocessing as mp
import os
import sys
import time
from typing import Dict, List, Tuple

from lift import Func, Program

_prog = None


def _init(path):
    global _prog
    _prog = Program(path)


def _lift(entry: int) -> Tuple[int, str, List[int], List[str]]:
    f = Func(_prog, entry)
    try:
        c = f.lift()
    except Exception as e:  # keep going; a broken function becomes a trap
        c = (f"void f_{entry:08x}(CPU *cpu) {{ rt_unimpl(cpu, 0x{entry:x}u, \"lifter crash\"); }}\n")
        f.unsupported.append(f"lifter crash: {type(e).__name__}: {e}")
    return entry, c, sorted(f.calls), f.unsupported


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("outdir")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--roots", default="")
    ap.add_argument("--per-file", type=int, default=400)
    ap.add_argument("-j", type=int, default=os.cpu_count())
    args = ap.parse_args()

    prog = Program(args.binary)
    roots = list(prog.starts) if args.all else [int(x, 16) for x in args.roots.split(",") if x]
    done: Dict[int, str] = {}
    unsupported = collections.Counter()
    unsupported_fns = 0
    pending = set(roots)
    with mp.Pool(args.j, initializer=_init, initargs=(args.binary,)) as pool:
        while pending:
            batch = sorted(pending)
            pending = set()
            for i, (entry, c, calls, unsup) in enumerate(pool.imap_unordered(_lift, batch, chunksize=64)):
                done[entry] = c
                if unsup:
                    unsupported_fns += 1
                for u in unsup:
                    unsupported[u.split(" ")[0] if not u.startswith("lifter") else u] += 1
                for t in calls:
                    if t not in done and prog.in_text(t):
                        pending.add(t)
                if i % 2000 == 0:
                    print("@@progress " + json.dumps({"done": len(done), "total": len(roots), "ts": time.time()}), flush=True)
            pending -= set(done)

    os.makedirs(args.outdir, exist_ok=True)
    entries = sorted(done)
    nfiles = 0
    for k in range(0, len(entries), args.per_file):
        chunk = entries[k:k + args.per_file]
        with open(os.path.join(args.outdir, f"part_{k // args.per_file:04d}.c"), "w") as out:
            out.write('#include "rt.h"\n')
            refs = set()
            for e in chunk:
                refs.update(int(x[2:10], 16) for x in _refs(done[e]))
            for r in sorted(refs | set(chunk)):
                out.write(f"void f_{r:08x}(CPU *cpu);\n")
            for e in chunk:
                out.write("\n" + done[e])
        nfiles += 1
    with open(os.path.join(args.outdir, "functab.c"), "w") as out:
        out.write('#include "rt.h"\n#include "functab.h"\n')
        for e in entries:
            out.write(f"void f_{e:08x}(CPU *cpu);\n")
        out.write("const FuncEntry g_functab[] = {\n")
        for e in entries:
            out.write(f"    {{0x{e:x}u, f_{e:08x}}},\n")
        out.write("};\n")
        out.write(f"const uint32_t g_functab_len = {len(entries)};\n")
        out.write("const char *const g_import_names[] = {\n")
        for n in prog.import_names:
            out.write(f'    "{n}",\n')
        out.write("};\n")
        out.write(f"const uint32_t g_import_count = {len(prog.import_names)};\n")
        # Guest address of every import stub, so indirect calls through the
        # lazy pointers can be routed to the right import.
        out.write("const uint32_t g_import_stub[] = {\n")
        by_name = {}
        for addr, name in prog.stub_import.items():
            by_name.setdefault(name, addr)
        for n in prog.import_names:
            out.write(f"    0x{by_name[n]:x}u,\n")
        out.write("};\n")

    total = len(entries)
    print(f"lifted {total} functions into {nfiles} files; {unsupported_fns} contain unsupported instructions")
    for k, v in unsupported.most_common(40):
        print(f"  {v:7d} {k}")


def _refs(c: str):
    import re
    return re.findall(r"f_[0-9a-f]{8}\(cpu\)", c)


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    main()
