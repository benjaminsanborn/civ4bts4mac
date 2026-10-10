"""The complete guest address space, statically linked at build time.

Layout (guest addresses):
  0x00001000  game executable (__TEXT, __DATA) at its original addresses
  0x01800000  host data cells (stderr pointers, CFString class ref, ...)
  0x01C00000  host import thunks: IMPORT_BASE + 4*id; calling one runs host shim `id`
  0x02000000  guest library modules (libstdc++, ...), linked by link.py

Every symbol is resolved here, with dyld's precedence: the game's own
definitions, then guest modules, then the host. The lifter and the runtime
both consume the result.
"""
import json
import os
import re
import struct
from typing import Dict, List, Optional, Tuple

from link import HOST_DATA, GuestModule, ObjFile, link
from macho import N_EXT, MachO

HOSTDATA_BASE = 0x01800000
IMPORT_BASE = 0x01C00000
MODULE_BASE = 0x02000000
PROT_R, PROT_RW, PROT_RX = 1, 3, 5

# Host-owned data symbols the game references (beyond the libc ones in HOST_DATA).
GAME_HOST_DATA = {
    "___CFConstantStringClassReference": 16, "___stack_chk_guard": 4, "_NXArgc": 4, "_NXArgv": 4,
    "_environ": 4, "___progname": 4, "_kCFAllocatorDefault": 4, "_kCFAllocatorNull": 4,
    "_kCFTypeDictionaryKeyCallBacks": 32, "_kCFTypeDictionaryValueCallBacks": 32,
    "_kCFTypeArrayCallBacks": 32, "_kCFBooleanTrue": 4, "_kCFBooleanFalse": 4,
    "_CGAffineTransformIdentity": 24, "_CGPointZero": 8,
}


def is_data_import(name: str) -> bool:
    """Framework constants (kCFBooleanTrue, kCFBundleNameKey, ...) are data, not code."""
    return re.match(r"^_k[A-Z]", name) is not None


def hostdata_layout(extra: Optional[List[str]] = None) -> Tuple[Dict[str, int], int]:
    """Addresses of the host data cells, and the end of the cell area.

    The fixed set comes first so guest modules linked against it keep valid
    addresses; `extra` framework constants found among the game's imports follow."""
    cells, cur = {}, HOSTDATA_BASE
    for n, size in sorted({**HOST_DATA, **GAME_HOST_DATA}.items()):
        cells[n] = cur
        cur += (size + 15) & ~15
    for n in sorted(set(extra or []) - set(cells)):
        cells[n] = cur
        cur += 32
    return cells, cur



# Small, explicit changes to the game's code, applied before lifting:
# address -> (original bytes, replacement, reason). Skipped if the original
# bytes don't match (a different game build).
GAME_PATCHES = {
    # Aspyr hardcodes the Low graphics level for new profiles (CvUserProfile::
    # reset, and a fallback in CvMenuManager::setupGame) and for Options >
    # Restore Defaults (CvUserProfile::resetOptions). Any Mac running this
    # harness handles High easily: use GRAPHICLEVEL_HIGH (0).
    0x00b4579d: (bytes([2, 0, 0, 0]), bytes([0, 0, 0, 0]), "profile reset: graphics level High"),
    0x00bc27f3: (bytes([2, 0, 0, 0]), bytes([0, 0, 0, 0]), "new profiles: graphics level High"),
    0x00b46c87: (bytes([2, 0, 0, 0]), bytes([0, 0, 0, 0]), "restore defaults: graphics level High"),
}

class World:
    def __init__(self, game_path: str, module_prefixes: List[str]):
        self.game = MachO(game_path)
        self.text = self.game.section("__TEXT", "__text")
        self.modules = [GuestModule.load(p) for p in module_prefixes]
        self.game_exports = {s.name: s.value for s in self.game.symbols if s.defined and (s.type & N_EXT)}
        self.guest_syms: Dict[str, int] = dict(self.game_exports)
        for m in self.modules:
            for n, a in m.symbols.items():
                self.guest_syms.setdefault(n, a)
        # Host imports: every referenced name that is not guest code or host data.
        names = set()
        for _a, n, _ad in self.game.binds("bind") + self.game.binds("lazy_bind"):
            names.add(n)
        names.update(self.game.stubs().values())
        for m in self.modules:
            names.update(m.stubs.values())
        self.hostdata, self.hostdata_end = hostdata_layout(
            [n for n in names if is_data_import(n) and n not in self.guest_syms])
        self.import_names = sorted(n for n in names if n not in self.guest_syms and n not in self.hostdata)
        self.import_id = {n: i for i, n in enumerate(self.import_names)}

    # ---- symbol resolution ----
    def resolve(self, name: str) -> Tuple[str, int]:
        """('guest', addr) for code/data in the guest, ('host', id) for host functions,
        ('data', addr) for host data cells."""
        if name in self.guest_syms:
            return "guest", self.guest_syms[name]
        if name in self.hostdata:
            return "data", self.hostdata[name]
        return "host", self.import_id[name]

    def address_of(self, name: str) -> int:
        kind, v = self.resolve(name)
        return IMPORT_BASE + 4 * v if kind == "host" else v

    def stub_targets(self) -> Dict[int, Tuple[str, int]]:
        """Every import stub (game and modules) -> ('guest', addr) or ('host', id)."""
        out = {}
        for addr, name in self.game.stubs().items():
            out[addr] = self.resolve(name)
        for m in self.modules:
            for addr, name in m.stubs.items():
                out[addr] = self.resolve(name)
        return out

    # ---- code ----
    def code_ranges(self) -> List[Tuple[int, int]]:
        r = [(self.text.addr, self.text.end)]
        for m in self.modules:
            r.extend(m.text_ranges)
        return r

    def functions(self) -> List[Tuple[int, str]]:
        if getattr(self, "_functions", None) is None:
            f = {s.value: s.name for s in self.game.functions()}
            for m in self.modules:
                for a, n in m.functions:
                    f.setdefault(a, n)
            # Functions known only from their unwind info (no symbol).
            for a in self.landing_pads():
                f.setdefault(a, f"fde_{a:08x}")
            self._functions = sorted(f.items())
        return self._functions

    def read(self, addr: int, size: int) -> bytes:
        for m in self.modules:
            if m.base <= addr < m.base + len(m.image):
                return m.read(addr, size)
        data = self.game.read(addr, size)
        for pa, (old, new) in self.patches().items():
            if pa < addr + size and addr < pa + len(new):
                buf = bytearray(data)
                for i, b in enumerate(new):
                    if addr <= pa + i < addr + size:
                        buf[pa + i - addr] = b
                data = bytes(buf)
        return data

    def patches(self) -> Dict[int, Tuple[bytes, bytes]]:
        """GAME_PATCHES whose original bytes match this binary."""
        if getattr(self, "_patches", None) is None:
            self._patches = {}
            for addr, (old, new, why) in GAME_PATCHES.items():
                if self.game.read(addr, len(old)) == old:
                    self._patches[addr] = (old, new)
                else:
                    print(f"warning: patch at {addr:08x} ({why}) does not match this binary; skipped")
        return self._patches

    # ---- exceptions ----
    def eh_frames(self) -> List[Tuple[int, int]]:
        out = []
        sec = self.game.section("__TEXT", "__eh_frame")
        if sec:
            out.append((sec.addr, sec.size))
        for m in self.modules:
            out.extend(m.eh_frames)
        return out

    def landing_pads(self) -> Dict[int, set]:
        """Function start (FDE pc_begin) -> landing pad addresses."""
        if getattr(self, "_pads", None) is not None:
            return self._pads
        from ehframe import Reader, landing_pads, parse_eh_frame
        r = Reader(self.read)
        out = {}
        for base, size in self.eh_frames():
            for fde in parse_eh_frame(r, base, size):
                try:
                    pads = landing_pads(r, fde)
                except (KeyError, IndexError, struct.error):
                    # Known issue: some LSDA pointers in linked guest modules are mis-relocated.
                    continue
                if pads:
                    out.setdefault(fde.pc_begin, set()).update(pads)
        self._pads = out
        return out

    # ---- initial memory image ----
    def regions(self) -> List[Tuple[int, bytes, int]]:
        """(addr, bytes, prot) for everything that must be mapped at startup.
        Addresses and sizes are not page aligned; the loader rounds them."""
        out = []
        g = self.game
        for seg in g.segments:
            if seg.segname in ("__PAGEZERO", "__LINKEDIT"):
                continue
            buf = bytearray(seg.vmsize)
            buf[:seg.filesize] = g.data[seg.fileoff:seg.fileoff + seg.filesize]
            # Apply dyld binds (non-lazy and lazy) inside this segment.
            for a, name, addend in g.binds("bind") + g.binds("lazy_bind"):
                if seg.vmaddr <= a < seg.vmaddr + seg.vmsize:
                    struct.pack_into("<I", buf, a - seg.vmaddr, (self.address_of(name) + addend) & 0xFFFFFFFF)
            for pa, (old, new) in self.patches().items():
                if seg.vmaddr <= pa < seg.vmaddr + seg.vmsize:
                    buf[pa - seg.vmaddr:pa - seg.vmaddr + len(new)] = new
            out.append((seg.vmaddr, bytes(buf), PROT_RW if seg.initprot & 2 else PROT_R))
        out.append((HOSTDATA_BASE, bytes(self.hostdata_end - HOSTDATA_BASE), PROT_RW))
        for m in self.modules:
            img = bytearray(m.image)
            # Module stub slots point at their resolved targets.
            for slot, name in m.slots.items():
                struct.pack_into("<I", img, slot - m.base, self.address_of(name))
            out.append((m.base, bytes(img), PROT_RW))
        return out

    def init_funcs(self) -> List[int]:
        """Static initializers in dyld order: libraries first, then the executable."""
        out = []
        for m in self.modules:
            out.extend(m.mod_init)
        sec = self.game.section("__DATA", "__mod_init_func")
        if sec:
            out.extend(struct.unpack("<%dI" % (sec.size // 4), self.game.read(sec.addr, sec.size)))
        return out

    def save_image(self, path: str) -> None:
        """world.img: 'C4WD', count, then {addr, size, prot} + bytes per region."""
        regs = self.regions()
        with open(path, "wb") as f:
            f.write(b"C4WD" + struct.pack("<I", len(regs)))
            for addr, data, prot in regs:
                f.write(struct.pack("<III", addr, len(data), prot))
                f.write(data)


def build_module(name: str, obj_dir: str, game_path: str, out_prefix: str, base: int) -> GuestModule:
    game = MachO(game_path)
    ext = {s.name: s.value for s in game.symbols if s.defined and (s.type & N_EXT)}
    objs = [ObjFile(os.path.join(obj_dir, f)) for f in sorted(os.listdir(obj_dir)) if f.endswith(".o")]
    hostdata, _ = hostdata_layout()
    mod = link(name, objs, base, {**hostdata, **ext})
    mod.save(out_prefix)
    return mod


if __name__ == "__main__":
    import sys
    game, obj_dir, out_prefix = sys.argv[1:4]
    base = int(sys.argv[4], 16) if len(sys.argv) > 4 else MODULE_BASE
    m = build_module(os.path.basename(out_prefix), obj_dir, game, out_prefix, base)
    print(f"{out_prefix}: {len(m.image):#x} bytes at {m.base:#x}, {len(m.functions)} functions, "
          f"{len(m.stubs)} host imports, {len(m.mod_init)} initializers")
