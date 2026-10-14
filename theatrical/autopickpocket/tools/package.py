# package.py -- build the mod archive (dist\TheatricalAutoPickpocket-<ver>.zip) from out\ (run build.bat first).
# The archive holds one mod folder at its root, to be placed in <game>\Mods (Vortex does this itself).
import os, shutil, zipfile, datetime

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
LICENSE = os.path.join(R, '..', '..', 'LICENSE')     # the repository's GPL v3
AUTHOR = 'Flubbermunchkin'
MOD = dict(folder='theatrical_autopickpocket', title='Theatrical AutoPickpocket', zip='TheatricalAutoPickpocket', dll='kcd2_autopickpocket.dll',
           ver='0.1.0', readme='theatrical_autopickpocket',
           desc="Henry pickpockets with the game's own inputs: lets go of the charge at a safe moment and plays the item grid, keeping every risk. F5 toggles.",
           text=[('tap_on', 'AutoPickpocket: on'), ('tap_off', 'AutoPickpocket: off (you pickpocket yourself)')],
           ini='[Keys]\n'
               '; key: F1-F12, a letter or digit, or a symbol key such as ]\nToggle=F5\n\n'
               '[Options]\n'
               '; 1 = on when the game starts (the key switches it at any time)\nEnabled=0\n'
               '; 1 = let go of the pickpocket key for you when the safety ring falls to ReleaseSafety\nCharge=1\n'
               '; 1 = play the item grid for you\nGrid=1\n'
               '; safety ring value (0.05 = almost red .. 1 = full green) at which the charge is released; higher is safer, with less time\nReleaseSafety=0.35\n'
               '; 0 = no limit; otherwise also release after this many seconds of charge\nMaxChargeSeconds=0\n'
               '; budget (ms) always kept beyond the way back to the exit; 800-6000\nSafetyMs=1500\n'
               '; do not take items worth less than this (groschen)\nMinValue=0\n'
               '; 1 = look under covered items when there is time\nUncover=1\n'
               '; 1 = show a short message when the key switches it\nNotices=1\n'
               '; 1 = write a detailed log (for bug reports)\nDebugLog=0\n')

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
    localization(m, d)

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
