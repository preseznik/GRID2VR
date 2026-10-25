"""Correlate bounded CPU snapshots with named VS camera bindings; not stereo proof."""
import argparse
import json
import math
from pathlib import Path
import re
import struct

SNAPSHOT = re.compile(r"constant snapshot=(constant-(\d+)-(\d+)\.bin) context=(\w+) resource=(\w+) bytes=(\d+)")
DRAW = re.compile(r"camera draw frame=(\d+) uploads_seen=(\d+) context=(\w+) shader=(\w+) slot=(\d+) resource=(\w+) allocated=(\d+) declared=(\d+) used_end=(\d+) viewport=(\d+x\d+) target=(\w+)")


def analyze(run):
    latest, matches = {}, []
    for line in (run / "trace.log").read_text().splitlines():
        snapshot = SNAPSHOT.search(line)
        if snapshot:
            name, frame, sample, context, resource, size = snapshot.groups()
            latest[context, resource] = (name, int(frame), int(sample), int(size))
        draw = DRAW.search(line)
        if not draw:
            continue
        frame, seen, context, shader, slot, resource, allocated, declared, used, viewport, target = draw.groups()
        saved = latest.get((context, resource))
        # Once the upload cap is reached, further unrecorded updates could make
        # the saved bytes stale. Never associate those later draws with a file.
        if not saved or saved[1] != int(frame) or int(seen) >= 64:
            continue
        name, _, sample, size = saved
        data = (run / name).read_bytes()
        if len(data) != size or size < 308 or int(used) > size:
            continue
        values = struct.unpack_from("<76f", data)
        if not all(math.isfinite(value) for value in values):
            continue
        match = dict(
            frame=int(frame), sample=sample, snapshot=name, context=context,
            resource=resource, shader=shader, slot=int(slot), allocated=int(allocated),
            declared=int(declared), usedEnd=int(used), viewport=viewport, target=target,
            projection=list(values[:16]), view=list(values[32:44]),
            eyePosition=list(values[56:59]), leftEyeRaw=struct.unpack_from("<I", data, 304)[0],
        )
        stamp = re.search(r"engine_camera=(\d+) engine_context=(\w+)", line)
        if stamp:
            camera_id, engine_context = stamp.groups()
            match.update(engineCamera=int(camera_id), engineContext=engine_context)
            cpu_path = run / f"engine-{camera_id}-camera-before.bin"
            if cpu_path.exists():
                cpu = cpu_path.read_bytes()
                match.update(
                    cpuProjectionMatches=cpu[0x110:0x150] == data[:64],
                    cpuViewProjectionMatches=cpu[0x190:0x1D0] == data[64:128],
                )
        matches.append(match)
    return matches


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    matches = analyze(args.run)
    destination = args.run / "camera-binding-evidence.json"
    destination.write_text(json.dumps(matches, indent=2, allow_nan=False) + "\n")
    print(f"{len(matches)} same-frame binding correlations: {destination}")
    print("Viewports:", ", ".join(sorted({match["viewport"] for match in matches})))
