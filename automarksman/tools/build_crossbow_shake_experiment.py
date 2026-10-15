#!/usr/bin/env python3
"""LOCAL EXPERIMENT ONLY: create a player-specific KCD2 WeaponShake override.
Preserves the exact ADB currently supplied by No Herb Picking Animation, and
inserts only player-specific crossbow WeaponShake fragments. Does not install.
Do not publish generated PAK: it includes the game's original animation database.
"""
from pathlib import Path
import zipfile, xml.etree.ElementTree as ET, uuid, hashlib, sys
GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2")
ROOT = Path(__file__).resolve().parents[1]
MANIFEST = '''<?xml version="1.0" encoding="utf-8"?>
<kcd_mod>
 <info><name>AutoMarksman Crossbow Shake EXPERIMENT</name>
 <modid>theatrical_crossbow_shake_test</modid>
 <description>LOCAL ONLY: player crossbow WeaponShake additive disabled; no effect on aiming pose or firing animations. Not F4 toggleable.</description>
 <author>Flubbermunchkin</author><version>0.0.1-local-only</version><created_on>2026-10-15</created_on></info>
</kcd_mod>
'''
SOURCE = GAME/'Mods'/'no_herb_picking_animation'/'Data'/'FPHerbPick.pak'
ADB='Animations/Mannequin/ADB/kcd_male_database.adb'
DEST=ROOT/'out'/'crossbow_shake_experiment'/'theatrical_crossbow_shake_test'
def build():
    assert SOURCE.is_file(),"Expected installed No Herb Picking Animation override missing, do NOT create an unmerged override"
    with zipfile.ZipFile(SOURCE) as z:
        native=z.read(ADB)
    xml=native.decode('utf-8-sig')
    tree=ET.fromstring(xml)
    shake=tree.find('.//FragmentList/WeaponShake')
    assert shake is not None
    existing={f.get('Tags') : f for f in shake.findall('Fragment')}
    assert len(existing)==8, f'Unexpected WeaponShake fragment count: {len(existing)}'
    assert any(a.get('name')=='crossbow_idle_pulled_shaking_add' for a in existing['l_crossbow+charge+prime'].findall('.//Animation'))
    assert any(a.get('name')=='crossbow_stealth_idle_pulled_shaking_add' for a in existing['stealth+l_crossbow+charge+prime'].findall('.//Animation'))
    tags=('l_crossbow+player+charge+prime','stealth+l_crossbow+player+charge+prime')
    assert all(t not in existing for t in tags)
    assert sum(1 for a in tree.findall('.//Animation') if a.get('name')=='Blank')>0, "Blank animation not used by baseline database"
    addition=''
    for tag in tags:
        guid=str(uuid.uuid5(uuid.NAMESPACE_URL,'theatrical_automarksman::WeaponShake::'+tag))
        assert guid not in xml
        # Use the game's existing "Blank" neutral animation, on the WeaponShake
        # layer ONLY, selected more specifically for player contexts.
        addition+=('\r\n\t\t\t<Fragment BlendOutDuration="0.2" GUID="'+guid+'" Tags="'+tag+'">'
                   '\r\n\t\t\t\t<AnimLayer>'
                   '\r\n\t\t\t\t\t<Blend ExitTime="0" StartTime="0" Duration="0.2" />'
                   '\r\n\t\t\t\t\t<Animation name="Blank" flags="Loop" />'
                   '\r\n\t\t\t\t</AnimLayer>'
                   '\r\n\t\t\t</Fragment>')
    assert xml.count('</WeaponShake>')==1
    merged=xml.replace('</WeaponShake>',addition+'\r\n\t\t</WeaponShake>',1)
    mt=ET.fromstring(merged)
    parent=mt.find('.//FragmentList/WeaponShake')
    assert len(parent.findall('Fragment'))==10
    assert all(any(a.get('name')=='Blank' for a in next(f for f in parent.findall('Fragment') if f.get('Tags')==tag).findall('.//Animation')) for tag in tags)
    assert merged.replace(addition+'\r\n\t\t</WeaponShake>','</WeaponShake>',1)==xml, "Unrelated source bytes changed"
    pak=DEST/'Data'/'CrossbowShakeExperiment.pak'
    pak.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(pak,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        z.writestr(ADB,merged.encode('utf-8'))
    (DEST/'mod.manifest').write_text(MANIFEST,encoding='utf-8')
    (DEST/'READ_ME_FIRST.txt').write_text(
        "LOCAL EXPERIMENT ONLY; NOT INSTALLED AUTOMATICALLY.\n"
        "Overrides WeaponShake only for standing and stealth player crossbows.\n"
        "Uses base ADB from installed No Herb Picking Animation to preserve its changes.\n"
        "Crossbow physical shake reduction NOT tested or established yet.\n"
        "Uses Blank instead of player additive-shake fragments; leaves AimPose, aiming and fire events intact.\n"
        "Standalone experiment is NOT F4-toggleable. If effective, integrate dynamically or develop selective native scope controls.\n"
        "Do not publish or distribute this PAK; full merged game database is proprietary.\n"
        "Remove the entire theatrical_crossbow_shake_test folder to revert.\n", encoding='utf-8')
    with zipfile.ZipFile(pak) as z:
        assert z.testzip() is None
    print("STAGED_ONLY",pak)
    print("BASE_MOD",SOURCE,"SHA256",hashlib.sha256(native).hexdigest())
    print("BYTES",len(native),"->",len(merged),"SHA256",hashlib.sha256(merged.encode('utf-8')).hexdigest())
    print("PLAYER_ONLY_TAGS",tags)
    print("NO_GAME_FILES_MODIFIED")
if __name__=='__main__':build()
