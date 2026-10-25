"""Compare per-use texture hashes without promoting them to complete replay proof."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

PRODUCERS = {"e8bc75f0f617aa9e": ("ao-volume", 3),
             "158f4162b8000a3f": ("shadow-mask", 5),
             "789227e243eff8ea": ("minmax-depth", 1),
             "922a49bc5dcd4775": ("main-material", 8)}


def valid(texture):
    parts = texture.get("subresources", [])
    mips, layers = texture.get("mips", 0), texture.get("array", 0)
    return (texture.get("status") == "ok" and 0 < mips <= 15 and 0 < layers <= 2048
            and texture.get("samples") == 1 and len(parts) == mips * layers
            and {(p["slice"], p["mip"]) for p in parts} == {(s, m) for s in range(layers) for m in range(mips)}
            and all(p["rowBytes"] > 0 and p["rows"] > 0 and re.fullmatch(r"[0-9a-f]{64}", p["sha256"]) for p in parts)
            and sum(p["rowBytes"] * p["rows"] for p in parts) == texture.get("bytes"))


def analyze(run):
    # This compact capture survives pruning and can still be analyzed afterward.
    rows = [json.loads(line) for line in (run / "producer-inputs.jsonl").read_text().splitlines() if line]
    pass_count = 3 if (run / "stereo-eyes.request").exists() else 2
    draws = {}
    for row in rows:
        match = re.fullmatch(r"work-(\d+)-pass([012])-(slot\d+-draw\d+)", row["mark"])
        if not match or row["shader"] not in PRODUCERS:
            raise ValueError("Unrecognized producer capture identity")
        scene, render_pass, mark = match.groups()
        if int(render_pass) >= pass_count: raise ValueError("Unexpected producer render pass")
        slots = draws.setdefault((scene, mark), [{} for _ in range(pass_count)])[int(render_pass)]
        if row["slot"] in slots:
            raise ValueError("Duplicate producer input record")
        slots[row["slot"]] = row
    pairs = []
    counts = Counter()
    for (scene, mark), passes in draws.items():
        shaders = {row["shader"] for slots in passes for row in slots.values()}
        identity = len(shaders) == 1
        shader = next(iter(shaders))
        producer, count = PRODUCERS[shader]
        expected = {"ps" + str(i) for i in range(count)} | {"target-before"}
        if any("depth-before" in slots for slots in passes): expected.add("depth-before")
        coverage = identity and all(set(slots) == expected for slots in passes)
        comparisons = {}
        for slot in sorted(expected | set().union(*(set(p) for p in passes))):
            records = [slots.get(slot) for slots in passes]
            a = records[0]
            captured = all(row and valid(row["texture"]) for row in records)
            # The selected material also samples Texture2DArray light maps.
            views = bool(a and all(row and a["view"] == row["view"] for row in records) and
                         (slot in ("target-before", "depth-before") or a["view"] and (a["view"].get("dimension") == 4 or
                          a["view"].get("dimension") == 5 and all(k in a["view"] for k in ("firstMip", "mips", "firstSlice", "slices")))))
            equal = captured and views and all(a["texture"] == row["texture"] for row in records)
            comparisons[slot] = dict(captured=captured, viewEqual=views, hashEqual=bool(equal),
                                     statuses=[row["texture"]["status"] if row else "missing" for row in records])
        counts[producer] += 1
        pairs.append(dict(scene=int(scene), mark=mark, producer=producer, shader=shader,
                          coverage=coverage, slots=comparisons,
                          sampledInputsEqual=coverage and all(comparisons["ps"+str(i)]["hashEqual"] for i in range(count)),
                          targetBeforeEqual=comparisons["target-before"]["hashEqual"]))
    return dict(schema=1, renderPasses=pass_count, draws=pairs, producerCounts=dict(counts), records=len(rows),
                capturedInputsEqual=bool(pairs) and all(p["sampledInputsEqual"] for p in pairs),
                limits="SHA-256 of all mip/slice texel or compressed-block rows at selected draws only. No MSAA resolve, sampling tolerance, full pipeline or simulation acceptance. Missing producers and unsupported reads remain unproven.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    path = args.run / "producer-input-summary.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k != "draws"}, indent=2))
