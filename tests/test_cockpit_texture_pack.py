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
        for relative in ('lab/originals-all', 'lab/dds', 'src', 'tools/data', 'launcher'):
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
        (self.root/'launcher/CarNames.cs').write_text('["aaa"] = "Alpha Coupe",\n["bbb"] = "Béta Roadster",\n', encoding='utf-8')
        (self.root/'tools/data/dlc-content.json').write_text(json.dumps(dict(cars=[dict(id='aa2', label='Alpha Coupe (Performance)')])))
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
            self.assertEqual(set(z.namelist()), {prefix+n for n in ('aaa.dds','bbb.dds','pack.json','README.txt','CARS.txt')})
            metadata = json.loads(z.read(prefix+'pack.json'))
            self.assertEqual(metadata['atlases'][0]['cars'], ['aaa', 'aa2'])
            self.assertEqual(z.read(prefix+'aaa.dds'), (self.lab/'dds/aaa.dds').read_bytes())
            reference = z.read(prefix+'CARS.txt').decode('utf-8')
            table = [[cell.strip() for cell in line.split('|')] for line in reference.splitlines() if ' | ' in line][1:]
            self.assertEqual(table, [['Alpha Coupe','aaa','aaa.dds'], ['Alpha Coupe (Performance)','aa2','aaa.dds'], ['Béta Roadster','bbb','bbb.dds']])
            self.assertIn('Shared textures\n---------------\naaa.dds\n  Alpha Coupe (aaa)\n  Alpha Coupe (Performance) (aa2)\n', reference)
            self.assertIn(b'CARS.txt', z.read(prefix+'README.txt'))
        catalog_path = self.root/'tools/data/cockpit-texture-pack.json'
        catalog = json.loads(catalog_path.read_text())
        self.assertEqual(catalog['zipSha256'], pack.sha(Path(result['archive']).read_bytes()))
        self.assertIn(pack.sha(catalog_path.read_bytes()), (self.root/'tools/Cockpit-TexturePack.ps1').read_text())
        with zipfile.ZipFile(result['archive']) as z:
            self.assertEqual(catalog['files']['CARS.txt'], pack.sha(z.read(prefix+'CARS.txt')))

    def test_missing_shared_car_name_prevents_release(self):
        (self.root/'tools/data/dlc-content.json').write_text('{"cars": []}')
        with self.assertRaisesRegex(ValueError, 'Missing car names: aa2'):
            pack.build(self.lab)
        self.assertFalse(list(self.lab.glob('*.zip')))
        self.assertFalse((self.root/'src/cockpit_texture_catalog.inc').exists())

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
            reference=z.read(f'cockpit-textures/{pack.VERSION}/CARS.txt').decode('utf-8')
            self.assertRegex(reference, r'Béta Roadster\s+\| bbb\s+\| Original game texture')

    def test_shared_atlas_cannot_be_partially_excluded(self):
        with self.assertRaisesRegex(ValueError, 'Shared atlas'):
            pack.build(self.lab, keep_stock=['aaa'])


if __name__ == '__main__':
    unittest.main()
