import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_producer_inputs import analyze


class ProducerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.run = Path(self.temp.name)
        self.rows = []
        for render_pass in range(2):
            for slot in ("ps0", "ps1", "ps2", "target-before"):
                self.rows.append(dict(mark=f"work-1-pass{render_pass}-slot0-draw1", slot=slot,
                                      shader="e8bc75f0f617aa9e", view=None if slot == "target-before" else dict(dimension=4, format=61, firstMip=0, mips=1),
                                      texture=dict(status="ok", mips=1, array=1, samples=1, bytes=1,
                                                   subresources=[dict(mip=0, slice=0, rowBytes=1, rows=1, sha256="a"*64)])))

    def result(self):
        (self.run / "producer-inputs.jsonl").write_text("\n".join(json.dumps(r) for r in self.rows))
        return analyze(self.run)

    def test_match_and_target_change(self):
        self.assertTrue(self.result()["capturedInputsEqual"])
        self.rows[-1]["texture"]["subresources"][0]["sha256"] = "b"*64
        result = self.result()
        self.assertTrue(result["capturedInputsEqual"])
        self.assertFalse(result["draws"][0]["targetBeforeEqual"])

    def test_changed_input_or_view(self):
        self.rows[-2]["texture"]["subresources"][0]["sha256"] = "b"*64
        self.assertFalse(self.result()["capturedInputsEqual"])
        self.rows[-2]["texture"]["subresources"][0]["sha256"] = "a"*64
        self.rows[-2]["view"]["firstMip"] = 1
        self.assertFalse(self.result()["capturedInputsEqual"])

    def test_missing_refused_partial_or_duplicate(self):
        original = copy.deepcopy(self.rows)
        for change in ("missing", "budget", "partial", "unknown_view"):
            self.rows = copy.deepcopy(original)
            if change == "missing": del self.rows[-2]
            elif change == "budget": self.rows[-2]["texture"]["status"] = "budget"
            elif change == "partial": self.rows[-2]["texture"]["mips"] = 2
            else: self.rows[-2]["view"]["dimension"] = 5
            self.assertFalse(self.result()["capturedInputsEqual"])
        self.rows = original + [original[0]]
        with self.assertRaises(ValueError): self.result()

    def test_empty(self):
        self.rows = []
        self.assertFalse(self.result()["capturedInputsEqual"])

    def test_three_pass_coverage_requires_both_eyes(self):
        right=copy.deepcopy(self.rows[:4])
        for row in right:row['mark']=row['mark'].replace('pass0','pass2')
        self.rows+=right
        with self.assertRaises(ValueError):self.result()
        (self.run/'stereo-eyes.request').touch()
        self.assertTrue(self.result()['capturedInputsEqual'])
        self.assertEqual(self.result()['renderPasses'],3)
        self.rows[-2]['texture']['subresources'][0]['sha256']='b'*64
        self.assertFalse(self.result()['capturedInputsEqual'])
        self.rows=self.rows[:-4]
        self.assertFalse(self.result()['capturedInputsEqual'])


if __name__ == "__main__":
    unittest.main()
