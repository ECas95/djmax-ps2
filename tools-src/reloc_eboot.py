#!/usr/bin/env python3
"""Apply the PSP loader's relocation step to the EBOOT, once, on disk.

    reloc_eboot.py <EBOOT.BIN> <out>      write the relocated ELF
    reloc_eboot.py --check <EBOOT.BIN>    relocate in memory and run the asserts

Addends are relative to the segment named by valbase: value = addend + seg[valbase].
Proof on this binary: valbase=1 addends reach 0x45CE9C, seg1 memsz is 0x45CEA0.

The module loads at 0, so segment 0 needs nothing and only valbase=1 changes bytes.
The emitter and the embedded segments both read the output, so code and data agree.
"""
import struct
import sys

R_MIPS_32, R_MIPS_HI16, R_MIPS_LO16 = 2, 5, 6


def sext16(x):
    return x - 0x10000 if x & 0x8000 else x


def segments(b):
    phoff = struct.unpack_from("<I", b, 0x1C)[0]
    phnum = struct.unpack_from("<H", b, 0x2C)[0]
    return [struct.unpack_from("<8I", b, phoff + i * 32) for i in range(phnum)]


def reloc_tables(b):
    shoff = struct.unpack_from("<I", b, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", b, 0x2E)
    for i in range(shnum):
        s = struct.unpack_from("<10I", b, shoff + i * shentsize)
        if s[1] in (9, 0x700000A0):
            yield [struct.unpack_from("<II", b, s[4] + r) for r in range(0, s[5] - 7, 8)]


def relocate(raw):
    """Returns (relocated bytes, resolved seg-1 HI16/LO16 addresses, counts)."""
    segs = segments(raw)
    vaddr = [s[2] for s in segs]
    fileoff = [s[1] for s in segs]
    filesz = [s[4] for s in segs]
    out = bytearray(raw)
    rd = lambda buf, p: struct.unpack_from("<I", buf, p)[0]
    wr = lambda p, v: struct.pack_into("<I", out, p, v & 0xFFFFFFFF)
    addrs, counts = [], {R_MIPS_32: 0, R_MIPS_HI16: 0, R_MIPS_LO16: 0}

    def pos(r_off, ofsbase):
        assert r_off + 4 <= filesz[ofsbase], "reloc outside segment file data"
        return fileoff[ofsbase] + r_off

    for table in reloc_tables(raw):
        for n, (r_off, info) in enumerate(table):
            typ, ofsbase, valbase = info & 0xFF, (info >> 8) & 0xFF, (info >> 16) & 0xFF
            if valbase == 0 or typ not in counts:
                continue
            base, p = vaddr[valbase], pos(r_off, ofsbase)
            word = rd(raw, p)
            if typ == R_MIPS_32:
                wr(p, word + base)
            elif typ == R_MIPS_LO16:
                wr(p, (word & 0xFFFF0000) | ((word + base) & 0xFFFF))
            else:
                # The carry into HI16 depends on the LO16 this HI pairs with: the
                # next one in the table. Read it from raw, it may be patched already.
                lo_off, lo_info = next(e for e in table[n + 1:] if e[1] & 0xFF == R_MIPS_LO16)
                assert (lo_info >> 16) & 0xFF == valbase, "HI16/LO16 valbase mismatch"
                lo = rd(raw, pos(lo_off, (lo_info >> 8) & 0xFF)) & 0xFFFF
                addr = ((word & 0xFFFF) << 16) + sext16(lo) + base
                wr(p, (word & 0xFFFF0000) | (((addr + 0x8000) >> 16) & 0xFFFF))
                addrs.append(addr)
            counts[typ] += 1
    return out, addrs, counts


def check(raw, out, addrs, counts):
    vaddr, memsz = segments(raw)[1][2], segments(raw)[1][5]
    assert addrs and all(vaddr <= a < vaddr + memsz for a in addrs), "address outside segment 1"
    assert counts[R_MIPS_HI16] == 1778 and counts[R_MIPS_LO16] == 1962, counts
    assert counts[R_MIPS_32] == 33, counts
    seg0 = segments(raw)[0][1]
    insn = lambda buf, a: struct.unpack_from("<I", buf, seg0 + a)[0]

    def resolved(buf, hi, lo):
        return ((insn(buf, hi) & 0xFFFF) << 16) + sext16(insn(buf, lo) & 0xFFFF)

    # Positive LO, the pair that walks .cplinit: 0x00000 + 0x000 -> 0xE15D8.
    assert resolved(out, 0x92E30, 0x92E34) == 0x000E15D8
    # Negative LO, needs the carry: raw 0x6B28 -> 0xE8100, hi must be 0xF, not 0xE.
    assert resolved(raw, 0x7ED3C, 0x7ED40) == 0x6B28
    assert resolved(out, 0x7ED3C, 0x7ED40) == 0x000E8100
    assert insn(out, 0x7ED3C) & 0xFFFF == 0xF


if __name__ == "__main__":
    args = sys.argv[1:]
    want_check = args[:1] == ["--check"]
    raw = open(args[1 if want_check else 0], "rb").read()
    out, addrs, counts = relocate(raw)
    check(raw, out, addrs, counts)
    if not want_check:
        open(args[1], "wb").write(out)
    print("relocated HI16=%d LO16=%d WORD32=%d, %d..%#x" % (
        counts[R_MIPS_HI16], counts[R_MIPS_LO16], counts[R_MIPS_32], len(raw), max(addrs)))
