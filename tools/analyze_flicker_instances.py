"""Decode bounded, actual GPU inputs for the fingerprinted instanced roadside shader."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import struct

from analyze_flicker_inputs import MAX_LOG, readback, sample_value
from analyze_flicker_visibility import add, mad, mul, matrix_position, project_clips

VS = 0x13dd94df551eb6d9
PS = 0x938d132c1aa95cbe
MAX_DRAWS = 288  # Eight scenes, three views, twelve bounded captures per pass.
MAX_INSTANCES = 64
INDEX_COUNTS = (6, 30, 126, 210)
ASM_SHA256 = 'cef5fbc51fd14611172bd33dd22e62cdbe88d9897ebd10a7102ed5b64f248aa9'
# Actual CB0[9..12] of the two ordinary draws before their disappearance.
TARGET_HEX = {
    'core_aframe_a001': '00c0233f00c023bc00a044bf0000000000e0d13d00e07d3f0040943d0000000000e0423f000000be00c0223f000000003d83b3420d96d3416db6bdc30000803f',
    'core_aframe_a002': '0060563f00800ebd00800bbf000000000040e13d00e07c3f0000d93d0000000000e0083f004018be00e0543f00000000d3e9cd42aa4bbc41ff9bb5c30000803f',
}
TARGETS = {name: struct.unpack('<16f', bytes.fromhex(value)) for name, value in TARGET_HEX.items()}
LIMITS = ('Only actual captured input bytes are decoded. CPU model observations and bound-but-unused '
          'streams are not substitute transforms. Shader position arithmetic is approximated on the CPU; '
          'rectangles do not simulate triangle clipping, alpha, depth, occlusion or final image visibility. '
          'Target matches use the retained ordinary-draw model, with 0.005 world-unit translation and '
          '0.001 basis-component tolerances. Matching a batch instance does not establish correct rendering.')


def uint(value):
    return type(value) is int and 0 <= value <= 0xffffffff


def unavailable(reason):
    return dict(status='unavailable', reason=reason)


def capture_buffer(draw, inputs, name):
    """Reuse strict readback hashing and geometry-resource validation."""
    return readback(dict(draw, flagInputs=inputs), name)


def layout_status(draw, layouts):
    layout = layouts.get(draw.get('inputLayout'))
    if not layout or not layout.get('known'):
        return 'unknown_layout'
    consumed = {}
    for element in layout.get('elements', []):
        if not isinstance(element, list) or len(element) != 7:
            return 'invalid_layout'
        name, index, fmt, slot, offset, classification, step = element
        if not isinstance(name, str) or not all(uint(n) for n in element[1:]):
            return 'invalid_layout'
        key = (name.upper(), index)
        if key not in [('POSITION', 0), *(('TANGENT', n) for n in range(1, 5))]:
            continue
        if key in consumed:
            return 'duplicate_consumed_semantic'
        consumed[key] = element[2:]
    expected = {('POSITION', 0): [6, 0, 0, 0, 0],
                **{('TANGENT', n): [10, 1, (n - 1) * 8, 1, 1] for n in range(1, 4)},
                ('TANGENT', 4): [2, 1, 24, 1, 1]}
    if any(key in consumed and consumed[key][-1] != 1 for key in expected if key[0] == 'TANGENT'):
        return 'unsupported_step_rate'
    return 'ok' if consumed == expected else 'unsupported_layout'


def target_matches(model):
    matches = []
    for name, reference in TARGETS.items():
        translation_error = max(abs(model[i] - reference[i]) for i in (12, 13, 14))
        if translation_error <= .005:
            basis_error = max(abs(model[i] - reference[i]) for i in (0, 1, 2, 4, 5, 6, 8, 9, 10))
            matches.append(dict(name=name, translationError=translation_error, basisError=basis_error,
                                fullModelMatch=basis_error <= .001))
    return matches


def decode_draw(draw, layouts):
    if draw.get('vs') != VS or draw.get('ps') != PS or draw.get('slot') != 6:
        return unavailable('unsupported_shader_or_stage')
    args, geometry = draw.get('args', []), draw.get('geometry', [])
    if (draw.get('kind') != 'IndexedInstanced' or len(args) != 5 or len(geometry) != 6 or
            not all(uint(x) for x in (*args[:3], args[4], *geometry)) or
            type(args[3]) is not int or not -0x80000000 <= args[3] <= 0xffffffff or
            args[0] not in INDEX_COUNTS or not 1 <= args[1] <= MAX_INSTANCES):
        return unavailable('unsupported_draw_range')
    inputs = draw.get('instanceInputs', {})
    if inputs.get('status') != 'ok':
        return unavailable('instanceInputs:' + str(inputs.get('status', 'missing')))
    state = layout_status(draw, layouts)
    if state != 'ok':
        return unavailable(state)
    bindings = [b for b in draw.get('instanceStreams', []) if isinstance(b, list) and len(b) == 4 and b[0] == 1]
    streams = inputs.get('streams', [])
    if (len(bindings) != 1 or not isinstance(streams, list) or len(streams) != 1 or
            not isinstance(streams[0], dict) or streams[0].get('slot') != 1):
        return unavailable('ambiguous_or_missing_consumed_stream')
    _, resource, stride, binding_offset = bindings[0]
    stream = streams[0]
    if not all(uint(n) for n in bindings[0]) or not resource or not 40 <= stride <= 256:
        return unavailable('invalid_instance_binding')
    if any(stream.get(k) != value for k, value in dict(firstElement=args[4], elementCount=args[1],
            stride=stride, bindingOffset=binding_offset, stepRate=1).items()):
        return unavailable('instance_range_metadata_mismatch')
    buffers = {}
    for name in ('cb3', 'index', 'vertex0'):
        buffers[name], state = capture_buffer(draw, inputs, name)
        if state != 'ok':
            return unavailable(name + ':' + state)
    item = stream.get('buffer', {})
    instance_data, state = capture_buffer(draw, {'instances': item}, 'instances')
    if state != 'ok':
        return unavailable('instances:' + state)
    if item.get('resource') != resource:
        return unavailable('instance_resource_mismatch')
    for name, item in [('vertex0', inputs['vertex0']), ('index', inputs['index']), ('instances', item)]:
        size = len(instance_data if name == 'instances' else buffers[name])
        if (not uint(item.get('offset')) or not uint(item.get('resourceBytes')) or
                item['offset'] + size > item['resourceBytes']):
            return unavailable(name + ':invalid_buffer_range')
    expected_offset = binding_offset + args[4] * stride
    if (stream['buffer']['offset'] != expected_offset or len(instance_data) != args[1] * stride):
        return unavailable('instance_readback_range_mismatch')
    if (inputs['cb3'].get('offset', 0) != 0 or len(buffers['cb3']) < 128 or len(buffers['cb3']) % 4):
        return unavailable('invalid_camera_buffer')
    index_width = {57: 2, 42: 4}.get(geometry[4])
    if not index_width or not 12 <= geometry[1] <= 256:
        return unavailable('unsupported_vertex_or_index_format')
    if (inputs['index']['offset'] != geometry[5] + args[2] * index_width or
            len(buffers['index']) != args[0] * index_width):
        return unavailable('index_readback_range_mismatch')
    base = args[3] - 0x100000000 if args[3] >= 0x80000000 else args[3]
    indices = [n + base for n in struct.unpack('<' + ('H' if index_width == 2 else 'I') * args[0], buffers['index'])]
    first, last = min(indices), max(indices)
    if first < 0:
        return unavailable('negative_actual_vertex')
    if (type(inputs.get('vertexFirst')) is not int or inputs['vertexFirst'] != first or
            inputs['vertex0']['offset'] != geometry[2] + first * geometry[1] or
            len(buffers['vertex0']) != (last - first + 1) * geometry[1]):
        return unavailable('vertex_readback_range_mismatch')
    vp = struct.unpack_from('<16f', buffers['cb3'], 64)
    positions = [struct.unpack_from('<3f', buffers['vertex0'], (n - first) * geometry[1]) for n in indices]
    if not all(math.isfinite(n) for n in (*vp, *(n for p in positions for n in p))):
        return unavailable('nonfinite_camera_or_vertex')
    viewport = draw.get('viewport', [])
    if (len(viewport) != 7 or viewport[0] != 1 or
            not all(type(n) in (int, float) and math.isfinite(n) for n in viewport) or
            not 0 < viewport[3] <= 4096 or not 0 < viewport[4] <= 4096):
        return unavailable('unsupported_viewport')
    instances = []
    try:
        for ordinal in range(args[1]):
            offset = ordinal * stride
            columns = [struct.unpack_from('<4e', instance_data, offset + n * 8)[:3] for n in range(3)]
            columns.append(struct.unpack_from('<4f', instance_data, offset + 24)[:3])
            model = [n for column in columns[:3] for n in (*column, 0.0)] + [*columns[3], 1.0]
            if not all(math.isfinite(n) for n in model):
                return unavailable('nonfinite_instance_transform')
            world = [[add(mad(model[8 + a], p[2], mad(model[a], p[0], mul(model[4 + a], p[1]))),
                          model[12 + a]) for a in range(3)] for p in positions]
            clips = [matrix_position([vp[n:n + 4] for n in range(0, 16, 4)], [*p, 1.0]) for p in world]
            projection = project_clips(clips, viewport, model[12:15])
            instances.append(dict(instanceOrdinal=ordinal, streamElement=args[4] + ordinal, model=model,
                                  modelSha256=hashlib.sha256(struct.pack('<16f', *model)).hexdigest(),
                                  targetMatches=target_matches(model), projection=projection,
                                  worldBounds=[[min(p[a] for p in world) for a in range(3)],
                                               [max(p[a] for p in world) for a in range(3)]]))
    except (OverflowError, ValueError, struct.error):
        return unavailable('invalid_transform_arithmetic')
    return dict(status='decoded', instances=instances,
                modelSequenceSha256=hashlib.sha256(b''.join(struct.pack('<16f', *i['model']) for i in instances)).hexdigest(),
                consumedStream=dict(slot=1, resource=resource, offset=expected_offset, bytes=len(instance_data),
                                    hash=stream['buffer']['hash']),
                targetMatchCount=sum(bool(i['targetMatches']) for i in instances))


def compare_views(draws):
    grouped = defaultdict(list)
    for draw in draws:
        identity = [draw['scene'], draw['drawable'], draw['properties'], draw['geometry'], draw['args'][0], *draw['args'][2:4]]
        grouped[json.dumps(identity)].append(draw)
    comparisons = []
    for key, group in grouped.items():
        by_eye = {eye: [d for d in group if d['eye'] == eye] for eye in (0, 1, 2)}
        status = 'missing_or_ambiguous_view'
        if all(len(value) == 1 for value in by_eye.values()):
            decoded = [by_eye[eye][0]['decoded'] for eye in (0, 1, 2)]
            if all(value['status'] == 'decoded' for value in decoded):
                models = [[i['modelSha256'] for i in value['instances']] for value in decoded]
                status = ('equal' if models[0] == models[1] == models[2] else
                          'same_models_different_order' if Counter(models[0]) == Counter(models[1]) == Counter(models[2]) else 'different')
            else:
                status = 'unavailable_inputs'
        comparisons.append(dict(identity=json.loads(key), status=status,
                                records=[dict(eye=d['eye'], ordinal=d['ordinal']) for d in group]))
    return comparisons


def analyze_records(rows):
    layouts, samples, counts = {}, defaultdict(list), Counter()
    for row in rows:
        if row.get('type') == 'layout':
            identity = row.get('id')
            layouts[identity] = row if identity not in layouts else None
        key = (row.get('scene'), row.get('pass'), row.get('ordinal'))
        if row.get('type') == 'draw':
            counts[key] += 1
        if row.get('type') == 'samples':
            samples[key].append(row)
    draws, candidates = [], 0
    for draw in rows:
        if (draw.get('type') != 'draw' or draw.get('vs') != VS or draw.get('ps') != PS or
                draw.get('slot') != 6 or draw.get('kind') != 'IndexedInstanced'):
            continue
        if not isinstance(draw.get('args'), list) or len(draw['args']) != 5 or draw['args'][0] not in INDEX_COUNTS:
            continue
        candidates += 1
        if len(draws) >= MAX_DRAWS:
            continue
        key = (draw.get('scene'), draw.get('pass'), draw.get('ordinal'))
        decoded = decode_draw(draw, layouts) if counts[key] == 1 else unavailable('ambiguous_draw')
        draws.append(dict(scene=draw.get('scene'), eye=draw.get('pass'), ordinal=draw.get('ordinal'),
                          drawable=draw.get('drawable'), properties=draw.get('properties'),
                          args=draw['args'], geometry=draw.get('geometry'), decoded=decoded,
                          samples=sample_value(samples[key])))
    comparisons = compare_views(draws)
    return dict(schema=1, candidateDraws=candidates, drawLimit=MAX_DRAWS, limitReached=candidates > MAX_DRAWS,
                statusCounts=dict(Counter(d['decoded']['status'] for d in draws)),
                reasonCounts=dict(Counter(d['decoded']['reason'] for d in draws if d['decoded']['status'] != 'decoded')),
                targetReference=dict(run='diagnostic-20261018-164822-082', scene=33335, eye=0,
                                     ordinals=[570, 571], modelHex=TARGET_HEX),
                shader=dict(vs=f'{VS:016x}', ps=f'{PS:016x}', retainedAssemblySha256=ASM_SHA256),
                draws=draws, viewComparisons=comparisons,
                viewComparisonCounts=dict(Counter(c['status'] for c in comparisons)), limits=LIMITS)


def analyze(run):
    path = Path(run) / 'flicker-draws.jsonl'
    if path.is_symlink() or path.stat().st_size > MAX_LOG:
        raise ValueError('Linked or oversized flicker log.')
    raw = path.read_bytes()
    if len(raw) > MAX_LOG:
        raise ValueError('Oversized flicker log.')
    rows, malformed = [], 0
    for line in raw.splitlines():
        try:
            value = json.loads(line)
            if not isinstance(value, dict):
                raise ValueError('Non-object record.')
            rows.append(value)
        except (ValueError, UnicodeDecodeError):
            malformed += 1
    result = analyze_records(rows)
    result.update(logSha256=hashlib.sha256(raw).hexdigest(), malformedLines=malformed,
                  logTruncated=any(r.get('type') == 'truncated' for r in rows),
                  burstEnded=any(r.get('type') == 'end' for r in rows))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.write_text(json.dumps(analyze(arguments.run), indent=2) + '\n')
    print(arguments.output)
