# package.py -- stage the test install folder dist\theatrical_automarksman (and a zip of it) from out\.
# Copy dist\theatrical_automarksman into <game>\Mods\.  Not a release: the mod is in development.
import os, shutil, zipfile, datetime
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
VER = '0.3.0'
FOLDER = 'theatrical_automarksman'

def write(path, text):
    with open(path, 'w', newline='\r\n', encoding='utf-8') as f: f.write(text)

MANIFEST = f'''<?xml version="1.0" encoding="utf-8"?>
<kcd_mod>
  <info>
    <name>Theatrical AutoMarksman</name>
    <modid>{FOLDER}</modid>
    <description>While a bow is drawn, the camera steadily turns onto the head of the target nearest the centre of the view, allowing for arrow drop and movement (F4). You still draw and release.</description>
    <author>Flubbermunchkin</author>
    <version>{VER}</version>
    <created_on>{datetime.date.today().isoformat()}</created_on>
  </info>
</kcd_mod>
'''

if __name__ == '__main__':
    dist = os.path.join(R, 'dist'); shutil.rmtree(dist, ignore_errors=True)
    d = os.path.join(dist, FOLDER); p = os.path.join(d, 'KCSE', 'Plugins'); os.makedirs(p)
    write(os.path.join(d, 'mod.manifest'), MANIFEST)
    shutil.copy(os.path.join(R, 'out', 'kcd2_automarksman.dll'), p)
    shutil.copy(os.path.join(R, 'kcd2_automarksman.ini'), p)
    shutil.copy(os.path.join(R, 'docs', 'theatrical_automarksman.txt'), os.path.join(d, 'README.txt'))
    shutil.copy(os.path.join(R, '..', 'LICENSE'), os.path.join(d, 'LICENSE.txt'))
    z = os.path.join(dist, f'TheatricalAutoMarksman-{VER}-test.zip')
    with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED) as zf:
        for dp, _, fs in os.walk(d):
            for f in sorted(fs): full = os.path.join(dp, f); zf.write(full, os.path.relpath(full, dist))
    print(z, os.path.getsize(z))
