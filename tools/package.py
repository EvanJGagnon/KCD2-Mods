# package.py -- build every release archive (dist\*.zip) from the built DLLs (run build.bat first), then audit them.
# Each archive holds one mod folder at its root, to be placed in <game>\Mods (Vortex does this itself).
#   python tools\package.py            all archives + dist\SHA256SUMS.txt + dist\RELEASE_CONTENTS.txt
#   python tools\package.py --audit    only re-audit the archives already in dist\
# The audit fails the run (exit code 1) on a missing file, a version mismatch, a hotkey used twice, or anything
# that looks like a game asset or a build leftover (.pdb, .exe, .adb, .gfx, game tables, ...).
import os, re, sys, shutil, zipfile, datetime, hashlib

R = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
AUTHOR = 'Flubbermunchkin'
DATA = os.path.join(R, 'data')
KEYHELP = '; key: F1-F12, a letter or digit, or a symbol key such as ]\n'
DEBUG = '; 1 = write a detailed log (for bug reports)\nDebugLog=0\n'
TH = os.path.join(R, 'theatrical')
LANGS = ['Chineses', 'Chineset', 'Czech', 'English', 'French', 'German', 'Italian', 'Japanese', 'Korean',
         'Polish', 'Portuguese', 'Russian', 'Spanish', 'Turkish', 'Ukrainian', 'Vietnamese']

def th(name):   # out\ and docs\ of a Theatrical Collection mod under theatrical\<name>\
    return dict(out=os.path.join(TH, name, 'out'), docs=os.path.join(TH, name, 'docs'))

def automarksman_ini():
    with open(os.path.join(TH, 'automarksman', 'kcd2_automarksman.ini'), encoding='utf-8') as f:
        return f.read().replace('\r\n', '\n')

def automarksman_text():
    with open(os.path.join(TH, 'automarksman', 'localization', 'text_ui_theatrical_automarksman.xml'), encoding='utf-8') as f: x = f.read()
    rows = re.findall(r'<Row><Cell>([^<]+)</Cell><Cell>([^<]+)</Cell>', x)
    assert {k for k, _ in rows} >= {'tam_on', 'tam_off', 'tam_bow_measured', 'tam_first_arrow'}, rows
    return rows

AM_DESC = ("Experimental aim assist: while a bow, crossbow or hand cannon is aimed, the camera steadily turns onto the head of the target nearest the centre "
           "of the view, allowing for drop and movement (F4). You still draw and release.")

# key = (.ini section, setting) holding the mod's hotkey; checked for conflicts across all archives
MODS = [
    # ---- published on Nexus (src\) ----
    dict(folder='theatrical_autoforge', title='Theatrical AutoForge', zip='TheatricalAutoForge', dll='kcd2_autoforge.dll', data=[],
         desc='Henry forges the chosen piece himself, stroke by stroke, through the game\'s own forge actions (F9).', ver='1.1.2', key='Forge',
         ini='[Keys]\n' + KEYHELP + 'Forge=F9\n\n[Options]\n'
             '; turn the piece over once at this % completion, like a smith would (0 = only when a side is worked out)\nFlipAt=50\n' + DEBUG),
    # the same mod with the planned flip off (FlipAt=0), forging as 1.0.0 did; same folder, so it replaces the other
    dict(folder='theatrical_autoforge', title='Theatrical AutoForge', zip='TheatricalAutoForge-NoFlip', dll='kcd2_autoforge.dll', data=[],
         desc='Henry forges the chosen piece himself, stroke by stroke, through the game\'s own forge actions (F9).', ver='1.1.2', key='Forge',
         variant_of='TheatricalAutoForge',
         ini='[Keys]\n' + KEYHELP + 'Forge=F9\n\n[Options]\n'
             '; turn the piece over once at this % completion, like a smith would (0 = only when a side is worked out)\nFlipAt=0\n' + DEBUG),
    dict(folder='theatrical_autobrew', title='Theatrical Autobrew', zip='TheatricalAutobrew', dll='kcd2_alchemy.dll', data=['alch_recipes.txt'],
         desc='Henry brews the open recipe step by step, in real time (F8). An adaptation of Autobrew by JerryYOJ.', ver='1.0.3', key='Brew',
         ini='[Keys]\n' + KEYHELP + 'Brew=F8\n\n[Options]\n' + DEBUG),
    dict(folder='horse_route_follow', title='Horse Route Follow', zip='HorseRouteFollow', dll='kcd2_autotravel.dll',
         data=['trosecko.amg', 'kutnohorsko.amg', 'klaster.amg'],
         desc='Horse auto-follow takes the road route to your custom map marker, drawn on the map.', ver='1.1.3',
         text=[('hrf_wrong_way', 'Wrong way - turn around')],
         ini='[Options]\n'
             '; 1 = briefly hold S to stop the horse on arrival\nBrakeOnArrival=1\n'
             '; stop this many metres before the point of road closest to the marker (it also brakes a little earlier at speed);\n'
             '; raise it if the horse overshoots, lower it if it stops short\nArriveDistance=3\n'
             '; routes always start the way you are travelling; a later turn-around (dead end) counts as this many extra metres\nUTurnPenalty=400\n'
             '; 1 = draw the planned route on the map\nMapRoute=1\n' + DEBUG),
    dict(folder='theatrical_autosharpening', title='Theatrical AutoSharpening', zip='TheatricalAutoSharpening', dll='kcd2_autosharpen.dll', data=[],
         desc="Henry sharpens the weapon himself at the grindstone, through the game's own controls (F7).", ver='1.0.1', key='Sharpen',
         ini='[Keys]\n' + KEYHELP + 'Sharpen=F7\n\n[Options]\n; 1 = get up from the grindstone when the edge is sharp\nAutoExit=1\n' + DEBUG),
    dict(folder='horse_route_follow_hardcore', title='Horse Route Follow - Hardcore Map Markers', zip='HorseRouteFollow-HardcoreMapMarkers',
         dll='kcd2_hardcore_markers.dll', data=[], ver='1.1.0', ini=None, readme='hardcore_map_markers',
         desc='Optional: allows custom map markers in hardcore mode (map only, not the compass).'),
    # ---- Theatrical Collection (theatrical\<mod>\), new ----
    dict(th('autolockpick'), folder='theatrical_autolockpick', title='Theatrical AutoLockpick', zip='TheatricalAutoLockpick', dll='kcd2_autolockpick.dll',
         data=[], ver='1.0.0', key='Pick',
         desc="Henry picks the lock himself with the game's own pick and turn controls, tremble, wear and broken picks included (F2).",
         ini='[Keys]\n' + KEYHELP + 'Pick=F2\n\n'
             '[Options]\n'
             '; 1 = start by itself whenever you begin picking a lock (never right after a pick breaks)\nAutoStart=0\n'
             '; let go and try again at most this many times per lock before handing it back to you\nMaxTries=6\n' + DEBUG),
    dict(th('autodice'), folder='theatrical_autodice', title='Theatrical AutoDice', zip='TheatricalAutoDice', dll='kcd2_autodice.dll',
         data=[], ver='1.0.0', key='Play',
         desc="Henry plays his own turns at dice with the game's own actions: he picks the dice worth keeping and decides when to pass. F3 starts and stops.",
         ini='[Keys]\n'
             '; start/stop key: F1-F12, a letter or digit, or a symbol key such as ]\nPlay=F3\n\n'
             '[Options]\n'
             '; how fast Henry plays, in percent (50 = slower and more deliberate, 200 = brisk)\nSpeedPercent=100\n'
             '; how boldly he plays, in percent (above 100 he presses on more, below 100 he banks sooner)\nRiskPercent=100\n' + DEBUG),
    dict(th('autopickpocket'), folder='theatrical_autopickpocket', title='Theatrical AutoPickpocket', zip='TheatricalAutoPickpocket',
         dll='kcd2_autopickpocket.dll', data=[], ver='1.0.0', key='Toggle',
         desc="Henry pickpockets with the game's own inputs: lets go of the charge at a safe moment and plays the item grid, keeping every risk. [ toggles.",
         ini='[Keys]\n' + KEYHELP + 'Toggle=[\n\n'
             '[Options]\n'
             '; 1 = on when the game starts (the key switches it at any time)\nEnabled=0\n'
             '; 1 = let go of the pickpocket key for you when the safety ring falls to ReleaseSafety\nCharge=1\n'
             '; 1 = play the item grid for you\nGrid=1\n'
             '; safety ring value (0.05 = almost red .. 1 = full green) at which the charge is released; higher is safer, with less time\nReleaseSafety=0.35\n'
             '; 0 = no limit; otherwise also release after this many seconds of charge\nMaxChargeSeconds=0\n'
             '; budget (ms) always kept beyond the way back to the exit; 800-6000\nSafetyMs=1500\n'
             '; do not take items worth less than this (groschen)\nMinValue=0\n'
             '; 1 = look under covered items when there is time\nUncover=1\n'
             '; 1 = show a short message when the key switches it\nNotices=1\n' + DEBUG),
    dict(th('autofollow'), folder='theatrical_autofollow', title='Theatrical AutoFollow', zip='TheatricalAutoFollow', dll='kcd2_autofollow.dll',
         data=[], ver='1.0.0', key='Toggle',
         desc="Henry can follow anyone with the game's own follow action, as if they were a guide. ] toggles.",
         ini='[Keys]\n' + KEYHELP + 'Toggle=]\n\n'
             '[Options]\n'
             '; 1 = follow-anyone is on when the game starts (the key switches it at any time)\nEnabled=1\n'
             '; 1 = show a short message when the key switches it\nNotices=1\n'
             "; 1 = also set the game's own (cheat-locked, otherwise unused) wh_player_FollowAlwaysCanFollow switch\nSetCvar=1\n"
             "; 1 = turn on the game's own follow debug drawing too (for testing; may show nothing in the release game)\nDebugDraw=0\n" + DEBUG),
    dict(th('automarksman'), folder='theatrical_automarksman', title='Theatrical AutoMarksman', zip='TheatricalAutoMarksman',
         dll='kcd2_automarksman.dll', data=[], ver='1.0.0', key='Toggle', desc=AM_DESC,
         text=automarksman_text(), ini=automarksman_ini()),

]

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
    shutil.copy(os.path.join(m.get('out', os.path.join(R, 'out')), m['dll']), p)
    for f in m['data']: shutil.copy(os.path.join(DATA, f), p)
    if m['ini']: write(os.path.join(p, m['dll'].replace('.dll', '.ini')), m['ini'])
    with open(os.path.join(m.get('docs', os.path.join(R, 'docs')), m.get('readme', m['folder']) + '.txt'), encoding='utf-8') as f:
        readme = f.read().replace('\r\n', '\n')
    write(os.path.join(d, 'README.txt'), readme)
    shutil.copy(os.path.join(R, 'LICENSE'), os.path.join(d, 'LICENSE.txt'))
    localization(m, d)

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

def zipname(m): return f"{m['zip']}-{m['ver']}.zip"

# ---------------- audit ----------------
ALLOWED_EXT = {'.dll', '.ini', '.txt', '.manifest', '.amg'}
def audit(dist):
    errs = []; keys = {}
    for m in MODS:
        z = os.path.join(dist, zipname(m))
        if not os.path.isfile(z): errs.append(f'{zipname(m)}: missing'); continue
        with zipfile.ZipFile(z) as zf:
            names = zf.namelist(); bad = zf.testzip()
            if bad: errs.append(f'{zipname(m)}: CRC error in {bad}')
            f = m['folder'] + '/'
            need = [f + 'mod.manifest', f + 'README.txt', f + 'LICENSE.txt', f + 'KCSE/Plugins/' + m['dll']]
            if m['ini']: need.append(f + 'KCSE/Plugins/' + m['dll'].replace('.dll', '.ini'))
            need += [f + 'KCSE/Plugins/' + d for d in m['data']]
            for n in need:
                if n not in names: errs.append(f'{zipname(m)}: missing {n}')
            for n in names:
                if not n.startswith(f): errs.append(f'{zipname(m)}: {n} is outside the mod folder')
                ext = os.path.splitext(n)[1].lower()
                if n.startswith(f + 'Localization/'):
                    if not (m.get('text') and re.fullmatch(re.escape(f) + r'Localization/[A-Za-z]+_xml\.pak', n)):
                        errs.append(f'{zipname(m)}: unexpected {n}'); continue
                    with zf.open(n) as pf, zipfile.ZipFile(pf) as inner:   # only our own text table inside
                        if inner.namelist() != [f"text_ui_{m['folder']}.xml"]: errs.append(f'{zipname(m)}: {n} holds {inner.namelist()}')
                elif ext not in ALLOWED_EXT: errs.append(f'{zipname(m)}: {n} is not a release file type (game asset or build leftover?)')
                if re.search(r'whgame|\.pdb$|\.exe$|\.adb$|\.gfx$|\.swf$|tables/|libs/', n, re.I): errs.append(f'{zipname(m)}: forbidden {n}')
                if n.endswith('.dll') and not n.endswith('KCSE/Plugins/' + m['dll']): errs.append(f'{zipname(m)}: extra DLL {n}')
                if n.endswith('.log') or 'calibration' in n: errs.append(f'{zipname(m)}: personal file {n}')
            man = zf.read(f + 'mod.manifest').decode('utf-8')
            if f"<version>{m['ver']}</version>" not in man or f"<modid>{m['folder']}</modid>" not in man:
                errs.append(f'{zipname(m)}: manifest version/modid mismatch')
            readme = zf.read(f + 'README.txt').decode('utf-8', 'replace')
            if m['ver'] not in readme.splitlines()[0]: errs.append(f"{zipname(m)}: README title does not say {m['ver']}")
            if m['ini'] and m.get('key'):
                ini = zf.read(f + 'KCSE/Plugins/' + m['dll'].replace('.dll', '.ini')).decode('utf-8')
                k = re.search(r'^' + m['key'] + r'=(\S+)', ini, re.M)
                if not k: errs.append(f"{zipname(m)}: no {m['key']}= hotkey in the ini")
                elif not m.get('variant_of'): keys.setdefault(k.group(1).upper(), []).append(m['zip'])
                dl = re.search(r'^DebugLog=(\d)', ini, re.M)
                if not dl or dl.group(1) != '0': errs.append(f'{zipname(m)}: stable ini must say DebugLog=0')
                if m['folder'] == 'theatrical_automarksman':
                    for opt in ('Bows', 'HandCannons', 'Crossbows'):
                        v = re.search(r'^' + opt + r'=(\d)', ini, re.M)
                        if not v or v.group(1) != '1': errs.append(f'{zipname(m)}: {opt}= must be 1')
                    if not re.search(r'^CrossbowPoseDiagnostics=0', ini, re.M): errs.append(f'{zipname(m)}: wrong CrossbowPoseDiagnostics= setting')
    for k, who in keys.items():
        if len(who) > 1: errs.append(f'hotkey {k} is used by {", ".join(who)}')
    return errs, keys

def summary(dist):
    lines = [f'KCD2 mods - release archives ({datetime.date.today().isoformat()})', '']
    for m in MODS:
        z = os.path.join(dist, zipname(m))
        lines.append(f"{zipname(m):58} {m['title']} {m['ver']}"
                     f"{'  (variant of ' + m['variant_of'] + ')' if m.get('variant_of') else ''}")
        with zipfile.ZipFile(z) as zf:
            for i in zf.infolist(): lines.append(f'    {i.file_size:>9}  {i.filename}')
    return '\n'.join(lines) + '\n'

if __name__ == '__main__':
    dist = os.path.join(R, 'dist')
    if '--audit' not in sys.argv:
        shutil.rmtree(dist, ignore_errors=True)
        stg = os.path.join(dist, 'stage')
        for m in MODS:
            root = os.path.join(stg, m['zip'])           # one stage per archive (two archives may share a mod folder)
            stage(m, root)
            zipdir(root, m['folder'], os.path.join(dist, zipname(m)))
        shutil.rmtree(stg)
    errs, keys = audit(dist)
    with open(os.path.join(dist, 'SHA256SUMS.txt'), 'w', newline='\n') as f:
        for m in MODS:
            z = os.path.join(dist, zipname(m))
            if os.path.isfile(z): f.write(f'{hashlib.sha256(open(z, "rb").read()).hexdigest()}  {zipname(m)}\n')
    if not errs:
        with open(os.path.join(dist, 'RELEASE_CONTENTS.txt'), 'w', newline='\n', encoding='utf-8') as f: f.write(summary(dist))
    for m in MODS:
        z = os.path.join(dist, zipname(m))
        if os.path.isfile(z): print(f'{zipname(m):58} {os.path.getsize(z):>8}')
    print('hotkeys: ' + ', '.join(f'{k} {"/".join(v)}' for k, v in sorted(keys.items())))
    if errs:
        print('AUDIT FAILED:'); [print('  ' + e) for e in errs]; sys.exit(1)
    print('audit OK: every archive complete, versions match, no hotkey conflicts, no game assets or build leftovers')
