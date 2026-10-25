import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_msaa import analyze
from analyze_ao_repeat import analyze as analyze_repeat
from analyze_shadow_repeat import analyze as analyze_shadow


class MsaaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name)
        self.lines = []
        for p in (0, 1):
            for phase in ("before", "after"):
                self.lines.append(f"0 tid=1 msaa_readback name=work-1-pass{p}-slot6-draw16 phase={phase} hr=00000000 restored=1 width=2 height=1 format=24 samples=2 quality=0")
                for sample in range(2):
                    path = self.run / f"work-1-pass{p}-slot6-draw16-{phase}-msaa{sample}.texture"
                    path.write_bytes(struct.pack("<8I", 2, 1, 42, 1, 1, 4, sample, sample))
        self.save()

    def save(self):
        (self.run / "trace.log").write_text("\n".join(self.lines))

    def test_exact_and_changed_sample(self):
        result = analyze(self.run)
        self.assertTrue(result["complete"])
        self.assertTrue(result["draws"][0]["phases"]["after"]["equal"])
        p = self.run / "work-1-pass1-slot6-draw16-after-msaa1.texture"
        p.write_bytes(struct.pack("<8I", 2, 1, 42, 1, 1, 4, 1, 2))
        result = analyze(self.run)
        self.assertTrue(result["complete"])
        self.assertTrue(result["draws"][0]["phases"]["before"]["equal"])
        self.assertFalse(result["draws"][0]["phases"]["after"]["equal"])
        self.assertEqual(result["draws"][0]["phases"]["after"]["samples"][1]["changedPixels"], 1)

    def test_missing_and_bad_restore(self):
        (self.run / "work-1-pass1-slot6-draw16-before-msaa0.texture").unlink()
        self.assertFalse(analyze(self.run)["complete"])
        self.lines[-1] = self.lines[-1].replace("restored=1", "restored=0")
        self.save()
        self.assertFalse(analyze(self.run)["draws"][0]["coverage"])

    def test_duplicates_and_pruned(self):
        self.lines.append(self.lines[0]); self.save()
        with self.assertRaises(ValueError): analyze(self.run)
        (self.run / "capture-cleanup.json").write_text(json.dumps(dict(status="complete")))
        with self.assertRaises(ValueError): analyze(self.run)

    def test_ao_repeat_requires_real_restore(self):
        name = "work-1-pass0-slot0-draw1"
        (self.run / "trace.log").write_text(f"0 tid=1 ao_repeat name={name} repeated=1 restore_requested=1 injected_draws=1\n")
        data = struct.pack("<6I", 2, 1, 49, 1, 1, 2) + bytes(4)
        for phase in ("original", "control", "restored"):
            (self.run / f"{name}-repeat-{phase}.texture").write_bytes(data)
        result = analyze_repeat(self.run)
        self.assertTrue(result["complete"] and result["restored"] and result["repeatedIdentically"])
        (self.run / f"{name}-repeat-control.texture").write_bytes(data[:-1] + b"\1")
        result = analyze_repeat(self.run)
        self.assertTrue(result["restored"])
        self.assertFalse(result["repeatedIdentically"])
        (self.run / f"{name}-repeat-restored.texture").unlink()
        result = analyze_repeat(self.run)
        self.assertFalse(result["complete"])
        self.assertFalse(result["restored"])
        (self.run / "capture-cleanup.json").write_text(json.dumps(dict(status="complete")))
        with self.assertRaises(ValueError): analyze_repeat(self.run)

    def test_shadow_repeat_does_not_accept_ao_or_refusal(self):
        name = "work-1-pass0-slot0-draw8"
        data = struct.pack("<6I", 2, 1, 28, 1, 1, 4) + bytes(8)
        for phase in ("original", "control", "restored"):
            (self.run / f"{name}-repeat-{phase}.texture").write_bytes(data)
        log = self.run / "trace.log"
        log.write_text(f"0 ao_repeat name={name} repeated=1 restore_requested=1 injected_draws=1\n")
        self.assertFalse(analyze_shadow(self.run)["complete"])
        log.write_text(f"0 shadow_repeat name={name} refused=pipeline\n")
        self.assertFalse(analyze_shadow(self.run)["complete"])
        log.write_text(f"0 shadow_repeat name={name} repeated=1 restore_requested=1 injected_draws=1\n")
        self.assertTrue(analyze_shadow(self.run)["repeatedIdentically"])
        (self.run / f"{name}-repeat-restored.texture").write_bytes(data[:-1] + b"\1")
        self.assertFalse(analyze_shadow(self.run)["restored"])

    def test_shadow_binding_control_requires_paired_draws_and_binding_restore(self):
        (self.run / "normalize-shadow-bindings.request").touch()
        data = struct.pack("<6I", 2, 1, 28, 1, 1, 4) + bytes(8)
        lines = []
        for p in (0, 1):
            name = f"work-1-pass{p}-slot0-draw8"
            lines.append(f"0 shadow_repeat name={name} repeated=1 restore_requested=1 injected_draws=1 binding_control=1 native_applied=1 normalized_restored=1")
            for phase in ("original", "control", "restored"):
                (self.run / f"{name}-repeat-{phase}.texture").write_bytes(data)
        log = self.run / "trace.log"; log.write_text("\n".join(lines))
        result = analyze_shadow(self.run)
        self.assertTrue(result["complete"] and result["restored"])
        (self.run / "work-1-pass1-slot0-draw8-repeat-control.texture").write_bytes(data[:-1] + b"\1")
        result = analyze_shadow(self.run)
        self.assertTrue(result["normalizedPair"]["equal"] and result["restored"])
        self.assertFalse(result["nativePair"]["equal"])
        self.assertFalse(result["passes"][1]["repeatedIdentically"])
        # Individually valid/restored passes must not certify an incompatible pair.
        different_size = struct.pack("<6I", 3, 1, 28, 1, 1, 4) + bytes(12)
        for phase in ("original", "control", "restored"):
            (self.run / f"work-1-pass1-slot0-draw8-repeat-{phase}.texture").write_bytes(different_size)
        result = analyze_shadow(self.run)
        self.assertTrue(all(p["complete"] and p["restored"] for p in result["passes"]))
        self.assertFalse(result["complete"])
        self.assertFalse(result["restored"])
        for phase in ("original", "control", "restored"):
            (self.run / f"work-1-pass1-slot0-draw8-repeat-{phase}.texture").write_bytes(data)
        log.write_text("\n".join(lines).replace("normalized_restored=1", "normalized_restored=0"))
        self.assertFalse(analyze_shadow(self.run)["restored"])
        log.write_text(lines[0])
        self.assertFalse(analyze_shadow(self.run)["complete"])
        log.write_text("\n".join(lines).replace("work-1-pass1", "work-2-pass1"))
        self.assertFalse(analyze_shadow(self.run)["complete"])
        log.unlink()
        self.assertFalse(analyze_shadow(self.run)["complete"])
        self.assertFalse(analyze_repeat(self.run)["complete"])


if __name__ == "__main__": unittest.main()
