"""Package only AutoForge, without deleting existing dist archives.
Usage: python tools/package_forge_rc.py [--tested-dll PATH --sha256 HASH]
Without an explicit preserved binary, packages the freshly built out DLL.
"""
import argparse, hashlib, pathlib, shutil, tempfile, zipfile, sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import package as pkg
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--tested-dll',type=pathlib.Path)
p.add_argument('--sha256')
a=p.parse_args()
if bool(a.tested_dll)!=bool(a.sha256): p.error('--tested-dll and --sha256 must be supplied together')
pkg.MODS=[m for m in pkg.MODS if m['folder']=='theatrical_autoforge']
dist=pathlib.Path(pkg.R)/'dist'/'autoforge-rc'; dist.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
    if a.tested_dll:
        digest=hashlib.sha256(a.tested_dll.read_bytes()).hexdigest()
        if digest!=a.sha256.lower(): raise SystemExit('Preserved DLL checksum mismatch')
        binary=pathlib.Path(tmp)/'binary'; binary.mkdir(); shutil.copy2(a.tested_dll,binary/'kcd2_autoforge.dll')
        for m in pkg.MODS: m['out']=str(binary)
    for m in pkg.MODS:
        target=dist/pkg.zipname(m)
        if target.exists(): raise SystemExit('Refusing to overwrite existing candidate: '+str(target))
        root=pathlib.Path(tmp)/m['zip']; pkg.stage(m,str(root))
        pkg.zipdir(str(root),m['folder'],str(target))
        with zipfile.ZipFile(target) as z:
            data=z.read(m['folder']+'/KCSE/Plugins/kcd2_autoforge.dll')
            expected=(pathlib.Path(m.get('out',pathlib.Path(pkg.R)/'out'))/'kcd2_autoforge.dll').read_bytes()
            assert data==expected,'Packaged DLL differs from source artifact'
    errors,_=pkg.audit(str(dist))
    if errors: raise SystemExit('\n'.join(errors))
(dist/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256((dist/pkg.zipname(m)).read_bytes()).hexdigest()+'  '+pkg.zipname(m)+'\n' for m in pkg.MODS))
(dist/'RELEASE_CONTENTS.txt').write_text(pkg.summary(str(dist)),encoding='utf-8')
print('PASS: AutoForge candidate archives, versions, exact DLL, localization, CRC and content audit:',dist)
