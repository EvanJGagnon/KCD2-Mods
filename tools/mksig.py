# mksig.py -- build unique, update-tolerant byte signatures for WHGame.dll.
# Wildcards: rip-relative displacements and relative branch targets.  Struct offsets,
# immediates and register encodings stay literal, so a layout change breaks the match
# (the mod then disables itself instead of touching the wrong memory).
import re, sys
from capstone import x86
from whimg import Img

im = Img()
TEXT = bytes(im.mem[im.text[0]:im.text[1]])

def insn_mask(i):
    b = list(i.bytes); m = [True] * len(b)
    def wild(off, size):
        for k in range(off, off + size): m[k] = False
    if i.disp_size and any(op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP for op in i.operands):
        wild(i.disp_offset, i.disp_size)
    if i.group(1) or i.group(7):   # jump / relative branch
        for op in i.operands:
            if op.type == x86.X86_OP_IMM and i.imm_size: wild(i.imm_offset, i.imm_size)
    if i.mnemonic == 'call' and i.operands[0].type == x86.X86_OP_IMM: wild(1, 4)
    return b, m

def matches(pat, cands):
    out = []
    for c in cands:
        s = TEXT[c:c + len(pat)]
        if len(s) == len(pat) and all(p is None or p == x for p, x in zip(pat, s)): out.append(c)
    return out

def initial_cands(pat):
    # longest literal run -> find all
    best = (0, 0); cur = None
    for k, p in enumerate(pat + [None]):
        if p is not None and cur is None: cur = k
        if p is None and cur is not None:
            if k - cur > best[1] - best[0]: best = (cur, k)
            cur = None
    lit = bytes(pat[best[0]:best[1]])
    out = []; pos = TEXT.find(lit)
    while pos >= 0:
        out.append(pos - best[0]); pos = TEXT.find(lit, pos + 1)
    return [c for c in out if c >= 0]

def gen(start, anchor, through=None, minlen=12, maxlen=160):
    """Pattern from insn boundary `start`; must cover `through` (inclusive byte); unique in .text."""
    ins = im.insns(start, 80); pat = []
    need = (through or anchor) + 1
    cands = None
    for i in ins:
        b, m = insn_mask(i); pat += [x if k else None for x, k in zip(b, m)]
        if start + len(pat) < need or len(pat) < minlen: continue
        if cands is None: cands = initial_cands(pat)
        cands = matches(pat, cands)
        if len(cands) == 1:
            assert cands[0] + im.text[0] == start
            return fmt(pat), anchor - start
        if len(pat) > maxlen: break
    raise SystemExit(f"no unique sig from {start:#x} ({len(cands) if cands else '?'} matches)")

def fmt(pat): return ' '.join('??' if p is None else f'{p:02X}' for p in pat)

def insn_start_before(rva, back):
    """Instruction boundary at least `back` bytes before rva (linear sweep from function start)."""
    f = im.func_of(rva); a = f[0]; bounds = []
    for i in im.cs.disasm(bytes(im.mem[a:rva + 16]), a):
        bounds.append(i.address)
        if i.address >= rva: break
    ok = [x for x in bounds if x <= rva - back]
    return ok[-1] if ok else f[0]

def insn_at(rva):
    for i in im.insns(insn_start_before(rva, 0), 3):
        if i.address <= rva < i.address + i.size: return i

if __name__ == '__main__':
    kind = sys.argv[1]; a = int(sys.argv[2], 16)
    if kind == 'func': p, o = gen(a, a, int(sys.argv[3], 16) if len(sys.argv) > 3 else None)
    elif kind == 'site':   # call/insn at a; include `back` bytes before and `after` bytes after
        back = int(sys.argv[3], 0); after = int(sys.argv[4], 0)
        s = insn_start_before(a, back); p, o = gen(s, a, a + after)
    print(f'"{p}", {o:#x}')

def verify_pat(a, minlen=24):
    pat = []
    for i in im.insns(a, 30):
        b, m = insn_mask(i); pat += [x if k else None for x, k in zip(b, m)]
        if len(pat) >= minlen or i.mnemonic in ('ret', 'jmp'): break
    return fmt(pat)

import struct
def q(r): return struct.unpack_from('<Q', im.mem, r)[0]
def rtti_vtables(name):
    """name like '.?AVC_Alchemy@playermodule@wh@@' -> {offset: vtable_rva}"""
    mem = bytes(im.mem); td_name = mem.find(name.encode() + b'\0')
    if td_name < 0: return {}
    td = td_name - 0x10; out = {}
    pat = struct.pack('<I', td); p = mem.find(pat)
    while p >= 0:
        col = p - 0xC
        if col >= 0:
            sig, off, cdo, tdr, chd, selfr = struct.unpack_from('<IIIIII', mem, col)
            if sig == 1 and tdr == td and selfr == col:
                ptr = struct.pack('<Q', im.base + col); v = mem.find(ptr)
                while v >= 0:
                    if v % 8 == 0: out[off] = v + 8
                    v = mem.find(ptr, v + 1)
        p = mem.find(pat, p + 1)
    return out
