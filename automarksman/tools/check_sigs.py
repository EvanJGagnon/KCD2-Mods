# check_sigs.py -- resolve every signature and vtable slot of src/automarksman.cpp against a WHGame.dll on
# disk, the same way the mod does at startup (kcd2_common.h Resolver), without Windows or a game install.
#   python check_sigs.py [path\to\WHGame.dll]        (default: $KCD2_DIR\Bin\Win64MasterMasterSteamPGO\WHGame.dll)
# Needs pefile (pip install pefile).  Exit code = number of failures.
import os, re, struct, sys
import pefile

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'automarksman.cpp')
DEFAULT = os.path.join(os.environ.get('KCD2_DIR', r'C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2'),
                       'Bin', 'Win64MasterMasterSteamPGO', 'WHGame.dll')

# name -> ('find', reference rva) | ('slot', rtti class, index, reference rva); mirrors resolve().
PLAN = {
    'SIG_SS_UPDATE':      ('slot', '.?AVCScriptSystem@@', 1, 0xA26B94),
    'SIG_SS_EXEC':        ('slot', '.?AVCScriptSystem@@', 6, 0x4D4484),
    'SIG_FRAMEWORK_REF':  ('find', None),
    'SIG_GETCLIENTACTOR': ('slot', '.?AVCCryAction@@', 64, None),
    'SIG_GETACTORSYS':    ('slot', '.?AVCCryAction@@', 25, 0x641150),
    'SIG_GENV_SYSTEM':    ('find', 0x393B5C),
    'SIG_VIEWCAMERA':     ('slot', '.?AVCSystem@@', 135, 0x5D77F0),
    'SIG_GETENTITY':      ('slot', '.?AVCEntitySystem@@', 14, 0x4B4F98),
    'SIG_GETBOUNDS':      ('slot', '.?AVCEntity@@', 34, 0x63E034),
    'SIG_GETACTOR':       ('slot', '.?AVCActorSystem@@', 3, 0x647E40),
    'SIG_MKITER':         ('slot', '.?AVCActorSystem@@', 7, 0x1794CB0),
    'SIG_ITER_NEXT':      ('slot', '.?AVCActorIterator@CActorSystem@@', 2, 0x4CB02C),
    'SIG_ITER_REL':       ('slot', '.?AVCActorIterator@CActorSystem@@', 4, 0x16F5194),
    'SIG_DEADBYHP':       ('slot', '.?AVC_Actor@entitymodule@wh@@', 38, 0x64583C),
    'SIG_GETVIEWROT':     ('slot', '.?AVC_Actor@entitymodule@wh@@', 57, 0x94DB70),
    'SIG_LAUNCH':         ('slot', '.?AVCProjectile@entitymodule@wh@@', 35, 0x2AA85A8),
    'SIG_LOOKTICK':       ('find', 0x4415E0),
    'SIG_AIMSTART':       ('find', 0x2AAA4C0),
    'SIG_LAUNCHSTORE':    ('find', 0x2AA8906),
    'SIG_GETCHAR':        ('slot', '.?AVCEntity@@', 101, 0x6411AC),
    'SIG_SLOTTM':         ('slot', '.?AVCEntity@@', 91, 0xB7258C),
    'SIG_CI_POSE':        ('slot', '.?AVCCharInstance@@', 7, 0x1AA6BC0),
    'SIG_CI_SKEL':        ('slot', '.?AVCCharInstance@@', 11, 0x8A49D0),
    'SIG_JOINTID':        ('slot', '.?AVCDefaultSkeleton@@', 7, 0x78BC4C),
    'SIG_ABSJOINT':       ('slot', '.?AVCSkeletonPose@@', 27, 0x641250),
    'SIG_JOINTCOUNT':     ('slot', '.?AVCDefaultSkeleton@@', 1, 0x1AAF020),
    'SIG_JOINTNAME':      ('slot', '.?AVCDefaultSkeleton@@', 6, 0x5AF5BC),
    'SIG_COMBATMODE':     ('find', 0x117F1E8),
    'SIG_PREVIEW':        ('find', 0x2896326),
}
RTTI_ONLY = ['.?AVC_Player@entitymodule@wh@@', '.?AVC_MissileWeaponPlayerController@entitymodule@wh@@', '.?AVCArrow@entitymodule@wh@@',
             '.?AVC_Human@entitymodule@wh@@', '.?AVC_NPCActor@entitymodule@wh@@', '.?AVC_Horse@entitymodule@wh@@', '.?AVC_Dog@entitymodule@wh@@', '.?AVC_Animal@entitymodule@wh@@',
             # weapon state (0.5.0; optional in the mod)
             '.?AVC_ActorShootingExpansion@entitymodule@wh@@', '.?AVC_ActorActionShootingMain@entitymodule@wh@@',
             '.?AVC_ActorActionShootingCharging@entitymodule@wh@@', '.?AVC_ActorActionShootingInstantCharging@entitymodule@wh@@',
             '.?AVC_ActorActionShootingAiming@entitymodule@wh@@', '.?AVC_ActorActionShootingFiring@entitymodule@wh@@',
             '.?AVC_ActorActionShootingReloading@entitymodule@wh@@', '.?AVC_ActorActionShootingUnloading@entitymodule@wh@@',
             '.?AVC_ActorActionShootingUncharging@entitymodule@wh@@', '.?AVC_ActorActionShootingTransitioningToIdle@entitymodule@wh@@',
             '.?AVC_ActorActionShootingBase@entitymodule@wh@@']

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    pe = pefile.PE(path, fast_load=True); mem = bytes(pe.get_memory_mapped_image()); base = pe.OPTIONAL_HEADER.ImageBase
    t = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]; t0, t1 = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
    text = mem[t0:t1]
    src = open(SRC, encoding='utf-8').read()
    sigs = dict(re.findall(r'static const char\* (SIG_\w+)\s*=\s*"([^"]+)"', src))
    def pat(s): return [None if x.startswith('?') else int(x, 16) for x in s.split()]
    def at(p, a): return all(b is None or mem[a + i] == b for i, b in enumerate(p))
    def scan(p):
        lit = max(((i, j) for i in range(len(p)) for j in range(i + 1, len(p) + 1) if all(b is not None for b in p[i:j])), key=lambda r: r[1] - r[0])
        needle = bytes(p[lit[0]:lit[1]]); out = []; k = text.find(needle)
        while k >= 0:
            s = t0 + k - lit[0]
            if s >= t0 and at(p, s): out.append(s)
            k = text.find(needle, k + 1)
        return out
    def q(a): return struct.unpack_from('<Q', mem, a)[0]
    vtc = {}
    def vtable(name):
        if name in vtc: return vtc[name]
        k = mem.find(name.encode() + b'\0'); vt = None
        if k >= 0:
            td = k - 0x10; p = mem.find(struct.pack('<I', td))
            while p >= 0 and vt is None:
                col = p - 0xC
                if col >= 0:
                    sig, off, _, tdr, _, selfr = struct.unpack_from('<IIIIII', mem, col)
                    if sig == 1 and off == 0 and tdr == td and selfr == col:
                        v = mem.find(struct.pack('<Q', base + col))
                        while v >= 0:
                            if v % 8 == 0: vt = v + 8; break
                            v = mem.find(struct.pack('<Q', base + col), v + 1)
                p = mem.find(struct.pack('<I', td), p + 1)
        vtc[name] = vt; return vt
    fails = 0
    for name, plan in PLAN.items():
        if name not in sigs: print(f'FAIL {name}: not in the source'); fails += 1; continue
        p = pat(sigs[name])
        if plan[0] == 'find':
            m = scan(p); ok = len(m) == 1 and (plan[1] is None or m[0] == plan[1])
            print(f'{"ok  " if ok else "FAIL"} {name}: {len(m)} match(es) {" ".join(hex(x) for x in m[:3])}')
        else:
            vt = vtable(plan[1]); f = (q(vt + 8 * plan[2]) - base) if vt else None
            ok = f is not None and t0 <= f < t1 and at(p, f) and (plan[3] is None or f == plan[3])
            print(f'{"ok  " if ok else "FAIL"} {name}: {plan[1]}[{plan[2]}] = {hex(f) if f is not None else "no vtable"}')
        fails += not ok
    for n in RTTI_ONLY:
        ok = vtable(n) is not None; fails += not ok
        print(f'{"ok  " if ok else "FAIL"} RTTI {n}: {hex(vtable(n)) if ok else "missing"}')
    # CArrow vtbl[35] must be a jmp to CProjectile::Launch (arrows reach Launch only through it)
    va, vp = vtable('.?AVCArrow@entitymodule@wh@@'), vtable('.?AVCProjectile@entitymodule@wh@@')
    a = q(va + 8 * 35) - base if va else None; l = q(vp + 8 * 35) - base if vp else None
    ok = a is not None and l is not None and (a == l or (mem[a] == 0xE9 and a + 5 + struct.unpack_from('<i', mem, a + 1)[0] == l))
    fails += not ok
    print(f'{"ok  " if ok else "FAIL"} CArrow[35] = {hex(a) if a is not None else "no vtable"} -> CProjectile::Launch {hex(l) if l is not None else "?"}')
    # calls taken from the trajectory preview block, each checked against its own signature
    m = scan(pat(sigs['SIG_PREVIEW']))
    for nm, off, sg, ref in (('loadout', 7, 'SIG_LOADOUT', 0x7F3310), ('weapon system', 0x20, None, 0x189F3C0),
                             ('launch pose', 0x3D, 'SIG_LAUNCHPOSE', 0x289B28C), ('arrow speed', 0x5F, 'SIG_BOWSPEED', 0x289B594)):
        tg = None
        if m and mem[m[0] + off] == 0xE8: tg = m[0] + off + 5 + struct.unpack_from('<i', mem, m[0] + off + 1)[0]
        ok = tg == ref and (sg is None or at(pat(sigs[sg]), tg)); fails += not ok
        print(f'{"ok  " if ok else "FAIL"} preview call {nm}: {hex(tg) if tg is not None else "?"}')
    # globals the mod reads through rip-relative operands
    m = scan(pat(sigs['SIG_GENV_SYSTEM']))
    if m: a = m[0] + 13; print(f'     gEnv.pSystem global at {hex(a + 4 + struct.unpack_from("<i", mem, a)[0])} (expected 0x492d8c0)')
    m = scan(pat(sigs['SIG_FRAMEWORK_REF']))
    if m: a = m[0] + 3; print(f'     CCryAction* global at {hex(a + 4 + struct.unpack_from("<i", mem, a)[0])} (expected 0x549d328)')
    print('ALL OK' if not fails else f'{fails} FAILED')
    return fails

if __name__ == '__main__':
    sys.exit(main())
