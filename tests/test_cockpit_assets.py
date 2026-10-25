import copy
import gzip
import json
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import cockpit_assets as assets
import cockpit_camera as camera


class CarAssetsTests(unittest.TestCase):
    def setUp(self):
        self.source=ET.fromstring(gzip.decompress((ROOT/'tests/fixtures/s20-cameras.xml.gz').read_bytes()))

    def test_start_cockpit_moves_only_existing_view_and_retains_legacy(self):
        legacy,_=camera.patch(self.source,[.3537,1.0665,-.5216],'head-only')
        initial,_=camera.patch(self.source,[.3537,1.0665,-.5216],'head-only',start_cockpit=True)
        self.assertEqual(legacy[0][0].get('ident'),'chase_close')
        self.assertEqual(initial[0][0].get('ident'),'bonnet')
        self.assertEqual(initial[0].attrib,legacy[0].attrib)
        old={v.get('ident'):camera.signature(v) for v in legacy[0]}
        new={v.get('ident'):camera.signature(v) for v in initial[0]}
        self.assertEqual(old,new)
        self.assertEqual([v.get('ident') for v in initial[0]][1:],
                         [v.get('ident') for v in legacy[0] if v.get('ident')!='bonnet'])

    def test_roof_clip_changes_only_two_cockpit_near_planes(self):
        for mode in ('head-only','hide-driver'):
            baseline,_=camera.patch(self.source,[.3537,1.0665,-.5216],mode,start_cockpit=True)
            candidate,_=camera.patch(self.source,[.3537,1.0665,-.5216],mode,start_cockpit=True,near_plane=.02)
            for view in candidate[0]:
                if view.get('ident') in camera.VIEWS:
                    plane=view.find("Parameter[@name='nearPlane']")
                    self.assertEqual(plane.get('value'),'0.0200')
                    plane.set('value','0.1000')
            self.assertEqual(camera.signature(candidate),camera.signature(baseline))
        with self.assertRaises(ValueError):
            camera.patch(self.source,[.3537,1.0665,-.5216],'head-only',near_plane=.001)

    def test_first_career_handoff_changes_only_destination_and_restore_flag(self):
        source=gzip.decompress((ROOT/'tests/fixtures/cockpit-intro.xml.gz').read_bytes())
        result=assets.patch_intro(source)
        self.assertEqual(result.replace(b'endCamera="bonnet"',b'endCamera="chase_close"').replace(
            b'toDefaultCamera="false"',b'toDefaultCamera="true"'),source)
        old=ET.fromstring(source);new=ET.fromstring(result)
        event=new.find(".//Event[@type='CameraTransition']")
        self.assertEqual(event.get('endCamera'),'bonnet')
        self.assertEqual(event.get('toDefaultCamera'),'false')
        event.attrib.update(old.find(".//Event[@type='CameraTransition']").attrib)
        self.assertEqual(camera.signature(old),camera.signature(new))
        with self.assertRaises(ValueError): assets.patch_intro(source+b' ')

    def test_each_car_uses_its_own_seat_and_retains_other_views(self):
        for car,seat in [('s20',[.3537,1.0665,-.5216]),('pag',[.4,.75,-.3])]:
            tree=copy.deepcopy(self.source);tree[0].set('name',car)
            fingerprint=camera.semantic_digest(tree)
            result,_=camera.patch(tree,seat,'head-only',car=car,source_semantic=fingerprint)
            for old,new in zip(tree[0],result[0]):
                if old.get('ident') not in camera.VIEWS:
                    self.assertEqual(camera.signature(old),camera.signature(new))
                else:
                    offset=new.find("Parameter[@name='offset']")
                    self.assertEqual([float(offset.get(k)) for k in 'xyz'],seat)
                    for name in ('fov','nearPlane','highResInterior','yaw'):
                        self.assertEqual(old.find(f"Parameter[@name='{name}']").attrib,new.find(f"Parameter[@name='{name}']").attrib)

    def test_optional_shake_fields_stay_absent(self):
        for view in self.source[0]:
            if view.get('ident') in camera.VIEWS:
                for name in ('vehicleSkidShake','rotationShake','positionShake'):
                    view.remove(view.find(f"Parameter[@name='{name}']"))
        result,_=camera.patch(self.source,[.3537,1.0665,-.5216],'head-only',source_semantic=camera.semantic_digest(self.source))
        for view in result[0]:
            if view.get('ident') in camera.VIEWS:
                self.assertIsNone(view.find("Parameter[@name='vehicleSkidShake']"))


    def test_wrong_car_or_changed_definition_is_rejected(self):
        with self.assertRaises(ValueError):
            camera.patch(self.source,[.3,1,-.5],'head-only',car='pag')
        self.source[0][0].set('spectator','false')
        with self.assertRaises(ValueError):
            camera.patch(self.source,[.3,1,-.5],'head-only')

    def test_catalog_rejects_duplicate_cars_paths_and_nonfinite_seats(self):
        row=dict(car='s20',position=[.3537,1.0665,-.5216],camera=dict(path=camera.SOURCE_PATH,
            sha256=camera.SOURCE_SHA256,decodedSha256=camera.DECODED_SHA256,semanticSha256=camera.SOURCE_SEMANTIC_SHA256))
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'catalog.json'
            path.write_text(json.dumps(dict(schema=1,cars=[row])))
            self.assertEqual(len(assets.profiles(path)['cars']),1)
            bad=[]
            for field,value in [('position',[float('nan'),1,0]),('car','../s20')]:
                other=copy.deepcopy(row);other[field]=value;bad.append([other])
            other=copy.deepcopy(row);other['camera']['path']='cars/models/pag/cameras.xml';bad.append([other])
            bad.append([row,row])
            for rows in bad:
                path.write_text(json.dumps(dict(schema=1,cars=rows)))
                with self.assertRaises(ValueError): assets.profiles(path)


if __name__=='__main__': unittest.main()
