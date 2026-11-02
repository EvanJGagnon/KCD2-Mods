# check_alchemy_sigs.py -- resolve every signature of src/alchemy.cpp (the 1.0.3 engine and the 2.0 queue)
# against a WHGame.dll on disk, the same way the mod does at startup, and print what was found.  No Windows needed.
# Usage: python tools/check_alchemy_sigs.py <path to WHGame.dll>      (pip install pefile)
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(__file__), '..', 'src', 'alchemy.cpp')
sig = dict(re.findall(r'static const char\* (SIG_\w+)\s*=\s*"([^"]+)"', open(SRC).read()))
pe = pefile.PE(sys.argv[1], fast_load=True); img = pe.get_memory_mapped_image(); B = pe.OPTIONAL_HEADER.ImageBase
secs = [(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize, s) for s in pe.sections]
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]; T0 = t.VirtualAddress; T1 = T0 + t.Misc_VirtualSize; text = img[T0:T1]
data = [(lo, hi) for lo, hi, s in secs if not (s.Characteristics & 0x22000000)]   # not discardable, not executable
fails = []

def rx(p): return re.compile(b''.join(b'.' if x.startswith('?') else re.escape(bytes([int(x, 16)])) for x in p.split()), re.S)
def find(n, what=None):
    m = [T0 + x.start() for x in rx(sig[n]).finditer(text)]
    if len(m) != 1: fails.append(f'{what or n}: {len(m)} matches'); return 0
    return m[0]
def at(rva, n): p = sig[n]; return rx(p).fullmatch(img[rva:rva + len(p.split())]) is not None
def r32(rva): return struct.unpack_from('<i', img, rva)[0]
def rel(rva): return rva + 4 + r32(rva)
def q(rva): return struct.unpack_from('<Q', img, rva)[0]
def branch(rva):
    if img[rva] not in (0xE8, 0xE9): fails.append(f'not a call at {rva:#x}'); return 0
    return rel(rva + 1)
def vtable(mangled, off):
    i = img.find(mangled.encode() + b'\0')
    if i < 0: fails.append(f'RTTI {mangled} not found'); return 0
    td = i - 0x10; pat = struct.pack('<I', td); j = img.find(pat)
    while j >= 0:
        col = j - 12
        if col % 4 == 0 and struct.unpack_from('<III', img, col)[:2] == (1, off) and struct.unpack_from('<I', img, col + 20)[0] == col:
            k = img.find(struct.pack('<Q', B + col))
            while k >= 0:
                if k % 8 == 0 and any(lo <= k < hi for lo, hi in data): return k + 8
                k = img.find(struct.pack('<Q', B + col), k + 1)
        j = img.find(pat, j + 1)
    fails.append(f'vtable {mangled} +{off:#x} not found'); return 0
def slot(what, vt, i, n):
    if not vt: return 0
    f = q(vt + 8 * i) - B
    if not at(f, n): fails.append(f'{what}: slot {i} code does not match {n}'); return 0
    return f
def consensus(what, n, disp, minhits):
    hits = [T0 + x.start() for x in rx(sig[n]).finditer(text)]
    tg = {rel(h + disp) for h in hits}
    if len(hits) < minhits or len(tg) != 1: fails.append(f'{what}: {len(hits)} matches, {len(tg)} targets'); return 0
    return tg.pop()

out = []
def show(name, rva): out.append(f'  {name:34} {rva:#x}' if rva else f'  {name:34} -')

print('1.0.3 engine')
for n in ('SIG_PERFORMVERB', 'SIG_CANPERFORM', 'SIG_GETSTATE', 'SIG_SLOTBUSY', 'SIG_APPLYING', 'SIG_CLEARSLOTS', 'SIG_FINDITEM', 'SIG_FINDCLASS', 'SIG_PARSEGUID'):
    show(n, find(n))
findclass = find('SIG_FINDCLASS')
db = find('SIG_ITEMDB_REF'); show('item database', rel(db + 3) if db else 0)
if db and branch(db + 0xD) != findclass: fails.append('item database reference does not call FindClassByGuid')
fw = find('SIG_FRAMEWORK_REF'); show('game framework global', rel(fw + 3) if fw else 0)
vt = vtable('.?AVC_Alchemy@playermodule@wh@@', 0); show('C_Alchemy vtable', vt)
show('C_Alchemy::Update', slot('Update', vt, 19, 'SIG_UPDATE')); show('C_Alchemy::Reset', slot('Reset', vt, 21, 'SIG_RESET'))
vca = vtable('.?AVCCryAction@@', 0); show('GetIItemSystem', slot('GetIItemSystem', vca, 0xD0 // 8, 'SIG_GETITEMSYS'))
vis = vtable('.?AVCItemSystem@@', 8); show('CItemSystem::GetItem', slot('GetItem', vis, 0xA8 // 8, 'SIG_GETITEM'))
print('\n'.join(out)); out.clear()

print('2.0 queue')
site = find('SIG_RECIPEDB_REF'); iface = rel(site + 0x15) if site else 0
if site and rel(site + 8) != iface: fails.append('recipe database references disagree')
show('recipe database (interface)', iface)
vrdb = vtable('.?AVC_AlchemyRecipeDatabase@playermodule@wh@@', 0x78); show('  its vtable', vrdb)
show('C_AlchemyRecipe vtable', vtable('.?AVC_AlchemyRecipe@playermodule@wh@@', 0))
rpg = find('SIG_RPGMODULE_REF'); show('C_RPGModule global', rel(rpg + 3) if rpg else 0)
vmg = vtable('.?AVC_RPGMinigames@rpgmodule@wh@@', 0); show('IsRecipeLearned', slot('IsRecipeLearned', vmg, 9, 'SIG_ISLEARNED'))
show('gEnv->pSystem', consensus('gEnv->pSystem', 'SIG_GETLOC_CALL', 3, 4))
vsys = vtable('.?AVCSystem@@', 0); show('GetLocalizationManager', slot('GetLocalizationManager', vsys, 173, 'SIG_SYS_GETLOC'))
vloc = vtable('.?AVCLocalizedStringsManager@@', 0); show('LocalizeLabel', slot('LocalizeLabel', vloc, 23, 'SIG_LOCALIZE'))
ui = find('SIG_ITEMUINAME'); show('GetItemUIName (class name key)', ui)
if ui and branch(ui + 0x1C) != findclass: fails.append('GetItemUIName does not use FindClassByGuid')
show('brew result presenter (+0x6A2)', find('SIG_PRESENTER'))
vss = vtable('.?AVCScriptSystem@@', 0)
show('CScriptSystem::Update', slot('ScriptSystem Update', vss, 1, 'SIG_SS_UPDATE')); show('CScriptSystem::ExecuteBuffer', slot('ExecuteBuffer', vss, 6, 'SIG_SS_EXEC'))
print('\n'.join(out))
if fails: print('FAILED:\n  ' + '\n  '.join(fails)); sys.exit(1)
print('OK: every signature matches exactly as the mod expects')
