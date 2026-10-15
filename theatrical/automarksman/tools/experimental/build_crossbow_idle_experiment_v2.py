#!/usr/bin/env python3
"""LOCAL TEST ONLY. Player crossbow's additive shake AND aim-idle loop bypass.
Merged atop user's preinstalled No Herb Picking Animation database.
Aims to isolate the source of residual bouncing. Do not distribute.
"""
from pathlib import Path
import copy, hashlib, uuid, zipfile, xml.etree.ElementTree as ET
GAME=Path(r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2")
ROOT=Path(__file__).resolve().parents[1]
SOURCE=GAME/"Mods/no_herb_picking_animation/Data/FPHerbPick.pak"
ADB="Animations/Mannequin/ADB/kcd_male_database.adb"
DEST=ROOT/"out/crossbow_idle_experiment_v2/theatrical_crossbow_shake_test"
def frag_text(f):
    return ET.tostring(f,encoding="unicode").rstrip()
def extract(xml,group):
    root=ET.fromstring(xml)
    q=root.find(".//FragmentList/"+group)
    assert q is not None
    return q, {f.get("Tags",""):f for f in q.findall("Fragment")}
def generate():
    with zipfile.ZipFile(SOURCE) as z:
        original=z.read(ADB)
    xml=original.decode('utf-8-sig')
    w,wt=extract(xml,"WeaponShake")
    motion,mt=extract(xml,"MotionIdle")
    assert len(wt)==8
    origShake={
        "l_crossbow+charge+prime":"crossbow_idle_pulled_shaking_add",
        "stealth+l_crossbow+charge+prime":"crossbow_stealth_idle_pulled_shaking_add"
    }
    baseIdle={
        "l_crossbow+charge+prime+cocked":("crossbow_idle_aim","crossbow_idle_loaded"),
        "stealth+l_crossbow+charge+prime+cocked":("crossbow_stealth_idle_aim","crossbow_stealth_idle_loaded")
    }
    def assert_clip(fragment,clip):
        assert any(a.get("name")==clip for a in fragment.findall(".//Animation")),clip
    for k,clip in origShake.items():assert_clip(wt[k],clip)
    for k,(clip,_) in baseIdle.items():assert_clip(mt[k],clip)
    assert all("player" not in k for k in origShake)
    assert all(k.replace("+charge","+player+charge") not in wt for k in origShake)
    assert all(k.replace("+charge","+player+charge") not in mt for k in baseIdle)
    insertionShake=[]
    for old in origShake:
        f=copy.deepcopy(wt[old])
        tag=old.replace("+charge","+player+charge")
        f.set("Tags",tag)
        f.set("GUID",str(uuid.uuid5(uuid.NAMESPACE_URL,"autoMarksman::WeaponShake::"+tag)))
        anims=f.findall(".//Animation");assert len(anims)==1
        anims[0].set("name","Blank")
        insertionShake.append(frag_text(f))
    insertionIdle=[]
    for old,(clip,replacement) in baseIdle.items():
        f=copy.deepcopy(mt[old])
        tag=old.replace("+charge","+player+charge")
        f.set("Tags",tag)
        f.set("GUID",str(uuid.uuid5(uuid.NAMESPACE_URL,"autoMarksman::MotionIdle::"+tag)))
        anims=f.findall(".//Animation");assert len(anims)==1
        assert_clip(f,clip)
        anims[0].set("name",replacement)
        # Preserve original vanilla CameraNear / AnimationOverride ProcLayer elements.
        insertionIdle.append(frag_text(f))
    # Only additions: leave No Herb Picking Animation's original XML byte-for-byte identical.
    insertShake="\r\n"+"\r\n".join(insertionShake)+"\r\n"
    insertIdle="\r\n"+"\r\n".join(insertionIdle)+"\r\n"
    assert xml.count("</WeaponShake>")==1 and xml.count("</MotionIdle>")==1
    merged=xml.replace("</WeaponShake>",insertShake+"</WeaponShake>",1)
    merged=merged.replace("</MotionIdle>",insertIdle+"</MotionIdle>",1)
    assert merged.replace(insertIdle+"</MotionIdle>","</MotionIdle>",1).replace(insertShake+"</WeaponShake>","</WeaponShake>",1)==xml
    nW,w2=extract(merged,"WeaponShake");nI,i2=extract(merged,"MotionIdle")
    assert len(nW.findall("Fragment"))==len(w.findall("Fragment"))+2
    assert len(nI.findall("Fragment"))==len(motion.findall("Fragment"))+2
    for old in origShake:assert_clip(w2[old.replace("+charge","+player+charge")],"Blank")
    for old,(src,dst) in baseIdle.items():assert_clip(i2[old.replace("+charge","+player+charge")],dst)
    pak=DEST/"Data"/"CrossbowShakeExperiment.pak"
    pak.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(pak,"w",zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        z.writestr(ADB,merged.encode("utf-8"))
    assert zipfile.ZipFile(pak).testzip() is None
    (DEST/"mod.manifest").write_text('''<?xml version="1.0" encoding="utf-8"?>
<kcd_mod><info><name>AutoMarksman Crossbow Shake EXPERIMENT</name><modid>theatrical_crossbow_shake_test</modid><description>LOCAL V2: player crossbow WeaponShake neutral and aim-idle alternative</description><author>Flubbermunchkin</author><version>0.0.2-local-only</version><created_on>2026-10-15</created_on></info></kcd_mod>
''',encoding="utf-8")
    (DEST/"READ_ME_FIRST.txt").write_text('''LOCAL IN-GAME TEST V2. DO NOT DISTRIBUTE.\n
Two standing/stealth player-only WeaponShake fragments are neutralized.
Also replaces ONLY player drawn crossbow MotionIdle aim loops with loaded stance loops.
All original game ADB entries unchanged; all existing No Herb Picking Animation changes preserved.
Possible drawback: different stance can alter the live projectile orientation, so check one controlled bolt shot.
If the pose becomes unnatural, do NOT shoot; disable this mod and restore the previous V1.
F4 still controls the normal AutoMarksman, not the standalone animation experiment.
''',encoding="utf-8")
    print("STAGED_V2",pak)
    print("SOURCE",hashlib.sha256(original).hexdigest())
    print("PAK_SHA256",hashlib.sha256(pak.read_bytes()).hexdigest())
    print("XML_ADDITIONS",len(insertionShake),"shake",len(insertionIdle),"aim-idle")
    print("ORIGINAL_XML_BYTE_PRESERVED_TRUE")
if __name__=="__main__":generate()
