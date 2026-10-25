import copy
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch as mock_patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import cockpit_camera as cockpit

# Exact inspected source, compressed to keep this offline fixture below 9 KiB.
SOURCE = gzip.decompress((ROOT/'tests/fixtures/s20-cameras.xml.gz').read_bytes())
SEAT = [0.3537, 1.0665, -0.5216]


def view(root, ident):
    return root.find(f"ViewManager/View[@ident='{ident}']")


def value(root, ident, name, group=None):
    path = f"{group}/" if group else ''
    return view(root, ident).find(f"{path}Parameter[@name='{name}']")


class CockpitPatchTests(unittest.TestCase):
    def setUp(self):
        self.source = ET.fromstring(SOURCE)

    def test_fixture_is_the_inspected_source(self):
        self.assertEqual(hashlib.sha256(SOURCE).hexdigest(), cockpit.DECODED_SHA256)
        self.assertEqual(cockpit.semantic_digest(self.source), cockpit.SOURCE_SEMANTIC_SHA256)

    def test_both_directions_use_seat_preserve_cycling_clipping_and_other_views(self):
        before = cockpit.signature(self.source)
        result, changes = cockpit.patch(self.source, SEAT, 'head-only')
        self.assertEqual(cockpit.signature(self.source), before)
        self.assertGreater(len(changes), 10)
        for original, candidate in zip(self.source[0], result[0]):
            self.assertEqual(original.attrib, candidate.attrib)
            if original.get('ident') not in cockpit.VIEWS:
                self.assertEqual(cockpit.signature(original), cockpit.signature(candidate))
        for ident, yaw, fov in [('bonnet', '0.0000', '60.0000'), ('bonnet_reverse', '3.1420', '65.0000')]:
            self.assertEqual(value(result, ident, 'offset').attrib,
                             dict(name='offset', type='vector3', x='0.3537', y='1.0665', z='-0.5216'))
            for name, expected in [('yaw', yaw), ('fov', fov), ('nearPlane', '0.1000'),
                                   ('pitch', '0.0000'), ('roll', '0.0000'),
                                   ('highResInterior', 'false'), ('DisableDriverHead', 'true'),
                                   ('DisableCoDriver', 'true'), ('DisableDriver', 'false')]:
                self.assertEqual(value(result, ident, name).get('value'), expected, (ident, name))

    def test_head_only_and_hidden_driver_differ_only_in_two_driver_flags(self):
        body, _ = cockpit.patch(self.source, SEAT, 'head-only')
        hidden, _ = cockpit.patch(self.source, SEAT, 'hide-driver')
        for ident in cockpit.VIEWS:
            self.assertEqual(value(hidden, ident, 'DisableDriver').get('value'), 'true')
            value(body, ident, 'DisableDriver').set('value', 'true')
        self.assertEqual(cockpit.signature(body), cockpit.signature(hidden))

    def test_camera_local_motion_and_active_shakes_are_disabled(self):
        result, _ = cockpit.patch(self.source, SEAT, 'head-only')
        for ident in cockpit.VIEWS:
            for name in ('axisScale', 'axisScaleForTilt', 'twistScale'):
                self.assertEqual([value(result, ident, name).get(axis) for axis in 'xyz'], ['0.0000']*3)
            for name in ('maximumShift', 'movementSpeed', 'finalMovementScale'):
                self.assertEqual(value(result, ident, name).get('value'), '0.0000')
            for group in ('BrakeBasedShake', 'ScrapeShake', 'DriftShake', 'WheelSpinShake'):
                self.assertEqual(value(result, ident, 'enabled', group).get('value'), 'false')
            # Unused acceleration noise remains the known zero-amplitude source.
            self.assertEqual(cockpit.signature(view(result, ident).find('AccelerationBasedShake')),
                             cockpit.signature(view(self.source, ident).find('AccelerationBasedShake')))

    def test_change_receipt_accounts_for_every_modified_field(self):
        result, changes = cockpit.patch(self.source, SEAT, 'head-only')
        for change in changes:
            node = value(result, change['view'], change['parameter'], change['group'])
            self.assertEqual(node.get(change['field']), change['after'])
            node.set(change['field'], change['before'])
        self.assertEqual(cockpit.signature(result), cockpit.signature(self.source))

    def test_source_drift_and_ambiguous_structure_are_rejected(self):
        for mutation in ('root', 'car', 'duplicate-view', 'missing-view', 'type', 'reverse',
                         'missing-parameter', 'duplicate-parameter', 'type-parameter',
                         'near-plane', 'extra-child', 'other-view', 'shake', 'unrelated-attribute'):
            with self.subTest(mutation=mutation):
                source = copy.deepcopy(self.source)
                target = view(source, 'bonnet')
                offset = value(source, 'bonnet', 'offset')
                if mutation == 'root': source.tag = 'Other'
                if mutation == 'car': source[0].set('name', 'other')
                if mutation == 'duplicate-view': source[0].append(copy.deepcopy(target))
                if mutation == 'missing-view': source[0].remove(target)
                if mutation == 'type': target.set('type', 'Cockpit')
                if mutation == 'reverse': target.set('reverseCamera', 'bumper_reverse')
                if mutation == 'missing-parameter': target.remove(offset)
                if mutation == 'duplicate-parameter': target.append(copy.deepcopy(offset))
                if mutation == 'type-parameter': offset.set('type', 'scalar')
                if mutation == 'near-plane': value(source, 'bonnet', 'nearPlane').set('value', '0.0100')
                if mutation == 'extra-child': ET.SubElement(offset, 'Unknown')
                if mutation == 'other-view': value(source, 'chase_close', 'fov').set('value', '120')
                if mutation == 'shake': value(source, 'bonnet', 'enabled', 'BrakeBasedShake').set('value', 'false')
                if mutation == 'unrelated-attribute': source[0].set('unknown', 'true')
                with self.assertRaises(ValueError): cockpit.patch(source, SEAT, 'head-only')

    def test_position_requires_explicit_finite_bounded_numbers(self):
        cases = [None, [], [0, 1], [0, 1, 0, 1], [True, 1, 0], ['0', 1, 0],
                 [float('nan'), 1, 0], [0, float('inf'), 0], [0, 1, -float('inf')],
                 [-1.50001, 1, 0], [1.50001, 1, 0], [0, 0.19999, 0],
                 [0, 2.50001, 0], [0, 1, -2.00001], [0, 1, 2.00001]]
        for position in cases:
            with self.subTest(position=position), self.assertRaises(ValueError):
                cockpit.patch(self.source, position, 'head-only')
        with self.assertRaises(ValueError): cockpit.patch(self.source, SEAT, 'unknown')

    def test_roundtrip_checks_unrelated_views_as_well(self):
        expected, _ = cockpit.patch(self.source, SEAT, 'hide-driver')
        actual = copy.deepcopy(expected)
        cockpit.verify_roundtrip(expected, actual)
        value(actual, 'chase_far', 'fov').set('value', '120')
        with self.assertRaises(ValueError): cockpit.verify_roundtrip(expected, actual)


class CockpitBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cockpit-test-', dir=ROOT/'build')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.game = self.root/'game'
        self.source = self.game/cockpit.SOURCE_PATH
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(b'inspected binary fixture')
        (self.game/'grid2_avx.exe').write_bytes(b'executable fixture')
        self.converter = self.root/'converter.exe'
        self.converter.write_bytes(b'converter fixture')
        hashes = dict(SOURCE_SHA256=cockpit.digest(self.source),
                      SUPPORTED_AVX_SHA256=cockpit.digest(self.game/'grid2_avx.exe'),
                      CONVERTER_SHA256=cockpit.digest(self.converter))
        constants = mock_patch.multiple(cockpit, **hashes)
        constants.start(); self.addCleanup(constants.stop)
        conversion = mock_patch.object(cockpit, 'convert', side_effect=self.convert)
        conversion.start(); self.addCleanup(conversion.stop)
        self.bundle = self.root/'bundle'

    def convert(self, converter, mode, source, target):
        self.assertFalse(target.exists())
        if mode == 'encode': target.write_bytes(b'BINARY:'+source.read_bytes())
        elif source == self.source: target.write_bytes(SOURCE)
        else:
            binary = source.read_bytes()
            self.assertTrue(binary.startswith(b'BINARY:'))
            target.write_bytes(binary[7:])

    def prepare(self):
        return cockpit.prepare(self.game, self.bundle, self.converter, SEAT, 'head-only')

    def test_preparation_roundtrip_and_verification_never_touch_installation(self):
        before = {str(p): p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        data = self.prepare()
        self.assertEqual(cockpit.verify(self.game, self.bundle, self.converter), data)
        self.assertEqual(data['position'], SEAT)
        self.assertEqual(data['mode'], 'head-only')
        for flag in ('installed', 'desktopValidated', 'headsetValidated', 'headHidingValidated'):
            self.assertIs(data[flag], False)
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.game.rglob('*') if p.is_file()})
        self.assertEqual({p.name for p in self.bundle.iterdir()}, set(cockpit.BUNDLE_FILES)|{'manifest.json'})

    def test_changed_inputs_rejected_before_creating_output(self):
        for path in (self.source, self.game/'grid2_avx.exe', self.converter):
            with self.subTest(path=path):
                original = path.read_bytes()
                path.write_bytes(b'foreign')
                with self.assertRaises(ValueError): self.prepare()
                self.assertFalse(self.bundle.exists())
                path.write_bytes(original)

    def test_existing_or_installed_output_and_bad_position_refused_without_writes(self):
        for output in (self.game/'new-output', self.game, self.root):
            with self.subTest(output=output), self.assertRaises(ValueError):
                cockpit.prepare(self.game, output, self.converter, SEAT, 'head-only')
        self.assertFalse((self.game/'new-output').exists())
        with self.assertRaises(ValueError):
            cockpit.prepare(self.game, self.bundle, self.converter, [0, float('nan'), 0], 'head-only')
        self.assertFalse(self.bundle.exists())

    def test_forged_binary_and_its_updated_manifest_hash_are_rejected(self):
        data = self.prepare()
        target = self.bundle/'cameras.xml'
        target.write_bytes(b'foreign binary')
        data['modifiedSha256'] = data['files']['cameras.xml'] = cockpit.digest(target)
        (self.bundle/'manifest.json').write_text(json.dumps(data), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'not the expected bounded patch'):
            cockpit.verify(self.game, self.bundle, self.converter)
        self.assertFalse(any(p.name.startswith('verify-') for p in self.bundle.iterdir()))

    def test_changed_xml_with_updated_manifest_is_rejected(self):
        data = self.prepare()
        for name in ('source-cameras.xml', 'cockpit-cameras.xml', 'roundtrip-cameras.xml'):
            with self.subTest(name=name):
                path = self.bundle/name
                original = path.read_bytes()
                root = ET.fromstring(original)
                value(root, 'bonnet', 'nearPlane').set('value', '0.0100')
                path.write_bytes(ET.tostring(root))
                changed = copy.deepcopy(data)
                changed['files'][name] = cockpit.digest(path)
                (self.bundle/'manifest.json').write_text(json.dumps(changed), encoding='utf-8')
                with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)
                path.write_bytes(original)

    def test_manifest_scope_flags_paths_positions_and_file_set_are_guarded(self):
        data = self.prepare()
        cases = [('installed', True), ('desktopValidated', True), ('headsetValidated', True),
                 ('headHidingValidated', True), ('car', 'bac'), ('gameRoot', 'elsewhere'),
                 ('target', '../foreign.xml'), ('files', {'../foreign.xml': '0'*64}),
                 ('position', [0, float('nan'), 0]), ('position', [0, 1, 0]),
                 ('mode', 'hide-driver'), ('changes', []), ('schema', 2),
                 ('schema', True), ('installed', 0), ('semanticRoundtrip', 1)]
        for field, replacement in cases:
            with self.subTest(field=field, replacement=replacement):
                changed = copy.deepcopy(data); changed[field] = replacement
                (self.bundle/'manifest.json').write_text(json.dumps(changed), encoding='utf-8')
                with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)

    def test_failed_converter_roundtrip_does_not_produce_qualified_manifest(self):
        def corrupt(converter, mode, source, target):
            self.convert(converter, mode, source, target)
            if mode == 'decode' and source != self.source:
                root = ET.fromstring(target.read_bytes())
                value(root, 'bonnet_reverse', 'DisableDriverHead').set('value', 'false')
                target.write_bytes(ET.tostring(root))
        before = self.source.read_bytes()
        with mock_patch.object(cockpit, 'convert', side_effect=corrupt):
            with self.assertRaisesRegex(ValueError, 'changed camera semantics'): self.prepare()
        self.assertFalse((self.bundle/'manifest.json').exists())
        self.assertEqual(self.source.read_bytes(), before)

    def test_missing_oversize_directory_and_linked_bundle_files_are_rejected(self):
        self.prepare()
        target = self.bundle/'cameras.xml'
        original = target.read_bytes()
        target.unlink()
        with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)
        target.mkdir()
        with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)
        target.rmdir()
        target.write_bytes(b'x'*(cockpit.MAX_ASSET_BYTES+1))
        with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)
        target.unlink()
        outside = self.root/'outside.xml'; outside.write_bytes(original)
        try: target.symlink_to(outside)
        except OSError: return  # Creating symlinks requires a Windows privilege.
        with self.assertRaises(ValueError): cockpit.verify(self.game, self.bundle, self.converter)
        self.assertEqual(outside.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
