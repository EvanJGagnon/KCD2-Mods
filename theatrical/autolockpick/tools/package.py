# package.py -- build the mod archive (dist\TheatricalAutoLockpick-<ver>.zip) from out\ (run build.bat first).
# The archive holds one mod folder at its root, to be placed in <game>\Mods (Vortex does this itself).
import os, shutil, zipfile, datetime

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
LICENSE = os.path.join(R, '..', '..', 'LICENSE')     # the repository's GPL v3
AUTHOR = 'Flubbermunchkin'
MOD = dict(folder='theatrical_autolockpick', title='Theatrical AutoLockpick', zip='TheatricalAutoLockpick', dll='kcd2_autolockpick.dll',
           ver='0.1.0', readme='theatrical_autolockpick',
           desc="Henry picks the lock himself with the game's own pick and turn controls, tremble, wear and broken picks included (F2).",
           ini='[Keys]\n'
               '; key: F1-F12, a letter or digit, or a symbol key such as ]\nPick=F2\n\n'
               '[Options]\n'
               '; 1 = start by itself whenever you begin picking a lock (never right after a pick breaks)\nAutoStart=0\n'
               '; let go and try again at most this many times per lock before handing it back to you\nMaxTries=6\n'
               '; 1 = write a detailed log (for bug reports)\nDebugLog=1\n')

def manifest(m):
    return f'''<?xml version="1.0" encoding="utf-8"?>
<kcd_mod>
  <info>
    <name>{m['title']}</name>
    <modid>{m['folder']}</modid>
    <description>{m['desc']}</description>
    <author>{AUTHOR}</author>
    <version>{m['ver']}</version>
    <created_on>{datetime.date.today().isoformat()}</created_on>
  </info>
</kcd_mod>
'''

def write(path, text):
    with open(path, 'w', newline='\r\n', encoding='utf-8') as f: f.write(text)

def stage(m, root):
    d = os.path.join(root, m['folder']); p = os.path.join(d, 'KCSE', 'Plugins')
    os.makedirs(p)
    write(os.path.join(d, 'mod.manifest'), manifest(m))
    shutil.copy(os.path.join(R, 'out', m['dll']), p)
    write(os.path.join(p, m['dll'].replace('.dll', '.ini')), m['ini'])
    shutil.copy(os.path.join(R, 'docs', m['readme'] + '.txt'), os.path.join(d, 'README.txt'))
    shutil.copy(LICENSE, os.path.join(d, 'LICENSE.txt'))

def zipdir(root, name, zpath):
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for dp, _, fs in os.walk(os.path.join(root, name)):
            for f in sorted(fs):
                full = os.path.join(dp, f); z.write(full, os.path.relpath(full, root))

if __name__ == '__main__':
    dist = os.path.join(R, 'dist'); shutil.rmtree(dist, ignore_errors=True)
    stg = os.path.join(dist, 'stage')
    stage(MOD, stg)
    z = os.path.join(dist, f"{MOD['zip']}-{MOD['ver']}.zip")
    zipdir(stg, MOD['folder'], z)
    print(os.path.basename(z), os.path.getsize(z))
