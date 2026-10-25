"""Compare packed, unresolved RGB10A2 samples from the bounded draw diagnostic."""
import argparse
import json
from pathlib import Path
import re
import numpy as np
from analyze_engine_trace import fields
from analyze_replay_work import compute_resource_pair


def analyze(run):
    if (run / "capture-cleanup.json").exists():
        raise ValueError("MSAA captures were retired; use the retained summary.")
    groups = {}
    for line in (run / "trace.log").read_text().splitlines():
        if "msaa_readback name=" not in line: continue
        row = fields(line)
        match = re.fullmatch(r"work-(\d+)-pass([01])-(slot6-draw\d+)", row["name"])
        if not match or row.get("phase") not in ("before", "after"):
            raise ValueError("Unknown MSAA checkpoint")
        scene, render_pass, draw = match.groups()
        key = (scene, draw)
        records = groups.setdefault(key, {})
        identity = (int(render_pass), row["phase"])
        if identity in records: raise ValueError("Duplicate MSAA checkpoint")
        records[identity] = row
    draws = []
    for (scene, draw), records in groups.items():
        coverage = set(records) == {(p, phase) for p in (0, 1) for phase in ("before", "after")}
        coverage &= all(row["hr"] == "00000000" and row["restored"] == "1" and row["format"] == "24"
                        and int(row["samples"]) in (2, 4, 8) for row in records.values())
        layouts = {tuple(row[k] for k in ("width", "height", "format", "samples", "quality")) for row in records.values()}
        coverage &= len(layouts) == 1
        width, height, _, samples, _ = map(int, next(iter(layouts)))
        phases = {}
        if samples not in (2, 4, 8): coverage = False; samples = 0
        for phase in ("before", "after"):
            comparisons = []
            for sample in range(samples):
                paths = [run / f"work-{scene}-pass{p}-{draw}-{phase}-msaa{sample}.texture" for p in (0, 1)]
                pair = compute_resource_pair(*paths)
                pair["sample"] = sample
                if pair.get("valid") and pair["texture"] != [width, height, 42, 1, 1, 4]: pair = dict(valid=False, sample=sample)
                if pair.get("valid"):
                    a, b = (np.fromfile(path, dtype="<u4", offset=24) for path in paths)
                    changed = np.flatnonzero(a != b)
                    pair["changedPixels"] = len(changed)
                    pair["firstPixels"] = [dict(x=int(i % width), y=int(i // width), original=int(a[i]), replay=int(b[i])) for i in changed[:8]]
                comparisons.append(pair)
            phases[phase] = dict(samples=comparisons, equal=bool(coverage and comparisons and all(p.get("equal") for p in comparisons)))
        draws.append(dict(scene=int(scene), draw=draw, coverage=coverage, source=[width, height, 24, samples], phases=phases))
    return dict(schema=1, draws=draws, complete=bool(draws) and all(d["coverage"] and all(p.get("valid") for phase in d["phases"].values() for p in phase["samples"]) for d in draws),
                limits="Exact packed RGB10A2 samples of the selected draw target only; diagnostic compute dispatches restore CS state. No depth samples, full pipeline or simulation acceptance.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    (args.run / "msaa-summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
