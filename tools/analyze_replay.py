"""Evaluate one identical-view replay; never certifies stereo or simulation isolation."""
import argparse
import json
from pathlib import Path
import struct
from analyze_engine_trace import fields
from analyze_scene_boundary import analyze as analyze_scenes
from analyze_replay_work import raw_pair, compute_resource_pair, vpl_sort_pair


def word(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def image_pair(first, second):
    from PIL import Image, ImageChops
    if not first.exists() or not second.exists():
        return dict(valid=False)
    with Image.open(first) as a, Image.open(second) as b:
        if a.mode != "RGB" or a.size != b.size or b.mode != "RGB":
            return dict(valid=False)
        difference = ImageChops.difference(a, b)
        hist = difference.histogram()
        changed = ImageChops.lighter(ImageChops.lighter(*difference.split()[:2]), difference.split()[2])
        pixels = a.width * a.height
        return dict(valid=True, size=list(a.size), equal=difference.getbbox() is None,
                    changedPixelFraction=1-changed.histogram()[0]/pixels,
                    meanAbsoluteChannelError=sum((i % 256)*count for i, count in enumerate(hist))/(pixels*3),
                    maximumChannelError=max(high for _, high in difference.getextrema()),
                    precision="8-bit RGB readback; source RGB10 is quantized")


def analyze(run, scene_id=None):
    if (run / "capture-cleanup.json").exists():
        raise ValueError("Raw captures were retired; read retained-evidence.json for the historical result.")
    lines = (run / "engine.log").read_text().splitlines()
    starts = [fields(line) for line in lines if "replay begin " in line]
    ends = [fields(line) for line in lines if "replay end " in line]
    if scene_id is not None:
        starts=[r for r in starts if r.get('id')==str(scene_id)]
        ends=[r for r in ends if r.get('id')==str(scene_id)]
    result = dict(run=run.name, attempts=len(starts), completions=len(ends), accepted=False,
                  refusals=[fields(line) for line in lines if "replay refused " in line],
                  limits="One render-thread replay with bounded CPU snapshots. GPU histories, pointed-to packet contents, physics ticks, stereo and headset behavior are not certified.")
    result["mode"]="geometry-offset" if (run/"eye-offset.request").exists() else "identical-view"
    if len(starts) != 1 or len(ends) != 1:
        return result
    start, end = starts[0], ends[0]
    key = start["id"]
    gates = result["gates"] = {}
    gates["matchingReturn"] = all(start.get(field) and start[field] == end.get(field) for field in ("id", "frame", "tid"))
    for flag in ("same_frame", "image_saved", "packet_same", "context_same", "history_other_same", "history_restored", "history_slot1_current"):
        gates[flag] = end.get(flag) == "1"
    gates["renderCounterPreserved"] = bool(end.get("counter_before") and end["counter_before"] == end.get("counter_after"))
    gates["historyCounterPreserved"] = bool(start.get("history_counter") and start["history_counter"] == end.get("history_counter_after"))
    phases = ("scene-entry", "scene-complete", "replay-entry", "replay-complete", "replay-restored")
    sizes = {"manager": 0x200, "context": 0x200, "view": 0x90, "packet": 0x128, "object": 0x160, "render-data": 0x200}
    data = {}
    for phase in phases:
        for field, size in sizes.items():
            path = run / f"stage-{key}-{phase}-{field}.bin"
            if path.exists() and path.stat().st_size == size:
                data[phase, field] = path.read_bytes()
    gates["snapshotsComplete"] = len(data) == len(phases)*len(sizes)
    states = {fields(line)["phase"]: fields(line) for line in lines if "scene state " in line and fields(line).get("id") == key}
    gates["managerIdentity"] = len(states) >= len(phases) and all(
        states.get(p, {}).get("manager") == start.get("manager") and
        states.get(p, {}).get("manager_valid") == "1" and states.get(p, {}).get("manager_saved") == "1" for p in phases)
    if (run / "wait-clip-transition.request").exists():
        triggers=[fields(line) for line in lines if "capture trigger observed=clip_transition " in line]
        clips=[fields(line) for line in lines if "replay clip " in line and fields(line).get("id")==key]
        gates["requestedClipTransitionObserved"] = bool(len(triggers)==1 and
            triggers[0].get("tid")==start.get("tid") and int(triggers[0].get("frame",-2))+1==int(start["frame"]))
        gates["requestedClipTransitionReplayed"] = bool(gates["snapshotsComplete"] and len(clips)==1 and
            clips[0].get("clip_changed")=="1" and
            data["scene-entry","context"][0xe8:0xf0]==struct.pack('<ff',.1,15000) and
            data["scene-complete","context"][0xe8:0xf0]==struct.pack('<ff',.2,100))
    if gates["snapshotsComplete"]:
        before = data["scene-complete", "manager"]
        replay = data["replay-complete", "manager"]
        gates["restoredBytes"] = data["replay-restored", "manager"] == before
        gates["primaryHistoryPreserved"] = before[0x10:0x50] == replay[0x10:0x50]
        gates["secondaryHistorySeed"] = data["replay-entry", "manager"][0x50:0x90] == data["scene-entry", "manager"][0x10:0x50]
        gates["cameraEntryRestored"] = data["replay-entry", "context"] == data["scene-entry", "context"]
        gates["cameraCompletionRestored"] = data["replay-restored", "context"] == data["scene-complete", "context"]
        gates["cameraNearPrepared"] = data["replay-entry", "object"][0x40:0x44] == data["scene-entry", "object"][0x40:0x44]
        gates["cameraNearRestored"] = data["replay-restored", "object"][0x40:0x44] == data["scene-complete", "object"][0x40:0x44]
        gates["primaryHistoryMatchesInput"] = before[0x10:0x50] == data["scene-entry", "context"][0x190:0x1d0]
        gates["secondaryHistoryMatchesInput"] = replay[0x50:0x90] == data["scene-entry", "context"][0x190:0x1d0]
        gates["viewCopyOnlyFlag"] = data["replay-entry", "view"] == data["scene-complete", "view"][:0x8c] + b"\0" + data["scene-complete", "view"][0x8d:]
        result["changedDwords"] = {field: [hex(i) for i in range(0, size, 4)
            if data["scene-complete", field][i:i+4] != data["replay-complete", field][i:i+4]] for field, size in sizes.items()}
        for field in ("context", "packet", "render-data", "object"):
            gates[field+"Unchanged"] = data["scene-complete", field] == data["replay-complete", field]
    graphics = (run / "trace.log").read_text().splitlines() if (run / "trace.log").exists() else []
    if (run / "fresh-replay-color.request").exists():
        import re
        import numpy as np
        rows=[fields(line) for line in graphics if "fresh_color name=" in line]
        complete=len(rows)==2 and [r.get("pass") for r in rows]==["0","1"]
        layouts=[]
        for p,row in enumerate(rows):
            name=row.get("name","")
            valid_name=bool(re.fullmatch(rf"work-{key}-pass{p}-slot6-draw\d+",name))
            complete &= valid_name and row.get("applied")=="1" and row.get("saved")=="1" and row.get("seed")==("magenta","green")[p%2]
            if not valid_name: continue
            path=run/(name+"-fresh-seed.raw")
            data_bytes=path.read_bytes() if path.exists() else b""
            if len(data_bytes)<16: complete=False; continue
            width,height,format,samples=struct.unpack_from("<4I",data_bytes)
            layouts.append((width,height,format,samples,name.split('-slot6-')[-1]))
            complete &= bool(width and height and width*height<=3840*2160 and format==24 and samples==1 and len(data_bytes)==16+width*height*4)
            if len(data_bytes)%4: complete=False; continue
            values=np.frombuffer(data_bytes,dtype='<u4',offset=16)
            complete &= bool(np.all(values==((0xfff003ff,0xc00ffc00)[p%2])))
        gates["freshColorSeedsVerified"]=bool(complete and len(layouts)==2 and layouts[0]==layouts[1])
        result["freshColorSeeds"]=rows
    boundaries = {fields(line)["phase"]: fields(line) for line in graphics
                  if "stage boundary " in line and fields(line).get("id") == key and fields(line).get("slot") == "100"}
    targets = {fields(line)["phase"]: fields(line) for line in graphics
               if "stage target " in line and fields(line).get("id") == key and fields(line).get("kind") == "color" and fields(line).get("index") == "0"}
    a, b = (targets.get(p, {}) for p in ("scene-complete", "replay-complete"))
    gates["sameOutputTarget"] = bool(a and b and all(a.get(f) == b.get(f) for f in ("resource", "size", "format", "samples")))
    draws = {}
    if all(p in boundaries for p in phases[:4]):
        for prefix in ("scene", "replay"):
            draws[prefix] = {metric: int(boundaries[prefix+"-complete"][metric])-int(boundaries[prefix+"-entry"][metric]) for metric in ("draws", "dispatches")}
    result["work"] = draws
    gates["matchingRenderWork"] = bool(draws and draws["scene"] == draws["replay"] and draws["scene"]["draws"] > 0)
    result["images"] = image_pair(run / f"stage-{key}-scene-complete.ppm", run / f"stage-{key}-replay-complete.ppm")
    gates["identicalReadback"] = result["images"].get("equal") is True
    if (run / "replay-work.request").exists() or (run / f"stage-{key}-scene-complete.raw").exists():
        result["sourceImages"]=raw_pair(run / f"stage-{key}-scene-complete.raw", run / f"stage-{key}-replay-complete.raw")
        gates["identicalSourceReadback"]=result["sourceImages"].get("equal") is True
    work_path=run / "replay-work.txt"
    work_lines=work_path.read_text().splitlines() if work_path.exists() else []
    if scene_id is not None:
        work_lines=[line for line in work_lines if fields(line).get('scene',key)==key]
    if (run / "replay-samples.request").exists():
        from analyze_msaa import analyze as analyze_samples
        result["sampleReadbacks"]=analyze_samples(run)
        gates["sampleReadbacksRestored"]=result["sampleReadbacks"]["complete"]
    if (run / "repeat-ao.request").exists():
        from analyze_ao_repeat import analyze as analyze_repeat
        result["aoRepeat"]=analyze_repeat(run)
        gates["aoRepeatRestored"]=result["aoRepeat"]["complete"] and result["aoRepeat"]["restored"]
    if (run / "repeat-shadow.request").exists():
        from analyze_shadow_repeat import analyze as analyze_shadow
        result["shadowRepeat"]=analyze_shadow(run)
        gates["shadowRepeatRestored"]=result["shadowRepeat"]["complete"] and result["shadowRepeat"]["restored"]
    if (run / "isolate-shadow-sampler.request").exists():
        rows=[fields(line) for line in work_lines if line.startswith("shadow_sampler ")]
        ordinals={r.get("ordinal") for r in rows}
        gates["shadowSamplerRestored"]=bool(len(rows)==3 and len(ordinals)==1 and None not in ordinals and
            not any("refused" in r for r in rows) and
            all(sum(r.get(flag)=="1" for r in rows)==1 for flag in ("captured","applied","restored")))
        result["shadowSampler"]=rows
    if (run / "normalize-shadow-bindings.request").exists():
        rows=[fields(line) for line in work_lines if line.startswith("shadow_unused ")]
        ordinals={r.get("ordinal") for r in rows}
        gates["unusedShadowBindingsRestored"]=bool(len(rows)==4 and len(ordinals)==1 and None not in ordinals and
            all(sum(r.get("pass")==str(p) and r.get(flag)=="1" for r in rows)==1
                for p in (0,1) for flag in ("applied","restored")))
        result["unusedShadowBindings"]=rows
    if (run / "isolate-models.request").exists():
        rows=[fields(line) for line in work_lines if line.startswith("model_isolation ")]
        sets=[{r["ordinal"] for r in rows if r.get(flag)=="1"} for flag in ("captured","applied","restored")]
        gates["modelIsolationRestored"]=bool(sets[0] and sets[0]==sets[1]==sets[2] and not any("refused" in r for r in rows))
        result["modelIsolation"]=dict(captured=len(sets[0]),applied=len(sets[1]),restored=len(sets[2]),
                                      limits="One identified model field per matching draw; not simulation isolation")
    if (run / "restore-compute-state.request").exists():
        rows=[fields(line) for line in work_lines if line.startswith("compute_state ")]
        gates["computeBindingsRestored"]=all(sum(r.get(flag)=="1" for r in rows)==1 for flag in ("prepared","restored"))
        history=[fields(line) for line in work_lines if line.startswith("ao_history ")]
        gates["aoHistoryRestored"]=bool(history and not any("refused" in r for r in history) and
            all(sum(r.get(flag)=="1" for r in history)==1 for flag in ("captured","prepared","completed_restored")))
        result["aoRestorationReadback"]=compute_resource_pair(run / f"stage-{key}-scene-complete-ao.texture",
                                                             run / f"stage-{key}-replay-restored-ao.texture")
        gates["aoRestorationReadback"]=result["aoRestorationReadback"].get("equal") is True
    if (run / "sort-vpl.request").exists():
        rows=[fields(line) for line in work_lines if line.startswith("vpl_order ")]
        gates["vplOrderApplied"]=bool(len(rows)==2 and {r.get("pass") for r in rows}=={"0","1"} and
            all(r.get("applied")=="1" and r.get("restored")=="1" and "refused" not in r for r in rows))
        result["vplOrder"]=[vpl_sort_pair(run / f"work-{key}-pass{p}-compute4-uav1-native.texture",
                                         run / f"work-{key}-pass{p}-compute4-uav1.texture") for p in (0,1)]
        gates["vplOrderContents"]=all(all(v.get(k) is True for k in
            ("valid","membershipPreserved","ordered","terminatorAndTailPreserved")) for v in result["vplOrder"])
    following = [s for s in analyze_scenes(run)["samples"] if s["frame"] == int(start["frame"])+1 and s["sameFrameAndThread"] and s["stage6Validated"]]
    gates["followingHistoryContinuity"] = False
    if len(following) == 1 and gates["snapshotsComplete"]:
        sample = following[0]
        path = run / f"stage-{sample['id']}-scene-entry-manager.bin"
        following_states = [fields(line) for line in lines if "scene state " in line and fields(line).get("id") == str(sample["id"])]
        history = sample["history"]
        following_bytes = path.read_bytes() if path.exists() else b""
        restored = data["replay-restored", "manager"]
        # Native slots 9 (0x542e20) and 10 (0x783a00) write these four
        # floating-point inputs between scenes, including passive control runs.
        # They are never restored by replay. All remaining bytes must match.
        continuity = len(following_bytes) == 0x200 and following_bytes[:0x1dc] == restored[:0x1dc] and following_bytes[0x1ec:] == restored[0x1ec:]
        gates["followingHistoryContinuity"] = (bool(following_states) and all(s.get("manager") == start["manager"] and s.get("tid") == start["tid"] for s in following_states) and
            continuity and history.get("valid") and
            history["counter"] == [int(start["history_counter"]), int(start["history_counter"])+1] and
            history["eyeIndex"] == [0, 0] and history["slot0AfterMatchesEntryViewProjection"] and not history["slot1Changed"])
        result["following"] = dict(id=sample["id"], frame=sample["frame"], history=history,
            entryChangedDwords=[hex(i) for i in range(0, 0x200, 4) if restored[i:i+4] != following_bytes[i:i+4]])
    result["accepted"] = result["mode"]=="identical-view" and all(gates.values())
    result["failedGates"] = [key for key, value in gates.items() if not value]
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    (args.run / "replay-summary.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result, indent=2))
