import struct
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from unpack_snapshots import unpack,retire_expanded
import json
import os
from unittest.mock import patch

def record(name,data):
    name=name.encode('ascii');return struct.pack('<HH',len(name),len(data))+name+data

class SnapshotTests(unittest.TestCase):
    def test_installed_grouped_run_retirement(self):
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ, {'GRID2VR_DATA_ROOT': folder}):
            run=Path(folder)/'logs/20261026-212549-730';run.mkdir(parents=True)
            target=run/'stage-1-x.bin'
            (run/'cpu-snapshots.bin').write_bytes(b'G2VRSNP1'+record(target.name,b'exact'))
            unpack(run)
            (run/'capture-cleanup.json').write_text(json.dumps(dict(status='complete')))
            self.assertEqual(retire_expanded(run)['removedThisPass'],1)
            self.assertFalse(target.exists())
            self.assertTrue((run/'cpu-snapshots.bin').exists())
            for outside in (Path(folder)/'logs/not-a-run',run/'nested'):
                outside.mkdir(parents=True,exist_ok=True)
                with self.assertRaises(ValueError):retire_expanded(outside)

    def test_retire_only_exact_recoverable_snapshots_after_cleanup(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parents[1]/'artifacts') as folder:
            run=Path(folder);target=run/'stage-1-x.bin';foreign=run/'manual-backup.bin';foreign.write_bytes(b'keep')
            archive=b'G2VRSNP1'+record(target.name,b'exact')
            (run/'cpu-snapshots.bin').write_bytes(archive);unpack(run)
            (run/'capture-cleanup.json').write_text(json.dumps(dict(status='in_progress')))
            with self.assertRaises(ValueError):retire_expanded(run)
            self.assertTrue(target.exists())
            (run/'capture-cleanup.json').write_text(json.dumps(dict(status='complete')))
            target.write_bytes(b'changed')
            with self.assertRaises(ValueError):retire_expanded(run)
            self.assertEqual(target.read_bytes(),b'changed');target.write_bytes(b'exact')
            self.assertEqual(retire_expanded(run)['removedThisPass'],1)
            self.assertFalse(target.exists());self.assertEqual(foreign.read_bytes(),b'keep')
            self.assertEqual((run/'cpu-snapshots.bin').read_bytes(),archive)
            self.assertEqual(retire_expanded(run)['removedThisPass'],0)
            unpack(run);self.assertEqual(target.read_bytes(),b'exact')
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):retire_expanded(Path(folder))
    def test_exact_bytes_last_record_and_idempotence(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);a=bytes(range(256))*2;b=bytes(range(144))
            (run/'cpu-snapshots.bin').write_bytes(b'G2VRSNP1'+record('stage-7-scene-entry-context.bin',b'old')+record('stage-7-scene-entry-context.bin',a)+record('engine-8-camera-after.bin',b))
            result=unpack(run);self.assertEqual((result['records'],result['files']),(3,2))
            self.assertEqual((run/'stage-7-scene-entry-context.bin').read_bytes(),a)
            self.assertEqual((run/'engine-8-camera-after.bin').read_bytes(),b)
            self.assertEqual(unpack(run),result)
    def test_invalid_archive_writes_nothing(self):
        for bad in (b'wrong',b'G2VRSNP1'+b'\x01',b'G2VRSNP1'+record('../outside.bin',b'x'),b'G2VRSNP1'+record('stage-1-x.bin',bytes(513)),b'G2VRSNP1'+record('stage-1-x.bin',b'valid')+b'bad'):
            with tempfile.TemporaryDirectory() as folder:
                run=Path(folder);(run/'cpu-snapshots.bin').write_bytes(bad)
                with self.assertRaises(ValueError):unpack(run)
                self.assertEqual(len(list(run.iterdir())),1)
    def test_changed_existing_snapshot_is_preserved(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);target=run/'stage-1-x.bin';target.write_bytes(b'foreign')
            (run/'cpu-snapshots.bin').write_bytes(b'G2VRSNP1'+record(target.name,b'new'))
            with self.assertRaises(ValueError):unpack(run)
            self.assertEqual(target.read_bytes(),b'foreign')

if __name__=='__main__':unittest.main()
