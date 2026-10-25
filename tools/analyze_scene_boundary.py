"""Inspect guarded complete-scene candidates. This does not certify replay safety."""
import argparse
import json
from pathlib import Path
import struct
from analyze_engine_trace import fields


def snapshots(run, key, field, size, phases=("entry", "complete"), identity_valid=True):
    paths = [run / f"stage-{key}-scene-{phase}-{field}.bin" for phase in phases]
    valid = identity_valid and all(path.exists() and path.stat().st_size == size for path in paths)
    change = None
    data = None
    if valid:
        data = [path.read_bytes() for path in paths]
        change = [f"0x{i:x}" for i in range(0, size, 4) if data[0][i:i+4] != data[1][i:i+4]]
    return dict(valid=bool(valid), changedDwords=change), data


def state_identity(states, field, phases=("entry", "complete")):
    before, after = (states.get(f"scene-{phase}", {}) for phase in phases)
    if field == "manager" and (before.get("manager_valid") != "1" or after.get("manager_valid") != "1"):
        return False
    return bool(before.get(field) and before.get(field) == after.get(field) and
                before.get(field + "_saved") == "1" and after.get(field + "_saved") == "1")


def analyze(run):
    lines = (run / "engine.log").read_text().splitlines()
    entries = {fields(line)["id"]: fields(line) for line in lines if "scene candidate " in line}
    exits = {fields(line)["id"]: fields(line) for line in lines if "scene complete " in line}
    result = []
    for key, entry in entries.items():
        end = exits.get(key, {})
        same_frame_thread = bool(end and entry["frame"] == end["frame"] and entry["tid"] == end["tid"])
        pair_valid = entry["valid"] == "1" and end.get("stage6_seen") == "1" and same_frame_thread
        states = {fields(line)["phase"]: fields(line) for line in lines
                  if "scene state " in line and fields(line).get("id") == key}
        compared = {}
        captured = {}
        sizes = {"object": 0x160, "packet": 0x128, "view": 0x90, "context": 0x200,
                 "manager": 0x200, "render-data": 0x200}
        for field, size in sizes.items():
            identity = pair_valid and state_identity(states, field.replace("-", "_")) if field in ("manager", "render-data") else True
            compared[field], captured[field] = snapshots(run, key, field, size, identity_valid=identity)
        if captured["object"]:
            compared["object"]["renderCounter"] = [struct.unpack_from("<I", data, 0xc8)[0] for data in captured["object"]]
        history = dict(valid=False)
        if captured["manager"] and captured["context"]:
            before, after = captured["manager"]
            history = dict(valid=True,
                eyeIndex=[struct.unpack_from("<I", data, 0xa0)[0] for data in (before, after)],
                counter=[struct.unpack_from("<I", data, 0x94)[0] for data in (before, after)],
                slot0Changed=before[0x10:0x50] != after[0x10:0x50],
                slot1Changed=before[0x50:0x90] != after[0x50:0x90],
                slot0AfterMatchesEntryViewProjection=after[0x10:0x50] == captured["context"][0][0x190:0x1d0],
                slot0AfterMatchesCurrentViewProjection=after[0x10:0x50] == captured["context"][1][0x190:0x1d0])
            if captured["view"]:
                history["viewFirstPassFlag"] = [data[0x8c] for data in captured["view"]]
        transitions = {}
        for phases in (("entry", "after5"), ("after5", "before6"), ("before6", "complete")):
            transitions["_to_".join(phases)] = {}
            for field in ("packet", "manager", "render-data"):
                identity = pair_valid and (field == "packet" or state_identity(states, field.replace("-", "_"), phases))
                transitions["_to_".join(phases)][field], _ = snapshots(run, key, field, sizes[field], phases, identity)
        result.append(dict(
            id=int(key), frame=int(entry["frame"]), stackValidated=entry["valid"] == "1",
            complete=bool(end), stage6Validated=end.get("stage6_seen") == "1",
            sameFrameAndThread=same_frame_thread,
            nativeStereo=end.get("native_stereo"), snapshots=compared, history=history, transitions=transitions,
        ))
    return dict(run=run.name, candidates=len(entries), matchedComplete=len(exits), samples=result,
                limits="Entry is after initial camera setup. Bounded snapshots exclude pointed-to data, GPU history and simulation state. This report does not assess replay; use analyze_replay.py for an opted-in replay run.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    (args.run / "scene-boundary-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
