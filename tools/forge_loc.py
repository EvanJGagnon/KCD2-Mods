"""forge_loc.py -- write AutoForge's HUD texts as game localization packs.

The game resolves every Game.SendInfoText string as a localization key; plain text shows up as "@word @word".
So the HUD notices are keys ("@taf_started"), and this script writes their English text, taken from the note list in
src/forge_queue2.h (FQ_NOTE_LIST, FQ_PIECE_*), as Localization/<Language>_xml.pak for every language the game has -
the same layout Horse Route Follow uses.

  python tools/forge_loc.py <output Localization dir>     write the 16 packs
  python tools/forge_loc.py --check <Localization dir>    verify the packs against the header (exit code 1 on a mismatch)
"""
import os, re, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, '..', 'src', 'forge_queue2.h')
LANGS = ['Chineses', 'Chineset', 'Czech', 'English', 'French', 'German', 'Italian', 'Japanese', 'Korean',
         'Polish', 'Portuguese', 'Russian', 'Spanish', 'Turkish', 'Ukrainian', 'Vietnamese']
ENTRY = 'text_ui_theatrical_autoforge.xml'


def rows():
    src = open(HEADER, encoding='utf-8').read()
    out = re.findall(r'X\(N_\w+,\s*"(taf_\w+)",\s*"([^"]+)"\)', src)
    pmax = int(re.search(r'#define\s+FQ_PIECE_MAX\s+(\d+)', src).group(1))
    ptext = re.search(r'#define\s+FQ_PIECE_TEXT\s+"([^"]+)"', src).group(1)
    pnext = re.search(r'#define\s+FQ_PIECE_NEXT_TEXT\s+"([^"]+)"', src).group(1)
    for of in range(1, pmax + 1):
        for n in range(1, of + 1):
            out.append((f'taf_item_{n}_{of}', ptext % (n, of)))
    out.append(('taf_item_next', pnext))
    keys = [k for k, _ in out]
    assert len(keys) == len(set(keys)), 'duplicate key'
    for k, t in out:
        assert '@' not in t and not re.search(r'[<>&"]', t) and len(t) <= 60, f'{k}: text must be short and plain: {t!r}'
    return out


def xml(rs):
    return '<?xml version="1.0" encoding="utf-8"?>\n<Table>\n' + ''.join(
        f'\t<Row><Cell>{k}</Cell><Cell>{v}</Cell><Cell>{v}</Cell></Row>\n' for k, v in rs) + '</Table>\n'


def write(outdir):
    os.makedirs(outdir, exist_ok=True)
    rs = rows(); x = xml(rs)
    for lang in LANGS:
        with zipfile.ZipFile(os.path.join(outdir, f'{lang}_xml.pak'), 'w', zipfile.ZIP_DEFLATED) as z:
            z.writestr(ENTRY, x)
    print(f'wrote {len(LANGS)} packs, {len(rs)} keys, to {outdir}')


def check(d):
    rs = rows(); bad = 0
    for lang in LANGS:
        p = os.path.join(d, f'{lang}_xml.pak')
        if not os.path.exists(p):
            print('missing', p); bad += 1; continue
        with zipfile.ZipFile(p) as z:
            if z.namelist() != [ENTRY]: print('unexpected content', p, z.namelist()); bad += 1; continue
            got = re.findall(r'<Cell>([^<]*)</Cell><Cell>([^<]*)</Cell><Cell>([^<]*)</Cell>', z.read(ENTRY).decode('utf-8'))
        if [(k, v) for k, v, _ in got] != rs: print('keys/text differ in', p); bad += 1
    print('OK' if not bad else f'{bad} problem(s)')
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--check': sys.exit(check(sys.argv[2]))
    if len(sys.argv) == 2: write(sys.argv[1]); sys.exit(0)
    print(__doc__); sys.exit(2)
