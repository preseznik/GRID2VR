"""Prepare an offline, guarded normal-race entry and startup-skip route.

This does not install anything or claim player control. The native attract loader
is retained; only its choice of the recorded cutscene is redirected to the
existing normal start sequence. Startup skip uses the matching normal route.
Run deployment must back up and recover flow.bin.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from session_config import SUPPORTED_AVX_SHA256, plain_path

FLOW_SHA256 = '8b2f7fce029ed2fe2e82d837d0e180f14347e14737f71566b0a318c46bd32894'
STATES_SHA256 = '07e593b3891171a7c4caa67b0b2e0ee1a9c166fc2d4bd25814ad695c439d9adc'
PRACTICE_SHA256 = 'f2dc0085047a3cf29feac15e62002d9866c71261a7aa325ebb27571ed8b07d24'
CONVERTER_SHA256 = '4448ec5a685b30beeb27cf21f6492ae4f4ad64d137c19d354b678bf52f10d029'
CHANGES = [dict(node='2Ic', link='yes', before='2Id', after='2Iy'),
           dict(node='2Im', link='yes', before='2Ii', after='2In')]


def digest(path):
    return hashlib.sha256(plain_path(path).read_bytes()).hexdigest()


def signature(element):
    # Formatting whitespace and attribute order have no flow semantics.
    return (element.tag, sorted(element.attrib.items()), (element.text or '').strip(),
            tuple(signature(child) for child in element))


def unique(root, tag, identity):
    matches = [node for node in root.iter(tag) if node.get('id') == identity]
    if len(matches) != 1:
        raise ValueError(f'Expected exactly one {tag} {identity}')
    return matches[0]


def redirect(flow, states):
    """Reject a changed graph instead of guessing a compatible race-start node."""
    if flow.tag != 'flow':
        raise ValueError('Unexpected flow root')
    for identity in ('4n', '4p'):
        decision = unique(states, 'StateDecisionBool', identity)
        if decision.attrib != dict(id=identity, data='attract_mode_chosen', ifTrue='yes', ifFalse='no'):
            raise ValueError('Attract decision changed')
    normal = unique(states, 'StateCutscene', 'dt')
    if normal.attrib != dict(id='dt', cutscene='establish_root', disable_ffb='true'):
        raise ValueError('Normal start cutscene changed')
    original = signature(flow)
    result = copy.deepcopy(flow)
    for change, state, before_state, after_state in zip(CHANGES, ('4n', '4p'), ('mt', 'jD'), ('dt', 'dP')):
        node = unique(result, 'node', change['node'])
        if node.attrib != dict(id=change['node'], state=state) or len(node) != 2:
            raise ValueError('Attract choice changed')
        if [child.attrib for child in node] != [dict(id='yes', target=change['before']), dict(id='no', target=change['after'])]:
            raise ValueError('Attract branches changed')
        if any(child.tag != 'link' or len(child) for child in node):
            raise ValueError('Unexpected choice children')
        if unique(result, 'node', change['before']).get('state') != before_state or unique(result, 'node', change['after']).get('state') != after_state:
            raise ValueError('Attract or normal destination changed')
        node[0].set('target', change['after'])
    if signature(flow) != original:
        raise ValueError('Source flow was modified')
    return result


def verify_roundtrip(expected, actual):
    if signature(expected) != signature(actual):
        raise ValueError('Converter changed flow semantics')


def prepare(game_root, output, converter):
    game, output, converter = map(plain_path, (game_root, output, converter))
    if output.is_relative_to(game) or output.exists():
        raise ValueError('Use a new output directory outside the game')
    if digest(converter) != CONVERTER_SHA256:
        raise ValueError('Unsupported converter fingerprint')
    for file, expected in [('grid2_avx.exe', SUPPORTED_AVX_SHA256),
                           ('system/flow.bin', FLOW_SHA256), ('system/states.bin', STATES_SHA256)]:
        if digest(game / file) != expected:
            raise ValueError(f'Unsupported installed file: {file}')
    output.mkdir(parents=True)
    def convert(mode, source, target):
        subprocess.run([str(converter), mode, str(source), str(target)], check=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    convert('decode', game/'system/flow.bin', output/'source-flow.xml')
    convert('decode', game/'system/states.bin', output/'source-states.xml')
    expected = redirect(ET.parse(output/'source-flow.xml').getroot(),
                        ET.parse(output/'source-states.xml').getroot())
    ET.ElementTree(expected).write(output/'practice-flow.xml', encoding='utf-8', xml_declaration=True)
    convert('encode', output/'practice-flow.xml', output/'flow.bin')
    convert('decode', output/'flow.bin', output/'roundtrip-flow.xml')
    verify_roundtrip(expected, ET.parse(output/'roundtrip-flow.xml').getroot())
    if digest(output/'flow.bin') != PRACTICE_SHA256:
        raise ValueError('Practice flow differs from the expected two-edge build')
    result = dict(schema=2, gameRoot=str(game), executableSha256=SUPPORTED_AVX_SHA256,
                  originalFlowSha256=FLOW_SHA256, statesSha256=STATES_SHA256,
                  modifiedFlowSha256=digest(output/'flow.bin'), converterSha256=digest(converter),
                  changes=CHANGES,
                  semanticRoundtrip=True, installed=False, playerControlValidated=False)
    (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    return result


def verify(game_root, bundle):
    """Accept only the exact two-edge binary, never a caller-supplied hash alone."""
    game, folder = map(plain_path, (game_root, bundle))
    metadata = plain_path(folder/'manifest.json')
    if folder.is_relative_to(game) or metadata.stat().st_size > 32768:
        raise ValueError('Invalid practice bundle location or size')
    data = json.loads(metadata.read_text(encoding='utf-8'))
    expected = dict(schema=2, gameRoot=str(game), executableSha256=SUPPORTED_AVX_SHA256,
                    originalFlowSha256=FLOW_SHA256, statesSha256=STATES_SHA256,
                    modifiedFlowSha256=PRACTICE_SHA256, converterSha256=CONVERTER_SHA256,
                    changes=CHANGES, semanticRoundtrip=True, installed=False,
                    playerControlValidated=False)
    if data != expected:
        raise ValueError('Practice manifest differs; regenerate the bundle')
    for path, sha256 in ((game/'grid2_avx.exe', SUPPORTED_AVX_SHA256),
                         (game/'system/flow.bin', FLOW_SHA256),
                         (game/'system/states.bin', STATES_SHA256),
                         (folder/'flow.bin', PRACTICE_SHA256)):
        if digest(path) != sha256:
            raise ValueError(f'Practice input changed: {path}')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--converter', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.game_root, args.output, args.converter), indent=2))
