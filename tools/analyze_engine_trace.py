"""Summarize observed engine calls. Does not certify replay safety or stereo."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r"(\w+)=([^\s]+)", line))


def analyze_stages(run, lines, rows):
    entries = {fields(line)["id"]: fields(line) for line in lines if "stage enter " in line}
    exits = {fields(line)["id"]: fields(line) for line in lines if "stage exit " in line}
    setup = next((fields(line) for line in lines if "main stage trace " in line), {})
    graphics = (run / "trace.log").read_text().splitlines() if (run / "trace.log").exists() else []
    boundaries = {}
    targets = {}
    for line in graphics:
        if "stage boundary " in line or "stage target " in line:
            data = fields(line)
            key = (data["id"], data["phase"])
            if "stage boundary " in line:
                boundaries[key] = data
            else:
                targets.setdefault(key, []).append(data)
    samples = []
    for key, entry in entries.items():
        slot = int(entry["slot"])
        snapshots = {}
        for field, size in (("object", 0x160), ("context", 0x200), ("view", 0x90)):
            if field != "object" and slot not in (5, 6):
                continue
            paths = [run / f"stage-{key}-{phase}-{field}.bin" for phase in ("before", "after")]
            valid = all(path.exists() and path.stat().st_size == size for path in paths)
            changes = None
            if valid:
                before, after = (path.read_bytes() for path in paths)
                changes = [f"0x{i:x}" for i in range(0, size, 4) if before[i:i+4] != after[i:i+4]]
            snapshots[field] = dict(valid=valid, changedDwords=changes)
        before, after = (boundaries.get((key, phase)) for phase in ("before", "after"))
        graphics_valid = bool(before and after and before["tid"] == after["tid"] == entry["tid"]
                              and before["context"] == after["context"])
        delta = {name: int(after[name]) - int(before[name]) for name in ("draws", "dispatches")} if graphics_valid else None
        if delta and any(value < 0 for value in delta.values()):
            graphics_valid, delta = False, None
        samples.append(dict(
            id=int(key), frame=int(entry["frame"]), slot=slot, type=entry["type"],
            object=entry["self"], validObject=entry["valid"] == "1", caller=entry["caller_rva"],
            matchedExit=key in exits, snapshots=snapshots, graphicsValid=graphics_valid, graphicsDelta=delta,
            targetsBefore=targets.get((key, "before"), []), targetsAfter=targets.get((key, "after"), []),
        ))
    return dict(
        installed=setup.get("installed") == "1", capturedEntries=len(entries),
        matchedExits=sum(sample["matchedExit"] for sample in samples),
        countedCalls={str(slot): sum(int(row.get(f"slot{slot}", 0)) for row in rows) for slot in (3, 5, 6, 9)},
        observedThreads=sorted({entry["tid"] for entry in entries.values()}), samples=samples,
        limits="Bounded object/camera windows and current D3D bindings only. No replay, simulation isolation, complete state restoration or resource ownership proof.",
    )


def analyze(run):
    if (run / "engine-vtable-frames.csv").exists():
        lines = (run / "engine.log").read_text().splitlines()
        calls = [fields(line) for line in lines if re.search(r"\bframe=\d+ camera id=", line)]
        exits = [fields(line) for line in lines if " camera exit id=" in line]
        rows = list(csv.DictReader((run / "engine-vtable-frames.csv").open()))
        setup = next((fields(line) for line in lines if "engine vtable trace " in line), {})
        fault = run / "engine-faults.txt"
        return dict(
            run=run.name, mode="camera_vtable", installed=setup.get("installed") == "1",
            instructionsUnchanged=setup.get("instructions_unchanged") == "1",
            countedCameraCalls=sum(int(row["camera_uploads"]) for row in rows),
            capturedCameraCalls=len(calls), matchedCameraExits=len(exits),
            changedContextSamples=sum(row["before"] != row["after"] for row in exits),
            observedThreads=sorted({row["tid"] for row in calls}),
            callerCounts=dict(Counter(row["caller_rva"] for row in calls)),
            stereoStates=sorted({row["stereo"] for row in calls}),
            stereoWidths=sorted({row["stereo_width"] for row in calls}),
            vendorHandles=sorted({row["vendor_handle"] for row in calls}),
            exceptionObservations=fault.exists() and fault.stat().st_size != 0,
            stages=analyze_stages(run, lines, rows),
            limits="Observations only; do not demonstrate replay, simulation isolation, or two eye outputs.",
        )
    entries, exits, cameras = {}, {}, []
    for line in (run / "engine.log").read_text().splitlines():
        data = fields(line)
        if "scene enter " in line:
            entries[data["id"]] = data
        elif "scene exit " in line:
            exits[data["id"]] = data
        elif " camera id=" in line:
            cameras.append(data)
    paired = [(entry, exits[key]) for key, entry in entries.items() if key in exits]
    frame_counts = list(csv.DictReader((run / "engine-frames.csv").open()))
    types = Counter(entry["type"] for entry in entries.values())
    counters = Counter(tuple(row[key] for key in ("wrappers", "scenes", "camera_uploads", "eye_selections")) for row in frame_counts)
    main = []
    for entry, result in paired:
        if entry["type"] != "0x400":
            continue
        main.append(dict(
            frame=int(entry["frame"]), id=int(entry["id"]), wrapper=int(entry["wrapper"]),
            caller=entry["caller_rva"], stereo=int(entry["stereo"]), left=int(entry["left"]),
            secondAllowed=int(entry["second_allowed"]), draws=int(result["draws"]), dispatches=int(result["dispatches"]),
            viewUnchanged=result["view_before"] != "0000000000000000" and result["view_before"] == result["view_after"],
            contextUnchanged=result["context_before"] != "0000000000000000" and result["context_before"] == result["context_after"],
            targetObject=entry["target_object"], pairValid=entry.get("pair_valid"),
            leftObject=entry.get("left_object"), rightObject=entry.get("right_object"),
            stereoWidth=entry.get("stereo_width"), vendorHandle=entry.get("vendor_handle"),
        ))
    fault = run / "engine-faults.txt"
    return dict(
        run=run.name, capturedSceneEntries=len(entries), matchedSceneExits=len(paired),
        sceneTypes=dict(types), observedThreads=sorted({entry["tid"] for entry in entries.values()}),
        capturedCameraCalls=len(cameras), observedEyeSelections=sum(int(row["eye_selections"]) for row in frame_counts),
        commonFrameCounts=[dict(frames=count, wrappers=int(key[0]), scenes=int(key[1]), cameraUploads=int(key[2]), eyeSelections=int(key[3])) for key, count in counters.most_common(5)],
        exceptionObservations=fault.exists() and fault.stat().st_size != 0,
        mainScenes=main,
        limits="Hashes cover only the captured 0x90-byte view and 0x200-byte context; unchanged hashes do not prove simulation or full resource restoration.",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    destination = args.run / "engine-summary.json"
    destination.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
