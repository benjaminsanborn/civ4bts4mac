"""Static linker for i386 Mach-O object files (MH_OBJECT) into the guest address space.

Guest libraries (libstdc++, later Python, ...) are compiled with
`clang -target i386-apple-macos10.6 -mdynamic-no-pic` and linked here at a fixed
base address. Symbols resolve in this order:

  1. the game executable's exported symbols (its weak definitions win, like dyld),
  2. definitions in the linked objects (first definition wins),
  3. otherwise an import: a host shim function or host data cell.

The result is a GuestModule: one flat image plus symbol/stub metadata, saved as
<name>.bin + <name>.json.
"""
import json
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from macho import N_EXT, N_SECT, N_STAB, N_TYPE, N_UNDF

MH_OBJECT = 1
S_ZEROFILL = 0x1
S_NON_LAZY_SYMBOL_POINTERS = 0x6
S_LAZY_SYMBOL_POINTERS = 0x7
S_MOD_INIT_FUNC_POINTERS = 0x9
SECTION_TYPE = 0xFF
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x400
INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000

R_VANILLA, R_PAIR, R_SECTDIFF, R_PB_LA_PTR, R_LOCAL_SECTDIFF = 0, 1, 2, 3, 4

# Undefined symbols that are data, not functions. The host runtime owns these
# cells (it fills them at startup); everything else undefined is a function.
HOST_DATA = {
    "___stderrp": 4, "___stdoutp": 4, "___stdinp": 4, "__DefaultRuneLocale": 4096,
    "___mb_cur_max": 4, "___dso_handle": 4,
}

STUB_SIZE = 8  # jmp *[slot]; 2 bytes padding


@dataclass
class ObjSection:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int
    align: int
    reloff: int
    nreloc: int
    flags: int
    reserved1: int
    new_addr: int = 0


@dataclass
class ObjSymbol:
    name: str
    type: int
    sect: int
    desc: int
    value: int

    @property
    def is_defined(self) -> bool:
        return (self.type & N_STAB) == 0 and (self.type & N_TYPE) == N_SECT

    @property
    def is_undef(self) -> bool:
        return (self.type & N_STAB) == 0 and (self.type & N_TYPE) == N_UNDF

    @property
    def is_ext(self) -> bool:
        return bool(self.type & N_EXT)


class ObjFile:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        magic, cpu, _sub, ftype, ncmds, _sz, _flags = struct.unpack_from("<7I", self.data, 0)
        if magic != 0xFEEDFACE or cpu != 7 or ftype != MH_OBJECT:
            raise ValueError(f"{path}: not an i386 Mach-O object")
        self.sections: List[ObjSection] = []
        self.symbols: List[ObjSymbol] = []
        self.indirect: List[int] = []
        off = 28
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<2I", self.data, off)
            if cmd == 0x1:  # LC_SEGMENT
                nsects = struct.unpack_from("<I", self.data, off + 48)[0]
                so = off + 56
                for _ in range(nsects):
                    sect = self.data[so:so + 16].rstrip(b"\0").decode()
                    seg = self.data[so + 16:so + 32].rstrip(b"\0").decode()
                    addr, size, offset, align, reloff, nreloc, flags, r1, _r2 = struct.unpack_from("<9I", self.data, so + 32)
                    self.sections.append(ObjSection(seg, sect, addr, size, offset, align, reloff, nreloc, flags, r1))
                    so += 68
            elif cmd == 0x2:  # LC_SYMTAB
                symoff, nsyms, stroff, _strsize = struct.unpack_from("<4I", self.data, off + 8)
                for i in range(nsyms):
                    strx, t, sect, desc, value = struct.unpack_from("<IBBhI", self.data, symoff + 12 * i)
                    end = self.data.index(b"\0", stroff + strx)
                    self.symbols.append(ObjSymbol(self.data[stroff + strx:end].decode("latin-1"), t, sect, desc, value))
            elif cmd == 0xB:  # LC_DYSYMTAB
                f = struct.unpack_from("<18I", self.data, off + 8)
                self.indirect = list(struct.unpack_from("<%dI" % f[13], self.data, f[12]))
            off += cmdsize

    def section_of(self, addr: int) -> Optional[int]:
        """1-based index of the section containing an original address."""
        for i, s in enumerate(self.sections):
            if s.addr <= addr < s.addr + max(s.size, 1):
                return i + 1
        return None

    def contents(self, s: ObjSection) -> bytes:
        if (s.flags & SECTION_TYPE) == S_ZEROFILL:
            return b"\0" * s.size
        return self.data[s.offset:s.offset + s.size]


@dataclass
class GuestModule:
    name: str
    base: int
    image: bytearray = field(default_factory=bytearray)
    symbols: Dict[str, int] = field(default_factory=dict)       # defined here
    functions: List[Tuple[int, str]] = field(default_factory=list)  # code symbols (addr, name)
    text_ranges: List[Tuple[int, int]] = field(default_factory=list)
    stubs: Dict[int, str] = field(default_factory=dict)          # stub addr -> import name
    slots: Dict[int, str] = field(default_factory=dict)          # pointer slot -> import name
    data_cells: Dict[str, int] = field(default_factory=dict)     # host data symbol -> addr
    mod_init: List[int] = field(default_factory=list)            # initializer function addrs
    eh_frames: List[Tuple[int, int]] = field(default_factory=list)

    def save(self, path_prefix: str) -> None:
        with open(path_prefix + ".bin", "wb") as f:
            f.write(self.image)
        meta = {
            "name": self.name, "base": self.base, "size": len(self.image),
            "symbols": self.symbols, "functions": self.functions, "text_ranges": self.text_ranges,
            "stubs": {str(k): v for k, v in self.stubs.items()},
            "slots": {str(k): v for k, v in self.slots.items()},
            "data_cells": self.data_cells, "mod_init": self.mod_init, "eh_frames": self.eh_frames,
        }
        with open(path_prefix + ".json", "w") as f:
            json.dump(meta, f)

    @classmethod
    def load(cls, path_prefix: str) -> "GuestModule":
        with open(path_prefix + ".json") as f:
            meta = json.load(f)
        with open(path_prefix + ".bin", "rb") as f:
            image = bytearray(f.read())
        m = cls(meta["name"], meta["base"], image)
        m.symbols = meta["symbols"]
        m.functions = [tuple(x) for x in meta["functions"]]
        m.text_ranges = [tuple(x) for x in meta["text_ranges"]]
        m.stubs = {int(k): v for k, v in meta["stubs"].items()}
        m.slots = {int(k): v for k, v in meta["slots"].items()}
        m.data_cells = meta["data_cells"]
        m.mod_init = meta["mod_init"]
        m.eh_frames = [tuple(x) for x in meta["eh_frames"]]
        return m

    def read(self, addr: int, size: int) -> bytes:
        o = addr - self.base
        return bytes(self.image[o:o + size])


def link(name: str, objs: List[ObjFile], base: int, external: Dict[str, int]) -> GuestModule:
    """Lay out all sections of `objs` from `base`, resolve and apply relocations.

    `external` maps symbol names to addresses defined outside this module (the
    game executable); those win over definitions here.
    """
    mod = GuestModule(name, base)
    # ---- layout: group sections by kind so code is contiguous ----
    def order(s: ObjSection) -> int:
        if s.flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS):
            return 0
        if s.segname == "__TEXT":
            return 1
        if (s.flags & SECTION_TYPE) == S_ZEROFILL:
            return 3
        return 2

    placed = []
    for o in objs:
        for s in o.sections:
            placed.append((order(s), o, s))
    placed.sort(key=lambda t: t[0])
    cur = base
    text_lo = text_hi = None
    for kind, o, s in placed:
        a = 1 << s.align
        cur = (cur + a - 1) & ~(a - 1)
        s.new_addr = cur
        if kind == 0:
            text_lo = cur if text_lo is None else text_lo
            text_hi = cur + s.size
        cur += s.size
    # Synthetic stub area for undefined functions, and data cells for host data.
    undef_funcs: List[str] = []
    undef_data: List[str] = []
    for o in objs:
        for sym in o.symbols:
            if sym.is_undef and sym.is_ext and not sym.value and sym.name not in external:
                lst = undef_data if sym.name in HOST_DATA else undef_funcs
                if sym.name not in lst:
                    lst.append(sym.name)

    # ---- global symbol table ----
    defs: Dict[str, int] = {}
    for o in objs:
        for sym in o.symbols:
            if sym.is_defined and sym.is_ext and sym.name not in defs:
                s = o.sections[sym.sect - 1]
                defs[sym.name] = sym.value - s.addr + s.new_addr
    # Common symbols (tentative C definitions): undefined + external with a size.
    # Allocate each once, unless a real definition exists somewhere.
    commons: Dict[str, Tuple[int, int]] = {}
    for o in objs:
        for sym in o.symbols:
            if sym.is_undef and sym.is_ext and sym.value and sym.name not in defs and sym.name not in external:
                align = 1 << ((sym.desc >> 8) & 0xF)
                size, al = commons.get(sym.name, (0, 1))
                commons[sym.name] = (max(size, sym.value), max(al, align))
    for n, (size, align) in sorted(commons.items()):
        cur = (cur + align - 1) & ~(align - 1)
        defs[n] = cur
        cur += size
    cur = (cur + 15) & ~15
    stub_base = cur
    undef_funcs = [n for n in undef_funcs if n not in defs]
    undef_data = [n for n in undef_data if n not in defs]
    stub_addr = {n: stub_base + i * STUB_SIZE for i, n in enumerate(undef_funcs)}
    cur = stub_base + len(undef_funcs) * STUB_SIZE
    slot_base = (cur + 3) & ~3
    slot_addr = {n: slot_base + 4 * i for i, n in enumerate(undef_funcs)}
    cur = slot_base + 4 * len(undef_funcs)
    data_addr = {}
    for n in undef_data:
        cur = (cur + 15) & ~15
        data_addr[n] = cur
        cur += HOST_DATA[n]
    mod.image = bytearray(cur - base)

    def resolve(sym_name: str) -> int:
        if sym_name in external:
            return external[sym_name]
        if sym_name in defs:
            return defs[sym_name]
        if sym_name in data_addr:
            return data_addr[sym_name]
        return stub_addr[sym_name]

    # ---- copy contents ----
    for kind, o, s in placed:
        if kind != 3:
            off = s.new_addr - base
            mod.image[off:off + s.size] = o.contents(s)
    for n in undef_funcs:
        st = stub_addr[n] - base
        mod.image[st:st + 6] = b"\xff\x25" + struct.pack("<I", slot_addr[n])  # jmp *[slot]
        mod.stubs[stub_addr[n]] = n
        mod.slots[slot_addr[n]] = n
        struct.pack_into("<I", mod.image, slot_addr[n] - base, stub_addr[n])
    mod.data_cells = data_addr

    # ---- relocations ----
    for o in objs:
        delta = [0] + [s.new_addr - s.addr for s in o.sections]

        def sect_delta(orig_addr: int) -> int:
            i = o.section_of(orig_addr)
            if i is None:
                raise ValueError(f"{o.path}: address {orig_addr:#x} outside sections")
            return delta[i]

        for si, s in enumerate(o.sections):
            fix_delta = delta[si + 1]
            relocs = []
            for k in range(s.nreloc):
                w0, w1 = struct.unpack_from("<II", o.data, s.reloff + 8 * k)
                relocs.append((w0, w1))
            k = 0
            while k < len(relocs):
                w0, w1 = relocs[k]
                k += 1
                if w0 & 0x80000000:  # scattered
                    rtype = (w0 >> 24) & 0xF
                    length = (w0 >> 28) & 3
                    pcrel = (w0 >> 30) & 1
                    raddr = w0 & 0xFFFFFF
                    rvalue = w1
                    where = s.new_addr + raddr
                    old = _get(mod, where, length)
                    if rtype in (R_SECTDIFF, R_LOCAL_SECTDIFF):
                        pw0, pw1 = relocs[k]
                        k += 1
                        new = old + sect_delta(rvalue) - sect_delta(pw1)
                    elif rtype == R_VANILLA:
                        new = old + sect_delta(rvalue) - (fix_delta if pcrel else 0)
                    else:
                        raise ValueError(f"{o.path}: scattered reloc type {rtype}")
                    _put(mod, where, length, new)
                    continue
                raddr = w0
                symnum = w1 & 0xFFFFFF
                pcrel = (w1 >> 24) & 1
                length = (w1 >> 25) & 3
                extern = (w1 >> 27) & 1
                rtype = (w1 >> 28) & 0xF
                if rtype != R_VANILLA:
                    raise ValueError(f"{o.path}: reloc type {rtype}")
                where = s.new_addr + raddr
                old = _get(mod, where, length)
                if extern:
                    sym = o.symbols[symnum]
                    if sym.is_defined and not sym.is_ext:
                        tgt = sym.value - o.sections[sym.sect - 1].addr + o.sections[sym.sect - 1].new_addr
                    else:
                        tgt = resolve(sym.name)
                    new = old + tgt - (fix_delta if pcrel else 0)
                else:
                    new = old + delta[symnum] - (fix_delta if pcrel else 0)
                _put(mod, where, length, new)

        # Indirect pointer sections (non-lazy / lazy pointers) get filled directly.
        for s in o.sections:
            t = s.flags & SECTION_TYPE
            if t in (S_NON_LAZY_SYMBOL_POINTERS, S_LAZY_SYMBOL_POINTERS):
                for i in range(s.size // 4):
                    idx = o.indirect[s.reserved1 + i]
                    if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                        continue  # local: already relocated content
                    sym = o.symbols[idx]
                    if sym.is_defined and not sym.is_ext:
                        tgt = sym.value - o.sections[sym.sect - 1].addr + o.sections[sym.sect - 1].new_addr
                    else:
                        tgt = resolve(sym.name)
                    struct.pack_into("<I", mod.image, s.new_addr + 4 * i - base, tgt)
            if t == S_MOD_INIT_FUNC_POINTERS:
                for i in range(s.size // 4):
                    mod.mod_init.append(struct.unpack_from("<I", mod.image, s.new_addr + 4 * i - base)[0])
            if s.sectname == "__eh_frame":
                mod.eh_frames.append((s.new_addr, s.size))

    # ---- unrelocated pc-relative fields in __eh_frame ----
    for o in objs:
        for si, sec in enumerate(o.sections):
            if sec.sectname == "__eh_frame":
                _fix_eh_frame(mod, o, sec)

    # ---- exported metadata ----
    mod.symbols = {n: a for n, a in defs.items() if n not in external}
    funcs = {}
    for o in objs:
        for sym in o.symbols:
            if sym.is_defined:
                s = o.sections[sym.sect - 1]
                if order(s) == 0:
                    a = sym.value - s.addr + s.new_addr
                    funcs.setdefault(a, sym.name)
    mod.functions = sorted(funcs.items())
    if text_lo is not None:
        mod.text_ranges = [(text_lo, text_hi)]
    return mod


def _fix_eh_frame(mod: GuestModule, o: "ObjFile", sec: "ObjSection") -> None:
    """clang emits __eh_frame pc-relative fields (FDE pc_begin, LSDA and
    personality pointers) without relocations, assuming the object's section
    layout. Re-point them for the linked layout."""
    from ehframe import sleb, uleb

    data = o.contents(sec)
    delta_of = [0] + [s.new_addr - s.addr for s in o.sections]
    # Fields that do have a relocation were already fixed by the relocation pass.
    relocated = set()
    for k in range(sec.nreloc):
        w0 = struct.unpack_from("<I", o.data, sec.reloff + 8 * k)[0]
        relocated.add((w0 & 0xFFFFFF) if w0 & 0x80000000 else w0)
    fix_delta = sec.new_addr - sec.addr

    def repoint(off: int, enc: int) -> int:
        """Rewrite the encoded pointer at section offset `off`; returns its size."""
        if enc == 0xFF:
            return 0
        fmt = enc & 0x0F
        if fmt not in (0x00, 0x03, 0x0B):
            raise ValueError(f"{o.path}: eh_frame pointer format {enc:#x}")
        if (enc & 0x70) == 0x10 and off not in relocated:  # pcrel, no relocation
            v = struct.unpack_from("<i", data, off)[0]
            field_old = sec.addr + off
            target_old = (field_old + v) & 0xFFFFFFFF
            i = o.section_of(target_old)
            if i is not None:
                new_v = (target_old + delta_of[i]) - (field_old + fix_delta)
                struct.pack_into("<i", mod.image, sec.new_addr + off - mod.base, new_v)
        return 4

    cies = {}
    p = 0
    while p + 4 <= len(data):
        length = struct.unpack_from("<I", data, p)[0]
        if length == 0:
            break
        rec = p + 4
        nxt = rec + length
        cid = struct.unpack_from("<I", data, rec)[0]
        if cid == 0:
            q = rec + 5
            end = data.index(b"\0", q)
            aug = data[q:end].decode("latin-1")
            q = end + 1
            _, q = uleb(data, q)
            _, q = sleb(data, q)
            q += 1
            fde_enc, lsda_enc = 0, 0xFF
            if aug.startswith("z"):
                _, q = uleb(data, q)
                for c in aug[1:]:
                    if c == "R":
                        fde_enc = data[q]
                        q += 1
                    elif c == "L":
                        lsda_enc = data[q]
                        q += 1
                    elif c == "P":
                        penc = data[q]
                        q += 1
                        q += repoint(q, penc)
            cies[p] = (fde_enc, lsda_enc, aug.startswith("z"))
        else:
            fde_enc, lsda_enc, has_z = cies.get(rec - cid, (0, 0xFF, False))
            q = rec + 4
            q += repoint(q, fde_enc)  # pc_begin
            q += 4                    # pc_range (absolute)
            if has_z:
                _, q = uleb(data, q)
                if lsda_enc != 0xFF:
                    repoint(q, lsda_enc)
        p = nxt


def _get(mod: GuestModule, addr: int, length: int) -> int:
    fmt = {0: "<b", 1: "<h", 2: "<i"}[length]
    return struct.unpack_from(fmt, mod.image, addr - mod.base)[0]


def _put(mod: GuestModule, addr: int, length: int, value: int) -> None:
    fmt = {0: "<B", 1: "<H", 2: "<I"}[length]
    struct.pack_into(fmt, mod.image, addr - mod.base, value & ((1 << (8 << length)) - 1))
