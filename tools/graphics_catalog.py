"""Bounded authored quality catalog and launcher transport; no installed settings imports."""
import base64
import binascii
import hashlib
import json
from pathlib import Path
import re

CATALOG_PATH = Path(__file__).with_name('data') / 'graphics-options.json'
MAX_CATALOG_BYTES = 65536
MAX_QUALITY_BASE64 = 4096
LEGACY_FIELDS = frozenset(('TreeDetail', 'ObjectDetail', 'CrowdDetail', 'TextureFiltering'))
UNAVAILABLE_FIELDS = frozenset(('IntelAOIT', 'IntelAVSM'))
# A catalog edit cannot grant access to CPU, input, physics, stereo or display policy.
QUALITY_ATTRIBUTES = {
    'NightLighting': ('night_lighting@volumes', 'night_lighting@lights', 'night_lighting@shadows'),
    'Shadows': ('shadows@enabled', 'shadows@size', 'shadows@maskQuality', 'shadows@particles'),
    'AdvancedFog': ('advanced_fog@enabled',),
    'Particles': ('particles@enabled', 'particles@wind', 'particles@dynamicRes', 'particles@lowResScalar'),
    'CrowdDetail': ('crowd@enabled', 'crowd@detail'),
    'Cloth': ('cloth@enabled', 'cloth@tessellation'),
    'AmbientOcclusion': ('dynamic_ambient_occ@enabled', 'dynamic_ambient_occ@quality'),
    'SoftAmbientOcclusion': ('dynamic_ambient_occ_soft@enabled',),
    'GroundCover': ('groundcover@mode', 'groundcover@clutter'),
    'VehicleDetail': ('vehicles@lodQuality', 'vehicles@characterQuality', 'vehicles@hires_ai_livery'),
    'TreeDetail': ('trees@lod', 'trees@maxlod'),
    'ObjectDetail': ('objects@lod', 'objects@maxlod'),
    'CarReflections': ('envmap@faces', 'envmap@size', 'envmap@drawallobjects', 'envmap@scaleminsize', 'envmap@mindistance'),
    'Water': ('water@update', 'water@detail', 'water@scale', 'water@tessellation'),
    'PostProcessing': ('postprocess@quality',),
    'Skidmarks': ('skidmarks@enabled',),
    'AdvancedLighting': ('graphics_card/advanced@forward',),
    'GlobalIllumination': ('graphics_card/advanced@global_illumination',),
    'TextureFiltering': ('anisotropic@min', 'anisotropic@max'),
    'IntelAOIT': ('graphics_card/intel@tree_aoit',),
    'IntelAVSM': ('graphics_card/intel@particle_avsm',),
}
TRANSPORT_FIELDS = frozenset(QUALITY_ATTRIBUTES) - LEGACY_FIELDS


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate graphics JSON key')
        result[key] = value
    return result


def _json(payload):
    try:
        return json.loads(payload.decode('utf-8'), object_pairs_hook=_unique_object,
                          parse_constant=lambda value: _invalid_constant())
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError('Malformed graphics JSON') from error


def _invalid_constant():
    raise ValueError('Non-finite graphics JSON value')


def _choice_id(value):
    return type(value) is str and re.fullmatch(r'[a-z][a-z0-9]{0,31}', value) is not None


def load_catalog(path=CATALOG_PATH):
    """Read authored data only; validate every write target, including unused choices."""
    with Path(path).open('rb') as stream:
        payload = stream.read(MAX_CATALOG_BYTES + 1)
    if len(payload) > MAX_CATALOG_BYTES:
        raise ValueError('Oversized graphics catalog')
    catalog = _json(payload)
    if (type(catalog) is not dict or type(catalog.get('schema')) is not int or catalog['schema'] != 1 or
            type(catalog.get('sourceOptionsSha256')) is not str or
            re.fullmatch(r'[0-9a-f]{64}', catalog['sourceOptionsSha256']) is None):
        raise ValueError('Unsupported graphics catalog')
    categories = catalog.get('categories')
    if type(categories) is not list or len(categories) != len(QUALITY_ATTRIBUTES):
        raise ValueError('Unsupported graphics categories')
    fields = {}
    for category in categories:
        if type(category) is not dict:
            raise ValueError('Malformed graphics category')
        field = category.get('field')
        if type(field) is not str or field not in QUALITY_ATTRIBUTES or field in fields:
            raise ValueError('Unknown or duplicate graphics category')
        available = category.get('available', True)
        if type(available) is not bool:
            raise ValueError('Malformed graphics availability')
        if field in UNAVAILABLE_FIELDS and (available or category.get('default') != 'off'):
            raise ValueError('Intel graphics extensions are unavailable')
        choices = category.get('choices')
        if type(choices) is not list or not 1 <= len(choices) <= 8:
            raise ValueError('Malformed graphics choices')
        ids = set()
        for choice in choices:
            if type(choice) is not dict or not _choice_id(choice.get('id')) or choice['id'] in ids:
                raise ValueError('Unknown or duplicate graphics choice')
            ids.add(choice['id'])
            attributes = choice.get('attributes')
            if type(attributes) is not dict or set(attributes) != set(QUALITY_ATTRIBUTES[field]):
                raise ValueError('Graphics mapping exceeds approved quality attributes')
            if any(type(value) is not str or re.fullmatch(r'[a-zA-Z0-9.\-]{1,32}', value) is None
                   for value in attributes.values()):
                raise ValueError('Malformed graphics attribute value')
        if not _choice_id(category.get('default')) or category['default'] not in ids:
            raise ValueError('Unknown graphics default')
        if field in UNAVAILABLE_FIELDS:
            default = next(choice for choice in choices if choice['id'] == 'off')
            if set(default['attributes'].values()) != {'false'}:
                raise ValueError('Intel graphics extensions must remain off')
        fields[field] = category
    presets = catalog.get('presets')
    if type(presets) is not list or not 1 <= len(presets) <= 16:
        raise ValueError('Malformed graphics presets')
    ids = set()
    for preset in presets:
        if type(preset) is not dict or not _choice_id(preset.get('id')) or preset['id'] in ids:
            raise ValueError('Unknown or duplicate graphics preset')
        ids.add(preset['id'])
        values = preset.get('values')
        if type(values) is not dict or set(values) != set(fields):
            raise ValueError('Incomplete graphics preset')
        for field, value in values.items():
            if type(value) is not str or value not in {choice['id'] for choice in fields[field]['choices']}:
                raise ValueError('Unknown preset graphics choice')
    return catalog, hashlib.sha256(payload).hexdigest()


def normalize_preset(catalog, preset_id):
    """Native Intel preset requests never override the capability gate."""
    if not _choice_id(preset_id):
        raise ValueError('Unknown graphics preset')
    preset = next((item for item in catalog['presets'] if item['id'] == preset_id), None)
    if preset is None:
        raise ValueError('Unknown graphics preset')
    return {category['field']: (preset['values'][category['field']] if category.get('available', True)
                               else category['default']) for category in catalog['categories']}


def decode_quality(quality_base64, catalog):
    """Only the 17 new field IDs cross this transport; paths and legacy IDs cannot."""
    if type(quality_base64) is not str or len(quality_base64) > MAX_QUALITY_BASE64:
        raise ValueError('Invalid or oversized graphics quality transport')
    if not quality_base64:
        return {}
    try:
        encoded = quality_base64.encode('ascii')
        payload = base64.b64decode(encoded, validate=True)
    except (UnicodeError, binascii.Error) as error:
        raise ValueError('Malformed graphics quality base64') from error
    if base64.b64encode(payload) != encoded:
        raise ValueError('Non-canonical graphics quality base64')
    values = _json(payload)
    if type(values) is not dict or not set(values) <= TRANSPORT_FIELDS:
        raise ValueError('Unknown graphics quality fields')
    categories = {category['field']: category for category in catalog['categories']}
    for field, value in values.items():
        category = categories[field]
        if type(value) is not str or value not in {choice['id'] for choice in category['choices']}:
            raise ValueError('Unsupported graphics quality choice')
        if not category.get('available', True) and value != category['default']:
            raise ValueError('Graphics quality choice is unavailable: ' + field)
    return values
