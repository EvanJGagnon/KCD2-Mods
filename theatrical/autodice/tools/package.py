# package.py -- build the mod archive (dist\TheatricalAutoDice-<ver>.zip) from out\ (run build.bat first).
# The archive holds one mod folder at its root, to be placed in <game>\Mods (Vortex does this itself).
import os, shutil, zipfile, datetime

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
LICENSE = os.path.join(R, '..', '..', 'LICENSE')     # the repository's GPL v3
AUTHOR = 'Flubbermunchkin'
MOD = dict(folder='theatrical_autodice', title='Theatrical AutoDice', zip='TheatricalAutoDice', dll='kcd2_autodice.dll',
           ver='0.1.1', readme='theatrical_autodice',
           desc="Henry plays his own turns at dice with the game's own actions: he picks the dice worth keeping and decides when to pass. F3 starts and stops.",
           text=[],
           ini='[Keys]\n'
               '; start/stop key: F1-F12, a letter or digit, or a symbol key such as ]\nPlay=F3\n\n'
               '[Options]\n'
               '; how fast Henry plays, in percent (50 = slower and more deliberate, 200 = brisk)\nSpeedPercent=100\n'
               '; how boldly he plays, in percent (above 100 he presses on more, below 100 he banks sooner)\nRiskPercent=100\n'
               '; 1 = write a detailed log (for bug reports)\nDebugLog=1\n')

LANGS = ['Chineses', 'Chineset', 'Czech', 'English', 'French', 'German', 'Italian', 'Japanese', 'Korean',
         'Polish', 'Portuguese', 'Russian', 'Spanish', 'Turkish', 'Ukrainian', 'Vietnamese']

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

def localization(m, d):
    """Mod text entries (English in every language), packed like the game's own <Language>_xml.pak."""
    xml = '<?xml version="1.0" encoding="utf-8"?>\n<Table>\n' + ''.join(
        f'\t<Row><Cell>{k}</Cell><Cell>{v}</Cell><Cell>{v}</Cell></Row>\n' for k, v in m['text']) + '</Table>\n'
    ld = os.path.join(d, 'Localization'); os.makedirs(ld)
    for lang in LANGS:
        with zipfile.ZipFile(os.path.join(ld, f'{lang}_xml.pak'), 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(f"text_ui_{m['folder']}.xml", xml)

def stage(m, root):
    d = os.path.join(root, m['folder']); p = os.path.join(d, 'KCSE', 'Plugins')
    os.makedirs(p)
    write(os.path.join(d, 'mod.manifest'), manifest(m))
    shutil.copy(os.path.join(R, 'out', m['dll']), p)
    write(os.path.join(p, m['dll'].replace('.dll', '.ini')), m['ini'])
    shutil.copy(os.path.join(R, 'docs', m['readme'] + '.txt'), os.path.join(d, 'README.txt'))
    shutil.copy(LICENSE, os.path.join(d, 'LICENSE.txt'))
    if m['text']: localization(m, d)

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
