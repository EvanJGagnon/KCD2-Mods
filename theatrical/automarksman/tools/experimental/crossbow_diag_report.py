"""Summarize signed crossbow camera/weapon pose diagnostics from a KCD2 log.
Usage: python tools/crossbow_diag_report.py [path_to_kcd2_automarksman.log]
Reads the game log, does not change it. All reported observations are from THIS save only.
"""
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

DEFAULT = Path(r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2\Mods\theatrical_automarksman\KCSE\Plugins\kcd2_automarksman.log")
def num(line, key):
    m = re.search(r"\b"+re.escape(key)+r"=([-+]?\d+(?:\.\d+)?)", line)
    return float(m.group(1)) if m else None
def angles(line, key):
    m = re.search(re.escape(key)+r"=\(yaw ([-+]?\d+(?:\.\d+)?) pitch ([-+]?\d+(?:\.\d+)?)\)", line)
    return (float(m.group(1)),float(m.group(2))) if m else None
def analyze(lines, output=True):
    samples=defaultdict(list)
    obsolete=0
    for l in lines:
        if "crossbow diagnostic:" not in l:continue
        if "relative_delta=" not in l:
            obsolete+=1
            continue
        a=re.search(r"\bF4=(ON|OFF) target=(YES|NONE) sample=(OK|FIRST)",l)
        if not a:continue
        state,target,status=a.groups()
        if status!="OK":continue
        row={"dt":num(l,"dt"),"input":angles(l,"input"),
             "view":angles(l,"cam_delta"),"weapon":angles(l,"weapon_delta"),
             "relative":angles(l,"relative_delta"),"sway":num(l,"native_sway_peak_deg_s")}
        # Standard stationary diagnostic: ignore manual input and nonuniform / long animation-transition intervals.
        if row["dt"] is None or not .18 <= row["dt"] <= .40:continue
        if not all(row[k] is not None for k in ("view","weapon","relative","sway")):continue
        if a and not row["input"]: # input=(x y), not yaw/pitch
            m=re.search(r"input=\(([-+\d.]+) ([-+\d.]+)\)",l)
            if not m:continue
            row["input"]=(float(m.group(1)),float(m.group(2)))
        if max(abs(x) for x in row["input"])>.002:continue
        samples[(state,target)].append(row)
    result={}
    for key,rows in sorted(samples.items()):
        def rms(xs):
            return math.sqrt(sum(v*v for v in xs)/len(xs))
        d={}
        for k in ("view","weapon","relative"):
            d[k+"_yaw_rms"]=rms([r[k][0] for r in rows])
            d[k+"_pitch_rms"]=rms([r[k][1] for r in rows])
            d[k+"_pitch_max"]=max(abs(r[k][1]) for r in rows)
        sway=[r["sway"] for r in rows if r["sway"]>=0]
        d["native_view_sway_peak"]=max(sway) if sway else None
        d["samples"]=len(rows)
        result[key]=d
    if output:
        print("Crossbow weapon-versus-camera diagnostic (high-skill Henry is NOT a low-skill test)")
        print("Older-format lines ignored:",obsolete)
        if not result:print("No clean signed-delta samples yet. Run the new diagnostic DLL in game.")
        for (state,target),d in result.items():
            print(" F4=%s target=%s: %d clean samples"%(state,target,d["samples"]))
            print("   camera RMS yaw/pitch: %.3f / %.3f degrees per ~0.25s"%(d["view_yaw_rms"],d["view_pitch_rms"]))
            print("   weapon RMS yaw/pitch: %.3f / %.3f"%(d["weapon_yaw_rms"],d["weapon_pitch_rms"]))
            print("   relative RMS yaw/pitch: %.3f / %.3f (peak pitch %.3f)"%(d["relative_yaw_rms"],d["relative_pitch_rms"],d["relative_pitch_max"]))
            if d["native_view_sway_peak"] is not None:
                print("   peak native view sway BEFORE percentage scaling: %.2f deg/s"%d["native_view_sway_peak"])
            if d["samples"]<20:print("   CAUTION: short sample, not enough for a firm conclusion.")
        print("Relative drift may be animation, IK, camera lag or transition. It cannot alone prove the cause.")
    return result

def selftest():
    base="crossbow diagnostic: F4=ON target=NONE sample=OK dt=0.250 input=(0.0000 0.0000) camera_step=0.000 deg weapon_step=0.000 deg "
    def ln(view,weapon,sway=5):
        rel=tuple(weapon[i]-view[i] for i in (0,1))
        return base+f"cam_delta=(yaw {view[0]:+.3f} pitch {view[1]:+.3f}) weapon_delta=(yaw {weapon[0]:+.3f} pitch {weapon[1]:+.3f}) relative_delta=(yaw {rel[0]:+.3f} pitch {rel[1]:+.3f}) native_sway_peak_deg_s={sway:.3f}"
    r=analyze([ln((0,0),(.3,0)),ln((.3,.2),(.3,.2)),ln((0,0),(0,0),50)],False)
    d=r["ON","NONE"]
    assert d["samples"]==3 and abs(d["native_view_sway_peak"]-50)<1e-8
    assert abs(d["relative_yaw_rms"]-math.sqrt(.09/3))<1e-5
    print("PASS: no-motion, camera-only/weapon-only, high/low native-sway magnitudes")

if __name__=="__main__":
    if "--selftest" in sys.argv:selftest()
    else:
        path=Path(sys.argv[1]) if len(sys.argv)>1 else DEFAULT
        analyze(path.read_text(encoding="utf-8",errors="replace").splitlines())
