"""Run the delay slot of a call whose slot address another function owns.

allegrexrecomp emits a `jal`'s delay slot before the call only when the slot belongs
to the same body. When function discovery splits the code right at the slot, the call
goes out without it and the slot runs only after the callee returns, through the
"falls into 0x..., owned by another function" exit. In DJMAX that dropped arguments:

  0x3479C  jal 0x8279C / ori $a1,$zero,0xFF  -> layer alpha 0: the name screen's
           background, header and frame never drew (alpha test GREATER 0).
  0x1000C  jal 0x6D514 / addiu $a0,$s0,13696 -> the callee got a stale $a0.

This copies the slot's C in front of the call. The fall-into still runs it once more
after the return, which is only harmless for a slot that writes an argument register
from a register the call does not change; anything else stops the build.

usage: fix_foreign_slots.py <dir with djmax_funcs_*.c>   (rewrites the files in place)
"""
import glob
import os
import re
import sys

CALL = re.compile(r'^    /\* ([0-9A-F]{8})  (jal|jalr) .*\*/$')
FALLS = re.compile(r'^    /\* falls into 0x([0-9A-F]{8}), owned by another function \*/$')
ARG_REGS = ('$a0', '$a1', '$a2', '$a3', '$t0', '$t1', '$t2', '$t3')


def slot_code(files, addr):
    """The disassembly comment and C statement(s) of the instruction at addr."""
    tag = '    /* %08X  ' % addr
    for lines in files.values():
        for i, l in enumerate(lines):
            if not l.startswith(tag):
                continue
            body = []
            for k in range(i + 1, len(lines)):
                s = lines[k]
                if s.startswith('    /*') or s.startswith('L_') or s.startswith('}'):
                    break
                body.append(s)
            return l, body
    return None, None


def safe(asm):
    """ori/addiu/lui/addu/or into an argument register: idempotent across the call
    only if it does not read $v0/$v1/an argument register the callee may have changed."""
    m = re.match(r'/\* [0-9A-F]{8}  (ori|addiu|lui|addu|or)\s+(\$\w+)(.*)\*/', asm.strip())
    if not m or m.group(2) not in ARG_REGS:
        return False
    srcs = re.findall(r'\$\w+', m.group(3))
    return all(r in ('$zero', '$sp', '$gp') or r.startswith('$s') or r == '$fp' for r in srcs)


def main(d):
    paths = sorted(glob.glob(os.path.join(d, 'djmax_funcs_*.c')))
    files = {p: open(p, encoding='utf-8').read().split('\n') for p in paths}
    fixed = 0
    for p, lines in files.items():
        out, i, changed = [], 0, False
        while i < len(lines):
            m = CALL.match(lines[i])
            if not m:
                out.append(lines[i]); i += 1; continue
            call = int(m.group(1), 16)
            # find the end of this call block and check it falls into call+4
            j, falls = i + 1, None
            while j < len(lines) and j < i + 12:
                f = FALLS.match(lines[j])
                if f: falls = int(f.group(1), 16); break
                if CALL.match(lines[j]) or lines[j].startswith('L_'): break
                j += 1
            nxt = lines[i + 1] if i + 1 < len(lines) else ''
            has_slot = nxt.strip().startswith('PSP_I(') and i + 2 < len(lines) and \
                lines[i + 2].startswith('    /* %08X  ' % (call + 4))
            if falls != call + 4 or has_slot or lines[i + 1:i + 3].count('    /* fixed: foreign slot */'):
                out.append(lines[i]); i += 1; continue
            asm, body = slot_code(files, call + 4)
            if asm is None or not safe(asm):
                sys.exit('fix_foreign_slots: unsafe or missing slot at 0x%08X: %s' % (call + 4, asm))
            out.append(lines[i])
            # keep the call's own PSP_I line first
            k = i + 1
            if lines[k].strip().startswith('PSP_I('):
                out.append(lines[k]); k += 1
            out.append('    /* fixed: foreign slot */')
            out.append(asm)
            out.extend(body)
            i = k
            fixed += 1
            changed = True
        if changed:
            files[p] = out
            open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('fix_foreign_slots: %d call(s) given their delay slot' % fixed)


if __name__ == '__main__':
    main(sys.argv[1])
