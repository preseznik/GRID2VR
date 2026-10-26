"""Prepare a guarded native ornament-capacity input; never install or launch.

The native factory copies main_scene to lane 14 and conditionally lane 1 too.
Those aliases remain native. Only the high style's XML main_scene value changes.
"""
from game_executable import validate_avx
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from practice_entry import CONVERTER_SHA256, signature, verify_roundtrip
from session_config import SUPPORTED_AVX_SHA256, plain_path

RELATIVE_PATH = Path('tracks/generic/ornament_system_settings.xml')
ORIGINAL_SHA256 = 'e838736a38d9de5a83c5fa966c37805c3f9097a4ec193b0dc8602983cf8b9737'
MODIFIED_SHA256 = 'e11fd1dc2c9b27a9fd5fdf4c2755999febdcdb3e20ad37bffae3f5fc60d51e3e'
CAPACITY = 4096
MAX_CAPACITY = 16384
STYLES = ('default', 'night', 'test', 'high', 'medium', 'low', 'la_stadium',
          'battersea_night', 'la_stadium_night', 'monaco')
TRAVERSALS = ('main_scene', 'shadow_1', 'shadow_2', 'shadow_3', 'envmap_0',
              'envmap_1', 'envmap_2', 'envmap_3', 'envmap_4', 'envmap_5',
              'rear_view', 'reflection', 'track_reflection')
SHORT_STYLES = {'la_stadium', 'battersea_night', 'la_stadium_night', 'monaco'}


def digest(path):
    return hashlib.sha256(plain_path(path).read_bytes()).hexdigest()


def transform(root):
    """Return a copy and one bounded high/main_scene change, or a semantic no-op."""
    if root.tag != 'ornament_system_settings' or root.attrib or (root.text or '').strip():
        raise ValueError('Unexpected ornament settings root')
    if tuple(style.get('name') for style in root) != STYLES:
        raise ValueError('Unexpected ornament setting styles')
    for style in root:
        if style.tag != 'setting_style' or set(style.attrib) != {'name'} or (style.text or '').strip():
            raise ValueError('Unexpected ornament style schema')
        expected = TRAVERSALS[:-1] if style.get('name') in SHORT_STYLES else TRAVERSALS
        if tuple(node.get('name') for node in style) != expected:
            raise ValueError('Unexpected ornament traversal settings')
        for node in style:
            if (node.tag != 'traversal_settings' or set(node.attrib) != {'name', 'instance_buffer'}
                    or len(node) or (node.text or '').strip()):
                raise ValueError('Unexpected ornament traversal schema')
            value = node.get('instance_buffer')
            if not re.fullmatch(r'0|[1-9][0-9]{0,4}', value) or not 0 <= int(value) <= MAX_CAPACITY:
                raise ValueError('Invalid native instance capacity')
            if node.get('name') == 'main_scene' and not int(value):
                raise ValueError('Main scene capacity must be positive')
    result = copy.deepcopy(root)
    node = result.find("setting_style[@name='high']/traversal_settings[@name='main_scene']")
    before = int(node.get('instance_buffer'))
    changes = []
    if before < CAPACITY:
        node.set('instance_buffer', str(CAPACITY))
        changes.append(dict(style='high', traversal='main_scene', attribute='instance_buffer',
                            before=before, after=CAPACITY))
    return result, changes


def prepare(game_root, output, converter):
    game, output, converter = map(plain_path, (game_root, output, converter))
    if output.is_relative_to(game) or output.exists():
        raise ValueError('Use a new output directory outside the game')
    source = plain_path(game / RELATIVE_PATH)
    executable_hash = validate_avx(game / 'grid2_avx.exe', SUPPORTED_AVX_SHA256)
    if source.stat().st_size != 5972 or digest(source) != ORIGINAL_SHA256:
        raise ValueError('Installed ornament settings differ from the supported baseline')
    if digest(converter) != CONVERTER_SHA256:
        raise ValueError('Unsupported converter fingerprint')
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != ORIGINAL_SHA256:
        raise ValueError('Ornament settings changed during preparation')
    output.mkdir(parents=True)
    (output / 'original-ornament-system.bin').write_bytes(original)

    def convert(mode, source_path, target_path):
        subprocess.run([str(converter), mode, str(source_path), str(target_path)], check=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

    convert('decode', output / 'original-ornament-system.bin', output / 'source-ornament-system.xml')
    source_root = ET.parse(output / 'source-ornament-system.xml').getroot()
    before_signature = signature(source_root)
    expected, changes = transform(source_root)
    if changes != [dict(style='high', traversal='main_scene', attribute='instance_buffer', before=1200, after=CAPACITY)]:
        raise ValueError('Supported baseline did not produce the exact capacity change')
    if signature(source_root) != before_signature:
        raise ValueError('Preparation changed the source tree')
    text = output / 'candidate-ornament-system.xml'
    target = output / 'ornament-system-capacity.bin'
    ET.ElementTree(expected).write(text, encoding='utf-8', xml_declaration=True)
    convert('encode', text, target)
    convert('decode', target, output / 'roundtrip-ornament-system.xml')
    verify_roundtrip(expected, ET.parse(output / 'roundtrip-ornament-system.xml').getroot())
    if digest(target) != MODIFIED_SHA256:
        raise ValueError('Encoded capacity candidate fingerprint changed')
    if digest(source) != ORIGINAL_SHA256:
        raise ValueError('Installed ornament settings changed during preparation')
    result = dict(schema=1, gameRoot=str(game), relativePath=RELATIVE_PATH.as_posix(),
                  executableSha256=executable_hash, originalSha256=ORIGINAL_SHA256,
                  modifiedSha256=digest(target), converterSha256=CONVERTER_SHA256,
                  changes=changes, semanticRoundtrip=True, installed=False,
                  nativeAliases='Factory also copies main_scene to lane14 and conditionally lane1; unchanged native behavior.')
    (output / 'instance-capacity-plan.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--converter', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.game_root, args.output, args.converter), indent=2))
