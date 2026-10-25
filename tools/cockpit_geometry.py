"""Derive bounded per-car cockpit estimates from exported authored neutral poses.

This offline tool reads game assets and only writes an evidence/catalog output.
It does not patch a camera or launch the game. The eye point is anatomical,
not an authored eye marker; only S2000 has passed a user headset check.
"""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

from practice_entry import CONVERTER_SHA256, signature
from session_config import plain_path

SKELETON_SHA256 = 'ea990fb74ede129aa521ded061a8309220568be25ce6ab636fd104315dd3c113'
LIBRARY_SHA256 = 'b27a45447cd08a8e46423f16151a9d375a179a374ab83475b8029749ff528d98'
HAND_TOLERANCE_METERS = .001
POSITION_BOUNDS = ((-1.5, 1.5), (.2, 2.5), (-2, 2))
MAX_INPUT_BYTES = 4 * 1024 * 1024


def digest(path):
    with plain_path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


class Packet:
    def __init__(self, data):
        if not 0 < len(data) < 64 * 1024:
            raise ValueError('Unexpected animation packet size')
        self.data, self.pos = data, 0

    def read(self, fmt):
        size = struct.calcsize(fmt)
        if self.pos + size > len(self.data):
            raise ValueError('Truncated animation packet')
        values = struct.unpack_from(fmt, self.data, self.pos)
        self.pos += size
        if any(isinstance(x, float) and not math.isfinite(x) for x in values):
            raise ValueError('Nonfinite animation value')
        return values

    def string(self):
        size, = self.read('>I')
        if not 0 < size <= 128 or self.pos + size > len(self.data):
            raise ValueError('Invalid animation channel string')
        result = self.data[self.pos:self.pos + size].decode('ascii')
        self.pos += size
        return result

    def end(self):
        if self.pos != len(self.data):
            raise ValueError('Trailing animation packet data')


def translations(data):
    packet = Packet(data)
    count, keys, header, times, values = packet.read('>5I')
    if (count, keys, header, times, values) != (53, 1, 28, 452, 512):
        raise ValueError('Unsupported neutral translation layout')
    channels = [(packet.string(), packet.string()) for _ in range(count)]
    if len(set(name for name, _ in channels)) != count:
        raise ValueError('Duplicate neutral translation joint')
    if packet.read('>f') != (0.,):
        raise ValueError('Neutral translation must have one time-zero key')
    result = {}
    for name, kind in channels:
        if kind != 'translation':
            raise ValueError('Unexpected neutral translation channel')
        *xyz, padding = packet.read('>4f')
        if padding != 0 or any(abs(v) > 10 for v in xyz):
            raise ValueError('Invalid authored translation')
        result[name] = xyz
    packet.end()
    return result


def rotations(data):
    packet = Packet(data)
    count, alignment = packet.read('>2I')
    if (count, alignment) != (67, 16):
        raise ValueError('Unsupported neutral rotation layout')
    result = {}
    for _ in range(count):
        name, channel = packet.string(), packet.string()
        keys, _time_offset, _value_offset = packet.read('>3I')
        if name in result or keys != 1 or channel != 'rotation_quaternion':
            raise ValueError('Invalid neutral rotation channel')
        if packet.read('>f') != (0.,):
            raise ValueError('Neutral rotation must have one time-zero key')
        quaternion = packet.read('>4e')
        if abs(sum(v * v for v in quaternion) - 1) >= .003:
            raise ValueError('Invalid half-float rotation quaternion')
        result[name] = quaternion
    packet.end()
    return result


def matrix(value):
    values = [float(v) for v in value.split()]
    if len(values) != 16 or not all(math.isfinite(v) for v in values):
        raise ValueError('Invalid authored transform')
    if any(values[i] != 0 for i in (3, 7, 11)) or values[15] != 1:
        raise ValueError('Nonaffine authored transform')
    return [values[i:i + 4] for i in range(0, 16, 4)]


def multiply(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
            for i in range(4)]


def pose_matrix(quaternion, translation):
    length = math.sqrt(sum(v * v for v in quaternion))
    x, y, z, w = [v / length for v in quaternion]
    return [[1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y), 0],
            [2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x), 0],
            [2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y), 0],
            [*translation, 1]]


def compose(nodes, translation=None, rotation=None):
    translation, rotation = translation or {}, rotation or {}
    by_path = {n['path']: n for n in nodes}
    by_id = {n['id']: n for n in nodes}
    if len(by_path) != len(nodes) or len(by_id) != len(nodes):
        raise ValueError('Duplicate authored transform')
    if not (set(translation) | set(rotation)) <= set(by_id):
        raise ValueError('Animation references an unknown joint')
    world = {}
    pending = set()

    def visit(path):
        if path in world:
            return world[path]
        if path in pending:
            raise ValueError('Cycle in authored transforms')
        pending.add(path)
        node = by_path[path]
        local = matrix(node['matrix'])
        if node['id'] in rotation:
            local = pose_matrix(rotation[node['id']], translation.get(node['id'], local[3][:3]))
        elif node['id'] in translation:
            local[3][:3] = translation[node['id']]
        parent = node['parent']
        world[path] = multiply(local, visit(parent)) if parent in by_path else local
        pending.remove(path)
        return world[path]

    return {name: visit(node['path']) for name, node in by_id.items()}


def derive(car, skeleton):
    if not re.fullmatch('[a-z0-9]{3}', car['car']) or car['status'] != 'exported':
        raise ValueError('Car source export unavailable')
    animation = car['animation']['data']
    packets = {p['type']: p for p in animation['packets']}
    if (animation['clip'] != 'd_Neutral' or len(packets) != 3 or
            set(packets) != {'NeAnimPacket_L4_KA', 'NeAnimPacket_SC4', 'NeAnimPacket_B1'}):
        raise ValueError('Unexpected authored neutral packet set')
    translation = translations(base64.b64decode(packets['NeAnimPacket_L4_KA']['data'], validate=True))
    rotation = rotations(base64.b64decode(packets['NeAnimPacket_SC4']['data'], validate=True))
    # The clip also animates car belt attachments that are not driver joints.
    # Their names omit the skeleton's d_ prefix; neither is a head/hand ancestor.
    for attachment in ('tl_belt_Dummy', 'tr_belt_Dummy'):
        rotation.pop(attachment, None)
    world = compose(skeleton, translation, rotation)
    dummy = compose(car['dummies']['data'])
    # Hand dummies are authored relative to steer_master, but their PSSG nodes
    # are siblings rather than scene-graph children. Compose that mount explicitly.
    checks = []
    for side in ('L', 'R'):
        joint = world[f'Bip01 {side} Hand'][3][:3]
        expected = multiply(dummy[f'D_{side}hand_Dummy'], dummy['steer_master'])[3][:3]
        residual = math.dist(joint, expected)
        checks.append(dict(side=side, animationJoint=joint, steeringDummy=expected,
                           distanceMeters=residual))
    hand_status = ('consistent' if max(check['distanceMeters'] for check in checks) < HAND_TOLERANCE_METERS
                   else 'authored-mismatch')
    warnings = ([] if hand_status == 'consistent' else
                ['Neutral pose does not match both steering hand dummies within 1mm: ' +
                 ', '.join(f"{v['side']}={v['distanceMeters']:.6f}m" for v in checks)])
    # The neutral pose is already car-local. Refuse a changed attachment mount
    # rather than reapplying a per-car mount twice or ignoring it silently.
    identity = [[float(i == j) for j in range(4)] for i in range(4)]
    for mount in (world['Scene Root'], dummy['d_bip_position']):
        if any(abs(mount[i][j]-identity[i][j]) > .00001 for i in range(4) for j in range(4)):
            raise ValueError('Unsupported nonidentity driver attachment mount')
    head, nub = (world[name][3][:3] for name in ('Bip01 Head', 'Bip01 HeadNub'))
    if not .1 < math.dist(head, nub) < .3 or nub[1] <= head[1]:
        raise ValueError('Unexpected authored head dimensions')
    eye = [a+(b-a)*2/3 for a, b in zip(head, nub)]
    eye[2] += .05
    position = [round(v, 4) for v in eye]
    if not all(lo <= value <= hi for value, (lo, hi) in zip(position, POSITION_BOUNDS)):
        raise ValueError('Derived seat is outside bounded car-local coordinates')
    return dict(car=car['car'], position=position, head=head, headNub=nub,
                handChecks=checks, handChecksStatus=hand_status, warnings=warnings,
                sources=[car['animation']['source'], car['dummies']['source']],
                validation='s20-user-accepted' if car['car'] == 's20' else 'offline-derived')


def camera_metadata(game, source, converter, temporary):
    path = game/source['path']
    if path.is_symlink() or path.stat().st_size > 1024 * 1024 or digest(path) != source['sha256']:
        raise ValueError('Camera source changed since export')
    target = temporary/'camera.xml'
    if target.exists():
        target.unlink()
    subprocess.run([str(converter), 'decode', str(path), str(target)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    root = ET.parse(target).getroot()
    if digest(path) != source['sha256']:
        raise ValueError('Camera source changed during decode')
    return dict(path=source['path'], sha256=source['sha256'], decodedSha256=digest(target),
                semanticSha256=hashlib.sha256(json.dumps(signature(root), separators=(',', ':')).encode()).hexdigest())


def apply_visual_default(row, calibration, mesh_sha256):
    """Apply a reviewed base height, never a user's additive seat adjustment."""
    if (calibration['car'] != row['car'] or
            calibration['derivedPosition'] != row['position'] or
            calibration['meshSha256'] != mesh_sha256):
        raise ValueError('Visual cockpit calibration no longer matches its pose/mesh')
    height = calibration['heightCentimeters']
    if type(height) is not int or not -12 <= height <= 0:
        raise ValueError('Visual cockpit height is outside reviewed bounds')
    if row['car'] == 's20' and height != 0:
        raise ValueError('Preserve the accepted S2000 position')
    row['derivedPosition'] = list(row['position'])
    row['position'] = [row['position'][0], round(row['position'][1] + height / 100, 4), row['position'][2]]
    if not all(lo <= v <= hi for v, (lo, hi) in zip(row['position'], POSITION_BOUNDS)):
        raise ValueError('Reviewed seat is outside bounded car-local coordinates')
    row['visualReview'] = dict(heightCentimeters=height, meshSha256=mesh_sha256,
                               note=calibration['review'], evidence='offline-geometry')
    return row


def build(export_path, game, converter, output):
    export_path, game, converter, output = map(plain_path, (export_path, game, converter, output))
    if output.is_relative_to(game) or export_path.is_relative_to(game) or output.exists():
        raise ValueError('Catalog must be a new file outside the installed game')
    if export_path.stat().st_size > MAX_INPUT_BYTES:
        raise ValueError('Oversized geometry export')
    export = json.loads(export_path.read_text(encoding='utf-8-sig'))
    if export['schema'] != 1 or export['skeleton']['source']['sha256'] != SKELETON_SHA256:
        raise ValueError('Unrecognized driver skeleton')
    if export['library']['sha256'] != LIBRARY_SHA256 or digest(converter) != CONVERTER_SHA256:
        raise ValueError('Unrecognized asset parser or camera converter')
    if digest(game/export['skeleton']['source']['path']) != SKELETON_SHA256:
        raise ValueError('Installed driver skeleton changed')
    defaults_path = Path(__file__).parent/'data/cockpit-defaults.json'
    defaults = json.loads(defaults_path.read_text(encoding='utf-8'))
    reviewed = {r['car']: r for r in defaults['cars']}
    if (defaults['schema'] != 1 or len(reviewed) != len(defaults['cars']) or
            set(reviewed) != {r['car'] for r in export['cars']}):
        raise ValueError('Visual cockpit review must cover every exported car exactly once')
    rows, unsupported = [], []
    with tempfile.TemporaryDirectory(prefix='cockpit-catalog-', dir=export_path.parent) as temporary:
        for car in export['cars']:
            try:
                if car['status'] != 'exported':
                    raise ValueError(car['error'])
                for entry in (car['animation'], car['dummies']):
                    if digest(game/entry['source']['path']) != entry['source']['sha256']:
                        raise ValueError('Authored geometry changed since export')
                row = derive(car, export['skeleton']['data'])
                row['camera'] = camera_metadata(game, car['camera'], converter, Path(temporary))
                mesh = plain_path(game/f"cars/models/{row['car']}/{row['car']}_highLOD.pssg")
                apply_visual_default(row, reviewed[row['car']], digest(mesh))
                rows.append(row)
            except (ValueError, KeyError) as exc:
                unsupported.append(dict(car=car['car'], reason=str(exc)))
    if next((r['position'] for r in rows if r['car'] == 's20'), None) != [.3537, 1.0665, -.5216]:
        raise ValueError('S2000 eye calibration no longer reproduces the accepted position')
    if len({r['car'] for r in rows}) != len(rows):
        raise ValueError('Duplicate car in catalog')
    result = dict(schema=1, method='d_Neutral eye estimate plus individually reviewed cockpit height',
                  visualDefaultsSha256=digest(defaults_path),
                  handToleranceMeters=HAND_TOLERANCE_METERS, skeleton=export['skeleton']['source'],
                  exporterInputSha256=digest(export_path), converterSha256=CONVERTER_SHA256,
                  cars=rows, unsupported=unsupported,
                  limits=['Eye positions are estimates from each car authored neutral pose, not eye markers.',
                          'All 51 defaults have offline geometry visual review; only unchanged S2000 has user desktop and headset acceptance.',
                          'Authored hand-pose mismatches remain recorded; visual height corrections do not qualify those animation poses.',
                          'This does not qualify cockpit mesh coverage, clipping or head-hiding behavior on other cars.'])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    return dict(cars=len(rows), unsupported=unsupported,
                handCheckWarnings=[r['car'] for r in rows if r['handChecksStatus'] != 'consistent'],
                catalog=str(output), sha256=digest(output),
                maximumHandResidualMeters=max(c['distanceMeters'] for r in rows for c in r['handChecks']))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', required=True, type=Path)
    parser.add_argument('--game-root', required=True, type=Path)
    parser.add_argument('--converter', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.export, args.game_root, args.converter, args.output), indent=2))
