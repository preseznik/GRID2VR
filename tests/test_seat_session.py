import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from seat_session import prepare, request, apply

def fixture():
    return tempfile.TemporaryDirectory(prefix='seat-session-',dir=Path(__file__).resolve().parents[1]/'build')


class SeatSessionTests(unittest.TestCase):
    def test_roundtrip_preserves_vr_offsets_unrelated_cars_and_preferences(self):
        with fixture() as folder:
            path=Path(folder)/'seats.json'
            initial=dict(Version=1,ApplyToDesktop=False,InGameSeat=True,DesktopFov=10,Cars=dict(s20=dict(Height=10,Forward=2,Right=3),pag=dict(Height=4)),FutureSetting='retain')
            path.write_text(json.dumps(initial))
            raw=prepare(path,['s20','pag'],True,True,10,False).encode()
            self.assertEqual(request(raw)['cars']['s20'],(10,2,3))
            def commit(rows='',fov=20):return f'grid2vr-seat-commit-v1 {hashlib.sha256(raw).hexdigest()} {fov}\n{rows}'.encode()
            result=apply(path,raw,commit('s20 -12 5 -3\n'))
            saved=json.loads(path.read_text())
            self.assertTrue(result['changed']);self.assertEqual(saved['Cars']['s20'],dict(Height=-12,Forward=5,Right=-3))
            self.assertEqual(saved['Cars']['pag'],initial['Cars']['pag']);self.assertFalse(saved['ApplyToDesktop']);self.assertEqual(saved['FutureSetting'],'retain');self.assertEqual(saved['DesktopFov'],20)
            self.assertEqual(list(Path(folder).glob('*.tmp')),[])
            before=path.read_bytes();self.assertFalse(apply(path,raw,commit('s20 -12 5 -3\n'))['changed']);self.assertEqual(path.read_bytes(),before)

    def test_conflicts_and_invalid_commits_never_overwrite_settings(self):
        with fixture() as folder:
            path=Path(folder)/'seats.json';path.write_text('{"Version":1,"Cars":{}}')
            raw=prepare(path,['s20'],True,True,0,False).encode();prefix=f'grid2vr-seat-commit-v1 {hashlib.sha256(raw).hexdigest()}'
            for text in [f'{prefix} 31\n',f'{prefix} 1.5\n',f'{prefix} 0\ns20 51 0 0\n',f'{prefix} 0\npag 1 0 0\n',f'{prefix} 0\ns20 1 0 0\ns20 2 0 0\n','grid2vr-seat-commit-v1 wrong 0\n']:
                before=path.read_bytes()
                with self.assertRaises(ValueError):apply(path,raw,text.encode())
                self.assertEqual(path.read_bytes(),before)
            path.write_text('{"Version":1,"Cars":{"s20":{"Height":7}}}')
            before=path.read_bytes()
            with self.assertRaises(ValueError):apply(path,raw,(prefix+' 0\ns20 2 0 0\n').encode())
            self.assertEqual(path.read_bytes(),before)

    def test_fov_only_and_vr_commits_keep_the_other_modes_settings(self):
        with fixture() as folder:
            path=Path(folder)/'seats.json';path.write_text('{"Version":1,"DesktopFov":10,"Cars":{"s20":{"Height":12}}}')
            raw=prepare(path,['s20'],True,False,10,False).encode()
            commit=lambda raw,fov,rows='':f'grid2vr-seat-commit-v1 {hashlib.sha256(raw).hexdigest()} {fov}\n{rows}'.encode()
            apply(path,raw,commit(raw,-5));self.assertEqual(json.loads(path.read_text())['Cars']['s20']['Height'],12)
            with self.assertRaises(ValueError):apply(path,raw,commit(raw,0,'s20 0 0 0\n'))
            raw=prepare(path,['s20'],False,True,0,False).encode()
            apply(path,raw,commit(raw,0,'s20 0 0 0\n'))
            self.assertEqual(json.loads(path.read_text())['DesktopFov'],-5);self.assertEqual(json.loads(path.read_text())['Cars'],{})
            before=path.read_bytes();self.assertFalse(apply(path,raw,commit(raw,0))['changed']);self.assertEqual(path.read_bytes(),before)
            with self.assertRaises(ValueError):apply(path,raw,commit(raw,1))


if __name__=='__main__':unittest.main()
