"""History observations require complete bytes and the same guarded object."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from analyze_scene_boundary import analyze


class HistoryEvidenceTests(unittest.TestCase):
    def test_missing_truncated_and_replaced_manager_are_not_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lines = ["tid=7 frame=12 scene candidate id=9 valid=1",
                     "tid=7 frame=12 scene complete id=9 stage6_seen=1 native_stereo=0"]
            for phase in ("entry", "complete"):
                lines.append(f"scene state id=9 phase=scene-{phase} manager=ABCD manager_valid=1 manager_saved=1")
                (run / f"stage-9-scene-{phase}-context.bin").write_bytes(bytes(0x200))
            (run / "engine.log").write_text("\n".join(lines))
            self.assertFalse(analyze(run)["samples"][0]["history"]["valid"])
            for phase in ("entry", "complete"):
                (run / f"stage-9-scene-{phase}-manager.bin").write_bytes(bytes(0x100))
            self.assertFalse(analyze(run)["samples"][0]["snapshots"]["manager"]["valid"])
            before = bytes(0x200)
            after = bytearray(before)
            struct.pack_into("<I", after, 0x94, 1)
            after[0x10] = 42
            context = bytearray(0x200)
            context[0x190] = 42
            (run / "stage-9-scene-entry-manager.bin").write_bytes(before)
            (run / "stage-9-scene-complete-manager.bin").write_bytes(after)
            (run / "stage-9-scene-complete-context.bin").write_bytes(context)
            sample = analyze(run)["samples"][0]
            self.assertEqual(sample["snapshots"]["manager"]["changedDwords"], ["0x10", "0x94"])
            self.assertEqual(sample["history"]["counter"], [0, 1])
            self.assertTrue(sample["history"]["slot0AfterMatchesCurrentViewProjection"])
            self.assertFalse(sample["history"]["slot1Changed"])
            # Identical files belonging to different objects cannot establish continuity.
            lines[-1] = lines[-1].replace("manager=ABCD", "manager=EF00")
            (run / "engine.log").write_text("\n".join(lines))
            sample = analyze(run)["samples"][0]
            self.assertFalse(sample["history"]["valid"])
            self.assertIsNone(sample["snapshots"]["manager"]["changedDwords"])
            lines[-1] = lines[-1].replace("manager=EF00", "manager=ABCD")
            lines[1] = lines[1].replace("tid=7", "tid=8")
            (run / "engine.log").write_text("\n".join(lines))
            self.assertFalse(analyze(run)["samples"][0]["history"]["valid"])
            lines.pop(1)
            (run / "engine.log").write_text("\n".join(lines))
            self.assertFalse(analyze(run)["samples"][0]["history"]["valid"])
            json.dumps(sample, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
