"""Protect the accepted VR profile while preparing reversible graphics choices."""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import graphics_profile as graphics
import graphics_catalog as catalog_tools

ROOT = Path(__file__).parents[1]
LEGACY_OPTIONS = {'TreeDetail': 'tree_detail', 'ObjectDetail': 'object_detail',
                  'CrowdDetail': 'crowd_detail', 'TextureFiltering': 'texture_filtering'}
# Independently authored value tuples, not expectations generated from the catalog.
# Attribute order here is intentional and also checks complete native option tuples.
FIXTURES = {
    'NightLighting': ('night_lighting@volumes night_lighting@lights night_lighting@shadows',
                      {'low': 'false 150 false', 'medium': 'true 0 false', 'high': 'true 0 true'}),
    'Shadows': ('shadows@enabled shadows@size shadows@maskQuality shadows@particles',
                {'ultralow': 'false 512 0 false', 'low': 'true 768 0 false', 'medium': 'true 1024 0 false',
                 'high': 'true 2048 1 true', 'ultra': 'true 2048 2 true'}),
    'AdvancedFog': ('advanced_fog@enabled', {'off': 'false', 'on': 'true'}),
    'Particles': ('particles@enabled particles@wind particles@dynamicRes particles@lowResScalar',
                  {'low': 'true false true 4', 'medium': 'true true true 4',
                   'high': 'true true false 4', 'ultra': 'true true false 2'}),
    'CrowdDetail': ('crowd@enabled crowd@detail', {'off': 'false 0', 'low': 'true 0', 'medium': 'true 1',
                                               'high': 'true 2', 'ultra': 'true 3'}),
    'Cloth': ('cloth@enabled cloth@tessellation', {'off': 'false false', 'low': 'true false', 'high': 'true true'}),
    'AmbientOcclusion': ('dynamic_ambient_occ@enabled dynamic_ambient_occ@quality',
                        {'low': 'true 0', 'high': 'true 1', 'ultra': 'true 2'}),
    'SoftAmbientOcclusion': ('dynamic_ambient_occ_soft@enabled', {'off': 'false', 'on': 'true'}),
    'GroundCover': ('groundcover@mode groundcover@clutter', {'off': 'off false', 'low': 'atoc false', 'high': 'blended true'}),
    'VehicleDetail': ('vehicles@lodQuality vehicles@characterQuality vehicles@hires_ai_livery',
                      {'low': '0 0 0', 'medium': '1 1 1', 'high': '2 2 1'}),
    'TreeDetail': ('trees@lod trees@maxlod', {'medium': '1.0 0', 'high': '1.25 0', 'ultra': '1.5 0'}),
    'ObjectDetail': ('objects@lod objects@maxlod', {'medium': '1.0 0', 'high': '1.25 0', 'ultra': '1.5 0'}),
    'CarReflections': ('envmap@faces envmap@size envmap@drawallobjects envmap@scaleminsize envmap@mindistance',
                       {'low': '0 256 false 1.0 0.0', 'medium': '6 256 false 1.5 0.0',
                        'high': '6 256 false 1.0 0.0', 'ultra': '6 1024 true 1.0 200.0'}),
    'Water': ('water@update water@detail water@scale water@tessellation',
              {'low': 'false 0 8 false', 'medium': 'true 1 8 true', 'high': 'true 2 2 true'}),
    'PostProcessing': ('postprocess@quality', {'low': '0', 'medium': '1', 'high': '2'}),
    'Skidmarks': ('skidmarks@enabled', {'off': 'false', 'on': 'true'}),
    'AdvancedLighting': ('graphics_card/advanced@forward', {'off': 'false', 'on': 'true'}),
    'GlobalIllumination': ('graphics_card/advanced@global_illumination', {'off': 'false', 'on': 'true'}),
    'TextureFiltering': ('anisotropic@min anisotropic@max', {'off': '1 1', 'low': '1 2', 'high': '1 8', 'ultra': '4 16'}),
    'IntelAOIT': ('graphics_card/intel@tree_aoit', {'off': 'false', 'on': 'true'}),
    'IntelAVSM': ('graphics_card/intel@particle_avsm', {'off': 'false', 'on': 'true'}),
}
DEFAULT_CHOICES = dict(zip(FIXTURES, 'high ultra on ultra ultra high ultra on high high ultra ultra ultra high high on on on ultra off off'.split(), strict=True))
PRESET_FIXTURES = {
    'ultralow': 'low ultralow off low off off low off off low medium medium low low low off off off off off off',
    'low': 'low low off low low low low off low low medium medium low low low off off off off off off',
    'medium': 'medium medium off medium medium high high off high medium medium medium medium medium medium on off off low on off',
    'high': 'high high on high high high high off high high high high high medium high on off off high on on',
    'ultra': 'high ultra on ultra ultra high ultra off high high ultra ultra ultra high high on on on ultra on on',
}
NATIVE_CATEGORIES = dict(zip(FIXTURES, 'night_lighting shadows advanced_fog particles crowd cloth dynamic_ambient_occ dynamic_ambient_occ_soft groundCover vehicles trees objects car_reflection water post skidmarks advanced_lighting global_illumination anisotropic_filtering intel_aoit intel_avsm'.split(), strict=True))


def encode_quality(values):
    return base64.b64encode(json.dumps(values, separators=(',', ':')).encode('utf-8')).decode('ascii')


def expected_mapping(field, choice):
    targets, options = FIXTURES[field]
    return dict(zip(targets.split(), options[choice].split(), strict=True))


def expected_attributes(choices):
    return {('/' + target.split('@')[0], target.split('@')[1]): value
            for field, choice in choices.items() for target, value in expected_mapping(field, choice).items()}


def prepare_options(choices):
    return {**{argument: choices[field] for field, argument in LEGACY_OPTIONS.items() if field in choices},
            'quality_base64': encode_quality({field: choice for field, choice in choices.items() if field not in LEGACY_OPTIONS})}


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
        self.catalog, self.catalog_hash = catalog_tools.load_catalog()

    def prepare(self, **options):
        self.counter += 1
        output = self.folder / f'profile-{self.counter}.xml'
        receipt = graphics.prepare(output, **options)
        payload = output.read_bytes()
        self.assertEqual(hashlib.sha256(payload).hexdigest(), receipt['sha256'])
        return payload, receipt

    def test_defaults_preserve_exact_accepted_bytes_in_both_modes(self):
        for mode in ('VR', 'Desktop'):
            # A launcher sends all 17 new defaults, including unavailable Intel off.
            for options in ({}, prepare_options(DEFAULT_CHOICES)):
                payload, receipt = self.prepare(mode=mode, **options)
                self.assertEqual(payload, self.baseline)
                self.assertTrue(receipt['isBaseline'])
                self.assertEqual(receipt['changes'], [])
                self.assertEqual(receipt['qualityChoices'], DEFAULT_CHOICES)
                self.assertEqual(receipt['catalogSha256'], self.catalog_hash)
                self.assertEqual(self.catalog_hash, hashlib.sha256(catalog_tools.CATALOG_PATH.read_bytes()).hexdigest())
                self.assertLessEqual(len(json.dumps(receipt, indent=2).encode('utf-8')), graphics.MAX_RECEIPT_BYTES)

    def test_all_catalog_choices_match_independent_native_tuples(self):
        categories = {category['field']: category for category in self.catalog['categories']}
        self.assertEqual(set(categories), set(FIXTURES))
        for field, (_, options) in FIXTURES.items():
            self.assertEqual(categories[field]['default'], DEFAULT_CHOICES[field])
            self.assertEqual({item['id'] for item in categories[field]['choices']}, set(options))
            self.assertEqual(categories[field].get('available', True), field not in ('IntelAOIT', 'IntelAVSM'))
            for choice in categories[field]['choices']:
                with self.subTest(field=field, choice=choice['id']):
                    self.assertEqual(choice['attributes'], expected_mapping(field, choice['id']))
                    selected = prepare_options({field: choice['id']})
                    if field in ('IntelAOIT', 'IntelAVSM') and choice['id'] != 'off':
                        output = self.folder / 'unavailable.xml'
                        with self.assertRaisesRegex(ValueError, 'unavailable'):
                            graphics.prepare(output, **selected)
                        self.assertFalse(output.exists())
                    else:
                        for mode in ('VR', 'Desktop'):
                            payload, receipt = self.prepare(mode=mode, **selected)
                            self.assertEqual(attributes(payload), attributes(self.baseline) |
                                             expected_attributes({field: choice['id']}))
                            self.assertEqual(receipt['qualityChoices'], DEFAULT_CHOICES | {field: choice['id']})

    def test_every_native_preset_matches_independent_quality_expectations(self):
        original = copy.deepcopy(self.catalog)
        self.assertEqual({preset['id'] for preset in self.catalog['presets']}, set(PRESET_FIXTURES))
        for preset in self.catalog['presets']:
            with self.subTest(preset=preset['id']):
                expected = dict(zip(FIXTURES, PRESET_FIXTURES[preset['id']].split(), strict=True))
                self.assertEqual(preset['values'], expected)
                expected.update(IntelAOIT='off', IntelAVSM='off')
                choices = catalog_tools.normalize_preset(self.catalog, preset['id'])
                self.assertEqual(choices, expected)
                for mode in ('VR', 'Desktop'):
                    payload, receipt = self.prepare(mode=mode, **prepare_options(choices))
                    self.assertEqual(attributes(payload), attributes(self.baseline) | expected_attributes(expected))
                    self.assertEqual(receipt['qualityChoices'], expected)
                    allowed = set(expected_attributes(expected))
                    self.assertTrue({('/' + item['path'], item['attribute']) for item in receipt['changes']} <= allowed)
        self.assertEqual(self.catalog, original)  # Normalization must not rewrite authored native presets.
        for value in ('auto', '', None, []):
            with self.assertRaises(ValueError):
                catalog_tools.normalize_preset(self.catalog, value)

    def test_ultralow_minimal_independent_quality_tuples(self):
        choices = catalog_tools.normalize_preset(self.catalog, 'ultralow')
        payload, _ = self.prepare(**prepare_options(choices))
        root = ET.fromstring(payload)
        for path, expected in (
                ('shadows', ('false', '512', '0', 'false')),
                ('particles', ('true', 'false', 'true', '4')),
                ('crowd', ('false', '0')),
                ('vehicles', ('0', '0', '0')),
                ('envmap', ('0', '256', 'false', '1.0', '0.0')),
                ('water', ('false', '0', '8', 'false'))):
            keys = {'shadows': 'enabled size maskQuality particles', 'particles': 'enabled wind dynamicRes lowResScalar',
                    'crowd': 'enabled detail', 'vehicles': 'characterQuality lodQuality hires_ai_livery',
                    'envmap': 'faces size drawallobjects scaleminsize mindistance', 'water': 'update detail scale tessellation'}[path]
            self.assertEqual(tuple(root.find(path).get(key) for key in keys.split()), expected)
        self.assertEqual(root.find('cpu/threadStrategy').get('parallelUpdateRender'), 'false')
        self.assertEqual(root.find('graphics_card/resolution').get('width'), '1280')

    def test_optional_archived_ultralow_quality_only_audit(self):
        archive = Path(os.environ.get('GRID2VR_ULTRALOW_AUDIT',
                       str(ROOT / 'artifacts/logs/20261028-100743-811/backup/game-written-settings.xml')))
        if not archive.is_file():
            self.skipTest('Archived 20261028-100743-811 profile is optional')
        self.assertLessEqual(archive.stat().st_size, 16384)
        original = archive.read_bytes()
        archived = attributes(original)
        choices = catalog_tools.normalize_preset(self.catalog, 'ultralow')
        payload, _ = self.prepare(**prepare_options(choices))
        quality = expected_attributes(choices)
        self.assertEqual({key: archived[key] for key in quality}, quality)
        self.assertEqual({key: attributes(payload)[key] for key in quality}, quality)
        self.assertEqual(archive.read_bytes(), original)

    def test_optional_installed_native_catalog_read_only_audit(self):
        source = Path(os.environ.get('GRID2VR_NATIVE_OPTIONS_AUDIT',
                      r'E:\SteamLibrary\steamapps\common\grid 2\system\hardware_settings_options.xml'))
        if not source.is_file():
            self.skipTest('Installed native options are optional')
        self.assertLessEqual(source.stat().st_size, 512 * 1024)
        original = source.read_bytes()
        self.assertEqual(hashlib.sha256(original).hexdigest(), self.catalog['sourceOptionsSha256'])
        native = ET.fromstring(original)
        exposed = native.find("category[@name='exposed_configurations']/quality_set[@name='quality']")
        self.assertEqual({item.get('category') for item in exposed}, set(NATIVE_CATEGORIES.values()))
        for category in self.catalog['categories']:
            native_category = native.find("category[@name='" + NATIVE_CATEGORIES[category['field']] + "']")
            for choice in category['choices']:
                option = native_category.find("option[@name='" + choice['id'] + "']")
                self.assertIsNotNone(option)
                mapped = {}
                def visit(node, prefix=''):
                    for child in node:
                        path = prefix + child.tag
                        mapped.update({path + '@' + key: value for key, value in child.attrib.items()})
                        visit(child, path + '/')
                visit(option)
                self.assertEqual(choice['attributes'], mapped)
        reverse = {value: key for key, value in NATIVE_CATEGORIES.items()}
        for preset in self.catalog['presets']:
            option = native.find("category[@name='graphicsDetail']/option[@name='" + preset['id'] + "']")
            values = {}
            for ref in option.findall('ref'):
                if ref.get('category') in reverse:
                    case = ref.find("case[@dx11Only='true']")
                    values[reverse[ref.get('category')]] = case.get('option') if case is not None else ref.get('option')
            self.assertEqual(preset['values'], values)  # Display, firstBoot and physics refs never enter quality.
        self.assertEqual(source.read_bytes(), original)

    def test_quality_cannot_override_legacy_fields_or_display(self):
        # Non-default new quality never overrides original flags (including their implicit ultra defaults).
        values = {field: choice for field, choice in catalog_tools.normalize_preset(self.catalog, 'ultralow').items()
                  if field not in LEGACY_OPTIONS}
        payload, receipt = self.prepare(quality_base64=encode_quality(values), tree_detail='high', crowd_detail='off')
        expected = DEFAULT_CHOICES | values | {'TreeDetail': 'high', 'CrowdDetail': 'off'}
        self.assertEqual(receipt['qualityChoices'], expected)
        self.assertEqual(attributes(payload), attributes(self.baseline) | expected_attributes(expected))

    def test_quality_and_resolution_remain_separate(self):
        choices = catalog_tools.normalize_preset(self.catalog, 'low')
        expected = expected_attributes(choices)
        payload, _ = self.prepare(scene_width=2400, scene_height=2600, vr_msaa='4xmsaa', **prepare_options(choices))
        expected.update({('/graphics_card/resolution', 'width'): '2400', ('/graphics_card/resolution', 'height'): '2600',
                         ('/graphics_card/resolution', 'multisampling'): '4xmsaa'})
        self.assertEqual(attributes(payload), attributes(self.baseline) | expected)

    def test_quality_transport_invalid_inputs_never_write(self):
        raw_json = [b'[]', b'null', b'1', b'"off"', b'{', b'\xff',
                    b'{"Shadows":"low","Shadows":"high"}', b'{"Shadows":"low","Shad\\u006fws":"high"}',
                    b'{"Shadows":{"id":"low","id":"ultra"}}', b'{"Shadows":NaN}',
                    b'{"Shadows":Infinity}', b'[' * 1000 + b']' * 1000]
        invalid = [None, False, 17, {}, [], b'e30=', '!!!', 'e30=\n', 'e30===', 'e31=', 'e30', 'é',
                   'A' * 4097, base64.b64encode(b' ' * 3073).decode('ascii')]
        invalid += [base64.b64encode(value).decode('ascii') for value in raw_json]
        invalid += [encode_quality(value) for value in [
            {'Unknown': 'low'}, {'shadows': 'low'}, {'Shadows': 'Low'}, {'Shadows': 'off'},
            {'Shadows': 0}, {'Shadows': False}, {'Shadows': None}, {'Shadows': ['low']},
            {'Shadows': {'id': 'low'}}, {'Shadows': 'x' * 300}, {'IntelAOIT': 'on'}, {'IntelAVSM': 'on'},
            {'TreeDetail': 'ultra'}, {'ObjectDetail': 'medium'}, {'CrowdDetail': 'off'}, {'TextureFiltering': 'off'},
            {'source': str(self.folder / 'foreign.xml')}, {'output': str(self.folder / 'foreign.xml')},
            {'graphics_card/resolution@width': '640'}, {'cpu/threadStrategy@parallelUpdateRender': 'true'},
            {'physics@vehicleDamage': 'false'}, {'input@device_type': 'wheel'}, {'game_root': str(self.folder)}]]
        for index, value in enumerate(invalid):
            output = self.folder / 'invalid-quality.xml'
            with self.subTest(index=index), self.assertRaises(ValueError):
                graphics.prepare(output, quality_base64=value)
            self.assertFalse(output.exists())
        # The exact encoded bound is accepted, not silently truncated.
        boundary = base64.b64encode(b'{}' + b' ' * 3070).decode('ascii')
        self.assertEqual(len(boundary), 4096)
        payload, _ = self.prepare(quality_base64=boundary)
        self.assertEqual(payload, self.baseline)

    def test_invalid_catalog_mappings_and_defaults_never_write(self):
        def target_mutation(target):
            changed = copy.deepcopy(self.catalog)
            changed['categories'][0]['choices'][0]['attributes'][target] = 'true'
            return changed
        cases = [target_mutation(target) for target in ('cpu/threadStrategy@parallelUpdateRender', 'input@device_type',
                 'physics@vehicleDamage', 'graphics_card/resolution@width', 'graphics_card@firstBoot',
                 'graphics_card/stereo@enabled', '../cpu@enabled')]
        for mutation in ('duplicate-field', 'duplicate-choice', 'unknown-field', 'missing-field', 'missing-attribute',
                         'invalid-value', 'wrong-default', 'intel-available', 'intel-default', 'intel-off-true',
                         'unknown-preset-choice', 'missing-preset-field', 'schema-bool'):
            changed = copy.deepcopy(self.catalog)
            category = changed['categories'][0]
            if mutation == 'duplicate-field': changed['categories'][-1] = copy.deepcopy(category)
            if mutation == 'duplicate-choice': category['choices'].append(copy.deepcopy(category['choices'][0]))
            if mutation == 'unknown-field': category['field'] = 'CPU'
            if mutation == 'missing-field': changed['categories'].pop()
            if mutation == 'missing-attribute': category['choices'][0]['attributes'].pop('night_lighting@volumes')
            if mutation == 'invalid-value': category['choices'][0]['attributes']['night_lighting@lights'] = 150
            if mutation == 'wrong-default': category['default'] = 'low'
            if mutation == 'intel-available': changed['categories'][-1]['available'] = True
            if mutation == 'intel-default': changed['categories'][-1]['default'] = 'on'
            if mutation == 'intel-off-true': changed['categories'][-1]['choices'][0]['attributes']['graphics_card/intel@particle_avsm'] = 'true'
            if mutation == 'unknown-preset-choice': changed['presets'][0]['values']['Shadows'] = 'bogus'
            if mutation == 'missing-preset-field': changed['presets'][0]['values'].pop('Shadows')
            if mutation == 'schema-bool': changed['schema'] = True
            cases.append(changed)
        encoded = [json.dumps(case).encode('utf-8') for case in cases]
        encoded += [b' ' * (catalog_tools.MAX_CATALOG_BYTES + 1), b'{"schema":1,"schema":1}', b'\xff', b'[]']
        for index, payload in enumerate(encoded):
            source = self.folder / 'catalog.json'
            source.write_bytes(payload)
            output = self.folder / 'invalid-catalog.xml'
            with self.subTest(index=index), mock.patch.object(graphics, 'load_catalog',
                    side_effect=lambda: catalog_tools.load_catalog(source)), self.assertRaises(ValueError):
                graphics.prepare(output)
            self.assertFalse(output.exists())

    def test_cli_quality_transport_and_crowd_off(self):
        output = self.folder / 'cli.xml'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/graphics_profile.py'), '--output', str(output),
                                 '--crowd-detail', 'off', '--quality-base64', encode_quality({'Shadows': 'low'})],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        receipt = json.loads(result.stdout)
        self.assertEqual(receipt['qualityChoices'], DEFAULT_CHOICES | {'Shadows': 'low', 'CrowdDetail': 'off'})
        self.assertEqual(attributes(output.read_bytes()), attributes(self.baseline) |
                         expected_attributes({'Shadows': 'low', 'CrowdDetail': 'off'}))
        rejected = self.folder / 'cli-rejected.xml'
        result = subprocess.run([sys.executable, str(ROOT / 'tools/graphics_profile.py'), '--output', str(rejected),
                                 '--quality-base64', encode_quality({'IntelAOIT': 'on'})],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 1)
        self.assertIn('unavailable', result.stderr)
        self.assertFalse(rejected.exists())

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
                             ('crowd_detail', ('off', 'low', 'medium', 'high', 'ultra')), ('texture_filtering', ('off', 'low', 'high', 'ultra'))):
            for value in values:
                self.prepare(**{name: value})

    def test_invalid_choices_write_nothing(self):
        for options in ({'render_scale': 49}, {'render_scale': 301}, {'render_scale': 150.5}, {'render_scale': True}, {'desktop_vsync': 2}, {'mode': 'Headset'},
                        {'desktop_msaa': '16xmsaa'}, {'vr_msaa': '16xmsaa'}, {'tree_detail': 'low'}, {'object_detail': 'off'},
                        {'crowd_detail': 'none'}, {'texture_filtering': 'custom'}, {'tree_detail': []}, {'crowd_detail': False},
                        {'desktop_msaa': {}}, {'scene_width': False}, {'scene_height': None}):
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
