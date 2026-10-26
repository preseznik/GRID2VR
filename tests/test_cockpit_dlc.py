import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import cockpit_dlc as cockpit
import game_executable
import session_config


class DlcCockpitTests(unittest.TestCase):
    def setUp(self):
        self.temp = self.enterContext(tempfile.TemporaryDirectory())
        self.game = Path(self.temp) / 'game'
        self.game.mkdir()
        self.data, self.rows = cockpit.calibrations()
        self.installed = dict(cars=[dict(id=r['car'], archives=r['archives']) for r in self.rows.values()], tracks=[])

    def test_catalog_covers_exactly_owned_dlc_and_keeps_sources_guarded(self):
        manifest = json.loads(session_config.DLC_MANIFEST.read_text())
        self.assertEqual(set(self.rows), {r['id'] for r in manifest['cars']})
        self.assertEqual(len(self.rows), 25)
        found = cockpit.available(self.game, self.installed)
        self.assertEqual(set(found), set(self.rows) - {'722', 'gtr', 'mpg'})
        source = self.game / 'dr.nic'
        source.write_bytes(b'foreign')
        self.assertEqual(set(cockpit.available(self.game, self.installed)), set(found))
        foreign = copy.deepcopy(self.installed)
        foreign['cars'][0]['archives'] = ['download/foreign.nefs']
        self.assertNotIn(foreign['cars'][0]['id'], cockpit.available(self.game, foreign))

    def test_embedded_cameras_require_exact_archive_hash(self):
        data = copy.deepcopy(self.data)
        source = self.game / 'dr.nic'
        source.write_bytes(b'known embedded source')
        data['embeddedArchive'].update(bytes=source.stat().st_size, sha256=hashlib.sha256(source.read_bytes()).hexdigest())
        with patch.object(cockpit, 'calibrations', return_value=(data, self.rows)):
            self.assertEqual(len(cockpit.available(self.game, self.installed)), 25)
            source.write_bytes(b'X' * source.stat().st_size)
            self.assertEqual(len(cockpit.available(self.game, self.installed)), 22)

    def test_request_filters_selected_car_and_applies_same_centimetre_axes(self):
        self.enterContext(patch.object(game_executable, 'validate_avx', return_value='supported'))
        self.enterContext(patch.object(session_config, 'installed_dlc', return_value=self.installed))
        seats = Path(self.temp) / 'seats.json'
        seats.write_text(json.dumps(dict(Version=1, Cars={'240': dict(Height=-50, Forward=50, Right=50)})))
        result = cockpit.request(self.game, 'head-only', car='240', alignments=seats)
        base = self.rows['240']['position']
        self.assertEqual(result['cars'], [dict(car='240', position=[round(base[0]-.5,4), round(base[1]-.5,4), round(base[2]+.5,4)])])
        self.assertEqual(len(result['text'].splitlines()), 2)
        self.assertTrue(result['text'].startswith('dlc-cockpit-v1 head-only\n240 '))
        normal = cockpit.request(self.game, 'hide-driver', normal=True)
        self.assertEqual(len(normal['cars']), 22)
        self.assertLess(len(normal['text']), 4096)
        for args in (dict(car='unknown'), dict(car='mpg'), dict(car='240', normal=True), dict()):
            with self.assertRaises(ValueError): cockpit.request(self.game, 'head-only', **args)
        for value in (51, -51, 1.5, True, '5'):
            seats.write_text(json.dumps(dict(Version=1, Cars={'240': dict(Height=value, Forward=0, Right=0)})))
            with self.assertRaises(ValueError): cockpit.request(self.game, 'head-only', car='240', alignments=seats)

    def test_changed_calibration_catalog_is_refused(self):
        catalog = Path(self.temp) / 'catalog.json'
        catalog.write_bytes(cockpit.CATALOG.read_bytes() + b' ')
        with patch.object(cockpit, 'CATALOG', catalog):
            with self.assertRaisesRegex(ValueError, 'catalog changed'): cockpit.calibrations()


if __name__ == '__main__': unittest.main()
