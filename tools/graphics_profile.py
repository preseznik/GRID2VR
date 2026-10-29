"""Prepare a private hardware profile; never deploy it or change installed settings."""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

from session_config import plain_path
from graphics_catalog import decode_quality, load_catalog

BASELINE_SHA256 = '8597ff39855c5a18e721c932ebe170c21ecd283d2d40462fff9e4c94275d2747'
RENDER_SCALES = range(50, 301)
MSAA = ('off', '2xmsaa', '4xmsaa', '8xmsaa')
DETAIL = {'medium': '1.0', 'high': '1.25', 'ultra': '1.5'}
CROWD = {'off': '0', 'low': '0', 'medium': '1', 'high': '2', 'ultra': '3'}
FILTERING = {'off': ('1', '1'), 'low': ('1', '2'), 'high': ('1', '8'), 'ultra': ('4', '16')}
MAX_RECEIPT_BYTES = 32768


def prepare(output, mode='VR', render_scale=100, desktop_vsync=1, desktop_msaa='off',
            tree_detail='ultra', object_detail='ultra', crowd_detail='ultra', texture_filtering='ultra',
            source=Path(__file__).with_name('practice-settings.xml'),
            game_root=Path(r'E:\SteamLibrary\steamapps\common\grid 2'), vr_msaa='off', scene_width=0, scene_height=0,
            quality_base64=''):
    if any(type(value) is not str for value in (mode, desktop_msaa, vr_msaa, tree_detail, object_detail,
                                               crowd_detail, texture_filtering)):
        raise ValueError('Unsupported graphics choice')
    if (mode not in ('VR', 'Desktop') or type(render_scale) is not int or render_scale not in RENDER_SCALES or
            type(desktop_vsync) is not int or desktop_vsync not in (0, 1) or desktop_msaa not in MSAA or vr_msaa not in MSAA or
            tree_detail not in DETAIL or object_detail not in DETAIL or crowd_detail not in CROWD or texture_filtering not in FILTERING):
        raise ValueError('Unsupported graphics choice')
    if type(scene_width) is not int or type(scene_height) is not int:
        raise ValueError('Unsupported headset-relative scene size')
    if scene_width or scene_height:
        if (mode != 'VR' or type(scene_width) is not int or type(scene_height) is not int or
                not 320 <= scene_width <= 8192 or not 180 <= scene_height <= 8192 or scene_width * scene_height > 64 * 1024 * 1024):
            raise ValueError('Unsupported headset-relative scene size')
    else:
        scene_width, scene_height = 1280 * render_scale // 100, 720 * render_scale // 100
    catalog, catalog_sha256 = load_catalog()
    choices = {category['field']: category['default'] for category in catalog['categories']}
    choices.update(decode_quality(quality_base64, catalog))
    # Existing flags are the sole authority for the original four fields.
    choices.update(TreeDetail=tree_detail, ObjectDetail=object_detail, CrowdDetail=crowd_detail,
                   TextureFiltering=texture_filtering)
    source, output, game_root = map(plain_path, (source, output, game_root))
    if output.is_relative_to(game_root) or output == source:
        raise ValueError('Graphics output must be outside the game and baseline profile')
    if source.stat().st_size > 16384:
        raise ValueError('Oversized baseline profile')
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != BASELINE_SHA256:
        raise ValueError('Baseline graphics profile changed; review it before preparing a session')
    root = ET.fromstring(original)
    for category in catalog['categories']:
        default = next(choice for choice in category['choices'] if choice['id'] == category['default'])
        for target, value in default['attributes'].items():
            path, attribute = target.split('@')
            node = root.find(path)
            if node is None or node.get(attribute) != value:
                raise ValueError('Graphics catalog defaults differ from the accepted baseline')
    changes = []

    def change(path, **attributes):
        node = root.find(path)
        if node is None:
            raise ValueError('Baseline is missing ' + path)
        for key, value in attributes.items():
            if key not in node.attrib:
                raise ValueError('Baseline is missing graphics attribute')
            if node.get(key) != value:
                changes.append(dict(path=path, attribute=key, before=node.get(key), after=value))
                node.set(key, value)

    change('graphics_card/resolution', width=str(scene_width), height=str(scene_height))
    # Each mode owns its AA choice; VR keeps the accepted VSync policy.
    if mode == 'Desktop':
        change('graphics_card/resolution', vsync=str(desktop_vsync), multisampling=desktop_msaa)
    else:
        change('graphics_card/resolution', multisampling=vr_msaa)
    for category in catalog['categories']:
        choice = next((item for item in category['choices'] if item['id'] == choices[category['field']]), None)
        if choice is None:
            raise ValueError('Unsupported graphics quality choice')
        for target, value in choice['attributes'].items():
            path, attribute = target.split('@')
            change(path, **{attribute: value})
    # Preserve the exact accepted profile bytes when all effective settings match.
    payload = ET.tostring(root, encoding='utf-8', xml_declaration=True) + b'\n' if changes else original
    receipt = dict(schema=1, configuration=str(output), sha256=hashlib.sha256(payload).hexdigest(),
                baselineSha256=BASELINE_SHA256, isBaseline=not changes, mode=mode, changes=changes,
                catalogSha256=catalog_sha256, qualityChoices=choices,
                resolution=dict(root.find('graphics_card/resolution').attrib),
                installed=False, limits='Configuration preparation only; non-default graphics require game/headset acceptance.')
    if len(json.dumps(receipt, indent=2).encode('utf-8')) > MAX_RECEIPT_BYTES:
        raise ValueError('Oversized graphics receipt')
    with output.open('xb') as stream:
        stream.write(payload)
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, default=Path(r'E:\SteamLibrary\steamapps\common\grid 2'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=('VR', 'Desktop'), default='VR')
    parser.add_argument('--render-scale', type=int, choices=RENDER_SCALES, default=100)
    parser.add_argument('--scene-width', type=int, default=0)
    parser.add_argument('--scene-height', type=int, default=0)
    parser.add_argument('--desktop-vsync', type=int, choices=(0, 1), default=1)
    parser.add_argument('--desktop-msaa', choices=MSAA, default='off')
    parser.add_argument('--vr-msaa', choices=MSAA, default='off')
    parser.add_argument('--tree-detail', choices=DETAIL, default='ultra')
    parser.add_argument('--object-detail', choices=DETAIL, default='ultra')
    parser.add_argument('--crowd-detail', choices=CROWD, default='ultra')
    parser.add_argument('--texture-filtering', choices=FILTERING, default='ultra')
    parser.add_argument('--quality-base64', default='')
    try:
        result = prepare(**vars(parser.parse_args()))
    except (ValueError, OSError, ET.ParseError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
