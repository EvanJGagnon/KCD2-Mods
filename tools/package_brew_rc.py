"""Build only the private AutoBrew acceptance archive; never install or delete dist.
Usage: python tools/package_brew_rc.py [--tested-dll PATH --sha256 HASH]
"""
import argparse,hashlib,pathlib,shutil,sys,tempfile,zipfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parent))
import package as pkg
p=argparse.ArgumentParser(description=__doc__); p.add_argument('--tested-dll',type=pathlib.Path); p.add_argument('--sha256'); a=p.parse_args()
if bool(a.tested_dll)!=bool(a.sha256): p.error('supply --tested-dll and --sha256 together')
pkg.MODS=[m for m in pkg.MODS if m['folder']=='theatrical_autobrew']
d=pathlib.Path(pkg.R)/'dist'/'autobrew-rc'; d.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as td:
    for m in pkg.MODS:
        # Keep the production engine unchanged; avoid known literal SendInfoText
        # tokenization during acceptance. Dynamic details remain in overlay/log.
        m['ini']=m['ini'].replace('Queue=0\n','Queue=1\n').replace('QueueNotices=1\n','QueueNotices=0\n')
        if a.tested_dll:
            assert hashlib.sha256(a.tested_dll.read_bytes()).hexdigest()==a.sha256.lower(),'DLL checksum mismatch'
            b=pathlib.Path(td)/'binary'; b.mkdir(exist_ok=True); shutil.copy2(a.tested_dll,b/'kcd2_alchemy.dll'); m['out']=str(b)
        z=d/pkg.zipname(m)
        if z.exists(): raise SystemExit('Refusing to overwrite '+str(z))
        root=pathlib.Path(td)/'stage'; pkg.stage(m,str(root)); pkg.zipdir(str(root),m['folder'],str(z))
        with zipfile.ZipFile(z) as archive:
            assert archive.read(m['folder']+'/KCSE/Plugins/kcd2_alchemy.dll')==(pathlib.Path(m.get('out',pathlib.Path(pkg.R)/'out'))/'kcd2_alchemy.dll').read_bytes()
    errors,_=pkg.audit(str(d))
    if errors: raise SystemExit('\n'.join(errors))
(d/'SHA256SUMS.txt').write_text(''.join(hashlib.sha256((d/pkg.zipname(m)).read_bytes()).hexdigest()+'  '+pkg.zipname(m)+'\n' for m in pkg.MODS))
(d/'RELEASE_CONTENTS.txt').write_text(pkg.summary(str(d)),encoding='utf-8')
print('PASS: private AutoBrew candidate package, CRC, version, recipe data, exact DLL, license and content audit:',d)
