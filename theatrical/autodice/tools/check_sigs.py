# check_sigs.py -- resolve every signature and RTTI lookup of src/autodice.cpp against a WHGame.dll on
# disk, the same way the mod does at startup, and print what was found.  No Windows needed.
# Usage: python tools/check_sigs.py <path to WHGame.dll>      (pip install pefile)
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(__file__), '..', 'src', 'autodice.cpp')
sig = dict(re.findall(r'static const char\* (SIG_\w+)\s*=\s*"([^"]+)"', open(SRC).read()))
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.Name.rstrip(b'\0'), s.VirtualAddress, s.Misc_VirtualSize, s.Characteristics) for s in pe.sections]
lo, sz = [(v, n) for (nm, v, n, c) in secs if nm == b'.text'][0]; text = img[lo:lo + sz]
data = [(v, v + n) for (nm, v, n, c) in secs if nm != b'.text' and not c & (0x02000000 | 0x20000000)]
fails = 0
def fail(what, why):
    global fails; fails += 1; print(f'FAIL {what}: {why}')

def rx(p): return b''.join(b'.' if x.startswith('?') else re.escape(bytes([int(x, 16)])) for x in p.split())
def find(what, n):
    m = [lo + x.start() for x in re.finditer(rx(sig[n]), text, re.S)]
    if len(m) != 1: fail(what, f'{len(m)} matches'); return 0
    print(f'  {what:34} {m[0]:#x}'); return m[0]
def at(r, n): return re.fullmatch(rx(sig[n]), img[r:r + len(sig[n].split())], re.S) is not None
def q(r): return struct.unpack_from('<Q', img, r)[0]
def i32(r): return struct.unpack_from('<i', img, r)[0]
def vtable(what, name, off):
    p = img.find(name.encode() + b'\0')
    if p < 0: fail(what, 'class not found'); return 0
    td = p - 0x10
    for (a, b) in data:
        for col in range(a + 3 & ~3, b - 0x18, 4):
            if struct.unpack_from('<I', img, col + 12)[0] != td: continue
            s0, o, _, _, _, selfr = struct.unpack_from('<IIIIII', img, col)
            if s0 != 1 or o != off or selfr != col: continue
            ptr = struct.pack('<Q', B + col); v = img.find(ptr)
            while v >= 0:
                if v % 8 == 0: print(f'  {what:34} vtable {v + 8:#x}'); return v + 8
                v = img.find(ptr, v + 1)
    fail(what, 'vtable not found'); return 0
def slot(what, vt, i, n):
    if not vt: return 0
    f = q(vt + 8 * i) - B
    if not at(f, n): fail(what, f'slot {i} -> {f:#x} does not match'); return 0
    print(f'  {what:34} {f:#x} (vtbl[{i}])'); return f

vt = vtable('C_Dice', '.?AVC_Dice@playermodule@wh@@', 0)
slot('C_Dice::Update', vt, 14, 'SIG_UPDATE'); slot('C_Dice input gate', vt, 47, 'SIG_GATE')
find('dice_score handler', 'SIG_SCORE'); find('dice_pass handler', 'SIG_PASS')
hd = find('HoldDie', 'SIG_HOLDDIE'); key = find('dice_hold handler', 'SIG_HOLD_KEY')
if key and (img[key + 0x56] != 0xE8 or key + 0x56 + 5 + i32(key + 0x57) != hd): fail('dice_hold -> HoldDie', 'call target differs')
vtable('C_DiceGameModel', '.?AVC_DiceGameModel@playermodule@wh@@', 0)
slot('score held dice', vtable('C_DiceGameModel delegate', '.?AVC_DiceGameModel@playermodule@wh@@', 0x18), 10, 'SIG_EVAL')
for c in ('C_PlayerTurnDiceGameState', 'C_DieModel', 'C_DiceGamePlayerModel'): vtable(c, f'.?AV{c}@playermodule@wh@@', 0)
for w, n in [('layout: C_Dice model', 'SIG_GETMODEL'), ('layout: current player', 'SIG_CURPM'), ('layout: dice on the table', 'SIG_COLLECT'),
             ('layout: points this turn', 'SIG_TURNPTS'), ('layout: banked total', 'SIG_TOTAL'), ('layout: players', 'SIG_PLAYERS'),
             ('layout: target score', 'SIG_TARGET'), ('layout: die weights', 'SIG_WEIGHTS')]:
    find(w, n)
print('OK' if not fails else f'{fails} FAILURES'); sys.exit(1 if fails else 0)
