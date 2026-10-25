import copy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import instance_capacity_config as capacity
from practice_entry import signature, verify_roundtrip


def fixture():
    root = ET.Element('ornament_system_settings')
    for name in capacity.STYLES:
        style = ET.SubElement(root, 'setting_style', name=name)
        traversals = capacity.TRAVERSALS[:-1] if name in capacity.SHORT_STYLES else capacity.TRAVERSALS
        for traversal in traversals:
            ET.SubElement(style, 'traversal_settings', name=traversal,
                          instance_buffer='1200' if traversal == 'main_scene' else '100')
    return root


class InstanceCapacityConfigTests(unittest.TestCase):
    def test_only_high_main_scene_changes_and_original_is_unchanged(self):
        source = fixture()
        before = signature(source)
        result, changes = capacity.transform(source)
        self.assertEqual(changes, [dict(style='high', traversal='main_scene', attribute='instance_buffer', before=1200, after=4096)])
        self.assertEqual(signature(source), before)
        result.find("setting_style[@name='high']/traversal_settings[@name='main_scene']").set('instance_buffer', '1200')
        self.assertEqual(signature(result), before)

    def test_sufficient_capacity_is_a_noop(self):
        for value in (4096, 8000, 16384):
            source = fixture()
            source.find("setting_style[@name='high']/traversal_settings[@name='main_scene']").set('instance_buffer', str(value))
            result, changes = capacity.transform(source)
            self.assertEqual(changes, [])
            self.assertEqual(signature(result), signature(source))
            self.assertIsNot(result, source)

    def test_invalid_capacities_are_refused_without_mutation(self):
        for value in ('-1', '0', '16385', '4294967295', '1.5', ' 1200', '01200', 'true', ''):
            source = fixture()
            source[3][0].set('instance_buffer', value)
            before = signature(source)
            with self.subTest(value=value), self.assertRaises(ValueError):
                capacity.transform(source)
            self.assertEqual(signature(source), before)

    def test_changed_schema_is_refused(self):
        for mutation in ('root', 'style', 'duplicate', 'missing', 'traversal', 'attribute', 'child', 'text'):
            source = fixture()
            if mutation == 'root': source.tag = 'other'
            if mutation == 'style': source[3].set('name', 'different')
            if mutation == 'duplicate': source.append(copy.deepcopy(source[3]))
            if mutation == 'missing': source[3].remove(source[3][0])
            if mutation == 'traversal': source[3][0].set('name', 'split_scene')
            if mutation == 'attribute': source[3][0].set('other', '1')
            if mutation == 'child': ET.SubElement(source[3][0], 'other')
            if mutation == 'text': source[3][0].text = 'unexpected'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                capacity.transform(source)

    def test_unrelated_roundtrip_change_is_refused(self):
        expected, _ = capacity.transform(fixture())
        actual = ET.fromstring(ET.tostring(expected))
        verify_roundtrip(expected, actual)
        actual[0][1].set('instance_buffer', '101')
        with self.assertRaises(ValueError):
            verify_roundtrip(expected, actual)

    def test_changed_installed_baseline_creates_no_output(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            game = root / 'game'
            source = game / capacity.RELATIVE_PATH
            source.parent.mkdir(parents=True)
            source.write_bytes(b'x' * 5972)
            output = root / 'prepared'
            converter = root / 'converter.exe'
            with patch.object(capacity, 'digest', side_effect=[capacity.SUPPORTED_AVX_SHA256, 'changed']):
                with self.assertRaisesRegex(ValueError, 'baseline'):
                    capacity.prepare(game, output, converter)
            self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main()
