"""Static audit of the KCD2 result popup's genuine native Esc/close path.
Read-only. Does not load the game, hook functions, or modify binary files.
For WHGame.dll build timestamp 0x6A350E20.
"""
from pathlib import Path
import pefile,struct
GAME=Path(r'C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2\Bin\Win64MasterMasterSteamPGO\WHGame.dll')
pe=pefile.PE(str(GAME),fast_load=True)
im=pe.get_memory_mapped_image()
base=pe.OPTIONAL_HEADER.ImageBase
def check(cond,msg):
    if not cond:raise AssertionError(msg)
    print('OK:',msg)
def rel32(at):
    op=im[at]
    check(op in (0xE8,0xE9),'relative branch opcode at '+hex(at))
    return at+5+struct.unpack_from('<i',im,at+1)[0]
check(pe.FILE_HEADER.TimeDateStamp==0x6A350E20,'expected native game build')
for site in (0x2B8E436,0x2341974):
    check(rel32(site)==0x2B14E80,'game item modal opens through '+hex(site))
check(im[0x2B14F79:0x2B14F81].find(b'\xA8\x00\x00\x00\x01')>=0,
      'modal open sets active flag at +0xA8')
check(im[0x2B11BD7:0x2B11BE0].find(b'\xA8\x00\x00\x00\x00')>=0,
      'native item close clears active flag')
check(rel32(0x1F53EA6)==0x2B11BC4,
      'Esc registered handler calls C_UIModalDialogItem::Close')
check(rel32(0x1F53EBB)==0x39AE30,
      'Esc handler also invokes completion delegate')
check(struct.unpack_from('<Q',im,0x3EDE078)[0]==base+0x1F53E90,
      'native item modal registers the same input close handler')
check(im[0x3EDB5B0:0x3EDB5C1].startswith(b'modal_dialog_item'),
      'modal dialogue is named modal_dialog_item')
print('RESULT: exact native Esc/close sequence verified offline')
