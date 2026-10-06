# package.py -- build the release archives (dist\*.zip) from out\ (run build.bat first).
# Each mod archive holds one mod folder at its root, to be placed in <game>\Mods (Vortex does this itself).
import os, shutil, zipfile, datetime

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
VER = '1.0.0'
AUTHOR = 'Flubbermunchkin'
DATA = os.path.join(R, 'data')
KEYHELP = '; key: F1-F12, a letter, or a virtual-key code\n'
DEBUG = '; 1 = write a detailed log (for bug reports)\nDebugLog=0\n'

MODS = [
    dict(folder='theatrical_autoforge', title='Theatrical AutoForge', zip='TheatricalAutoForge', dll='kcd2_autoforge.dll', data=[],
         desc='Henry forges the chosen piece himself, stroke by stroke, through the game\'s own forge actions (F9).',
         ini='[Keys]\n' + KEYHELP + 'Forge=F9\n\n[Options]\n' + DEBUG),
    dict(folder='theatrical_autobrew', title='Theatrical Autobrew', zip='TheatricalAutobrew', dll='kcd2_alchemy.dll', data=['alch_recipes.txt'],
         desc='Henry brews the open recipe step by step, in real time (F8). An adaptation of Autobrew by JerryYOJ.', ver='1.0.1',
         ini='[Keys]\n' + KEYHELP + 'Brew=F8\n\n[Options]\n' + DEBUG),
    dict(folder='horse_route_follow', title='Horse Route Follow', zip='HorseRouteFollow', dll='kcd2_autotravel.dll',
         data=['trosecko.amg', 'kutnohorsko.amg', 'klaster.amg'],
         desc='Horse auto-follow takes the road route to your custom map marker, drawn on the map.', ver='1.1.2',
         text=[('hrf_wrong_way', 'Wrong way - turn around')],
         ini='[Options]\n'
             '; 1 = briefly hold S to stop the horse on arrival\nBrakeOnArrival=1\n'
             '; stop this many metres before the point of road closest to the marker (it also brakes a little earlier at speed);\n'
             '; raise it if the horse overshoots, lower it if it stops short\nArriveDistance=3\n'
             '; routes always start the way you are travelling; a later turn-around (dead end) counts as this many extra metres\nUTurnPenalty=400\n'
             '; 1 = draw the planned route on the map\nMapRoute=1\n' + DEBUG),
    dict(folder='horse_route_follow_hardcore', title='Horse Route Follow - Hardcore Map Markers', zip='HorseRouteFollow-HardcoreMapMarkers',
         dll='kcd2_hardcore_markers.dll', data=[], ver='1.1.0', ini=None, readme='hardcore_map_markers',
         desc='Optional: allows custom map markers in hardcore mode (map only, not the compass).'),
]

def manifest(m):
    return f'''<?xml version="1.0" encoding="utf-8"?>
<kcd_mod>
  <info>
    <name>{m['title']}</name>
    <modid>{m['folder']}</modid>
    <description>{m['desc']}</description>
    <author>{AUTHOR}</author>
    <version>{m.get('ver', VER)}</version>
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
    for f in m['data']: shutil.copy(os.path.join(DATA, f), p)
    if m['ini']: write(os.path.join(p, m['dll'].replace('.dll', '.ini')), m['ini'])
    shutil.copy(os.path.join(R, 'docs', m.get('readme', m['folder']) + '.txt'), os.path.join(d, 'README.txt'))
    shutil.copy(os.path.join(R, 'LICENSE'), os.path.join(d, 'LICENSE.txt'))
    localization(m, d)

LANGS = ['Chineses', 'Chineset', 'Czech', 'English', 'French', 'German', 'Italian', 'Japanese', 'Korean',
         'Polish', 'Portuguese', 'Russian', 'Spanish', 'Turkish', 'Ukrainian', 'Vietnamese']

def localization(m, d):
    """Mod text entries (English in every language), packed like the game's own <Language>_xml.pak."""
    rows = m.get('text')
    if not rows: return
    xml = '<?xml version="1.0" encoding="utf-8"?>\n<Table>\n' + ''.join(
        f'\t<Row><Cell>{k}</Cell><Cell>{v}</Cell><Cell>{v}</Cell></Row>\n' for k, v in rows) + '</Table>\n'
    ld = os.path.join(d, 'Localization'); os.makedirs(ld)
    for lang in LANGS:
        with zipfile.ZipFile(os.path.join(ld, f'{lang}_xml.pak'), 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(f"text_ui_{m['folder']}.xml", xml)

def zipdir(root, name, zpath):
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for dp, _, fs in os.walk(os.path.join(root, name)):
            for f in sorted(fs):
                full = os.path.join(dp, f); z.write(full, os.path.relpath(full, root))

if __name__ == '__main__':
    dist = os.path.join(R, 'dist'); shutil.rmtree(dist, ignore_errors=True)
    stg = os.path.join(dist, 'stage')
    for m in MODS:
        stage(m, stg)
        zipdir(stg, m['folder'], os.path.join(dist, f"{m['zip']}-{m.get('ver', VER)}.zip"))
    for f in sorted(os.listdir(dist)):
        if f.endswith('.zip'): print(f, os.path.getsize(os.path.join(dist, f)))
