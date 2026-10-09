"""Minimal parser for 32-bit i386 Mach-O executables.

Only what the recompiler needs: segments/sections, the symbol table, and the
indirect symbol table (to name the import stubs and lazy pointers).
"""
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

MH_MAGIC = 0xFEEDFACE
LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_DYSYMTAB = 0xB
LC_DYLD_INFO_ONLY = 0x80000022
INDIRECT_SYMBOL_LOCAL = 0x80000000
INDIRECT_SYMBOL_ABS = 0x40000000
N_STAB = 0xE0
N_TYPE = 0x0E
N_EXT = 0x01
N_SECT = 0x0E
N_UNDF = 0x0


@dataclass
class Section:
    sectname: str
    segname: str
    addr: int
    size: int
    offset: int
    flags: int
    reserved1: int
    reserved2: int

    @property
    def end(self) -> int:
        return self.addr + self.size


@dataclass
class Segment:
    segname: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    maxprot: int
    initprot: int
    sections: List[Section] = field(default_factory=list)


@dataclass
class Symbol:
    name: str
    type: int
    sect: int
    desc: int
    value: int

    @property
    def defined(self) -> bool:
        return (self.type & N_STAB) == 0 and (self.type & N_TYPE) == N_SECT

    @property
    def undefined(self) -> bool:
        return (self.type & N_STAB) == 0 and (self.type & N_TYPE) == N_UNDF


class MachO:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            self.data = f.read()
        magic, cputype, _sub, filetype, ncmds, _sz, _flags = struct.unpack_from("<7I", self.data, 0)
        if magic != MH_MAGIC or cputype != 7:
            raise ValueError("not a 32-bit i386 Mach-O")
        self.segments: List[Segment] = []
        self.sections: List[Section] = []
        self.symbols: List[Symbol] = []
        self.indirect: List[int] = []
        self.dyld_info: Optional[Dict[str, Tuple[int, int]]] = None
        off = 28
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<2I", self.data, off)
            if cmd == LC_SEGMENT:
                self._parse_segment(off)
            elif cmd == LC_SYMTAB:
                symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", self.data, off + 8)
                self._parse_symtab(symoff, nsyms, stroff)
            elif cmd == LC_DYLD_INFO_ONLY:
                f = struct.unpack_from("<10I", self.data, off + 8)
                self.dyld_info = {"rebase": (f[0], f[1]), "bind": (f[2], f[3]), "weak_bind": (f[4], f[5]),
                                  "lazy_bind": (f[6], f[7]), "export": (f[8], f[9])}
            elif cmd == LC_DYSYMTAB:
                f = struct.unpack_from("<18I", self.data, off + 8)
                indirectsymoff, nindirectsyms = f[12], f[13]
                self.indirect = list(struct.unpack_from("<%dI" % nindirectsyms, self.data, indirectsymoff))
            off += cmdsize
        self._by_name: Dict[str, Symbol] = {}
        for s in self.symbols:
            if s.defined and s.name not in self._by_name:
                self._by_name[s.name] = s

    def _parse_segment(self, off: int) -> None:
        segname = self.data[off + 8:off + 24].rstrip(b"\0").decode()
        vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _flags = struct.unpack_from(
            "<8I", self.data, off + 24)
        seg = Segment(segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot)
        soff = off + 56
        for _ in range(nsects):
            sectname = self.data[soff:soff + 16].rstrip(b"\0").decode()
            sseg = self.data[soff + 16:soff + 32].rstrip(b"\0").decode()
            addr, size, offset, _align, _reloff, _nreloc, flags, r1, r2 = struct.unpack_from(
                "<9I", self.data, soff + 32)
            sec = Section(sectname, sseg, addr, size, offset, flags, r1, r2)
            seg.sections.append(sec)
            self.sections.append(sec)
            soff += 68
        self.segments.append(seg)

    def _parse_symtab(self, symoff: int, nsyms: int, stroff: int) -> None:
        for i in range(nsyms):
            strx, ntype, sect, desc, value = struct.unpack_from("<IBBhI", self.data, symoff + i * 12)
            end = self.data.index(b"\0", stroff + strx)
            name = self.data[stroff + strx:end].decode("latin-1")
            self.symbols.append(Symbol(name, ntype, sect, desc, value))

    def section(self, segname: str, sectname: str) -> Optional[Section]:
        for s in self.sections:
            if s.segname == segname and s.sectname == sectname:
                return s
        return None

    def defined_symbol(self, name: str) -> Optional[Symbol]:
        return self._by_name.get(name)

    def read(self, addr: int, size: int) -> bytes:
        for seg in self.segments:
            if seg.vmaddr <= addr < seg.vmaddr + seg.vmsize:
                rel = addr - seg.vmaddr
                if rel >= seg.filesize:
                    return b"\0" * size
                return self.data[seg.fileoff + rel:seg.fileoff + min(rel + size, seg.filesize)].ljust(size, b"\0")
        raise KeyError(hex(addr))

    def stubs(self) -> Dict[int, str]:
        """Map each __symbol_stub entry address to the symbol name it jumps to."""
        sec = self.section("__TEXT", "__symbol_stub")
        out: Dict[int, str] = {}
        if not sec:
            return out
        n = sec.size // sec.reserved2
        for i in range(n):
            idx = self.indirect[sec.reserved1 + i]
            if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                continue
            out[sec.addr + i * sec.reserved2] = self.symbols[idx].name
        return out

    def pointer_slots(self, sectname: str) -> Dict[int, str]:
        """Map each non-lazy/lazy pointer slot address to its symbol name."""
        sec = self.section("__DATA", sectname)
        out: Dict[int, str] = {}
        if not sec:
            return out
        for i in range(sec.size // 4):
            idx = self.indirect[sec.reserved1 + i]
            if idx & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS):
                continue
            out[sec.addr + i * 4] = self.symbols[idx].name
        return out

    def binds(self, which: str = "bind") -> List[Tuple[int, str, int]]:
        """Run the dyld bind opcode stream: [(address, symbol, addend)].

        `which` is "bind", "lazy_bind" or "weak_bind".
        """
        if self.dyld_info is None:
            return []
        off, size = self.dyld_info[which]
        segs = self.segments
        out: List[Tuple[int, str, int]] = []
        p, end = off, off + size
        sym, addend, seg_i, seg_off = "", 0, 0, 0
        ptr = 4

        def uleb(p):
            v = shift = 0
            while True:
                b = self.data[p]
                p += 1
                v |= (b & 0x7F) << shift
                shift += 7
                if b < 0x80:
                    return v, p

        def sleb(p):
            v = shift = 0
            while True:
                b = self.data[p]
                p += 1
                v |= (b & 0x7F) << shift
                shift += 7
                if b < 0x80:
                    if b & 0x40:
                        v -= 1 << shift
                    return v, p

        while p < end:
            b = self.data[p]
            p += 1
            op, imm = b & 0xF0, b & 0x0F
            if op == 0x00:  # DONE
                if which != "lazy_bind":
                    break
            elif op in (0x10, 0x20, 0x30):  # SET_DYLIB_ORDINAL_* (ignored)
                if op == 0x20:
                    _, p = uleb(p)
            elif op == 0x40:  # SET_SYMBOL_TRAILING_FLAGS_IMM
                e = self.data.index(b"\0", p)
                sym = self.data[p:e].decode("latin-1")
                p = e + 1
            elif op == 0x50:  # SET_TYPE_IMM
                pass
            elif op == 0x60:  # SET_ADDEND_SLEB
                addend, p = sleb(p)
            elif op == 0x70:  # SET_SEGMENT_AND_OFFSET_ULEB
                seg_i = imm
                seg_off, p = uleb(p)
            elif op == 0x80:  # ADD_ADDR_ULEB
                v, p = uleb(p)
                seg_off = (seg_off + v) & 0xFFFFFFFF
            elif op == 0x90:  # DO_BIND
                out.append((segs[seg_i].vmaddr + seg_off, sym, addend))
                seg_off += ptr
            elif op == 0xA0:  # DO_BIND_ADD_ADDR_ULEB
                out.append((segs[seg_i].vmaddr + seg_off, sym, addend))
                v, p = uleb(p)
                seg_off = (seg_off + ptr + v) & 0xFFFFFFFF
            elif op == 0xB0:  # DO_BIND_ADD_ADDR_IMM_SCALED
                out.append((segs[seg_i].vmaddr + seg_off, sym, addend))
                seg_off += ptr + imm * ptr
            elif op == 0xC0:  # DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                count, p = uleb(p)
                skip, p = uleb(p)
                for _ in range(count):
                    out.append((segs[seg_i].vmaddr + seg_off, sym, addend))
                    seg_off += ptr + skip
            else:
                raise ValueError(f"bind opcode {b:#x}")
        return out

    def functions(self) -> List[Symbol]:
        """Defined symbols that live in __TEXT,__text, sorted by address, de-duplicated."""
        text = self.section("__TEXT", "__text")
        text_idx = self.sections.index(text) + 1
        seen = {}
        for s in self.symbols:
            if s.defined and s.sect == text_idx and s.value not in seen:
                seen[s.value] = s
        return [seen[a] for a in sorted(seen)]
