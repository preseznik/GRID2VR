"""Offline authored-pose checks; these do not establish other cars' VR visuals."""
import base64
import copy
import gzip
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import cockpit_geometry as geometry


class CockpitGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = json.loads(gzip.decompress((ROOT/'tests/fixtures/cockpit-geometry.json.gz').read_bytes()))

    def car(self, ident='s20'):
        return copy.deepcopy(next(c for c in self.fixture['cars'] if c['car'] == ident))

    def skeleton(self):
        return copy.deepcopy(self.fixture['skeleton']['data'])

    def test_accepted_s2000_is_reproduced(self):
        result = geometry.derive(self.car(), self.skeleton())
        self.assertEqual(result['position'], [.3537, 1.0665, -.5216])
        self.assertAlmostEqual(result['handChecks'][0]['distanceMeters'], .000094876103, places=10)
        self.assertAlmostEqual(result['handChecks'][1]['distanceMeters'], .000143459832, places=10)
        self.assertEqual(result['validation'], 's20-user-accepted')

    def test_different_cars_have_independent_positions(self):
        results = [geometry.derive(self.car(c), self.skeleton()) for c in ('s20', 'bac', '300')]
        self.assertEqual(len({tuple(r['position']) for r in results}), 3)
        self.assertLess(abs(results[1]['position'][0]), .02)  # BAC's authored central seat.
        self.assertTrue(all(r['validation'] == 'offline-derived' for r in results[1:]))

    def test_authored_pose_mismatch_is_recorded_without_a_false_pass(self):
        for ident in ('cat', 'crt', 'rx1'):
            with self.subTest(car=ident):
                result = geometry.derive(self.car(ident), self.skeleton())
                self.assertEqual(result['handChecksStatus'], 'authored-mismatch')
                self.assertEqual(result['validation'], 'offline-derived')
                self.assertEqual(len(result['warnings']), 1)
                self.assertGreater(max(h['distanceMeters'] for h in result['handChecks']), .001)

    def test_left_right_hand_swap_cannot_pass_the_consistency_check(self):
        car = self.car()
        nodes = {n['id']: n for n in car['dummies']['data']}
        nodes['D_Lhand_Dummy']['matrix'], nodes['D_Rhand_Dummy']['matrix'] = (
            nodes['D_Rhand_Dummy']['matrix'], nodes['D_Lhand_Dummy']['matrix'])
        result = geometry.derive(car, self.skeleton())
        self.assertEqual(result['handChecksStatus'], 'authored-mismatch')

    def test_rotated_or_displaced_attachment_mount_is_rejected(self):
        car = self.car()
        node = next(n for n in car['dummies']['data'] if n['id'] == 'd_bip_position')
        values = node['matrix'].split()
        values[12] = '.5'
        node['matrix'] = ' '.join(values)
        with self.assertRaisesRegex(ValueError, 'attachment mount'):
            geometry.derive(car, self.skeleton())

    def test_head_nub_outside_anatomical_bounds_is_rejected(self):
        nodes = self.skeleton()
        node = next(n for n in nodes if n['id'] == 'Bip01 HeadNub')
        values = node['matrix'].split()
        values[12:15] = ['10', '10', '10']
        node['matrix'] = ' '.join(values)
        with self.assertRaisesRegex(ValueError, 'head dimensions'):
            geometry.derive(self.car(), nodes)

    def test_finite_but_implausible_seat_is_rejected(self):
        car = self.car()
        packet = car['animation']['data']['packets'][0]
        raw = bytearray(base64.b64decode(packet['data']))
        cursor = geometry.Packet(raw)
        count = cursor.read('>5I')[0]
        channels = [(cursor.string(), cursor.string()) for _ in range(count)]
        cursor.read('>f')
        joint_index = next(i for i, channel in enumerate(channels) if channel[0] == 'Bip01')
        struct.pack_into('>f', raw, cursor.pos+joint_index*16, 4.)
        packet['data'] = base64.b64encode(raw).decode()
        with self.assertRaisesRegex(ValueError, 'bounded car-local'):
            geometry.derive(car, self.skeleton())

    def test_unknown_animation_schema_is_rejected(self):
        car = self.car()
        car['animation']['data']['packets'][0]['type'] = 'other'
        with self.assertRaisesRegex(ValueError, 'packet set'):
            geometry.derive(car, self.skeleton())

    def test_catalog_or_scratch_inside_game_is_rejected_before_writes(self):
        with tempfile.TemporaryDirectory(prefix='cockpit-geometry-test-', dir=ROOT/'build') as temporary:
            root = Path(temporary)
            game = root/'game'
            game.mkdir()
            for export, output in ((root/'input.json', game), (root/'input.json', game/'output.json'),
                                   (game/'input.json', root/'output.json')):
                with self.subTest(output=output), self.assertRaisesRegex(ValueError, 'outside the installed game'):
                    geometry.build(export, game, root/'converter.exe', output)
                self.assertEqual(list(game.iterdir()), [])

    @unittest.skipUnless(sys.platform == 'win32', 'PowerShell exporter guard')
    def test_exporter_output_equal_to_game_is_rejected_before_writes(self):
        with tempfile.TemporaryDirectory(prefix='cockpit-geometry-test-', dir=ROOT/'build') as temporary:
            result = subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT/'tools/Export-CockpitGeometry.ps1'),
                                     '-GameRoot', temporary, '-OutputDirectory', temporary],
                                    capture_output=True, text=True, timeout=30,
                                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('outside the installed game', result.stderr)
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_parent_order_is_irrelevant(self):
        self.assertEqual(geometry.derive(self.car(), list(reversed(self.skeleton())))['position'],
                         [.3537, 1.0665, -.5216])

    def test_parent_cycle_is_rejected(self):
        nodes = self.skeleton()
        root = next(n for n in nodes if n['id'] == 'Scene Root')
        root['parent'] = next(n['path'] for n in nodes if n['id'] == 'Bip01 Head')
        with self.assertRaisesRegex(ValueError, 'Cycle'):
            geometry.derive(self.car(), nodes)

    def test_duplicate_node_is_rejected(self):
        nodes = self.skeleton()
        nodes.append(copy.deepcopy(nodes[0]))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            geometry.derive(self.car(), nodes)

    def test_unknown_animation_joint_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unknown joint'):
            geometry.compose(self.skeleton(), {'unknown': [0, 0, 0]})

    def test_nonfinite_transform_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Invalid authored transform'):
            geometry.matrix('nan ' + '0 '*15)

    def test_truncated_translation_packet_is_rejected(self):
        packet = self.car()['animation']['data']['packets'][0]
        raw = base64.b64decode(packet['data'])
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            geometry.translations(raw[:-1])

    def test_translation_layout_drift_is_rejected(self):
        packet = self.car()['animation']['data']['packets'][0]
        raw = bytearray(base64.b64decode(packet['data']))
        struct.pack_into('>I', raw, 4, 2)
        with self.assertRaisesRegex(ValueError, 'layout'):
            geometry.translations(raw)

    def test_rotation_layout_drift_is_rejected(self):
        packet = self.car()['animation']['data']['packets'][1]
        raw = bytearray(base64.b64decode(packet['data']))
        struct.pack_into('>I', raw, 4, 32)
        with self.assertRaisesRegex(ValueError, 'layout'):
            geometry.rotations(raw)

    def test_nonfinite_quaternion_is_rejected(self):
        packet = self.car()['animation']['data']['packets'][1]
        raw = bytearray(base64.b64decode(packet['data']))
        cursor = geometry.Packet(raw)
        cursor.read('>2I')
        cursor.string()
        cursor.string()
        cursor.read('>3If')
        struct.pack_into('>e', raw, cursor.pos, float('nan'))
        with self.assertRaisesRegex(ValueError, 'Nonfinite'):
            geometry.rotations(raw)

    def test_extra_packet_data_is_rejected(self):
        packet = self.car()['animation']['data']['packets'][1]
        with self.assertRaisesRegex(ValueError, 'Trailing'):
            geometry.rotations(base64.b64decode(packet['data']) + b'\0')

    def test_runtime_catalog_is_bounded_and_has_explicit_acceptance(self):
        catalog = json.loads((ROOT/'tools/data/cockpit-cars.json').read_text())
        self.assertEqual(catalog['schema'], 1)
        self.assertEqual(len(catalog['cars']), 51)
        self.assertEqual(catalog['unsupported'], [])
        self.assertEqual({r['car'] for r in catalog['cars'] if r['handChecksStatus'] != 'consistent'},
                         {'cat', 'crt', 'rx1'})
        self.assertEqual([r['car'] for r in catalog['cars'] if r['validation'] == 's20-user-accepted'], ['s20'])
        for row in catalog['cars']:
            if row['handChecksStatus'] == 'consistent':
                self.assertLess(max(h['distanceMeters'] for h in row['handChecks']), .001)
                self.assertEqual(row['warnings'], [])
            self.assertEqual(row['camera']['path'], f"cars/models/{row['car']}/cameras.xml")
            for key in ('sha256', 'decodedSha256', 'semanticSha256'):
                self.assertRegex(row['camera'][key], '^[a-f0-9]{64}$')
            for coordinate, (lo, hi) in zip(row['position'], geometry.POSITION_BOUNDS):
                self.assertTrue(lo <= coordinate <= hi)

    def test_visual_defaults_cover_all_cars_and_preserve_accepted_s2000(self):
        defaults = json.loads((ROOT/'tools/data/cockpit-defaults.json').read_text())
        catalog = json.loads((ROOT/'tools/data/cockpit-cars.json').read_text())
        reviews = {r['car']: r for r in defaults['cars']}
        self.assertEqual(len(reviews), 51)
        self.assertEqual(set(reviews), {r['car'] for r in catalog['cars']})
        for row in catalog['cars']:
            review = reviews[row['car']]
            derived = dict(car=row['car'], position=list(review['derivedPosition']))
            calibrated = geometry.apply_visual_default(derived, review, review['meshSha256'])
            self.assertEqual(calibrated['position'], row['position'])
            self.assertEqual(calibrated['visualReview'], row['visualReview'])
            self.assertEqual(row['position'][::2], review['derivedPosition'][::2])
        self.assertEqual(reviews['s20']['heightCentimeters'], 0)

    def test_visual_review_rejects_stale_pose_car_or_mesh(self):
        review = json.loads((ROOT/'tools/data/cockpit-defaults.json').read_text())['cars'][0]
        for change in ({'car': 'xxx'}, {'position': [0, 1, 0]}):
            row = dict(car=review['car'], position=list(review['derivedPosition']))
            row.update(change)
            with self.assertRaisesRegex(ValueError, 'pose/mesh'):
                geometry.apply_visual_default(row, review, review['meshSha256'])
        row = dict(car=review['car'], position=list(review['derivedPosition']))
        with self.assertRaisesRegex(ValueError, 'pose/mesh'):
            geometry.apply_visual_default(row, review, '0'*64)

    def test_visual_review_rejects_unbounded_or_noninteger_offsets(self):
        original = json.loads((ROOT/'tools/data/cockpit-defaults.json').read_text())['cars'][0]
        for value in [-13, 1, -4.5, True]:
            review = dict(original, heightCentimeters=value)
            row = dict(car=review['car'], position=list(review['derivedPosition']))
            with self.assertRaisesRegex(ValueError, 'reviewed bounds'):
                geometry.apply_visual_default(row, review, review['meshSha256'])

    def test_visual_review_cannot_move_accepted_s2000(self):
        reviews = json.loads((ROOT/'tools/data/cockpit-defaults.json').read_text())['cars']
        review = dict(next(r for r in reviews if r['car'] == 's20'), heightCentimeters=-4)
        row = dict(car='s20', position=list(review['derivedPosition']))
        with self.assertRaisesRegex(ValueError, 'accepted S2000'):
            geometry.apply_visual_default(row, review, review['meshSha256'])


if __name__ == '__main__':
    unittest.main()
