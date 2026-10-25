"""Preparation never modifies installed assets; practice requires guarded inputs."""
import hashlib
import json
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).parents[1]/'tools'))
import session_config as sessions
import practice_entry as practice


class SessionConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'game'
        self.root.mkdir()
        (self.root/'grid2_avx.exe').write_bytes(b'fixture executable')
        self.digest = hashlib.sha256(b'fixture executable').hexdigest()
        for car in ('z06', 'pag'):
            folder = self.root/'cars/models'/car
            (folder/'livery_user').mkdir(parents=True)
            (folder/'cameras.xml').write_bytes(b'native camera')
            (folder/f'{car}_highLOD.pssg').write_bytes(b'native mesh')
        for track in ('cities/chicago/route_0', 'p2p/okutama/route_1'):
            folder = self.root/'tracks/locations'/track
            folder.mkdir(parents=True)
            for name in ('track.jpk', 'route_objects.pssg', 'progress_track.xml'):
                (folder/name).write_bytes(b'native track')
        self.installed = sessions.catalog(self.root)

    def prepare(self, output, **options):
        selected = dict(car='z06', track='cities/chicago/route_0')
        selected.update(options)
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            return sessions.prepare(self.root, output, **selected)

    def practice_bundle(self):
        bundle = Path(self.temp.name)/'practice bundle'
        bundle.mkdir()
        (self.root/'system').mkdir()
        original, states, modified = b'original flow', b'states', b'practice flow'
        for name, value in (('SUPPORTED_AVX_SHA256', self.digest),
                            ('FLOW_SHA256', hashlib.sha256(original).hexdigest()),
                            ('STATES_SHA256', hashlib.sha256(states).hexdigest()),
                            ('PRACTICE_SHA256', hashlib.sha256(modified).hexdigest())):
            self.enterContext(patch.object(practice, name, value))
        (self.root/'system/flow.bin').write_bytes(original)
        (self.root/'system/states.bin').write_bytes(states)
        (bundle/'flow.bin').write_bytes(modified)
        data = dict(schema=2, gameRoot=str(self.root), executableSha256=self.digest,
                    originalFlowSha256=practice.FLOW_SHA256, statesSha256=practice.STATES_SHA256,
                    modifiedFlowSha256=practice.PRACTICE_SHA256, converterSha256=practice.CONVERTER_SHA256,
                    changes=practice.CHANGES, semanticRoundtrip=True, installed=False,
                    playerControlValidated=False)
        (bundle/'manifest.json').write_text(json.dumps(data), encoding='utf-8')
        return bundle

    def test_two_selections_and_installed_files_unchanged(self):
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        for car, track, mode in (('z06', 'cities/chicago/route_0', 'benchmark'), ('pag', 'p2p/okutama/route_1', 'attract')):
            destination = Path(self.temp.name)/f'selected {car}'
            result = self.prepare(destination, car=car, track=track, mode=mode)
            config = ET.parse(destination/'session.xml').getroot()
            self.assertEqual(config.attrib['cycle_camera'], 'false')
            self.assertEqual(config.attrib['infinite_loop'], 'false')
            route = config.find('track'); vehicle = route.find('car')
            self.assertEqual('/'.join(route.attrib[k] for k in ('country', 'name', 'route')), track)
            self.assertEqual(route.attrib['number_cars'], '1')
            self.assertEqual(vehicle.attrib['name'], car)
            self.assertFalse(result['playerControlValidated'])
            self.assertEqual(result['steamArguments'][-1], sessions.SESSION_FILENAME)
            self.assertEqual(result['steamArguments'][-2], '-benchmarkAttract' if mode == 'attract' else '-benchmark')
            self.assertEqual(hashlib.sha256((destination/'session.xml').read_bytes()).hexdigest(), result['configurationSha256'])
            with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
                self.assertEqual(sessions.verify(self.root, destination), result)
        self.assertEqual(before, {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()})

    def test_incomplete_content_is_not_advertised(self):
        (self.root/'cars/models/z06/z06_highLOD.pssg').unlink()
        (self.root/'tracks/locations/cities/chicago/route_0/track.jpk').unlink()
        found = sessions.catalog(self.root)
        self.assertEqual([v['id'] for v in found['cars']], ['pag'])
        self.assertEqual([v['id'] for v in found['tracks']], ['p2p/okutama/route_1'])

    def test_camera_cycle_is_explicit_ai_only_and_revalidated(self):
        destination = Path(self.temp.name)/'cycling AI'
        result = self.prepare(destination, cycle_camera=True)
        self.assertEqual(ET.parse(destination/'session.xml').getroot().get('cycle_camera'), 'true')
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            self.assertEqual(sessions.verify(self.root, destination), result)
        for options in (dict(cycle_camera=1), dict(cycle_camera='true'),
                        dict(cycle_camera=True, mode='attract'),
                        dict(cycle_camera=True, mode='practice', control='human')):
            with self.subTest(options=options), self.assertRaises(ValueError):
                sessions.configuration(self.installed, 'z06', 'cities/chicago/route_0', **options)

    def test_invalid_selection_is_rejected_before_writing(self):
        cases = [dict(car='../z06'), dict(track='cities/chicago/../../route_0'),
                 dict(livery='missing'), dict(control='human'), dict(mode='practice'),
                 dict(laps=0), dict(laps=21), dict(laps=True), dict(weather='snow')]
        for options in cases:
            destination = Path(self.temp.name)/'rejected'
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.prepare(destination, **options)
            self.assertFalse(destination.exists())

    def test_foreign_fingerprint_and_existing_output_are_preserved(self):
        destination = Path(self.temp.name)/'selected'
        with self.assertRaises(ValueError):
            sessions.prepare(self.root, destination, car='z06', track='cities/chicago/route_0')
        self.assertFalse(destination.exists())
        self.prepare(destination)
        original = (destination/'session.json').read_bytes()
        with self.assertRaises(FileExistsError):
            self.prepare(destination, car='pag')
        self.assertEqual((destination/'session.json').read_bytes(), original)
        with self.assertRaises(ValueError):
            self.prepare(self.root/'mod-output')
        self.assertFalse((self.root/'mod-output').exists())

    def test_reparse_points_are_rejected_in_parents_too(self):
        original = Path.lstat
        for target in (self.root, self.root/'cars/models/z06'):
            def inspected(path, **kwargs):
                if path == target:
                    return SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
                return original(path, **kwargs)
            with self.subTest(target=target), patch.object(Path, 'lstat', inspected), self.assertRaises(ValueError):
                sessions.catalog(self.root)

    def test_changed_bundle_and_removed_content_fail_revalidation(self):
        destination = Path(self.temp.name)/'selected'
        self.prepare(destination)
        xml = destination/'session.xml'; metadata = destination/'session.json'
        original_xml, original_metadata = xml.read_bytes(), metadata.read_bytes()
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            xml.write_bytes(original_xml.replace(b'name="z06"', b'name="pag"'))
            with self.assertRaises(ValueError):sessions.verify(self.root, destination)
            xml.write_bytes(original_xml)
            data = json.loads(original_metadata)
            data['steamArguments'] = ['-applaunch', '999']
            metadata.write_text(json.dumps(data))
            with self.assertRaises(ValueError):sessions.verify(self.root, destination)
            metadata.write_bytes(original_metadata)
            (self.root/'cars/models/z06/cameras.xml').unlink()
            with self.assertRaises(ValueError):sessions.verify(self.root, destination)

    def test_stage_uses_owned_config_and_preserves_existing_output(self):
        destination = Path(self.temp.name)/'selected'
        run = Path(self.temp.name)/'run with spaces & parentheses (1)'
        run.mkdir()
        self.prepare(destination, mode='attract')
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            staged = sessions.stage(self.root, destination, run)
            config = run/'session-config.xml'
            self.assertEqual(config.read_bytes(), (destination/'session.xml').read_bytes())
            self.assertEqual(staged['steamArguments'], ['-applaunch', '44350', '-benchmarkAttract', 'GRID2VR.session.xml'])
            self.assertEqual(staged['steamArgumentLine'], '-applaunch 44350 -benchmarkAttract GRID2VR.session.xml')
            self.assertEqual(staged['nativeConfigurationName'], sessions.SESSION_FILENAME)
            self.assertFalse(any(c in staged['steamArguments'][-1] for c in '(<>\\|:?*)!'))
            config.write_bytes(b'foreign existing file')
            with self.assertRaises(FileExistsError):
                sessions.stage(self.root, destination, run)
            self.assertEqual(config.read_bytes(), b'foreign existing file')
            with self.assertRaises(ValueError):
                sessions.stage(self.root, destination, self.root)
            self.assertFalse((self.root/'session-config.xml').exists())

    def test_old_absolute_path_bundle_stages_with_relative_native_name(self):
        destination = Path(self.temp.name)/'legacy selection'
        metadata = self.prepare(destination, mode='attract')
        metadata['schema'] = 1
        metadata['steamArguments'][-1] = str(destination/'session.xml')
        (destination/'session.json').write_text(json.dumps(metadata))
        run = Path(self.temp.name)/'legacy run'; run.mkdir()
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            staged = sessions.stage(self.root, destination, run)
        self.assertEqual(staged['steamArguments'][-1], sessions.SESSION_FILENAME)
        self.assertEqual(Path(staged['configuration']).read_bytes(), (destination/'session.xml').read_bytes())
        self.assertEqual(json.loads((destination/'session.json').read_text()), metadata)

    def test_malformed_metadata_is_rejected(self):
        destination = Path(self.temp.name)/'selected'
        original = self.prepare(destination)
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            for change in (dict(gameRoot=None), dict(configuration=[]), dict(selection={'car': []}),
                           dict(selection=None), dict(control='human'), dict(playerControlValidated=True)):
                with self.subTest(change=change):
                    (destination/'session.json').write_text(json.dumps({**original, **change}))
                    with self.assertRaises(ValueError):
                        sessions.verify(self.root, destination)

    def test_human_practice_requires_bundle_and_stages_owned_flow(self):
        destination = Path(self.temp.name)/'human session'
        with self.assertRaises(ValueError):
            self.prepare(destination, mode='practice', control='human')
        self.assertFalse(destination.exists())
        bundle = self.practice_bundle()
        before = (self.root/'system/flow.bin').read_bytes()
        with self.assertRaises(ValueError):
            self.prepare(destination, practice_bundle=bundle)
        data = self.prepare(destination, mode='practice', control='human', practice_bundle=bundle)
        self.assertEqual(data['control'], 'human')
        self.assertFalse(data['playerControlValidated'])
        self.assertEqual(data['steamArguments'][2], '-benchmarkAttract')
        run = Path(self.temp.name)/'owned run'; run.mkdir()
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            staged = sessions.stage(self.root, destination, run)
            self.assertEqual(staged['control'], 'human')
            self.assertEqual(Path(staged['practiceFlow']).read_bytes(), (bundle/'flow.bin').read_bytes())
            self.assertEqual(staged['practiceFlowSha256'], practice.PRACTICE_SHA256)
            with self.assertRaises(FileExistsError): sessions.stage(self.root, destination, run)
        self.assertEqual((self.root/'system/flow.bin').read_bytes(), before)

    def test_race_uses_shipped_grid_limit_and_driver_presets(self):
        bundle = self.practice_bundle()
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        for count in (1, 7):
            destination = Path(self.temp.name)/f'race-{count}'
            data = self.prepare(destination, mode='race', control='human', opponents=count,
                                laps=20, practice_bundle=bundle)
            route = ET.parse(destination/'session.xml').getroot().find('track')
            cars = route.findall('car')
            self.assertEqual(route.get('number_cars'), str(count+1))
            self.assertEqual(route.get('laps'), '20')
            self.assertEqual([v.get('grid') for v in cars], [str(i) for i in range(count+1)])
            self.assertTrue(all(v.get('name') == 'z06' and v.get('livery') == 'user' for v in cars))
            self.assertEqual(cars[0].attrib, dict(name='z06', livery='user', ai_driver='Cau_Demo_1_1',
                                                 ai_driver0='Cau_Demo_1_0', grid='0'))
            self.assertEqual([v.get('ai_driver') for v in cars[1:]], list(sessions.RACE_DRIVERS[:count]))
            self.assertTrue(all(v.get('ai_driver0') == 'Cau_Demo_1_0' for v in cars[1:]))
            self.assertEqual(data['steamArguments'], ['-applaunch', '44350', '-benchmarkAttract', sessions.SESSION_FILENAME])
            run = Path(self.temp.name)/f'race-run-{count}'; run.mkdir()
            with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
                self.assertEqual(sessions.verify(self.root, destination), data)
                staged = sessions.stage(self.root, destination, run)
            self.assertEqual(Path(staged['practiceFlow']).read_bytes(), (bundle/'flow.bin').read_bytes())
            self.assertFalse(staged['playerControlValidated'])
        self.assertEqual(before, {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob('*') if p.is_file()})

    def test_race_rejects_invalid_count_control_and_missing_flow(self):
        cases = [dict(mode='race', control='human', opponents=count) for count in (0, 8, -1, True, 1.5, '2')]
        cases += [dict(mode='race', opponents=1), dict(mode='race', control='human', opponents=1),
                  dict(opponents=1), dict(mode='practice', control='human', opponents=1),
                  dict(mode='normal', control='human')]
        for options in cases:
            destination = Path(self.temp.name)/'invalid race'
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.prepare(destination, **options)
            self.assertFalse(destination.exists())

    def test_race_revalidation_rejects_changed_opponents_and_flow(self):
        bundle = self.practice_bundle()
        destination = Path(self.temp.name)/'race'
        original = self.prepare(destination, mode='race', control='human', opponents=7, practice_bundle=bundle)
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            changed = {**original, 'selection': {**original['selection'], 'opponents': 6}}
            (destination/'session.json').write_text(json.dumps(changed))
            with self.assertRaises(ValueError): sessions.verify(self.root, destination)
            (destination/'session.json').write_text(json.dumps(original))
            (bundle/'flow.bin').write_bytes(b'changed flow')
            with self.assertRaises(ValueError): sessions.verify(self.root, destination)

    def test_practice_rejects_changed_files_even_with_forged_manifest(self):
        bundle = self.practice_bundle()
        destination = Path(self.temp.name)/'human session'
        self.prepare(destination, mode='practice', control='human', practice_bundle=bundle)
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            for path in (self.root/'system/flow.bin', self.root/'system/states.bin', bundle/'flow.bin'):
                original = path.read_bytes()
                path.write_bytes(b'changed')
                with self.subTest(path=path), self.assertRaises(ValueError): sessions.verify(self.root, destination)
                path.write_bytes(original)
            manifest_path = bundle/'manifest.json'
            manifest = json.loads(manifest_path.read_text())
            (bundle/'flow.bin').write_bytes(b'foreign flow')
            manifest['modifiedFlowSha256'] = hashlib.sha256(b'foreign flow').hexdigest()
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError): sessions.verify(self.root, destination)

    def test_old_practice_bundle_and_control_mismatch_are_refused(self):
        bundle = self.practice_bundle()
        destination = Path(self.temp.name)/'human session'
        data = self.prepare(destination, mode='practice', control='human', practice_bundle=bundle)
        with patch.object(sessions, 'SUPPORTED_AVX_SHA256', self.digest):
            (destination/'session.json').write_text(json.dumps({**data, 'control': 'ai'}))
            with self.assertRaises(ValueError): sessions.verify(self.root, destination)
            (destination/'session.json').write_text(json.dumps(data))
            manifest_path = bundle/'manifest.json'
            manifest = json.loads(manifest_path.read_text())
            manifest['schema'] = 1
            manifest['changes'] = manifest['changes'][:1]
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError): sessions.verify(self.root, destination)


if __name__ == '__main__':
    unittest.main()
