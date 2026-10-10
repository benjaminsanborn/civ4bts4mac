"""Recompile guest functions to C.

  gen.py BINARY OUTDIR --all [--module PREFIX ...]    lift every function (game + modules)
  gen.py BINARY OUTDIR --roots A,B,... [--module ...]  lift these entries plus their direct-call closure

Also writes OUTDIR/functab.c (runtime tables) and OUTDIR/world.img (initial guest memory).
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
from world import World

_prog = None


def _init(path, modules):
    global _prog
    _prog = Program(World(path, modules))


def _lift(entry: int) -> Tuple[int, str, List[int], List[str]]:
    f = Func(_prog, entry)
    try:
        c = f.lift()
    except Exception as e:  # keep going; a broken function becomes a trap
        c = (f"GRet f_{entry:08x}(LARGS) {{ rt_unimpl(cpu, 0x{entry:x}u, \"lifter crash\"); }}\n")
        f.unsupported.append(f"lifter crash: {type(e).__name__}: {e}")
    return entry, c, sorted(f.calls), f.unsupported


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("outdir")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--roots", default="")
    ap.add_argument("--module", action="append", default=[], help="guest module prefix (from link.py)")
    ap.add_argument("--overrides", default="", help="directory of host sources declaring OVERRIDE(symbol)")
    ap.add_argument("--per-file", type=int, default=400)
    ap.add_argument("-j", type=int, default=os.cpu_count())
    args = ap.parse_args()

    world = World(args.binary, args.module)
    prog = Program(world)
    overrides = find_overrides(args.overrides, world) if args.overrides else {}
    roots = list(prog.starts) if args.all else [int(x, 16) for x in args.roots.split(",") if x]
    done: Dict[int, str] = {}
    unsupported = collections.Counter()
    unsupported_fns = 0
    pending = set(roots)
    with mp.Pool(args.j, initializer=_init, initargs=(args.binary, args.module)) as pool:
        while pending:
            batch = sorted(pending)
            pending = set()
            for e in [b for b in batch if b in overrides]:
                done[e] = (f"void ov_{overrides[e]}(CPU *cpu);\n"
                           f"GRet f_{e:08x}(LARGS) {{ LPROLOGUE; SPILL; ov_{overrides[e]}(cpu); /* host override */\n"
                           f"    cpu->r[4] += 4; RELOAD; LRET; }}\n")
            batch = [b for b in batch if b not in overrides]
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
    written = set()
    for k in range(0, len(entries), args.per_file):
        chunk = entries[k:k + args.per_file]
        name = f"part_{k // args.per_file:04d}.c"
        parts = ['#include "rt.h"\n', 'extern "C" {\n']
        refs = set()
        for e in chunk:
            refs.update(int(x[2:10], 16) for x in _refs(done[e]))
        for r in sorted(refs | set(chunk)):
            parts.append(f"GRet f_{r:08x}(LARGS);\n")
        for e in chunk:
            parts.append("\n" + done[e])
        parts.append("}\n")
        _write_if_changed(os.path.join(args.outdir, name), "".join(parts))
        written.add(name)
        nfiles += 1
    for f in os.listdir(args.outdir):
        if f.startswith("part_") and f.endswith(".c") and f not in written:
            os.remove(os.path.join(args.outdir, f))
            obj = os.path.join(args.outdir, "obj", f[:-2] + ".o")
            if os.path.exists(obj):
                os.remove(obj)
    import io
    with io.StringIO() as out:
        out.write('#include "rt.h"\n#include "functab.h"\n')
        for e in entries:
            out.write(f"GRet f_{e:08x}(LARGS);\n")
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
        # Import stubs, so indirect calls to a stub address still reach the right target.
        stubs = sorted(world.stub_targets().items())
        out.write("const StubEntry g_stubs[] = {\n")
        for addr, (kind, v) in stubs:
            if kind in ("guest", "host"):
                out.write(f"    {{0x{addr:x}u, 0x{v:x}u, {1 if kind == 'host' else 0}}},\n")
        out.write("};\n")
        out.write(f"const uint32_t g_stubs_len = sizeof g_stubs / sizeof g_stubs[0];\n")
        inits = world.init_funcs()
        out.write("const uint32_t g_init_funcs[] = {\n" + "".join(f"    0x{a:x}u,\n" for a in inits) + "};\n")
        out.write(f"const uint32_t g_init_count = {len(inits)};\n")
        out.write("const HostData g_hostdata[] = {\n")
        for n, a in sorted(world.hostdata.items()):
            out.write(f'    {{"{n}", 0x{a:x}u}},\n')
        out.write("};\n")
        out.write(f"const uint32_t g_hostdata_count = {len(world.hostdata)};\n")
        out.write(f"const uint32_t g_main = 0x{world.guest_syms['_main']:x}u;\n")
        ehf = world.eh_frames()
        out.write("const uint32_t g_eh_frames[][2] = {\n" + "".join(f"    {{0x{a:x}u, {n}u}},\n" for a, n in ehf) + "};\n")
        out.write(f"const uint32_t g_eh_frame_count = {len(ehf)};\n")
        _write_if_changed(os.path.join(args.outdir, "functab.c"), out.getvalue())
    world.save_image(os.path.join(args.outdir, "world.img"))
    with open(os.path.join(args.outdir, "symbols.txt"), "w") as out:
        for a, n in world.functions():
            out.write(f"{a:08x} {n}\n")
    # Every exported guest symbol, for dlsym-style lookups at runtime.
    with open(os.path.join(args.outdir, "exports.txt"), "w") as out:
        for n, a in sorted(world.guest_syms.items()):
            out.write(f"{a:08x} {n}\n")

    total = len(entries)
    print(f"lifted {total} functions into {nfiles} files; {unsupported_fns} contain unsupported instructions")
    for k, v in unsupported.most_common(40):
        print(f"  {v:7d} {k}")


def find_overrides(src_dir: str, world: World) -> Dict[int, str]:
    """Guest functions replaced by host code: OVERRIDE(symbol) in runtime sources."""
    import re
    out = {}
    by_name = dict(world.guest_syms)
    for a, n in world.functions():
        by_name.setdefault(n, a)
    for f in sorted(os.listdir(src_dir)):
        if not f.endswith(".c"):
            continue
        for name in re.findall(r"^OVERRIDE\((\w+)\)", open(os.path.join(src_dir, f)).read(), re.M):
            if name not in by_name:
                raise SystemExit(f"override for unknown guest symbol {name} ({f})")
            out[by_name[name]] = name
    return out


def _write_if_changed(path: str, text: str) -> None:
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)


def _refs(c: str):
    import re
    return re.findall(r"\bf_[0-9a-f]{8}\b", c)


if __name__ == "__main__":
    sys.setrecursionlimit(10000)
    main()
