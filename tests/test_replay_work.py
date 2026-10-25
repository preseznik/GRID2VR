from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from analyze_replay_work import raw_pair, compute_resource_pair, compute_pairs, vpl_sort_pair, surface_pair, analyze, used_constants


class ReplayWorkTests(unittest.TestCase):
    def test_pruned_capture_refuses_fresh_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); (run/"capture-cleanup.json").write_text('{}')
            with self.assertRaisesRegex(ValueError, "retained-evidence.json"):
                analyze(run)

    def test_surface_mip_and_array_layout(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.surface"; b=Path(folder)/"b.surface"
            header=struct.pack("<6I",4,4,28,6,3,4)
            size=(4*4+2*2+1)*4*6
            a.write_bytes(header+bytes(size)); b.write_bytes(a.read_bytes())
            self.assertTrue(surface_pair(a,b)["equal"])
            b.write_bytes(header+bytes(size-1)+b"\1")
            self.assertEqual(surface_pair(a,b)["changedBytes"],1)
            b.write_bytes(header+bytes(size-1))
            self.assertFalse(surface_pair(a,b)["valid"])
            b.write_bytes(struct.pack("<6I",4,4,24,6,3,4)+bytes(size))
            self.assertFalse(surface_pair(a,b)["valid"])

    def test_vpl_order_rejects_changed_membership_and_tail(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.texture"; b=Path(folder)/"b.texture"
            header=struct.pack("<6I",16,32,57,1,1,2)
            original=[9,2,9,1,0]+[17]*507
            ordered=[1,2,9,9,0]+[17]*507
            a.write_bytes(header+struct.pack("<512H",*original))
            b.write_bytes(header+struct.pack("<512H",*ordered))
            result=vpl_sort_pair(a,b)
            self.assertTrue(all(result[k] for k in ("valid","membershipPreserved","ordered","terminatorAndTailPreserved")))
            b.write_bytes(a.read_bytes())
            self.assertFalse(vpl_sort_pair(a,b)["ordered"])
            ordered[0]=2
            b.write_bytes(header+struct.pack("<512H",*ordered))
            self.assertFalse(vpl_sort_pair(a,b)["membershipPreserved"])
            ordered[0]=1; ordered[5]=18
            b.write_bytes(header+struct.pack("<512H",*ordered))
            self.assertFalse(vpl_sort_pair(a,b)["terminatorAndTailPreserved"])
            a.write_bytes(header+struct.pack("<512H",*([1]*512)))
            self.assertFalse(vpl_sort_pair(a,b)["valid"])

    def test_compute_array_bytes_and_metadata_are_required(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.texture"; b=Path(folder)/"b.texture"
            header=struct.pack("<6I",2,1,57,3,1,2)
            a.write_bytes(header+bytes(12)); b.write_bytes(header+bytes(11)+b"\1")
            result=compute_resource_pair(a,b)
            self.assertTrue(result["valid"])
            self.assertFalse(result["equal"])
            self.assertEqual(result["changedBytes"],1)
            for invalid in (header+bytes(11), struct.pack("<6I",2,1,57,3,1,4)+bytes(24),
                            struct.pack("<6I",2,1,57,3,8,2)+bytes(12),
                            struct.pack("<6I",1,2,57,3,1,2)+bytes(12)):
                b.write_bytes(invalid)
                self.assertFalse(compute_resource_pair(a,b)["valid"])

    def test_vpl_partial_row_preserves_real_tail_and_requires_real_terminator(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.texture"; b=Path(folder)/"b.texture"
            for height in (1,16,33,48):
                header=struct.pack("<6I",16,height,57,1,1,2)
                original=[17]*(16*height); ordered=original.copy()
                for start in range(0,len(original),512):
                    original[start:start+5]=[9,65535,2,9,0]
                    ordered[start:start+5]=[2,9,9,65535,0]
                def save(path,values): path.write_bytes(header+struct.pack(f"<{len(values)}H",*values))
                save(a,original); save(b,ordered)
                result=vpl_sort_pair(a,b)
                self.assertTrue(all(result[k] for k in ("valid","membershipPreserved","ordered","terminatorAndTailPreserved")))
                ordered[-1]=18; save(b,ordered)
                self.assertFalse(vpl_sort_pair(a,b)["terminatorAndTailPreserved"])
                original[-(height%32 or 32)*16:]=[17]*((height%32 or 32)*16)
                save(a,original)
                self.assertEqual(vpl_sort_pair(a,b)["reason"],"unterminated_tile")

    def test_missing_compute_output_cannot_pass(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder)
            rows=[[dict(kind="Compute",uav0="V:R",uav1="V2:R2",c12="B:unknown:0")] for _ in range(2)]
            texture=struct.pack("<6I",1,1,49,1,1,2)+bytes(2)
            for p in (0,1):
                (run/f"work-9-pass{p}-compute1-uav0.texture").write_bytes(texture)
                (run/f"work-9-pass{p}-compute1-cb12.buffer").write_bytes(bytes(16))
            self.assertFalse(compute_pairs(run,"9",rows)["outputsIdentical"])
            for p in (0,1): (run/f"work-9-pass{p}-compute1-uav1.texture").write_bytes(texture)
            result=compute_pairs(run,"9",rows)
            self.assertTrue(result["outputsIdentical"])
            self.assertTrue(result["capturedInputsIdentical"])
            self.assertFalse(result["scratchRestored"]["valid"])
            # Files alone cannot prove all bound outputs were captured.
            self.assertFalse(compute_pairs(run,"9",[[dict(kind="Compute")]]*2)["outputsIdentical"])
            (run/"work-9-pass1-compute1-cb12.buffer").unlink()
            self.assertFalse(compute_pairs(run,"9",rows)["capturedInputsIdentical"])

    def test_depth_and_stencil_are_separate(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.raw"; b=Path(folder)/"b.raw"
            header=struct.pack("<4I",1,1,44,1)
            a.write_bytes(header+struct.pack("<I",500))
            b.write_bytes(header+struct.pack("<I",500+(1<<24)))
            result=raw_pair(a,b)
            self.assertTrue(result["rgbEqual"])
            self.assertFalse(result["equal"])
            self.assertEqual(result["stencilChangedPixelFraction"],1)
            self.assertEqual(result["channelMaximum"],16777215)
            b.write_bytes(struct.pack("<4I",1,1,44,8)+bytes(4))
            self.assertFalse(raw_pair(a,b)["valid"])

    def test_two_channel_unorm_precision(self):
        with tempfile.TemporaryDirectory() as folder:
            a=Path(folder)/"a.raw"; b=Path(folder)/"b.raw"
            header=struct.pack("<4I",1,1,35,1)
            a.write_bytes(header+struct.pack("<2H",1000,65000))
            b.write_bytes(header+struct.pack("<2H",1001,64998))
            result=raw_pair(a,b)
            self.assertTrue(result["valid"])
            self.assertEqual(result["maximumChannelError"],2)
            self.assertEqual(result["meanAbsoluteChannelError"],1.5)
            self.assertEqual(result["channelMaximum"],65535)
            self.assertIsNone(result["alphaChangedPixelFraction"])
            b.write_bytes(a.read_bytes())
            self.assertTrue(raw_pair(a,b)["equal"])

    def test_used_variables_exclude_padding_and_report_missing_uploads(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder)
            (run/"trace.log").write_text("shader object=VS stage=vs hash=abc\nshader object=PS stage=ps hash=def")
            (run/"shaders.txt").write_text("vs-abc bytes=12\n  Globals slot=0 bytes=128\n    model offset=0 size=64 used=1\n    unused offset=64 size=64 used=0\nps-def bytes=12\n  Structured slot=20 bytes=32\n    Element offset=0 size=32 used=1")
            a=dict(kind="Draw",vs="VS",ps="PS",v0="ptr:aaa:1",ordinal="1",slot="6")
            b=dict(a,v0="ptr:bbb:1")
            (run/"upload-aaa.bin").write_bytes(bytes(128))
            (run/"upload-bbb.bin").write_bytes(bytes(64)+b"\1"+bytes(63))
            result=used_constants(run,[[a],[b]])
            self.assertEqual(result["differences"],[])
            self.assertEqual(result["incomplete"],{"nonConstantResourceBindings":1})
            (run/"upload-bbb.bin").write_bytes(b"\1"+bytes(127))
            self.assertEqual(used_constants(run,[[a],[b]])["differences"][0]["variable"],"model")
            (run/"upload-bbb.bin").unlink()
            self.assertEqual(used_constants(run,[[a],[b]])["incomplete"]["missingUploadFile"],1)

    def test_native_precision_and_invalid_data(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder); a=run/"a.raw"; b=run/"b.raw"
            self.assertFalse(raw_pair(a,b)["valid"])
            header=struct.pack("<4I",2,1,24,8)
            a.write_bytes(header+struct.pack("<2I",1,0))
            b.write_bytes(header+struct.pack("<2I",2,0))
            result=raw_pair(a,b)
            # These red values both quantize to zero in an 8-bit preview.
            self.assertFalse(result["rgbEqual"])
            self.assertEqual(result["maximumChannelError"],1)
            self.assertEqual(result["changedPixelFraction"],.5)
            b.write_bytes(a.read_bytes())
            self.assertTrue(raw_pair(a,b)["equal"])
            b.write_bytes(header+struct.pack("<2I",1+(1<<30),0))
            self.assertTrue(raw_pair(a,b)["rgbEqual"])
            self.assertFalse(raw_pair(a,b)["equal"])
            for data in (b"",header,struct.pack("<4I",1,2,24,8)+bytes(8),struct.pack("<4I",2,1,0,1)+bytes(8)):
                b.write_bytes(data); self.assertFalse(raw_pair(a,b)["valid"])
            self.assertFalse(analyze(run)["valid"])
            (run/"replay-work.txt").write_text("scene=9 pass=0 ordinal=1 slot=6 kind=Draw\nscene=9 pass=1 ordinal=2 slot=6 kind=Draw")
            self.assertFalse(analyze(run)["valid"])


if __name__ == "__main__": unittest.main()
