"""Pack coverage and corruption gates, using synthetic DDS payloads."""
import importlib.util
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('pack', Path(__file__).resolve().parents[1]/'tools/build_cockpit_texture_pack.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def dds(size, mips, length, fill):
    data = bytearray([fill])*length
    data[:4] = b'DDS '
    struct.pack_into('<I', data, 4, 124)
    struct.pack_into('<2I', data, 12, size, size)
    struct.pack_into('<I', data, 28, mips)
    data[84:88] = b'DXT1'
    return bytes(data)


class PackTests(unittest.TestCase):
    def setUp(self):
        fixtures = Path(__file__).resolve().parents[1]/'build'
        fixtures.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='cockpit-pack-test-', dir=fixtures)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lab = self.root/'lab'
        for relative in ('lab/originals-all', 'lab/dds', 'src', 'tools/data'):
            (self.root/relative).mkdir(parents=True)
        self.rows = []
        for car, dlc, fill in [('aaa', False, 1), ('aa2', True, 1), ('bbb', False, 2)]:
            data = dds(512, 10, 174904, fill)
            (self.lab/f'originals-all/{car}.dds').write_bytes(data)
            self.rows.append(dict(car=car, dlc=dlc, ddsSha256=pack.sha(data)))
        self.inventory()
        for car, fill in [('aaa', 3), ('bbb', 4)]:
            (self.lab/f'dds/{car}.dds').write_bytes(dds(2048, 12, 2796344, fill))
        (self.root/'tools/Cockpit-TexturePack.ps1').write_text("$catalogHash='PENDING'\n")
        self.patch = patch.object(pack, 'ROOT', self.root)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def inventory(self):
        (self.lab/'originals-all/inventory.json').write_text(json.dumps(dict(cars=self.rows)))

    def test_complete_pack_deduplicates_and_excludes_originals(self):
        result = pack.build(self.lab)
        self.assertEqual((result['cars'], result['uniqueAtlases']), (3, 2))
        with zipfile.ZipFile(result['archive']) as z:
            prefix = f'cockpit-textures/{pack.VERSION}/'
            self.assertEqual(set(z.namelist()), {prefix+n for n in ('aaa.dds','bbb.dds','pack.json','README.txt')})
            metadata = json.loads(z.read(prefix+'pack.json'))
            self.assertEqual(metadata['atlases'][0]['cars'], ['aaa', 'aa2'])
            self.assertEqual(z.read(prefix+'aaa.dds'), (self.lab/'dds/aaa.dds').read_bytes())
        catalog_path = self.root/'tools/data/cockpit-texture-pack.json'
        catalog = json.loads(catalog_path.read_text())
        self.assertEqual(catalog['zipSha256'], pack.sha(Path(result['archive']).read_bytes()))
        self.assertIn(pack.sha(catalog_path.read_bytes()), (self.root/'tools/Cockpit-TexturePack.ps1').read_text())

    def test_missing_art_cannot_produce_release(self):
        (self.lab/'dds/aaa.dds').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing replacement artwork: aaa, aa2'):
            pack.build(self.lab)
        self.assertFalse((self.root/'src/cockpit_texture_catalog.inc').exists())
        result = pack.build(self.lab, draft=True)
        self.assertFalse(result['complete'])
        self.assertEqual(result['missingCars'], ['aaa','aa2'])
        self.assertFalse(list(self.lab.glob('*.zip')))

    def test_changed_source_and_malformed_replacement_are_rejected(self):
        original = self.lab/'originals-all/aaa.dds'
        data = original.read_bytes()
        original.write_bytes(data[:-1]+b'!')
        with self.assertRaisesRegex(ValueError, 'Changed source export'):
            pack.build(self.lab)
        original.write_bytes(data)
        (self.lab/'dds/aaa.dds').write_bytes(dds(1024, 12, 2796344, 3))
        with self.assertRaisesRegex(ValueError, 'Unexpected BC1 DDS layout'):
            pack.build(self.lab)

    def test_invalid_car_path_is_rejected(self):
        self.rows[0]['car'] = '../outside'
        self.inventory()
        with self.assertRaisesRegex(ValueError, 'Invalid car ID'):
            pack.build(self.lab)

    def test_explicit_stock_exception_is_recorded_without_replacement(self):
        (self.lab/'dds/bbb.dds').unlink()
        result = pack.build(self.lab, keep_stock=['bbb'])
        self.assertTrue(result['complete'])
        self.assertEqual(result['stockCars'], ['bbb'])
        self.assertEqual((result['cars'], result['uniqueAtlases']), (2, 1))
        with zipfile.ZipFile(result['archive']) as z:
            metadata=json.loads(z.read(f'cockpit-textures/{pack.VERSION}/pack.json'))
            self.assertEqual(metadata['stockCars'], ['bbb'])
            self.assertFalse(any(n.endswith('/bbb.dds') for n in z.namelist()))

    def test_shared_atlas_cannot_be_partially_excluded(self):
        with self.assertRaisesRegex(ValueError, 'Shared atlas'):
            pack.build(self.lab, keep_stock=['aaa'])


if __name__ == '__main__':
    unittest.main()
