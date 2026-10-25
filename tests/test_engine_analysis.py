"""Incomplete diagnostics must not be reported as unchanged render state."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("engine_analysis", Path(__file__).parents[1] / "tools/analyze_engine_trace.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class StageEvidenceTests(unittest.TestCase):
    def test_replay_camera_message_is_not_a_native_camera_call(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory)
            (run/'engine-vtable-frames.csv').write_text('camera_uploads\n1\n')
            (run/'engine.log').write_text(
                'tid=7 frame=12 camera id=9 caller_rva=0x25cccd stereo=0 stereo_width=0 vendor_handle=0\n'
                'tid=7 frame=12 replay camera id=9 clip_changed=1\n')
            result=analysis.analyze(run)
            self.assertEqual(result['capturedCameraCalls'],1)
            self.assertEqual(result['callerCounts'],{'0x25cccd':1})

    def test_missing_truncated_and_changed_snapshots(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lines = [
                "tid=7 frame=12 main stage trace installed=1",
                "tid=7 frame=12 stage enter id=9 slot=6 self=ABCD valid=1 type=0x400 caller_rva=0x3be1b8",
            ]
            for phase in ("before", "after"):
                (run / f"stage-9-{phase}-view.bin").write_bytes(bytes(0x90))
                (run / f"stage-9-{phase}-context.bin").write_bytes(bytes(0x100))
            before = bytes(0x160)
            after = bytearray(before)
            after[0xc8] = 1
            (run / "stage-9-before-object.bin").write_bytes(before)
            (run / "stage-9-after-object.bin").write_bytes(after)
            result = analysis.analyze_stages(run, lines, [])
            sample = result["samples"][0]
            self.assertFalse(sample["matchedExit"])
            self.assertFalse(sample["graphicsValid"])
            self.assertEqual(sample["snapshots"]["object"]["changedDwords"], ["0xc8"])
            self.assertEqual(sample["snapshots"]["view"], dict(valid=True, changedDwords=[]))
            self.assertEqual(sample["snapshots"]["context"], dict(valid=False, changedDwords=None))
            (run / "stage-9-after-view.bin").unlink()
            self.assertFalse(analysis.analyze_stages(run, lines, [])["samples"][0]["snapshots"]["view"]["valid"])

    def test_cross_thread_or_reset_draw_counts_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            lines = ["tid=7 frame=12 stage enter id=9 slot=6 self=ABCD valid=1 type=0x400 caller_rva=0x3be1b8"]
            for tid, draws in ((8, 20), (7, 5), (7, 20)):
                (run / "trace.log").write_text(
                    "tid=7 stage boundary id=9 phase=before context=A draws=10 dispatches=2\n"
                    f"tid={tid} stage boundary id=9 phase=after context=A draws={draws} dispatches=2\n")
                sample = analysis.analyze_stages(run, lines, [])["samples"][0]
                self.assertEqual(sample["graphicsValid"], tid == 7 and draws >= 10)


if __name__ == "__main__":
    unittest.main()
