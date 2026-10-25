"""Prepare GRID 2's guarded logo/race-intro skip without editing its installation."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

from practice_entry import CONVERTER_SHA256, FLOW_SHA256, STATES_SHA256, digest, signature, unique
from session_config import SUPPORTED_AVX_SHA256, plain_path

LEGACY_MODIFIED_SHA256 = '764651b98b6aa4ce8fe4ce6b5c9f48705d3a67b78a427628c3329c7ff9abdc0b'
MODIFIED_SHA256 = '39d145561efaadce27df1e26a57ababdb383860b83cdf03b378daa48f2a819e0'
MOVIES = {'4a': 'video/sting.bik', '4b': 'video/intel_sting.bik'}
FLOW_NODES = {'2Q4': ('4b', '2Q5'), '2Q5': ('4a', '2Pw')}
RACE_CUTSCENES = {'dt': 'establish_root', 'du': 'buildup_grid_root'}
RACE_FLOW_NODES = {'2Iy': ('dt', '2Iz'), '2IA': ('du', '2IB')}
RACE_CLEANUP = [('2In', 'dP'), ('2Io', 'jD'), ('2Ip', 'dX'), ('2Iq', 'ds'),
                ('2Ir', 'j4'), ('2Is', 'iX'), ('2It', 'iZ'), ('1r1', 'lb')]


def require_next(flow, identity, state, target):
    node = unique(flow, 'node', identity)
    if (node.attrib != dict(id=identity, state=state) or len(node) != 1 or
            node[0].tag != 'link' or node[0].attrib != dict(id='next', target=target) or len(node[0])):
        raise ValueError('Startup/intro graph changed')


def require_empty_state(states, tag, identity, **attributes):
    state = unique(states, tag, identity)
    if state.attrib != dict(id=identity, **attributes) or len(state) or (state.text or '').strip():
        raise ValueError('Startup/intro definition changed')


def patch(states, flow):
    """Skip logos and camera flythroughs; preserve native setup and cleanup."""
    if states.tag != 'xml' or flow.tag != 'flow':
        raise ValueError('Unexpected startup graph root')
    for identity, (state, target) in FLOW_NODES.items():
        require_next(flow, identity, state, target)
    if unique(flow, 'node', '2Pw').get('state') != 'b':
        raise ValueError('Startup logo destination changed')
    for identity, filename in MOVIES.items():
        require_empty_state(states, 'StateVideoPreStart', identity, filename=filename, skippable='true')
    for identity, (state, target) in RACE_FLOW_NODES.items():
        require_next(flow, identity, state, target)
        if [node.get('id') for node in flow.iter('node') if node.get('state') == state] != [identity]:
            raise ValueError('Race-intro cutscene has unexpected callers')
    for identity, cutscene in RACE_CUTSCENES.items():
        require_empty_state(states, 'StateCutscene', identity, cutscene=cutscene, disable_ffb='true')
    # Do not bypass the game's own fade/reset/audio cleanup or the countdown.
    # These remain native when the two presentation-only cutscenes complete.
    for (identity, state), (target, _) in zip(RACE_CLEANUP, RACE_CLEANUP[1:]):
        require_next(flow, identity, state, target)
    if unique(flow, 'node', RACE_CLEANUP[-1][0]).get('state') != RACE_CLEANUP[-1][1]:
        raise ValueError('Race-intro cleanup destination changed')
    require_empty_state(states, 'StateCutscene', 'ds', cutscene='prerace', disable_ffb='true')
    require_empty_state(states, 'StateRaceIntroSetup', 'dO')
    require_empty_state(states, 'StateCutscene', 'dz', cutscene='startsequence')
    result = copy.deepcopy(states)
    for tag, identities in (('StateVideoPreStart', MOVIES), ('StateCutscene', RACE_CUTSCENES)):
        for identity in identities:
            state = unique(result, tag, identity)
            state.tag = 'StateWayPoint'
            state.attrib.clear()
            state.attrib.update(id=identity, timeDelay='0.0')
    return result


def verify_roundtrip(expected, actual):
    if signature(expected) != signature(actual):
        raise ValueError('Converter changed startup-state semantics')


def manifest(game):
    return dict(schema=2, gameRoot=str(game), executableSha256=SUPPORTED_AVX_SHA256,
                originalStatesSha256=STATES_SHA256, modifiedStatesSha256=MODIFIED_SHA256,
                flowSha256=FLOW_SHA256, converterSha256=CONVERTER_SHA256,
                skippedStartupMovies=MOVIES, skippedRaceCutscenes=RACE_CUTSCENES,
                semanticRoundtrip=True, installed=False)


def prepare(game_root, output, converter):
    game, output, converter = map(plain_path, (game_root, output, converter))
    if output.is_relative_to(game) or output.exists():
        raise ValueError('Use a new output directory outside the game')
    if digest(converter) != CONVERTER_SHA256:
        raise ValueError('Unsupported converter fingerprint')
    for path, expected in (('grid2_avx.exe', SUPPORTED_AVX_SHA256), ('system/flow.bin', FLOW_SHA256),
                           ('system/states.bin', STATES_SHA256)):
        if digest(game/path) != expected:
            raise ValueError(f'Unsupported installed file: {path}')
    output.mkdir(parents=True)
    def convert(mode, source, target):
        subprocess.run([str(converter), mode, str(source), str(target)], check=True,
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    convert('decode', game/'system/flow.bin', output/'source-flow.xml')
    convert('decode', game/'system/states.bin', output/'source-states.xml')
    expected = patch(ET.parse(output/'source-states.xml').getroot(), ET.parse(output/'source-flow.xml').getroot())
    ET.ElementTree(expected).write(output/'startup-states.xml', encoding='utf-8', xml_declaration=True)
    convert('encode', output/'startup-states.xml', output/'states.bin')
    convert('decode', output/'states.bin', output/'roundtrip-states.xml')
    verify_roundtrip(expected, ET.parse(output/'roundtrip-states.xml').getroot())
    if digest(output/'states.bin') != MODIFIED_SHA256:
        raise ValueError('Startup states differ from the guarded logo/race-intro build')
    data = manifest(game)
    (output/'manifest.json').write_text(json.dumps(data, indent=2)+'\n', encoding='utf-8')
    return data


def verify(game_root, bundle):
    game, folder = map(plain_path, (game_root, bundle))
    metadata = plain_path(folder/'manifest.json')
    if folder.is_relative_to(game) or metadata.stat().st_size > 32768:
        raise ValueError('Invalid startup bundle location or size')
    data = json.loads(metadata.read_text(encoding='utf-8'))
    if data != manifest(game):
        raise ValueError('Startup manifest differs; regenerate the bundle')
    for path, sha256 in ((game/'grid2_avx.exe', SUPPORTED_AVX_SHA256), (game/'system/flow.bin', FLOW_SHA256),
                         (game/'system/states.bin', STATES_SHA256), (folder/'states.bin', MODIFIED_SHA256)):
        if digest(path) != sha256:
            raise ValueError(f'Startup input changed: {path}')
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--converter', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.game_root, args.output, args.converter), indent=2))
