"""Fuse lwr/lwl and swr/swl pairs into one unaligned 32-bit access.

The game reads its packed note/score records with `lwr $r,o($b); lwl $r,o+3($b)` (and
writes them with swr/swl). Emitted one by one, each half is a call that reads (and for a
store, rewrites) the aligned word: four memory calls for one field. psp_read32/psp_write32
take any alignment (runtime_min.c, packed access), so the pair is exactly one of them.

Only adjacent pairs in straight-line code are fused: same register, same base, offsets o
and o+3 in either order, the base not the loaded register, and no label in between.

usage: fuse_unaligned.py <dir with djmax_funcs_*.c>   (rewrites the files in place)
"""
import glob
import os
import re
import sys

LD = re.compile(r'^(\s*)(r_\w+) = psp_(lwl|lwr)\(\2, (r_\w+) \+ (-?\d+)\);$')
ST = re.compile(r'^(\s*)psp_(swl|swr)\((r_\w+), (r_\w+) \+ (-?\d+)\);$')
CMT = re.compile(r'^\s*/\* [0-9A-F]{8}  ')


def pair(a, b, rx, kinds):
    ma, mb = rx.match(a), rx.match(b)
    if not ma or not mb or ma.group(1) != mb.group(1):
        return None
    ia, ib = ma.groups(), mb.groups()
    # groups: indent, reg, kind, base, off  (load)  /  indent, kind, reg, base, off  (store)
    if rx is LD:
        reg_a, k_a, base_a, off_a = ia[1], ia[2], ia[3], int(ia[4])
        reg_b, k_b, base_b, off_b = ib[1], ib[2], ib[3], int(ib[4])
    else:
        k_a, reg_a, base_a, off_a = ia[1], ia[2], ia[3], int(ia[4])
        k_b, reg_b, base_b, off_b = ib[1], ib[2], ib[3], int(ib[4])
    if reg_a != reg_b or base_a != base_b or {k_a, k_b} != kinds:
        return None
    if rx is LD and reg_a == base_a:
        return None
    lo = off_a if k_a == ('lwr' if rx is LD else 'swr') else off_b
    hi = off_b if lo == off_a else off_a
    if hi != lo + 3:
        return None
    return ia[0], reg_a, base_a, lo


def main(d):
    total = 0
    for p in sorted(glob.glob(os.path.join(d, 'djmax_funcs_*.c'))):
        lines = open(p, encoding='utf-8').read().split('\n')
        out, i, n = [], 0, 0
        while i < len(lines):
            # comment, [PSP_I], stmt, comment, [PSP_I], stmt
            def stmt_at(k):
                if k < len(lines) and CMT.match(lines[k]):
                    k += 1
                    if k < len(lines) and lines[k].strip().startswith('PSP_I('):
                        k += 1
                    return k
                return None
            s1 = stmt_at(i)
            s2 = stmt_at(s1 + 1) if s1 is not None else None
            if s1 is not None and s2 is not None and s2 < len(lines):
                for rx, kinds, fmt in ((LD, {'lwl', 'lwr'}, '{0}{1} = psp_read32({2} + {3});'),
                                       (ST, {'swl', 'swr'}, '{0}psp_write32({2} + {3}, {1});')):
                    r = pair(lines[s1], lines[s2], rx, kinds)
                    if r:
                        out.extend(lines[i:s1])
                        out.append(fmt.format(*r) + '   /* fused unaligned pair */')
                        out.extend(lines[s1 + 1:s2])
                        i = s2 + 1
                        n += 1
                        break
                else:
                    out.append(lines[i]); i += 1
                continue
            out.append(lines[i]); i += 1
        if n:
            open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
            total += n
    print('fuse_unaligned: %d pair(s) fused' % total)


if __name__ == '__main__':
    main(sys.argv[1])
