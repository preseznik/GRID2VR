"""Locate replay differences without treating partial pipeline traces as simulation proof."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import numpy as np
from analyze_engine_trace import fields


def used_constants(run, passes):
    identities = {fields(line)["object"]: fields(line)["stage"]+"-"+fields(line)["hash"]
                  for line in (run / "trace.log").read_text().splitlines() if "shader object=" in line}
    metadata = {}
    shader = slot = None
    for line in (run / "shaders.txt").read_text().splitlines():
        if not line.startswith(" "):
            shader = line.split()[0]; metadata[shader] = {}
        elif not line.startswith("    "):
            slot = int(fields(line)["slot"]); metadata[shader][slot] = []
        elif "used=1" in line:
            item = fields(line)
            metadata[shader][slot].append((line.split()[0], int(item["offset"]), int(item["size"])))
    differences = []
    unknown = Counter()
    for a, b in zip(*passes):
        if a["kind"] == "Compute": continue
        for stage in ("v", "p"):
            name = identities.get(a[stage+"s"])
            reflection = metadata.get(name)
            if reflection is None:
                unknown["shaderMetadata"] += 1; continue
            if a[stage+"s"] != b[stage+"s"]:
                unknown["changedShader"] += 1; continue
            for slot, variables in reflection.items():
                if not variables: continue
                # The legacy reflection file also lists structured resources.
                # Slots >=14 cannot be D3D11 constant-buffer bindings.
                if slot>=14:
                    unknown["nonConstantResourceBindings"] += 1; continue
                field = stage+str(slot)
                bindings = [row.get(field, "") for row in (a, b)]
                if not all(bindings) or any(":unknown:" in binding or not binding.endswith(":1") for binding in bindings):
                    unknown["usedUpload"] += 1; continue
                hashes = [binding.split(":")[1] for binding in bindings]
                paths = [run / f"upload-{h}.bin" for h in hashes]
                if not all(path.exists() for path in paths):
                    unknown["missingUploadFile"] += 1; continue
                data = [path.read_bytes() for path in paths]
                for variable, offset, size in variables:
                    if any(offset+size > len(blob) for blob in data):
                        unknown["truncatedUsedVariable"] += 1; continue
                    if data[0][offset:offset+size] == data[1][offset:offset+size]: continue
                    changed = [i for i in range(offset, offset+size, 4) if data[0][i:i+4] != data[1][i:i+4]]
                    differences.append(dict(ordinal=int(a["ordinal"]), stage=a["slot"], binding=field, shader=name,
                        variable=variable, variableSize=size, changedDwords=len(changed), offsets=changed[:24], uploads=hashes))
    return dict(differences=differences, incomplete=dict(unknown),
                limits="Reflection marks variables used by a shader, not which branches or matrix-array entries a specific draw reads.")


def raw_pair(first, second):
    if not first.exists() or not second.exists(): return dict(valid=False)
    arrays = []
    headers = []
    for path in (first, second):
        with path.open("rb") as f:
            header = f.read(16)
            if len(header) != 16: return dict(valid=False)
            width, height, fmt, samples = struct.unpack("<4I", header)
            if not width or not height or width > 8192 or height > 8192 or fmt not in (24, 28, 29, 35, 44, 45, 87, 91) or not samples or (fmt in (44,45) and samples != 1):
                return dict(valid=False)
            if path.stat().st_size != 16+width*height*4: return dict(valid=False)
            headers.append((width, height, fmt, samples))
            arrays.append(np.fromfile(f, dtype="<u4"))
    if headers[0] != headers[1]: return dict(valid=False)
    a, b = arrays
    depth = headers[0][2] in (44,45)
    channels = 1 if depth else 2 if headers[0][2] == 35 else 3
    bits = 24 if depth else 16 if channels == 2 else 10 if headers[0][2] == 24 else 8
    mask = (1 << bits)-1
    changed = np.zeros(len(a), dtype=bool)
    total = maximum = 0
    for shift in range(0, channels*bits, bits):
        delta = np.abs(((a >> shift) & mask).astype(np.int32)-((b >> shift) & mask).astype(np.int32))
        changed |= delta != 0
        total += int(delta.sum()); maximum = max(maximum, int(delta.max()))
    return dict(valid=True, size=list(headers[0][:2]), format=headers[0][2], sourceSamples=headers[0][3],
                equal=bool(np.array_equal(a, b)), rgbEqual=not bool(changed.any()),
                channels=channels, changedPixelFraction=float(changed.mean()), meanAbsoluteChannelError=total/(len(a)*channels),
                maximumChannelError=maximum, channelMaximum=mask,
                alphaChangedPixelFraction=float(((a >> (3*bits)) != (b >> (3*bits))).mean()) if channels == 3 else None,
                stencilChangedPixelFraction=float(((a >> 24) != (b >> 24)).mean()) if depth else None,
                precision="Source-format channels after MSAA resolve; not individual MSAA samples")


def compute_resource_pair(first, second):
    """Compare packed GPU readbacks without format conversion or tolerance."""
    if not first.exists() or not second.exists() or first.suffix != second.suffix:
        return dict(valid=False)
    data = []
    headers = []
    for path in (first, second):
        if not 0 < path.stat().st_size <= 64*1024*1024+24: return dict(valid=False)
        blob = path.read_bytes()
        if path.suffix == ".texture":
            if len(blob) < 24: return dict(valid=False)
            header = struct.unpack("<6I", blob[:24])
            w, h, fmt, layers, samples, bpp = header
            formats = {39:4, 40:4, 41:4, 42:4, 44:4, 28:4, 30:4, 35:4, 54:2, 57:2, 49:2, 61:1, 62:1}
            if not (0 < w <= 8192 and 0 < h <= 8192 and layers > 0 and samples == 1 and
                    formats.get(fmt) == bpp and len(blob) == 24+w*h*layers*bpp):
                return dict(valid=False)
            headers.append(header); blob = blob[24:]
        elif path.suffix != ".buffer": return dict(valid=False)
        data.append(np.frombuffer(blob, dtype=np.uint8))
    if len(data[0]) != len(data[1]) or (headers and headers[0] != headers[1]): return dict(valid=False)
    changed = int(np.count_nonzero(data[0] != data[1]))
    return dict(valid=True, equal=changed == 0, bytes=len(data[0]), changedBytes=changed,
                texture=list(headers[0]) if headers else None)


def compute_pairs(run, key, passes):
    rows = [[row for row in p if row.get("kind") == "Compute"] for p in passes]
    pairs = {}
    output_coverage = bool(rows[0] and len(rows[0]) == len(rows[1]) and len(rows[0]) <= 8)
    for n, (a, b) in enumerate(zip(*rows), 1):
        outputs = {field for row in (a, b) for field in row if field.startswith("uav") and field[3:].isdigit()}
        output_coverage &= bool(outputs) and all(a.get(field) == b.get(field) for field in outputs)
        # Older traces did not record UAV bindings. Still compare their files,
        # but do not certify complete output coverage from file presence alone.
        captured = {p.stem.split(f"-compute{n}-", 1)[1] for p in run.glob(f"work-{key}-pass*-compute{n}-*")}
        names = captured | outputs
        names |= {"cb"+str(slot) for slot in (0, 3, 7, 12) if any("c"+str(slot) in row for row in (a,b))}
        for name in sorted(names):
            paths = []
            for p in (0, 1):
                matches = list(run.glob(f"work-{key}-pass{p}-compute{n}-{name}.*"))
                paths.append(matches[0] if len(matches) == 1 else run / "missing-compute-resource")
            pairs[f"compute{n}-{name}"] = compute_resource_pair(*paths)
    outputs = {k:v for k,v in pairs.items() if k.rsplit("-",1)[-1].startswith("uav") and k.rsplit("-",1)[-1][3:].isdigit()}
    inputs = {k:v for k,v in pairs.items() if "-cb" in k}
    restored = compute_resource_pair(run / f"stage-{key}-scene-complete-ao.texture",
                                     run / f"stage-{key}-replay-restored-ao.texture")
    return dict(dispatchCounts=list(map(len,rows)), outputCoverage=output_coverage,
                outputsIdentical=bool(output_coverage and outputs and all(v.get("equal") for v in outputs.values())),
                capturedInputsIdentical=bool(inputs and all(v.get("equal") for v in inputs.values())),
                pairs=pairs, scratchRestored=restored,
                limits="Bound UAV outputs for at most eight dispatches and captured CB0/3/7/12 only; not all compute inputs or simulation state.")


def vpl_sort_pair(before, after):
    """Verify per-view sorting against native output, not against the other eye."""
    tiles = []
    headers = []
    for path in (before, after):
        if not path.exists(): return dict(valid=False, reason="missing_vpl_texture")
        with path.open("rb") as f:
            header = f.read(24)
            if len(header) != 24: return dict(valid=False, reason="truncated_header")
            w,h,fmt,layers,samples,bpp = struct.unpack("<6I",header)
            if not (0 < w <= 8192 and 0 < h <= 8192 and w%16 == 0 and
                    (fmt,layers,samples,bpp) == (57,1,1,2) and path.stat().st_size == 24+w*h*2):
                return dict(valid=False, reason="unsupported_layout")
            headers.append(header)
            # Pad only for analysis; synthetic cells are never list terminators.
            # The native texture may omit the lower rows of the final tile band.
            pixels = np.full(((h+31)//32*32,w),65535,dtype=np.uint16)
            pixels[:h,:] = np.fromfile(f,dtype="<u2").reshape(h,w)
            pixels = pixels.reshape((h+31)//32,32,w//16,16)
            tiles.append(pixels.transpose(0,2,1,3).reshape(-1,512))
    if headers[0] != headers[1]: return dict(valid=False, reason="layout_mismatch")
    a,b = tiles
    zeros = a == 0
    if not zeros.any(axis=1).all(): return dict(valid=False, reason="unterminated_tile")
    lengths = zeros.argmax(axis=1)
    active = np.arange(512)[None,:] < lengths[:,None]
    canonical_a = np.sort(np.where(active,a,65535),axis=1)
    canonical_b = np.sort(np.where(active,b,65535),axis=1)
    return dict(valid=True, tiles=len(a), nonemptyTiles=int(np.count_nonzero(lengths)),
                changedTiles=int(np.count_nonzero(np.any(a != b,axis=1))),
                membershipPreserved=bool(np.array_equal(canonical_a,canonical_b)),
                ordered=not bool(np.any((b != canonical_b) & active)),
                terminatorAndTailPreserved=not bool(np.any((a != b) & ~active)))


def surface_pair(first, second):
    data=[]; headers=[]
    formats={2:16,10:8,11:8,16:8,24:4,26:4,27:4,28:4,29:4,39:4,41:4,44:4,35:4,54:2,49:2,61:1}
    for path in (first,second):
        if not path.exists(): return dict(valid=False,reason="missing_surface")
        with path.open("rb") as f:
            head=f.read(24)
            if len(head)!=24: return dict(valid=False,reason="truncated_header")
            w,h,fmt,layers,mips,bpp=struct.unpack("<6I",head)
            if not (0<w<=8192 and 0<h<=8192 and layers>0 and 0<mips<=14 and formats.get(fmt)==bpp):
                return dict(valid=False,reason="unsupported_layout")
            size=sum(max(1,w>>m)*max(1,h>>m)*bpp*layers for m in range(mips))
            if size>64*1024*1024 or path.stat().st_size!=24+size: return dict(valid=False,reason="invalid_size")
            headers.append(head);data.append(np.fromfile(f,dtype=np.uint8))
    if headers[0]!=headers[1]: return dict(valid=False,reason="layout_mismatch")
    changed=int(np.count_nonzero(data[0]!=data[1]))
    return dict(valid=True,equal=changed==0,changedBytes=changed,bytes=len(data[0]),layout=list(struct.unpack("<6I",headers[0])))


def analyze(run):
    if (run / "capture-cleanup.json").exists():
        raise ValueError("Raw captures were retired; read retained-evidence.json for the historical result.")
    work_path = run / "replay-work.txt"
    if not work_path.exists(): return dict(run=run.name, valid=False, reason="missing_work")
    rows = [fields(line) for line in work_path.read_text().splitlines() if line.startswith("scene=")]
    passes = [[row for row in rows if row.get("pass") == str(p)] for p in (0, 1)]
    matching = bool(passes[0] and len(passes[0]) == len(passes[1]) and len({r.get("scene") for r in rows}) == 1 and
                    all(r.get("ordinal") == str(i+1) for p in passes for i, r in enumerate(p)))
    result = dict(run=run.name, valid=matching, rowCounts=list(map(len, passes)),
                  limits="Bounded recorded draw/compute state, mapped uploads and selected actual GPU readbacks. Newer traces add graphics stages, streams 0-3, samplers, predication and LOD; older traces retain their original coverage. Uncaptured bindings/contents, later resource mutations, individual MSAA samples, indirect commands and physics ticks are not certified.")
    if not matching: return result
    key = passes[0][0]["scene"]
    changes = []
    unknown = Counter()
    for a, b in zip(*passes):
        row_changes = {}
        for field in sorted(a.keys() | b.keys()):
            if field in ("pass", "scene"): continue
            left, right = a.get(field), b.get(field)
            if field[0] in "vpc" and field[1:].isdigit():
                if not left or not right or ":unknown:" in left or ":unknown:" in right:
                    unknown[field] += 1
                # Compare bytes, not allocator/ring-buffer pointer reuse.
                if left and right: left, right = left.split(":", 1)[1], right.split(":", 1)[1]
            if left != right: row_changes[field] = [left, right]
        if row_changes:
            changes.append(dict(ordinal=int(a["ordinal"]), slot=a["slot"], kind=a["kind"], changes=row_changes))
    result.update(changedRows=len(changes), changes=changes, unknownBufferPairs=dict(unknown))
    command_fields={"args", "kind", "slot", "vs", "ps", "rtv", "dsv", "vertex", "vertex1", "vertex2", "vertex3", "index", "depthstate", "raster", "blend", "gs", "hs", "ds", "layout", "topology", "viewport", "scissor", "predicate"}
    result["commandChanges"] = []
    for row in changes:
        relevant = ({"args", "kind", "slot", "cs"} | {k for k in row["changes"] if k.startswith(("sampler", "uav"))}
                    if row["kind"] == "Compute" else command_fields)
        changed = sorted(set(row["changes"]) & relevant)
        if changed: result["commandChanges"].append(dict(ordinal=row["ordinal"], fields=changed))
    result["compute"] = compute_pairs(run, key, passes)
    if (run / "sort-vpl.request").exists():
        result["vplOrder"] = [vpl_sort_pair(run / f"work-{key}-pass{p}-compute4-uav1-native.texture",
                                           run / f"work-{key}-pass{p}-compute4-uav1.texture") for p in (0,1)]
    surfaces={p.name.replace("pass1","pass0") for p in run.glob(f"work-{key}-pass*-*.surface")}
    if surfaces:
        result["dynamicTextures"]={name:surface_pair(run/name,run/name.replace("pass0","pass1")) for name in sorted(surfaces)}
    early={p.name.replace("pass1","pass0") for p in run.glob(f"work-{key}-pass*-slot0-*.texture") if "-repeat-" not in p.name}
    if early:
        result["beforeColor"]={name:compute_resource_pair(run/name,run/name.replace("pass0","pass1")) for name in sorted(early)}
    if (run / "trace.log").exists() and (run / "shaders.txt").exists(): result["usedConstants"] = used_constants(run, passes)
    lines = (run / "engine.log").read_text().splitlines()
    begin = next((i for i, line in enumerate(lines) if "replay begin " in line), None)
    end = next((i for i, line in enumerate(lines) if "replay end " in line), None)
    stages = {}
    if begin is not None and end is not None:
        for p, section in enumerate((lines[:begin], lines[begin:end])):
            for slot in (5, 6):
                hits = [fields(line)["id"] for line in section if "stage enter " in line and fields(line).get("slot") == str(slot)]
                if hits: stages[p, slot] = hits[-1]
    pairs = {}
    for slot in (5, 6):
        if (0, slot) in stages and (1, slot) in stages:
            pairs[f"slot{slot}"] = raw_pair(run / f"stage-{stages[0,slot]}-slot{slot}.raw", run / f"stage-{stages[1,slot]}-slot{slot}.raw")
            if slot == 5 and (run / f"stage-{stages[0,slot]}-slot5-depth.raw").exists():
                pairs["slot5Depth"] = raw_pair(run / f"stage-{stages[0,slot]}-slot5-depth.raw", run / f"stage-{stages[1,slot]}-slot5-depth.raw")
    if (0, 6) in stages:
        name = f"stage-{stages[0,6]}-slot6"
        pairs["readbackControl"] = raw_pair(run / (name+".raw"), run / (name+"-control.raw"))
    for n in (*range(1,33), 40, 48, 56, 64, 128, 200, 400, 600):
        if not (run / f"work-{key}-pass0-slot6-draw{n}.raw").exists(): continue
        pairs[f"slot6Draw{n}"] = raw_pair(run / f"work-{key}-pass0-slot6-draw{n}.raw", run / f"work-{key}-pass1-slot6-draw{n}.raw")
    pairs["complete"] = raw_pair(run / f"stage-{key}-scene-complete.raw", run / f"stage-{key}-replay-complete.raw")
    result["images"] = pairs
    node_rows = {fields(line)["phase"]: fields(line) for line in lines if "packet nodes " in line and fields(line).get("id") == key}
    phases = ("scene-complete", "replay-complete")
    paths = [run / f"stage-{key}-{phase}-nodes.bin" for phase in phases]
    valid_nodes = all(node_rows.get(phase, {}).get("complete") == "1" and node_rows[phase].get("saved") == "1" and
                      path.exists() and path.stat().st_size == int(node_rows[phase]["count"])*72 for phase, path in zip(phases, paths))
    result["packetNodes"] = dict(valid=valid_nodes, counts=[node_rows.get(phase, {}).get("count") for phase in phases],
                               equal=paths[0].read_bytes() == paths[1].read_bytes() if valid_nodes else None,
                               scope="Three flattened list chains, addresses plus first 64 bytes per node; pointed-to payloads excluded")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    (args.run / "replay-work-summary.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("changes", "usedConstants")}, indent=2))
    semantic = result.get("usedConstants", {})
    print("Used-variable coverage gaps:", semantic.get("incomplete"))
    model = [d for d in semantic.get("differences", []) if d["variable"] in ("model", "prevModel", "modelViewProj", "viewProjection")]
    print("Changed model/camera variables:", json.dumps(model[:12], indent=2), "total:", len(model))
