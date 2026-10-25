"""Build/verify an offline S2000 cockpit candidate; never edit installed assets.

The native bonnet slots keep their camera type and cycling links. A supplied
car-local seat position replaces their origins, without changing headset pose,
stereo rendering or graphics settings. Interior quality and head hiding still
need an in-game check; a successful asset roundtrip does not establish either.
"""
import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from practice_entry import CONVERTER_SHA256, signature
from session_config import SUPPORTED_AVX_SHA256, plain_path

SOURCE_PATH = 'cars/models/s20/cameras.xml'
SOURCE_SHA256 = 'cc6883b9277ca1053e6ac8653f03c8ece95d2ddd5b871a448ef2665447ea24df'
DECODED_SHA256 = '03da5c8b235fd06dd301efe631855775a28f3c21bd7c2af3cc384bd2d5e9443d'
SOURCE_SEMANTIC_SHA256 = '4ed9af075d4f454452cc29f5fe207bb896ae0bddd27f0e8652fc273b7b0db640'
MAX_ASSET_BYTES = 1024 * 1024
MODES = ('head-only', 'hide-driver')
VIEWS = ('bonnet', 'bonnet_reverse')
BUNDLE_FILES = ('source-cameras.xml', 'cockpit-cameras.xml', 'cameras.xml', 'roundtrip-cameras.xml')
# These are bounds for an absolute car-local seat, not a headset displacement.
POSITION_BOUNDS = ((-1.5, 1.5), (0.2, 2.5), (-2.0, 2.0))
SHAKE_SWITCHES = (('BrakeBasedShake', 'enabled'), ('ScrapeShake', 'enabled'),
                  ('SpeedBasedNoise', 'enabled'), ('DriftShake', 'enabled'),
                  ('WheelSpinShake', 'enabled'), ('HighSpeedEffect', 'enabled'),
                  ('BoostEffect', 'enable'), ('BoostEffect', 'fallbackEnabled'),
                  ('BoostEffect', 'fovEnabled'), ('BoostEffect', 'shakeEnabled'),
                  ('HandyCam', 'handyCamEnabled'))


def read_file(path, limit=MAX_ASSET_BYTES):
    path = plain_path(path)
    if not path.is_file() or not 0 < path.stat().st_size <= limit:
        raise ValueError(f'Invalid cockpit file or size: {path}')
    return path.read_bytes()


def digest(path, limit=MAX_ASSET_BYTES):
    return hashlib.sha256(read_file(path, limit)).hexdigest()


def semantic_digest(root):
    data = json.dumps(signature(root), separators=(',', ':')).encode('utf-8')
    return hashlib.sha256(data).hexdigest()


def seat_position(position):
    if not isinstance(position, (list, tuple)) or len(position) != 3:
        raise ValueError('Supply exactly three car-local seat coordinates')
    result = []
    for value, (minimum, maximum) in zip(position, POSITION_BOUNDS):
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError('Seat coordinates must be finite numbers')
        if not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError('Seat coordinate is outside the bounded car-local range')
        # Native scalar/vector fields are serialized to four decimal places.
        result.append(float(f'{value:.4f}'))
    return result


def parameter(view, name, kind, group=None):
    container = view
    if group is not None:
        groups = view.findall(group)
        if len(groups) != 1:
            raise ValueError(f'Expected one camera group: {group}')
        container = groups[0]
    found = [node for node in container if node.tag == 'Parameter' and node.get('name') == name]
    if len(found) != 1 or found[0].get('type') != kind or len(found[0]):
        raise ValueError(f'Unexpected camera parameter: {group}/{name}')
    node = found[0]
    fields = {'name', 'type', 'x', 'y', 'z'} if kind == 'vector3' else {'name', 'type', 'value'}
    if set(node.attrib) != fields or (node.text or '').strip():
        raise ValueError(f'Unexpected camera parameter structure: {name}')
    return node


def patch(source, position, mode, *, car='s20', source_semantic=SOURCE_SEMANTIC_SHA256, start_cockpit=False, near_plane=None):
    """Return a copy and explicit field changes, rejecting any source drift."""
    position = seat_position(position)
    if mode not in MODES:
        raise ValueError('Unknown driver-hiding mode')
    if semantic_digest(source) != source_semantic:
        raise ValueError('Car camera definitions changed; refusing to guess')
    if source.tag != 'VehicleViewDefinition' or source.attrib or len(source) != 1:
        raise ValueError('Unexpected camera definition root')
    manager = source[0]
    if manager.tag != 'ViewManager' or manager.attrib != {'name': car}:
        raise ValueError('Expected the selected car view manager')
    result = copy.deepcopy(source)
    changes = []
    for ident in VIEWS:
        matches = [view for view in result[0] if view.tag == 'View' and view.get('ident') == ident]
        if len(matches) != 1 or matches[0].get('type') != 'Mounted':
            raise ValueError(f'Expected one native Mounted view: {ident}')
        view = matches[0]

        def change(name, kind, values, group=None):
            node = parameter(view, name, kind, group)
            for field, after in values.items():
                before = node.get(field)
                if before != after:
                    changes.append(dict(view=ident, group=group, parameter=name,
                                        field=field, before=before, after=after))
                    node.set(field, after)

        change('offset', 'vector3', dict(zip('xyz', (f'{x:.4f}' for x in position))))
        for name in ('pitch', 'roll'):
            change(name, 'scalar', {'value': '0.0000'})
        # Legacy diagnostic bundles retain their original clip distance. The
        # launcher bank explicitly uses 2 cm for nearby roof/interior geometry.
        if near_plane is not None:
            if near_plane != .02:
                raise ValueError('Unsupported cockpit near plane')
            change('nearPlane', 'scalar', {'value': '0.0200'})
        # Stock yaw and FOV remain exact in both look directions.
        for name in ('DisableDriverHead', 'DisableCoDriver'):
            change(name, 'bool', {'value': 'true'})
        change('DisableDriver', 'bool', {'value': 'false' if mode == 'head-only' else 'true'})
        for name in ('axisScale', 'axisScaleForTilt', 'twistScale'):
            change(name, 'vector3', dict.fromkeys('xyz', '0.0000'))
        for name in ('maximumShift', 'movementSpeed', 'finalMovementScale'):
            change(name, 'scalar', {'value': '0.0000'})
        for group, name in SHAKE_SWITCHES:
            change(name, 'bool', {'value': 'false'}, group)
        for name in ('vehicleSkidShake', 'rotationShake', 'positionShake'):
            # Several shipped cars omit these optional legacy switches. Preserve
            # absence rather than adding fields to their native definitions.
            if view.find(f"Parameter[@name='{name}']") is not None:
                change(name, 'bool', {'value': 'false'})
    if start_cockpit:
        # The vehicle manager activates its first non-hidden View after loading.
        # Reverse links and saved/cycled selections resolve by the retained name.
        view = next(v for v in result[0] if v.get('ident') == 'bonnet')
        if view.get('hidden') != 'false' or parameter(view, 'isHidden', 'bool').get('value') != 'false':
            raise ValueError('Cockpit must be a visible selectable view')
        result[0].remove(view)
        result[0].insert(0, view)
        changes.append(dict(view='bonnet', parameter='order', before='native', after='first'))
    return result, changes


def verify_roundtrip(expected, actual):
    if signature(expected) != signature(actual):
        raise ValueError('Converter changed camera semantics')


def guarded_inputs(game, converter):
    if digest(game/'grid2_avx.exe', 128 * 1024 * 1024) != SUPPORTED_AVX_SHA256:
        raise ValueError('Unsupported GRID 2 executable fingerprint')
    if digest(game/SOURCE_PATH) != SOURCE_SHA256:
        raise ValueError('Unsupported installed S2000 camera fingerprint')
    if digest(converter, 32 * 1024 * 1024) != CONVERTER_SHA256:
        raise ValueError('Unsupported converter fingerprint')


def convert(converter, mode, source, target):
    if plain_path(target).exists():
        raise ValueError('Converter output already exists')
    subprocess.run([str(converter), mode, str(source), str(target)], check=True,
                   timeout=30, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
                   cwd=target.parent, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    read_file(target)


def manifest(game, folder, position, mode, changes):
    return dict(schema=1, car='s20', gameRoot=str(game), target=SOURCE_PATH,
                executableSha256=SUPPORTED_AVX_SHA256, originalSha256=SOURCE_SHA256,
                modifiedSha256=digest(folder/'cameras.xml'), converterSha256=CONVERTER_SHA256,
                sourceSemanticSha256=SOURCE_SEMANTIC_SHA256, position=seat_position(position),
                mode=mode, views=list(VIEWS), changes=changes,
                files={name: digest(folder/name) for name in BUNDLE_FILES},
                semanticRoundtrip=True, installed=False, desktopValidated=False,
                headsetValidated=False, headHidingValidated=False)


def prepare(game_root, output, converter, position, mode):
    game, output, converter = map(plain_path, (game_root, output, converter))
    position = seat_position(position)
    if mode not in MODES:
        raise ValueError('Unknown driver-hiding mode')
    if output.is_relative_to(game) or output.exists():
        raise ValueError('Use a new output directory outside the game')
    guarded_inputs(game, converter)
    output.mkdir(parents=True)
    convert(converter, 'decode', game/SOURCE_PATH, output/'source-cameras.xml')
    if digest(output/'source-cameras.xml') != DECODED_SHA256:
        raise ValueError('Decoded source differs from the inspected S2000 asset')
    expected, changes = patch(ET.fromstring(read_file(output/'source-cameras.xml')), position, mode)
    ET.ElementTree(expected).write(output/'cockpit-cameras.xml', encoding='utf-8', xml_declaration=True)
    convert(converter, 'encode', output/'cockpit-cameras.xml', output/'cameras.xml')
    convert(converter, 'decode', output/'cameras.xml', output/'roundtrip-cameras.xml')
    verify_roundtrip(expected, ET.fromstring(read_file(output/'roundtrip-cameras.xml')))
    guarded_inputs(game, converter)
    data = manifest(game, output, position, mode, changes)
    (output/'manifest.json').write_text(json.dumps(data, indent=2)+'\n', encoding='utf-8')
    return data


def verify(game_root, bundle, converter):
    """Reconstruct and re-encode the expected asset; do not trust manifest hashes."""
    game, folder, converter = map(plain_path, (game_root, bundle, converter))
    if folder.is_relative_to(game) or not folder.is_dir():
        raise ValueError('Invalid cockpit bundle location')
    guarded_inputs(game, converter)
    data = json.loads(read_file(folder/'manifest.json', 32768))
    if not isinstance(data, dict):
        raise ValueError('Invalid cockpit manifest')
    if digest(folder/'source-cameras.xml') != DECODED_SHA256:
        raise ValueError('Source camera evidence changed')
    expected, changes = patch(ET.fromstring(read_file(folder/'source-cameras.xml')),
                              data.get('position'), data.get('mode'))
    expected_manifest = manifest(game, folder, data.get('position'), data.get('mode'), changes)
    # JSON comparison also distinguishes false from 0 and true from schema 1.
    if json.dumps(data, sort_keys=True) != json.dumps(expected_manifest, sort_keys=True):
        raise ValueError('Cockpit manifest differs; regenerate the bundle')
    verify_roundtrip(expected, ET.fromstring(read_file(folder/'cockpit-cameras.xml')))
    verify_roundtrip(expected, ET.fromstring(read_file(folder/'roundtrip-cameras.xml')))
    # Fixed file names only, inside the supplied guarded bundle. Deterministic
    # re-encoding binds the binary to the patch even if someone changes its hash.
    with tempfile.TemporaryDirectory(prefix='verify-', dir=folder) as temporary:
        temporary = plain_path(temporary)
        ET.ElementTree(expected).write(temporary/'expected.xml', encoding='utf-8', xml_declaration=True)
        convert(converter, 'encode', temporary/'expected.xml', temporary/'expected.bin')
        if digest(temporary/'expected.bin') != digest(folder/'cameras.xml'):
            raise ValueError('Camera binary is not the expected bounded patch')
    guarded_inputs(game, converter)
    return data


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--converter', type=Path, required=True)
    operation = parser.add_mutually_exclusive_group(required=True)
    operation.add_argument('--output', type=Path)
    operation.add_argument('--verify', type=Path, metavar='BUNDLE')
    parser.add_argument('--position', type=float, nargs=3, metavar=('X', 'Y', 'Z'))
    parser.add_argument('--mode', choices=MODES)
    args = parser.parse_args()
    if args.verify:
        if args.position is not None or args.mode is not None:
            parser.error('--verify reads position/mode from the guarded bundle')
        result = verify(args.game_root, args.verify, args.converter)
    else:
        if args.position is None or args.mode is None:
            parser.error('--output requires --position X Y Z and --mode')
        result = prepare(args.game_root, args.output, args.converter, args.position, args.mode)
    print(json.dumps(result, indent=2))
