"""Protect the accepted VR profile while preparing reversible graphics choices."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import graphics_profile as graphics


def attributes(payload):
    result = {}
    def visit(node, path):
        result.update({(path, key): value for key, value in node.attrib.items()})
        for child in node:
            visit(child, path + '/' + child.tag)
    visit(ET.fromstring(payload), '')
    return result


class GraphicsProfileTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.baseline = (Path(__file__).parents[1] / 'tools/practice-settings.xml').read_bytes()
        self.counter = 0

    def prepare(self, **options):
        self.counter += 1
        output = self.folder / f'profile-{self.counter}.xml'
        receipt = graphics.prepare(output, **options)
        payload = output.read_bytes()
        self.assertEqual(hashlib.sha256(payload).hexdigest(), receipt['sha256'])
        return payload, receipt

    def test_defaults_preserve_exact_accepted_bytes_in_both_modes(self):
        for mode in ('VR', 'Desktop'):
            payload, receipt = self.prepare(mode=mode)
            self.assertEqual(payload, self.baseline)
            self.assertTrue(receipt['isBaseline'])
            self.assertEqual(receipt['changes'], [])

    def test_headset_relative_portrait_and_odd_scene_dimensions(self):
        for width, height in [(2400, 2600), (3288, 3562), (1200, 1300), (4608, 4896), (6144, 6528), (8192, 8192)]:
            payload, receipt = self.prepare(scene_width=width, scene_height=height)
            self.assertEqual(receipt['resolution']['width'], str(width))
            self.assertEqual(receipt['resolution']['height'], str(height))
            changed = {key for key, value in attributes(payload).items() if attributes(self.baseline).get(key) != value}
            self.assertEqual(changed, {('/graphics_card/resolution', 'width'), ('/graphics_card/resolution', 'height')})
        for options in [dict(scene_width=2400), dict(scene_height=2600), dict(scene_width=8193, scene_height=2000),
                        dict(scene_width=8192, scene_height=8193), dict(mode='Desktop', scene_width=2400, scene_height=2600)]:
            with self.assertRaises(ValueError): self.prepare(**options)

    def test_desktop_preferences_cannot_change_vr_profile(self):
        for aa in ('off', '2xmsaa', '4xmsaa', '8xmsaa'):
            for vsync in (0, 1):
                payload, receipt = self.prepare(mode='VR', desktop_msaa=aa, desktop_vsync=vsync)
                self.assertEqual(payload, self.baseline)
                self.assertTrue(receipt['isBaseline'])

    def test_custom_vr_changes_only_allowed_attributes(self):
        payload, receipt = self.prepare(render_scale=150, tree_detail='medium', object_detail='high',
                                        crowd_detail='low', texture_filtering='off', desktop_msaa='8xmsaa', desktop_vsync=0)
        before, after = attributes(self.baseline), attributes(payload)
        expected = {('/graphics_card/resolution', 'width'): '1920', ('/graphics_card/resolution', 'height'): '1080',
                    ('/trees', 'lod'): '1.0', ('/objects', 'lod'): '1.25', ('/crowd', 'detail'): '0',
                    ('/anisotropic', 'min'): '1', ('/anisotropic', 'max'): '1'}
        self.assertEqual({key: after[key] for key in after if before.get(key) != after[key]}, expected)
        self.assertEqual(after, before | expected)  # No removed attributes, AO/shadow/post/CPU/input/stereo changes.
        self.assertEqual(after[('/graphics_card/resolution', 'multisampling')], 'off')
        self.assertEqual(after[('/graphics_card/resolution', 'vsync')], '1')
        self.assertFalse(receipt['isBaseline'])

    def test_desktop_msaa_and_vsync_are_scoped(self):
        payload, _ = self.prepare(mode='Desktop', desktop_msaa='4xmsaa', desktop_vsync=0, vr_msaa='8xmsaa')
        before = attributes(self.baseline)
        self.assertEqual(attributes(payload), before | {('/graphics_card/resolution', 'multisampling'): '4xmsaa',
                                                       ('/graphics_card/resolution', 'vsync'): '0'})

    def test_vr_msaa_changes_only_scene_sampling(self):
        before = attributes(self.baseline)
        for aa in graphics.MSAA:
            payload, receipt = self.prepare(vr_msaa=aa, desktop_msaa='8xmsaa', desktop_vsync=0)
            self.assertEqual(attributes(payload), before | {('/graphics_card/resolution', 'multisampling'): aa})
            self.assertEqual(receipt['resolution']['multisampling'], aa)

    def test_supported_choices_and_resolution_bounds(self):
        for scale, width, height in ((50, 640, 360), (75, 960, 540), (100, 1280, 720), (125, 1600, 900),
                                     (150, 1920, 1080), (151, 1932, 1087), (200, 2560, 1440), (300, 3840, 2160)):
            payload, _ = self.prepare(render_scale=scale)
            resolution = ET.fromstring(payload).find('graphics_card/resolution')
            self.assertEqual((int(resolution.get('width')), int(resolution.get('height'))), (width, height))
        for name, values in (('tree_detail', ('medium', 'high', 'ultra')), ('object_detail', ('medium', 'high', 'ultra')),
                             ('crowd_detail', ('low', 'medium', 'high', 'ultra')), ('texture_filtering', ('off', 'low', 'high', 'ultra'))):
            for value in values:
                self.prepare(**{name: value})

    def test_invalid_choices_write_nothing(self):
        for options in ({'render_scale': 49}, {'render_scale': 301}, {'render_scale': 150.5}, {'render_scale': True}, {'desktop_vsync': 2}, {'mode': 'Headset'},
                        {'desktop_msaa': '16xmsaa'}, {'vr_msaa': '16xmsaa'}, {'tree_detail': 'low'}, {'object_detail': 'off'},
                        {'crowd_detail': 'off'}, {'texture_filtering': 'custom'}):
            output = self.folder / 'rejected.xml'
            with self.subTest(options=options), self.assertRaises(ValueError):
                graphics.prepare(output, **options)
            self.assertFalse(output.exists())

    def test_changed_baseline_and_foreign_output_are_preserved(self):
        output = self.folder / 'foreign.xml'
        output.write_bytes(b'foreign profile')
        with self.assertRaises(FileExistsError):
            graphics.prepare(output)
        self.assertEqual(output.read_bytes(), b'foreign profile')
        changed = self.folder / 'changed.xml'
        changed.write_bytes(self.baseline.replace(b'parallelUpdateRender="false"', b'parallelUpdateRender="true"'))
        with self.assertRaises(ValueError):
            graphics.prepare(self.folder / 'rejected.xml', source=changed)
        self.assertFalse((self.folder / 'rejected.xml').exists())
        with self.assertRaises(ValueError):
            graphics.prepare(self.folder / 'installed.xml', game_root=self.folder)
        self.assertFalse((self.folder / 'installed.xml').exists())


if __name__ == '__main__':
    unittest.main()
