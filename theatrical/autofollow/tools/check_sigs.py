# check_sigs.py -- resolve the signatures of src/autofollow.cpp against a WHGame.dll on disk, the same
# way the mod does at startup, and print what was found.  No Windows needed.
# Usage: python tools/check_sigs.py <path to WHGame.dll>      (pip install pefile)
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(__file__), '..', 'src', 'autofollow.cpp')
sig = dict(re.findall(r'static const char\* (SIG_\w+)\s*=\s*"([^"]+)"', open(SRC).read()))
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]; lo = t.VirtualAddress; text = img[lo:lo + t.Misc_VirtualSize]

def rx(p): return b''.join(b'.' if x.startswith('?') else re.escape(bytes([int(x, 16)])) for x in p.split())
def find(n):
    m = [B + lo + x.start() for x in re.finditer(rx(sig[n]), text, re.S)]
    if len(m) != 1: sys.exit(f'FAIL {n}: {len(m)} matches')
    return m[0]
def at(a, n): return re.fullmatch(rx(sig[n]), img[a - B:a - B + len(sig[n].split())], re.S) is not None
def r32(a): return struct.unpack('<i', img[a - B:a - B + 4])[0]
def rel(a): return a + 4 + r32(a)
def branch(a):
    if img[a - B] not in (0xE8, 0xE9): sys.exit(f'FAIL not a call at {a:#x}')
    return rel(a + 1)
def cstr(a): o = a - B; return img[o:img.index(b'\0', o)].decode()

site = find('SIG_FOLLOWABLE'); patch = site + 0x51
center = site + 0x0E + struct.unpack('<b', img[site + 0x0D - B:site + 0x0E - B])[0]
if not at(center, 'SIG_CENTER'): sys.exit('FAIL followable center result')
if img[patch - B:patch - B + 2] != b'\xc6\x47': sys.exit('FAIL patch site bytes')
print(f'followable chk {site - B:#x}  patch {patch - B:#x}: C6 47 -> EB {center - (patch + 2):02X} (jmp {center - B:#x}: center, followable)')
call = find('SIG_CVARS_CALL'); getter = branch(call); regfn = branch(call + 8)
if not at(getter, 'SIG_GETTER'): sys.exit('FAIL getter')
block = rel(getter + 0x2A)
reg = find('SIG_REGISTER')
if not regfn <= reg <= regfn + 0x4000: sys.exit('FAIL registration not inside the player cvar registration')
name, help_ = cstr(rel(reg + 0x1E)), cstr(rel(reg + 3))
if name != 'wh_player_FollowAlwaysCanFollow': sys.exit('FAIL name ' + name)
var = block + r32(reg + 0x0F)
reg2 = reg + 0x2A
draw = block + r32(reg2 + 0x0F) if at(reg2, 'SIG_REGISTER_NEXT') and cstr(rel(reg2 + 0x1E)) == 'wh_player_FollowDebugDraw' else 0
print(f'call site      {call - B:#x}\ngetter         {getter - B:#x}\nregistration   {regfn - B:#x}\nplayer cvars   {block - B:#x}')
print(f'register site  {reg - B:#x}  "{name}"  default={"0" if img[reg + 0x22 - B:reg + 0x25 - B] == bytes.fromhex("4533c0") else "?"}\n  help: {help_}')
print(f'variable       {var - B:#x}  (block + {r32(reg + 0x0F):#x})')
print(f'debug draw     {draw - B:#x}' if draw else 'debug draw     not found')
for n in ('SIG_SS_UPDATE', 'SIG_SS_EXEC'): print(f'{n:14} {find(n) - B:#x}')
print('OK')
