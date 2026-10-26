import gzip
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import cockpit_start as start
from practice_entry import signature


class CockpitStartTests(unittest.TestCase):
    def setUp(self):
        self.source = gzip.decompress((ROOT / 'tests/fixtures/cockpit-startsequence.xml.gz').read_bytes())

    def test_handoff_follows_player_camera_enable_and_preserves_native_countdown(self):
        result = start.patch_start(self.source)
        self.assertEqual(result.replace(start.HANDOFF, b''), self.source)
        root = ET.fromstring(result)
        frame = root.findall("./Timeline/Keyframe[@name='kf_light_sequence']")[0]
        self.assertEqual(frame[-2].find('Camera').attrib, {'enabled': 'true'})
        self.assertEqual(frame[-1].attrib, dict(type='CameraTransition', startCamera='chase_close',
            endCamera='bonnet', transitionDuration='0.000000', toDefaultCamera='false',
            inhibitViewActions='false'))
        frame.remove(frame[-1])
        self.assertEqual(signature(root), signature(ET.fromstring(self.source)))

    def test_exact_source_guard_rejects_changed_or_previously_patched_sequence(self):
        self.assertEqual(hashlib.sha256(self.source).hexdigest(), start.START_SHA256)
        for source in (self.source + b' ', self.source.replace(b'green_sound', b'green_other'),
                       self.source.replace(b'enabled="true"', b'enabled="false"'),
                       start.patch_start(self.source)):
            with self.subTest(source_sha=hashlib.sha256(source).hexdigest()):
                with self.assertRaisesRegex(ValueError, 'Race-start sequence changed'):
                    start.patch_start(source)

    def test_upgrade_adds_separate_bank_and_rejects_foreign_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            original = game / start.START_PATH
            original.parent.mkdir(parents=True)
            original.write_bytes(self.source)
            bank = root / 'cockpit-start'
            start.prepare(game, bank)
            start.prepare(game, bank, verify=True)
            self.assertEqual(original.read_bytes(), self.source)
            with self.assertRaises(FileExistsError):
                start.prepare(game, bank)
            with self.assertRaises(ValueError):
                start.prepare(game, game / 'bad')
            (bank / 'startsequence.xml').write_bytes(b'foreign')
            with self.assertRaisesRegex(ValueError, 'Prepared race-start handoff changed'):
                start.prepare(game, bank, verify=True)


if __name__ == '__main__':
    unittest.main()
