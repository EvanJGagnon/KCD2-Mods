# check_sigs.py -- resolve the signatures and the vtable of src/autolockpick.cpp against a WHGame.dll on disk,
# the way the mod does at startup, and print what was found.  No Windows needed.
# Usage: python tools/check_sigs.py <path to WHGame.dll>      (pip install pefile)
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(__file__), '..', 'src', 'autolockpick.cpp')
src = open(SRC).read()
sig = {k: ''.join(re.findall(r'"([^"]*)"', v)) for k, v in
       re.findall(r'static const char\* (SIG_\w+)\s*=\s*((?:"[^"]*"\s*)+);', src)}
num = dict((k, int(v, 0)) for k, v in re.findall(r'(CURSOR_XI_[XY]|UPDATE_SLOT) = (0x[0-9A-Fa-f]+|\d+)', src))
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]; lo = t.VirtualAddress; hi = lo + t.Misc_VirtualSize
data = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections
        if not s.Characteristics & (0x02000000 | 0x20000000)]

def rx(p): return b''.join(b'.' if x.startswith('?') else re.escape(bytes([int(x, 16)])) for x in p.split())
def find(n):
    m = [lo + x.start() for x in re.finditer(rx(sig[n]), img[lo:hi], re.S)]
    if len(m) != 1: sys.exit(f'FAIL {n}: {len(m)} matches')
    return m[0]
def at(n, a):
    if not re.fullmatch(rx(sig[n]), img[a:a + len(sig[n].split())], re.S): sys.exit(f'FAIL {n} at {a:#x}')
def q(r): return struct.unpack_from('<Q', img, r)[0]
def rel32(f): return f + 4 + struct.unpack_from('<i', img, f)[0]

def vtable(mangled, offset):
    td = img.find(mangled.encode() + b'\0') - 0x10
    for a, b in data:
        p = img.find(struct.pack('<I', td), a, b)
        while p >= 0:
            col = p - 0xC
            sg, off, _, tdr, _, selfr = struct.unpack_from('<6I', img, col)
            if sg == 1 and off == offset and tdr == td and selfr == col:
                for a2, b2 in data:
                    v = img.find(struct.pack('<Q', B + col), a2, b2)
                    while v >= 0:
                        if v % 8 == 0: return v + 8
                        v = img.find(struct.pack('<Q', B + col), v + 1, b2)
            p = img.find(struct.pack('<I', td), p + 1, b)
    sys.exit('FAIL vtable ' + mangled)

vt = vtable('.?AVC_LockPicking@playermodule@wh@@', 0)
up = q(vt + 8 * num['UPDATE_SLOT']) - B; at('SIG_UPDATE', up)
if find('SIG_UPDATE') != up: sys.exit('FAIL Update signature hits elsewhere')
cur = find('SIG_CURSOR'); turn = find('SIG_TURN')
xx = rel32(cur + num['CURSOR_XI_X']); xy = rel32(cur + num['CURSOR_XI_Y']); at('SIG_XI_X', xx); at('SIG_XI_Y', xy)
print(f'C_LockPicking vtable {vt:#x}  [{num["UPDATE_SLOT"]}] Update {up:#x}')
print(f'pick handler         {cur:#x}  gamepad x {xx:#x}  gamepad y {xy:#x}')
print(f'turn key handler     {turn:#x}')
print('OK')
