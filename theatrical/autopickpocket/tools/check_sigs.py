# check_sigs.py -- resolve the signatures and vtables of src/autopickpocket.cpp against a WHGame.dll on disk,
# the same way the mod does at startup, and print what was found.  No Windows needed.
# Usage: python tools/check_sigs.py <path to WHGame.dll>      (pip install pefile)
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(__file__), '..', 'src', 'autopickpocket.cpp')
src = open(SRC).read()
sig = dict(re.findall(r'static const char\* (SIG_\w+)\s*=\s*"([^"]+)"', src))
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
secs = pe.sections
t = [s for s in secs if s.Name.startswith(b'.text')][0]; lo = t.VirtualAddress; text = img[lo:lo + t.Misc_VirtualSize]
data = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in secs
        if not s.Characteristics & (0x02000000 | 0x20000000)]          # not discardable, not executable

def rx(p): return b''.join(b'.' if x.startswith('?') else re.escape(bytes([int(x, 16)])) for x in p.split())
def find(n):
    m = [lo + x.start() for x in re.finditer(rx(sig[n]), text, re.S)]
    if len(m) != 1: sys.exit(f'FAIL {n}: {len(m)} matches')
    return m[0]
def q(r): return struct.unpack_from('<Q', img, r)[0]
def u32s(r, n): return struct.unpack_from('<%dI' % n, img, r)

def vtable(mangled, offset):
    td = img.find(mangled.encode() + b'\0') - 0x10
    for a, b in data:
        p = img.find(struct.pack('<I', td), a, b)
        while p >= 0:
            col = p - 0xC
            sg, off, _, tdr, _, selfr = u32s(col, 6)
            if sg == 1 and off == offset and tdr == td and selfr == col:
                for a2, b2 in data:
                    v = img.find(struct.pack('<Q', B + col), a2, b2)
                    while v >= 0:
                        if v % 8 == 0: return v + 8
                        v = img.find(struct.pack('<Q', B + col), v + 1, b2)
            p = img.find(struct.pack('<I', td), p + 1, b)
    sys.exit('FAIL vtable ' + mangled)

def vt_with(a, b, c):
    hits = []
    for x, y in data:
        for v in range((x + 7) & ~7, y - 24, 8):
            if q(v) == B + a and q(v + 16) == B + c and (b is None or q(v + 8) == B + b): hits.append(v)
    return hits

f = {n: find(n) for n in ('SIG_UPDATE', 'SIG_CHARGE', 'SIG_CALLFN', 'SIG_HINT', 'SIG_CLOSE', 'SIG_OPEN', 'SIG_BOOST', 'SIG_FILL', 'SIG_SS_UPDATE', 'SIG_SS_EXEC')}
for n, a in f.items(): print(f'{n:14} {a:#x}')
pp = vtable('.?AVC_Pickpocketing@playermodule@wh@@', 0)
item = vtable('.?AVC_UIFlashPickpocketingItem@guimodule@wh@@', 0)
if q(pp + 19 * 8) - B != f['SIG_UPDATE']: sys.exit('FAIL Update not in C_Pickpocketing vtbl[19]')
if q(item + 8) - B != f['SIG_FILL']: sys.exit('FAIL FillUIArgs not in item vtbl[1]')
h = vt_with(f['SIG_OPEN'], None, f['SIG_BOOST'])
if len(h) != 1: sys.exit(f'FAIL I_UIPickpocketing vtable: {len(h)} candidates')
ui = h[0]; close = q(ui + 8) - B
if len(vt_with(f['SIG_OPEN'], close, f['SIG_BOOST'])) != 1: sys.exit('FAIL I_UIPickpocketing vtable not unique')
print(f'C_Pickpocketing vtable      {pp:#x}  [19] Update')
print(f'item vtable                 {item:#x}  [1] FillUIArgs')
print(f'I_UIPickpocketing vtable    {ui:#x}  [0] Open [1] {close:#x} [2] SetBoostProgress')
# the inline-detour prologues must be whole instructions: compare with the signature bytes copied
for n, k in (('SIG_HINT', 'HINT_COPY'), ('SIG_CLOSE', 'CLOSE_COPY')):
    cnt = int(re.search(k + r' = (\d+)', src).group(1))
    if '??' in sig[n].split()[:cnt]: sys.exit(f'FAIL {n}: wildcard inside the copied prologue')
    print(f'{n:14} prologue {img[f[n]:f[n] + cnt].hex(" ")}')
print('OK')
