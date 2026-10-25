import copy
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_visibility_motion import (analyze, analyze_frame, convex_volume, coverage, decode_volume,
                                       eye_points, parse_engine, parse_witness, quaternion_rotate)


def raw(values):
    data = struct.pack("<" + "f" * len(values), *values)
    return dict(status="ok", bytes=len(data), hex=data.hex())


def view():
    values = np.zeros(36)
    values[4:7], values[8:11], values[12:15] = [0, 1, 0], [-1, 0, 0], [0, 0, -1]
    return raw(values)


def box(low, high):
    planes = []
    for axis in range(3):
        for direction, distance in ((1, -low[axis]), (-1, high[axis])):
            plane = [0., 0., 0., distance]
            plane[axis] = direction
            planes.append(plane)
    corners = [[x, y, z, float("nan")] for x in (low[0], high[0])
               for y in (low[1], high[1]) for z in (low[2], high[2])]
    return raw(np.array(planes + corners).flatten())


def pose(yaw=0):
    angle = math.radians(yaw) / 2
    return dict(qx=0, qy=math.sin(angle), qz=0, qw=math.cos(angle), x=-.032, y=0, z=0,
                left=-1, right=1, down=-1, up=1)


def fixtures():
    original = box([-100, -100, -100], [100, 100, -.2])
    pose_inputs = [pose(90), pose(90)]
    points = np.vstack([eye_points(view(), p, 100)[0] for p in pose_inputs])
    applied = box(points.min(axis=0) - 8, points.max(axis=0) + 8)
    snapshots = [dict(frame=7, scene=12, **{"pass": 0}, phase=phase, ordinal=i,
                      main=dict(visibility330=volume, view5f0=view(), view680=view()))
                 for i, (phase, volume) in enumerate([("visibility-before", original),
                                                    ("visibility-applied", applied),
                                                    ("prepare-before", applied)])]
    engine = dict(frozen=[dict(valid=1, index=2)], eyes=[[p] for p in pose_inputs],
                  restores=[dict(id=12, sourceFrame=7, restored=1, reason="scene_complete")],
                  completes=[dict(id=20, accepted=1, visibilityRestored=1)])
    return snapshots, engine


class VisibilityMotion(unittest.TestCase):
    def test_hamilton_rotation_has_native_right_up_back_convention(self):
        result = quaternion_rotate([0, math.sqrt(.5), 0, math.sqrt(.5)], [[0, 0, -1], [1, 0, 0]])
        np.testing.assert_allclose(result, [[-1, 0, 0], [0, 0, -1]], atol=1e-12)

    def test_rotated_views_escape_native_and_tight_box_contains_both(self):
        snapshots, engine = fixtures()
        result = analyze_frame(7, snapshots, engine)
        self.assertEqual(result["status"], "analyzed")
        self.assertTrue(all(not e["original"]["farCornersAllInside"] for e in result["eyes"]))
        self.assertTrue(all(e["applied"]["allEndpointsInside"] for e in result["eyes"]))
        self.assertAlmostEqual(result["eyes"][0]["yawDegrees"], 90)
        self.assertLess(result["appliedToOldCubeVolumeRatio"], .3)
        self.assertTrue(result["restoration"]["observedPhases"][0]["matchesApplied"])

    def test_real_signed_native_basis_is_used_for_translation_and_rays(self):
        values = np.zeros(36)
        # Native camera faces world +X. Local right is +Z, up +Y, back -X.
        values[4:7], values[8:11], values[12:15], values[16:19] = [0, 1, 0], [0, 0, -1], [1, 0, 0], [5, 6, 7]
        p = pose(0)
        p.update(x=1, y=.5, z=-.25)
        points, _ = eye_points(raw(values), p, 10)
        np.testing.assert_allclose(points[0], [5.25, 6.5, 8])
        np.testing.assert_allclose(points[1], [15.25, -3.5, -2])

    def test_corner_scratch_w_is_ignored(self):
        planes, corners = decode_volume(box([-2, -3, -4], [2, 3, 4]))
        self.assertAlmostEqual(convex_volume(corners), 8 * 2 * 3 * 4)
        self.assertTrue(coverage(planes, np.zeros((5, 3)))["allEndpointsInside"])

    def test_applied_union_retains_native_corners_independently(self):
        snapshots, engine = fixtures()
        native_planes, corners = decode_volume(snapshots[0]["main"]["visibility330"])
        points = np.vstack([eye_points(view(), e[0], 100)[0] for e in engine["eyes"]])
        union = np.vstack((corners, points))
        snapshots[1]["main"]["visibility330"] = box(union.min(axis=0) - 8, union.max(axis=0) + 8)
        self.assertTrue(analyze_frame(7, snapshots, engine)["originalCornersRetained"])
        snapshots[1]["main"]["visibility330"] = box([200, 200, 200], [300, 300, 300])
        self.assertFalse(analyze_frame(7, snapshots, engine)["originalCornersRetained"])

    def test_convex_volume_is_rotation_invariant(self):
        _, corners = decode_volume(box([-2, -3, -4], [2, 3, 4]))
        q = [0, math.sin(.3), 0, math.cos(.3)]
        self.assertAlmostEqual(convex_volume(quaternion_rotate(q, corners) + [123, 456, -12]), 192)

    def test_invalid_inputs_fail_closed(self):
        for change in (dict(qw=0), dict(x=3), dict(left=1), dict(up=float("nan"))):
            p = pose()
            p.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                eye_points(view(), p, 100)
        invalid = view()
        invalid["status"] = "unreadable"
        with self.assertRaises(ValueError):
            eye_points(invalid, pose(), 100)

    def test_missing_duplicate_or_conflicting_views_are_unavailable(self):
        snapshots, engine = fixtures()
        for value in ([], [pose(), pose()]):
            changed = copy.deepcopy(engine)
            changed["eyes"][0] = value
            self.assertEqual(analyze_frame(7, snapshots, changed)["status"], "unavailable")
        self.assertEqual(analyze_frame(7, snapshots + [snapshots[0]], engine)["status"], "unavailable")

    def test_inline_views_survive_missing_ring_logs_and_cross_check_available_eyes(self):
        snapshots, engine = fixtures()
        snapshots[0]["views"] = dict(frame=7, eyes=[dict(e[0], eye=i) for i, e in enumerate(engine["eyes"])])
        result = analyze_frame(7, snapshots, engine)
        self.assertEqual(result["poseSource"], "inline_snapshot")
        self.assertEqual(result["enginePoseCrossCheck"], "complete")
        engine["frozen"], engine["eyes"] = [], [[], []]
        result = analyze_frame(7, snapshots, engine)
        self.assertEqual(result["status"], "analyzed")
        self.assertEqual(result["enginePoseCrossCheck"], "unavailable")
        self.assertIsNone(result["index"])
        engine["eyes"][1] = [pose(90)]
        self.assertEqual(analyze_frame(7, snapshots, engine)["enginePoseCrossCheck"], "partial")

    def test_invalid_stale_or_conflicting_inline_views_fail_closed(self):
        snapshots, engine = fixtures()
        snapshots[0]["views"] = dict(frame=7, eyes=[dict(e[0], eye=i) for i, e in enumerate(engine["eyes"])])
        for mutation in ("stale", "missing", "duplicate", "nan", "bad_quaternion", "conflict"):
            changed = copy.deepcopy(snapshots)
            inline = changed[0]["views"]
            if mutation == "stale":
                inline["frame"] = 6
            elif mutation == "missing":
                inline["eyes"].pop()
            elif mutation == "duplicate":
                inline["eyes"][1]["eye"] = 0
            elif mutation == "nan":
                inline["eyes"][0]["x"] = float("nan")
            elif mutation == "bad_quaternion":
                inline["eyes"][0]["qw"] = 0
            elif mutation == "conflict":
                inline["eyes"][0]["x"] = .5
            with self.subTest(mutation=mutation):
                self.assertEqual(analyze_frame(7, changed, engine)["status"], "unavailable")

    def test_frames_join_exactly_across_engine_rotation(self):
        one = b"10 tid=2 frame=7 pass=0 views frozen valid=1 index=3\n"
        two = ("11 tid=2 frame=7 pass=0 views input eye=0 " +
               " ".join(f"{k}={v}" for k, v in pose(70).items()) + "\n" +
               "11 tid=2 frame=8 pass=0 views input eye=1 " +
               " ".join(f"{k}={v}" for k, v in pose().items())).encode()
        parsed = parse_engine([one, two], [7])
        self.assertEqual(parsed[7]["frozen"][0]["index"], 3)
        self.assertEqual(len(parsed[7]["eyes"][0]), 1)
        self.assertEqual(parsed[7]["eyes"][1], [])

    def test_witness_frame_limit_is_enforced(self):
        rows = [dict(type="snapshot", frame=i, scene=i, **{"pass": 0}, phase="visibility-before") for i in range(9)]
        with self.assertRaisesRegex(ValueError, "capture_limit"):
            parse_witness("\n".join(json.dumps(r) for r in rows).encode())

    def test_file_report_is_bounded_and_links_only_accepted_capture(self):
        snapshots, engine = fixtures()
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "sign-visibility.jsonl").write_text("\n".join(json.dumps(dict(r, type="snapshot")) for r in snapshots))
            lines = ["1 tid=2 frame=7 pass=0 views frozen valid=1 index=2"]
            for i in range(2):
                lines.append(f"2 tid=2 frame=7 pass=0 views input eye={i} " +
                             " ".join(f"{k}={v}" for k, v in engine["eyes"][i][0].items()))
            (path / "engine.log").write_text("\n".join(lines))
            (path / "flicker-draws.jsonl").write_text('{"type":"complete","frame":7,"scene":20,"accepted":1}\n')
            report = analyze(path)
            self.assertEqual(report["analyzedFrames"], 1)
            self.assertEqual(report["appliedEndpointCoverageFailures"], 0)
            self.assertEqual(report["frames"][0]["capture"]["scene"], 20)
            self.assertFalse(report["frames"][0]["capture"]["images"][0]["exists"])
            self.assertEqual(len(report["inputs"]), 3)
            json.dumps(report, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
