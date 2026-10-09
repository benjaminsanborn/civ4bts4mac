"""Parse guest __eh_frame (CIE/FDE) and GCC LSDAs (gcc_except_tab) to find
each function's exception landing pads.

Only what the lifter needs: for every FDE, its PC range and the landing pads
named by its LSDA call-site table. The runtime has its own unwinder that
interprets the CFI.
"""
import struct
from typing import Callable, Dict, List, Optional, Set, Tuple

DW_EH_PE_omit = 0xFF


def uleb(data: bytes, p: int) -> Tuple[int, int]:
    v = shift = 0
    while True:
        b = data[p]
        p += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            return v, p


def sleb(data: bytes, p: int) -> Tuple[int, int]:
    v = shift = 0
    while True:
        b = data[p]
        p += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if b < 0x80:
            if b & 0x40:
                v -= 1 << shift
            return v, p


class Reader:
    """Reads encoded pointers from guest memory at guest addresses."""

    def __init__(self, read: Callable[[int, int], bytes]):
        self.read = read

    def bytes_at(self, addr: int, n: int) -> bytes:
        return self.read(addr, n)

    def encoded(self, addr: int, enc: int, func_base: int = 0) -> Tuple[Optional[int], int]:
        """Decode a DW_EH_PE value at guest `addr`; returns (value, next_addr)."""
        if enc == DW_EH_PE_omit:
            return None, addr
        fmt = enc & 0x0F
        app = enc & 0x70
        if fmt == 0x01:  # uleb128
            buf = self.read(addr, 16)
            v, n = uleb(buf, 0)
        elif fmt == 0x09:  # sleb128
            buf = self.read(addr, 16)
            v, n = sleb(buf, 0)
        else:
            size, code = {0x00: (4, "<I"), 0x02: (2, "<H"), 0x03: (4, "<I"), 0x04: (8, "<Q"),
                          0x0A: (2, "<h"), 0x0B: (4, "<i"), 0x0C: (8, "<q")}[fmt]
            v = struct.unpack(code, self.read(addr, size))[0]
            n = size
        if app == 0x10:  # pcrel
            v += addr
        elif app == 0x30:  # datarel? (unused on i386 Darwin)
            pass
        elif app == 0x40:  # funcrel
            v += func_base
        if enc & 0x80:  # indirect
            v = struct.unpack("<I", self.read(v & 0xFFFFFFFF, 4))[0]
        return v & 0xFFFFFFFF, addr + n


class FDE:
    __slots__ = ("pc_begin", "pc_end", "lsda", "personality")

    def __init__(self, pc_begin, pc_end, lsda, personality):
        self.pc_begin, self.pc_end, self.lsda, self.personality = pc_begin, pc_end, lsda, personality


def parse_eh_frame(r: Reader, base: int, size: int) -> List[FDE]:
    out = []
    cies: Dict[int, Tuple[int, int, Optional[int]]] = {}  # cie addr -> (fde_enc, lsda_enc, personality)
    p = base
    end = base + size
    while p + 4 <= end:
        length = struct.unpack("<I", r.read(p, 4))[0]
        if length == 0:
            break
        rec = p + 4
        nxt = rec + length
        cie_id = struct.unpack("<I", r.read(rec, 4))[0]
        if cie_id == 0:
            data = r.read(rec, length)
            q = 5  # after id + version
            aug_end = data.index(b"\0", q)
            aug = data[q:aug_end].decode("latin-1")
            q = aug_end + 1
            _, q = uleb(data, q)  # code align
            _, q = sleb(data, q)  # data align
            q += 1  # return register (version 1)
            fde_enc, lsda_enc, pers = 0, DW_EH_PE_omit, None
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
                        pers, after = r.encoded(rec + q, penc)
                        q = after - rec
            cies[p] = (fde_enc, lsda_enc, pers)
        else:
            cie_addr = rec - cie_id
            fde_enc, lsda_enc, pers = cies.get(cie_addr, (0, DW_EH_PE_omit, None))
            pc_begin, a = r.encoded(rec + 4, fde_enc)
            pc_range, a = r.encoded(a, fde_enc & 0x0F)
            lsda = None
            if lsda_enc != DW_EH_PE_omit:
                buf = r.read(a, 8)
                _, n = uleb(buf, 0)
                a += n
                lsda, a = r.encoded(a, lsda_enc)
                if lsda == 0:
                    lsda = None
            out.append(FDE(pc_begin, (pc_begin + pc_range) & 0xFFFFFFFF, lsda, pers))
        p = nxt
    return out


def landing_pads(r: Reader, fde: FDE) -> Set[int]:
    """Landing pad addresses from the FDE's LSDA call-site table."""
    if fde.lsda is None:
        return set()
    a = fde.lsda
    lp_enc = r.read(a, 1)[0]
    a += 1
    lp_start = fde.pc_begin
    if lp_enc != DW_EH_PE_omit:
        lp_start, a = r.encoded(a, lp_enc)
    tt_enc = r.read(a, 1)[0]
    a += 1
    if tt_enc != DW_EH_PE_omit:
        _, n = uleb(r.read(a, 16), 0)
        a += n
    cs_enc = r.read(a, 1)[0]
    a += 1
    cs_len, n = uleb(r.read(a, 16), 0)
    a += n
    end = a + cs_len
    pads = set()
    while a < end:
        _start, a = r.encoded(a, cs_enc)
        _length, a = r.encoded(a, cs_enc)
        lp, a = r.encoded(a, cs_enc)
        _, n = uleb(r.read(a, 16), 0)
        a += n
        if lp:
            pads.add((lp_start + lp) & 0xFFFFFFFF)
    return pads
