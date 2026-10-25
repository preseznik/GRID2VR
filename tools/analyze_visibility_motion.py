"""Bounded, independent geometry audit of captured native and tracked visibility."""
import argparse
from collections import defaultdict
import hashlib
from itertools import combinations
import json
import math
from pathlib import Path
import re

import numpy as np

MAX_FRAMES = 8
MAX_WITNESS = 4 * 1024 * 1024
MAX_ENGINE = 32 * 1024 * 1024
MAX_DRAWS = 64 * 1024 * 1024
MAX_LINE = 256 * 1024
TOLERANCE = .02
LIMITATIONS = (
    "CPU half-space coverage of the current two posed eye origins and four far-plane corners each. "
    "Native origins can lie behind the normal near plane; origin and far-corner results are separate. "
    "Far depth is the maximum original inward-plane distance at the visibility camera, matching the "
    "conservative source-depth convention, not a measured per-eye clipping distance. "
    "No triangle clipping, LOD, occlusion, shadow coverage, object visibility, rendering correctness, "
    "OpenXR submission, headset texture scaling, tracking or headset acceptance is established. "
    "Convex-corner volumes and broad-cube estimates describe geometry, not workload. "
    "Log restoration is observational evidence and does not prove worker synchronization."
)


def unavailable(reason):
    return dict(status="unavailable", reason=reason)


def uint(value):
    return type(value) is int and value >= 0


def read_bounded(path, limit):
    if not path.is_file():
        raise ValueError("missing:" + path.name)
    if path.stat().st_size > limit:
        raise ValueError("size_limit:" + path.name)
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("size_limit:" + path.name)
    return raw, dict(path=str(path.resolve()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def raw_floats(record, size):
    if (not isinstance(record, dict) or record.get("status") != "ok" or
            record.get("bytes") != size or not isinstance(record.get("hex"), str) or
            len(record["hex"]) != 2 * size):
        raise ValueError("raw_state_unavailable")
    try:
        data = bytes.fromhex(record["hex"])
    except ValueError as error:
        raise ValueError("invalid_raw_hex") from error
    if len(data) != size:
        raise ValueError("invalid_raw_length")
    return np.frombuffer(data, dtype="<f4").astype(np.float64)


def decode_volume(record):
    values = raw_floats(record, 224).reshape(14, 4)
    planes, corners = values[:6], values[6:, :3]  # Corner W is native scratch data.
    if (not np.isfinite(planes).all() or not np.isfinite(corners).all() or
            np.max(np.abs(corners)) > 1e6 or
            np.max(np.abs(np.linalg.norm(planes[:, :3], axis=1) - 1)) > .001):
        raise ValueError("invalid_volume")
    if (corners @ planes[:, :3].T + planes[:, 3]).min() < -1:
        raise ValueError("corners_outside_planes")
    return planes, corners


def decode_view(record):
    values = raw_floats(record, 144)
    basis = np.array([-values[8:11], values[4:7], -values[12:15]])
    origin = values[16:19]
    if (not np.isfinite(basis).all() or not np.isfinite(origin).all() or
            np.max(np.abs(origin)) > 100000 or
            np.linalg.norm(basis @ basis.T - np.eye(3), ord=np.inf) > .001 or
            np.linalg.det(basis) <= 0):
        raise ValueError("invalid_native_basis")
    return basis, origin


def quaternion_rotate(quaternion, vectors):
    """Hamilton quaternion vector rotation, independent of the C++ axis-matrix expansion."""
    q = np.asarray(quaternion, dtype=np.float64)
    if q.shape != (4,) or not np.isfinite(q).all() or abs(q @ q - 1) > .001:
        raise ValueError("invalid_quaternion")
    q = q / np.linalg.norm(q)
    vectors = np.asarray(vectors, dtype=np.float64)
    t = 2 * np.cross(q[:3], vectors)
    return vectors + q[3] * t + np.cross(q[:3], t)


def eye_points(view, pose, far):
    basis, native_origin = decode_view(view)
    q = np.array([pose[name] for name in ("qx", "qy", "qz", "qw")])
    translation = np.array([pose[name] for name in ("x", "y", "z")])
    frustum = np.array([pose[name] for name in ("left", "right", "down", "up")])
    if (not np.isfinite(translation).all() or translation @ translation > 4 or
            not np.isfinite(frustum).all() or np.max(np.abs(frustum)) > 4 or
            not (frustum[0] < 0 < frustum[1] and frustum[2] < 0 < frustum[3]) or
            not math.isfinite(far) or not 0 < far <= 25000):
        raise ValueError("invalid_eye_inputs")
    local = np.array([[x * far, y * far, -far]
                      for y in frustum[2:] for x in frustum[:2]])
    origin = native_origin + translation @ basis
    points = np.vstack((origin, origin + quaternion_rotate(q, local) @ basis))
    forward = quaternion_rotate(q, np.array([0., 0., -1.]))
    yaw = math.degrees(math.atan2(-forward[0], -forward[2]))
    return points, yaw


def coverage(planes, points):
    distances = points @ planes[:, :3].T + planes[:, 3]
    minimum = distances.min(axis=1)
    return dict(originInside=bool(minimum[0] >= -TOLERANCE),
                farCornersInside=[bool(x >= -TOLERANCE) for x in minimum[1:]],
                farCornersAllInside=bool((minimum[1:] >= -TOLERANCE).all()),
                allEndpointsInside=bool((minimum >= -TOLERANCE).all()),
                minOriginPlaneDistance=float(minimum[0]),
                minFarCornerPlaneDistance=float(minimum[1:].min()),
                endpointPlaneDistances=distances.tolist())


def convex_volume(corners):
    """Find supporting faces of eight points and sum centroid-to-face pyramids."""
    corners = np.asarray(corners, dtype=np.float64)
    if corners.shape != (8, 3) or not np.isfinite(corners).all():
        raise ValueError("invalid_convex_corners")
    extent = float(np.linalg.norm(np.ptp(corners, axis=0)))
    epsilon = max(1e-5, extent * 2e-6)
    faces = set()
    for indices in combinations(range(8), 3):
        triangle = corners[list(indices)]
        normal = np.cross(triangle[1] - triangle[0], triangle[2] - triangle[0])
        length = np.linalg.norm(normal)
        if length < epsilon * epsilon:
            continue
        normal /= length
        distances = (corners - triangle[0]) @ normal
        if distances.min() < -epsilon and distances.max() > epsilon:
            continue
        face = tuple(np.flatnonzero(np.abs(distances) <= epsilon).tolist())
        if 3 <= len(face) < 8:
            faces.add(face)
    center, volume = corners.mean(axis=0), 0.
    for face in faces:
        vertices = corners[list(face)]
        middle = vertices.mean(axis=0)
        axis = vertices[0] - middle
        axis /= np.linalg.norm(axis)
        _, _, vh = np.linalg.svd(vertices - middle)
        normal = vh[-1]
        other = np.cross(normal, axis)
        delta = vertices - middle
        vertices = vertices[np.argsort(np.arctan2(delta @ other, delta @ axis))]
        area = sum(np.linalg.norm(np.cross(vertices[i] - middle,
                                            vertices[(i + 1) % len(vertices)] - middle)) / 2
                   for i in range(len(vertices)))
        volume += area * abs((middle - center) @ normal) / 3
    if not faces or not math.isfinite(volume) or volume <= 0:
        raise ValueError("degenerate_convex_volume")
    return float(volume)


def old_cube_volume(original_planes, original_corners, visibility_origin, render_origin, poses):
    far = float(np.max(original_planes[:, :3] @ visibility_origin + original_planes[:, 3]))
    native_radius = float(np.linalg.norm(original_corners - visibility_origin, axis=1).max()) + 8
    max_ray = max(math.sqrt(1 + max(abs(p["left"]), abs(p["right"])) ** 2 +
                           max(abs(p["down"]), abs(p["up"])) ** 2) for p in poses)
    radius = max(native_radius, np.linalg.norm(render_origin - visibility_origin) +
                 (far * max_ray + 2) * math.sqrt(1.001) + 8)
    return dict(radius=float(radius), volume=float((2 * radius) ** 3),
                rounding="double estimate; production radius rounds outward to float")


def parse_witness(raw):
    frames, malformed, snapshots = defaultdict(list), 0, 0
    ended = None
    for ordinal, line in enumerate(raw.splitlines()):
        if len(line) > MAX_LINE:
            raise ValueError("witness_line_limit")
        try:
            row = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            malformed += 1
            continue
        if not isinstance(row, dict):
            malformed += 1
            continue
        if row.get("type") == "end":
            ended = row
        if row.get("type") != "snapshot":
            continue
        if (not uint(row.get("frame")) or not uint(row.get("scene")) or
                type(row.get("pass")) is not int or row["pass"] not in (0, 1, 2) or
                not isinstance(row.get("phase"), str)):
            malformed += 1
            continue
        snapshots += 1
        frames[row["frame"]].append(dict(row, ordinal=ordinal))
        if len(frames) > MAX_FRAMES or snapshots > 256:
            raise ValueError("witness_capture_limit")
    return frames, dict(malformedLines=malformed, snapshots=snapshots, end=ended)


PREFIX = re.compile(r"^\d+ tid=\d+ frame=(\d+) pass=(\d+) (.*)$")
FIELDS = re.compile(r"(\w+)=([^\s]+)")
POSE_NAMES = ("qx", "qy", "qz", "qw", "x", "y", "z", "left", "right", "down", "up")


def parse_engine(raws, wanted):
    frames = {frame: dict(frozen=[], eyes=[[], []], restores=[], completes=[]) for frame in wanted}
    for raw in raws:
        for line in raw.decode("utf-8", errors="replace").splitlines():
            if len(line) > MAX_LINE:
                raise ValueError("engine_line_limit")
            match = PREFIX.match(line)
            if not match:
                continue
            frame, game_pass, message = int(match[1]), int(match[2]), match[3]
            if frame not in frames or game_pass != 0:
                continue
            target, values = frames[frame], dict(FIELDS.findall(message))
            try:
                if message.startswith("views frozen "):
                    target["frozen"].append(dict(valid=int(values["valid"]), index=int(values["index"])))
                elif message.startswith("views input "):
                    eye = int(values["eye"])
                    if eye not in (0, 1):
                        raise ValueError("bad_eye")
                    target["eyes"][eye].append({key: float(values[key]) for key in POSE_NAMES})
                elif message.startswith("visibility restore "):
                    target["restores"].append(dict(id=int(values["id"]), sourceFrame=int(values["source_frame"]),
                                                   restored=int(values["restored"]), reason=values.get("reason")))
                elif message.startswith("views complete "):
                    target["completes"].append(dict(id=int(values["id"]), accepted=int(values["accepted"]),
                                                    visibilityRestored=int(values["visibility_restored"])))
            except (KeyError, ValueError, OverflowError):
                target["malformedRelevantRecord"] = True
    return frames


def resolve_poses(frame, before, engine):
    """Prefer capture-window frozen inputs; never silently bypass a malformed/conflicting witness."""
    if engine.get("malformedRelevantRecord"):
        raise ValueError("malformed_engine_views")
    inline = before.get("views")
    if "views" not in before:
        if (len(engine["frozen"]) != 1 or engine["frozen"][0]["valid"] != 1 or
                any(len(e) != 1 for e in engine["eyes"])):
            raise ValueError("missing_or_ambiguous_frozen_views")
        return [e[0] for e in engine["eyes"]], engine["frozen"][0]["index"], dict(
            poseSource="engine_log", enginePoseCrossCheck="not_applicable")
    if (not isinstance(inline, dict) or type(inline.get("frame")) is not int or
            inline["frame"] != frame or not isinstance(inline.get("eyes"), list) or
            len(inline["eyes"]) != 2):
        raise ValueError("invalid_or_stale_inline_views")
    poses = [None, None]
    for eye in inline["eyes"]:
        if (not isinstance(eye, dict) or type(eye.get("eye")) is not int or eye["eye"] not in (0, 1) or
                poses[eye["eye"]] is not None or any(type(eye.get(key)) not in (int, float) or
                                                   not math.isfinite(eye[key]) for key in POSE_NAMES)):
            raise ValueError("invalid_inline_eye")
        poses[eye["eye"]] = {key: float(eye[key]) for key in POSE_NAMES}
    if len(engine["frozen"]) > 1 or any(len(e) > 1 for e in engine["eyes"]):
        raise ValueError("ambiguous_engine_views")
    if engine["frozen"] and engine["frozen"][0]["valid"] != 1:
        raise ValueError("conflicting_engine_freeze")
    compared = []
    for index, rows in enumerate(engine["eyes"]):
        if rows:
            if any(not math.isclose(poses[index][key], rows[0][key], rel_tol=2e-8, abs_tol=2e-9)
                   for key in POSE_NAMES):
                raise ValueError("conflicting_inline_and_engine_views")
            compared.append(index)
    status = "complete" if len(compared) == 2 else "partial" if compared else "unavailable"
    index = engine["frozen"][0]["index"] if engine["frozen"] else None
    return poses, index, dict(poseSource="inline_snapshot", enginePoseCrossCheck=status,
                             engineEyesCompared=compared, engineFreezePresent=bool(engine["frozen"]))


def analyze_frame(frame, snapshots, engine):
    base = dict(frame=frame, phases=[dict(phase=r["phase"], gamePass=r["pass"], scene=r["scene"])
                                   for r in snapshots])
    for key, phase in (("original", "visibility-before"), ("applied", "visibility-applied")):
        candidates = [r for r in snapshots if r["phase"] == phase and r["pass"] == 0]
        if len(candidates) != 1:
            return dict(base, **unavailable("missing_or_ambiguous_" + phase))
        base[key + "Snapshot"] = candidates[0]
    before, applied = base.pop("originalSnapshot"), base.pop("appliedSnapshot")
    try:
        poses, index, pose_evidence = resolve_poses(frame, before, engine)
        original_planes, original_corners = decode_volume(before["main"]["visibility330"])
        applied_planes, applied_corners = decode_volume(applied["main"]["visibility330"])
        _, visibility_origin = decode_view(before["main"]["view5f0"])
        _, render_origin = decode_view(before["main"]["view680"])
        far = float(np.max(original_planes[:, :3] @ visibility_origin + original_planes[:, 3]))
        eyes = []
        for eye, pose in enumerate(poses):
            points, yaw = eye_points(before["main"]["view680"], pose, far)
            eyes.append(dict(eye=eye, pose=pose, yawDegrees=yaw, originAndFarCorners=points.tolist(),
                             original=coverage(original_planes, points),
                             applied=coverage(applied_planes, points)))
        native_volume, applied_volume = convex_volume(original_corners), convex_volume(applied_corners)
        cube = old_cube_volume(original_planes, original_corners, visibility_origin, render_origin, poses)
        source_corner_margins = (original_corners @ applied_planes[:, :3].T + applied_planes[:, 3]).min(axis=1)
    except (KeyError, ValueError, TypeError, np.linalg.LinAlgError) as error:
        return dict(base, **unavailable(str(error)))
    originals = before["main"]["visibility330"]["hex"]
    applied_hex = applied["main"]["visibility330"]["hex"]
    retained = []
    for row in snapshots:
        if row["phase"] not in ("visibility-return", "prepare-before", "prepare-after", "scene-entry", "scene-complete"):
            continue
        raw = row.get("main", {}).get("visibility330", {})
        retained.append(dict(phase=row["phase"], gamePass=row["pass"], scene=row["scene"],
                             available=raw.get("status") == "ok",
                             matchesApplied=raw.get("status") == "ok" and raw.get("hex") == applied_hex,
                             matchesOriginal=raw.get("status") == "ok" and raw.get("hex") == originals))
    return dict(base, **pose_evidence, status="analyzed", index=index, farDepthBound=far,
                visibilityOrigin=visibility_origin.tolist(), renderOrigin=render_origin.tolist(),
                originalPlanes=original_planes.tolist(), originalCorners=original_corners.tolist(),
                appliedPlanes=applied_planes.tolist(), appliedCorners=applied_corners.tolist(),
                originalEqualsApplied=originals == applied_hex, eyes=eyes,
                originalCornersRetained=bool((source_corner_margins >= -TOLERANCE).all()),
                originalCornerMinPlaneDistances=source_corner_margins.tolist(),
                originalConvexVolume=native_volume, appliedConvexVolume=applied_volume,
                appliedToOriginalVolumeRatio=applied_volume / native_volume,
                oldExpandedCube=cube, appliedToOldCubeVolumeRatio=applied_volume / cube["volume"],
                restoration=dict(appliedFollowsOriginal=before["ordinal"] < applied["ordinal"],
                                 observedPhases=retained, restoreLog=engine["restores"],
                                 viewsCompleteLog=engine["completes"]))


def attach_images(run, frames, receipts):
    path = run / "flicker-draws.jsonl"
    try:
        raw, receipt = read_bounded(path, MAX_DRAWS)
    except ValueError as error:
        return unavailable(str(error))
    receipts.append(receipt)
    completions, malformed = defaultdict(list), 0
    for line in raw.splitlines():
        # Skip large draw records without decoding them; only exact completion records matter.
        if not line.startswith(b'{"type":"complete",'):
            continue
        if len(line) > MAX_LINE:
            raise ValueError("complete_line_limit")
        try:
            row = json.loads(line)
            if uint(row.get("frame")) and uint(row.get("scene")):
                completions[row["frame"]].append(row)
            else:
                malformed += 1
        except ValueError:
            malformed += 1
    for item in frames:
        complete = completions.get(item["frame"], [])
        item["capture"] = unavailable("missing_or_ambiguous_completion")
        if len(complete) != 1:
            continue
        row = complete[0]
        scene = row["scene"]
        if row.get("accepted") not in (True, 1):
            item["capture"] = dict(status="rejected", scene=scene)
            continue
        images = []
        for eye in range(3):
            stem = f"stage-{scene}-flicker-eye{eye}"
            image = run / (stem + ".png")
            if not image.is_file():
                image = run / (stem + ".ppm")
            images.append(dict(eye=eye, path=str(image.resolve()), exists=image.is_file()))
        item["capture"] = dict(status="accepted", scene=scene, images=images)
    return dict(status="read", malformedCompletions=malformed)


def analyze(run):
    run = Path(run)
    result = dict(schema=1, run=str(run.resolve()), inputs=[], limits=LIMITATIONS,
                  coverageToleranceNativeUnits=TOLERANCE)
    try:
        raw, receipt = read_bounded(run / "sign-visibility.jsonl", MAX_WITNESS)
        result["inputs"].append(receipt)
        snapshots, result["witness"] = parse_witness(raw)
        engine_raws = []
        for name in ("engine.log.previous", "engine.log"):
            path = run / name
            if path.is_file():
                raw, receipt = read_bounded(path, MAX_ENGINE)
                result["inputs"].append(receipt)
                engine_raws.append(raw)
        engine = parse_engine(engine_raws, snapshots)
        result["frames"] = [analyze_frame(frame, rows, engine[frame]) for frame, rows in sorted(snapshots.items())]
        result["images"] = attach_images(run, result["frames"], result["inputs"])
        analyzed = [f for f in result["frames"] if f["status"] == "analyzed"]
        yaws = [eye["yawDegrees"] for frame in analyzed for eye in frame["eyes"]]
        result.update(status="analyzed" if analyzed else "unavailable", analyzedFrames=len(analyzed),
                      observedFrames=len(snapshots),
                      yawRangeDegrees=[min(yaws), max(yaws)] if yaws else None,
                      originalFarCoverageFailures=sum(not e["original"]["farCornersAllInside"] for f in analyzed for e in f["eyes"]),
                      appliedFarCoverageFailures=sum(not e["applied"]["farCornersAllInside"] for f in analyzed for e in f["eyes"]),
                      appliedEndpointCoverageFailures=sum(not e["applied"]["allEndpointsInside"] for f in analyzed for e in f["eyes"]))
    except (ValueError, OSError) as error:
        result.update(unavailable(str(error)))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    report = analyze(args.run)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({k: report[k] for k in ("status", "analyzedFrames", "observedFrames", "yawRangeDegrees")
                      if k in report}))
