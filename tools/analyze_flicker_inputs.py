"""Compare bounded flag inputs and passive sample queries; not visual acceptance."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
import struct

MAX_LOG = 32 * 1024 * 1024
DEPTH_VS = 0xc90ec9a844625ee6
COLOUR_VS = 0x5365b368abb08738
FIELDS = {'winddirection': (336, 144, 16), '_WindMagnitude': (352, 160, 4),
          '_timer': (356, 164, 4), '_Magnitude': (360, 168, 4),
          '_SinScale': (364, 172, 4), 'perInstanceModelMatrix': (368, 176, 64)}
# Byte widths only, not conversions. Unknown formats remain unavailable.
FORMAT_BYTES = {**dict.fromkeys((2, 3, 4), 16), **dict.fromkeys((6, 7, 8), 12),
                **dict.fromkeys((10, 11, 12, 13, 14, 16, 17, 18), 8),
                **dict.fromkeys((24, 25, 26, 28, 29, 30, 31, 32, 34, 35, 36,
                                 37, 38, 41, 42, 43, 87, 91), 4),
                **dict.fromkeys((49, 50, 51, 52, 54, 56, 57, 58, 59), 2),
                **dict.fromkeys((61, 62, 63, 64, 65), 1)}
SEMANTICS = ('POSITION', 'TEXCOORD', 'COLOR')
ALPHA_SAMPLERS = {0x59bc124b8c63bdaa: 3, 0xaa19112331ae0872: 4, 0x938d132c1aa95cbe: 5}


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def readback(draw, name):
    item = draw.get('flagInputs', {}).get(name, {})
    if item.get('status') != 'ok':
        return None, item.get('status', 'missing')
    size, encoded = item.get('bytes'), item.get('hex')
    if (type(size) is not int or not 0 < size <= 8192 or
            not isinstance(encoded, str) or not re.fullmatch('[0-9a-f]{%d}' % (size * 2), encoded)):
        return None, 'invalid_bytes'
    data = bytes.fromhex(encoded)
    if item.get('hash') != fnv1a(data):
        return None, 'hash_mismatch'
    if name in ('vertex0', 'index'):
        geometry = draw.get('geometry', [])
        index = 0 if name == 'vertex0' else 3
        if len(geometry) != 6 or item.get('resource') != geometry[index]:
            return None, 'resource_mismatch'
    return data, 'ok'


def model_key(draw):
    observed = draw.get('objectInputs', {})
    model = observed.get('fields', {}).get('perInstanceModelMatrix')
    if (not observed.get('known') or observed.get('scene') != draw.get('scene') or
            not isinstance(model, str) or not re.fullmatch('[0-9a-f]{128}', model)):
        return None
    geometry, args = draw.get('geometry'), draw.get('args')
    if not isinstance(geometry, list) or len(geometry) != 6 or not isinstance(args, list):
        return None
    return json.dumps([draw.get('kind'), args, geometry, model], separators=(',', ':'))


def consumed_layout(draw, layouts):
    layout = layouts.get(draw.get('inputLayout'))
    if not layout or not layout.get('known'):
        return None, 'unknown_layout'
    resolved, next_offsets = {}, {}
    for element in layout.get('elements', []):
        if not isinstance(element, list) or len(element) != 7:
            return None, 'invalid_layout'
        name, index, fmt, slot, offset, classification, step = element
        width = FORMAT_BYTES.get(fmt)
        if offset == 0xffffffff:
            offset = next_offsets.get(slot, 0)
        next_offsets[slot] = offset + width if width and offset is not None else None
        if isinstance(name, str) and name.upper() in SEMANTICS and index == 0:
            name = name.upper()
            if name in resolved:
                return None, 'duplicate_semantic'
            if slot != 0 or classification != 0 or step != 0:
                return None, 'consumed_input_not_vertex0'
            if width is None or offset is None or offset < 0:
                return None, 'unsupported_format_or_offset'
            resolved[name] = (fmt, offset, width)
    if set(resolved) != set(SEMANTICS):
        return None, 'missing_consumed_semantic'
    geometry = draw.get('geometry', [])
    if len(geometry) != 6 or any(offset + width > geometry[1] for _, offset, width in resolved.values()):
        return None, 'semantic_outside_stride'
    return resolved, 'ok'


def vertex_inputs(draw, layout, vertex, index):
    if layout is None or vertex is None or index is None:
        return None, 'missing_layout_or_readback'
    args, geometry = draw.get('args', []), draw.get('geometry', [])
    if draw.get('kind') != 'Indexed' or len(args) != 3 or len(geometry) != 6:
        return None, 'unsupported_draw'
    count, start, base = args
    if not all(type(x) is int for x in args) or count < 1 or count > 4096 or start < 0:
        return None, 'invalid_draw_range'
    if base >= 0x80000000:
        base -= 0x100000000  # Native signed BaseVertexLocation logged as UINT.
    _, stride, offset, _, fmt, index_offset = geometry
    width = {57: 2, 42: 4}.get(fmt)
    if not width or stride <= 0 or min(offset, index_offset) < 0:
        return None, 'unsupported_geometry'
    begin, end = index_offset + start * width, index_offset + (start + count) * width
    if end > len(index):
        return None, 'index_range'
    indices = struct.unpack('<' + ('H' if width == 2 else 'I') * count, index[begin:end])
    values = {name: bytearray() for name in SEMANTICS}
    for item in indices:
        vertex_index = item + base
        if vertex_index < 0:
            return None, 'negative_vertex_index'
        for name, (_, semantic_offset, size) in layout.items():
            begin = offset + vertex_index * stride + semantic_offset
            if begin + size > len(vertex):
                return None, 'vertex_range'
            values[name].extend(vertex[begin:begin + size])
    return {name: bytes(value) for name, value in values.items()}, 'ok'


def comparison(a, b):
    return 'unavailable' if a is None or b is None else 'equal' if a == b else 'changed'


def sample_value(rows):
    if len(rows) != 1:
        return dict(status='missing' if not rows else 'ambiguous', passed=None)
    row = rows[0]
    if row.get('hr') != 0 or type(row.get('passed')) is not int or row['passed'] < 0:
        return dict(status='unavailable', hr=row.get('hr'), passed=None)
    return dict(status='ok', passed=row['passed'])


def alpha_reference(draw):
    item = draw.get('pcb5', [])
    if len(item) == 9 and item[1] and item[3] == draw['scene'] and item[4] == 16:
        return item[5]
    return None


def native_query_summary(records):
    rows = [row for row in records if row.get('type') == 'nativeQuery']
    groups = defaultdict(list)
    for row in rows:
        key = (bool(row.get('kindKnown')), row.get('kind') if row.get('kindKnown') else None,
               row.get('pass'), row.get('operation'), row.get('callerRva'))
        groups[key].append(row)
    summary = []
    for (known, kind, eye, operation, caller), values in groups.items():
        payloads = 0
        for row in values:
            size, encoded = row.get('bytes'), row.get('hex')
            if (row.get('hr') == 0 and type(size) is int and 0 < size <= 16 and
                    isinstance(encoded, str) and re.fullmatch('[0-9a-f]{%d}' % (size * 2), encoded)):
                payloads += 1
        summary.append(dict(kindKnown=known, kind=kind, eye=eye, operation=operation, callerRva=caller,
                            records=len(values), results=dict(Counter(str(row.get('hr')) for row in values)),
                            payloadsObserved=payloads))
    ends = [row for row in records if row.get('type') == 'end']
    declared = ends[0].get('nativeQueryRecords') if len(ends) == 1 else None
    limit = ends[0].get('nativeQueryLimitReached') if len(ends) == 1 else None
    return dict(observedRecords=len(rows), declaredRecords=declared,
                recordCountMatches=(declared == len(rows)) if type(declared) is int else None,
                limitReached=limit if type(limit) is bool else None, groups=summary,
                hookCoverage='unknown',
                limits=('Only calls recorded during captured scene intervals are summarized. Zero observations '
                        'means no recorded calls in those intervals, not absence of native query use. '
                        'Hook installation and caller coverage require separate evidence; end counters do not '
                        'establish them. Captured return bytes are counted, not interpreted as visibility.'))


def alpha_sampling_state(draw):
    state = draw.get('alphaSampling')
    if not isinstance(state, dict):
        return None, 'missing'
    word = lambda value: type(value) is int and 0 <= value <= 0xffffffff
    if (not word(state.get('slot')) or state['slot'] != ALPHA_SAMPLERS.get(draw.get('ps')) or
            type(state.get('bound')) not in (int, bool) or state['bound'] not in (0, 1)):
        return None, 'invalid_slot_or_bound'
    descriptor = state.get('descriptor')
    if (not isinstance(descriptor, list) or len(descriptor) != (13 if state['bound'] else 0) or
            not all(map(word, descriptor))):
        return None, 'invalid_sampler_descriptor'
    required = ('view', 'resource', 'viewFormat', 'viewDimension', 'mostDetailedMip',
                'mipLevels', 'resourceMinLodBits')
    if not all(word(state.get(name)) for name in required):
        return None, 'missing_or_invalid_texture_metadata'
    texture = state.get('texture')
    if (not state['view'] or not state['resource'] or not state['viewFormat'] or state['viewDimension'] != 4 or
            not isinstance(texture, list) or len(texture) != 5 or
            not all(word(value) and value > 0 for value in texture)):
        return None, 'unbound_or_unsupported_texture'
    # A TEXTURE2D view must select an existing, nonempty mip interval.
    if (state['mostDetailedMip'] >= texture[2] or state['mipLevels'] == 0 or
            (state['mipLevels'] != 0xffffffff and
             state['mipLevels'] > texture[2] - state['mostDetailedMip'])):
        return None, 'invalid_mip_range'
    return state, 'ok'


def alpha_sampling_summary(draws):
    groups = defaultdict(lambda: [[], [], []])
    counts = Counter(matched=0, equal=0, changed=0, unavailable=0, ambiguous=0, unmatched=0,
                     invalidIdentity=0)
    examples = []
    fields = ('slot', 'kind', 'vs', 'ps', 'drawable', 'properties', 'args', 'geometry', 'instanceStreams')
    for draw in draws:
        if draw.get('ps') not in ALPHA_SAMPLERS:
            continue
        if (draw.get('pass') not in (0, 1, 2) or not all(name in draw for name in fields) or
                not isinstance(draw['args'], list) or not isinstance(draw['geometry'], list) or
                len(draw['geometry']) != 6 or not isinstance(draw['instanceStreams'], list)):
            counts['invalidIdentity'] += 1
            continue
        identity = json.dumps([draw[name] for name in fields], separators=(',', ':'))
        groups[(draw.get('scene'), identity)][draw['pass']].append(draw)
    for (scene, _), passes in groups.items():
        for eye in (1, 2):
            native, rendered = passes[0], passes[eye]
            if not native and not rendered:
                continue
            if len(native) > 1 or len(rendered) > 1:
                counts['ambiguous'] += 1
                continue
            if not native or not rendered:
                counts['unmatched'] += 1
                continue
            a, b = native[0], rendered[0]
            left, left_status = alpha_sampling_state(a)
            right, right_status = alpha_sampling_state(b)
            status = comparison(left, right)
            counts['matched'] += 1
            counts[status] += 1
            if status != 'equal' and len(examples) < 12:
                changed = [name for name in sorted(left.keys() | right.keys())
                           if left.get(name) != right.get(name)] if left is not None and right is not None else []
                examples.append(dict(scene=scene, eye=eye, ordinals=[a.get('ordinal'), b.get('ordinal')],
                                     shader=f"{a['ps']:016x}", status=status,
                                     inputStatus=[left_status, right_status], changedFields=changed,
                                     values=[left, right]))
    return dict(counts, examples=examples,
                limits=('Exact native-to-eye unique draw identity, including bound stream identities. '
                        'Only three fingerprinted alpha shaders and observed TEXTURE2D metadata are covered. '
                        'Null samplers are recorded states, not inferred descriptors. Equal metadata does '
                        'not prove equal texture contents, mip alpha coverage, filtering results or visibility.'))


def compare_pair(depth, colour, layouts, queries, clears):
    readbacks, statuses, inputs, layout_status = [], [], [], []
    for draw in (depth, colour):
        data, status = {}, {}
        for name in ('cb0', 'cb3', 'vertex0', 'index'):
            data[name], status[name] = readback(draw, name)
        layout, state = consumed_layout(draw, layouts)
        consumed, consumed_state = vertex_inputs(draw, layout, data['vertex0'], data['index'])
        readbacks.append(data)
        statuses.append(status)
        inputs.append((layout, consumed))
        layout_status.append(dict(status=state, consumed=consumed_state, elements=layout))
    a, b = readbacks
    fields = {}
    for name, (left, right, size) in FIELDS.items():
        x = a['cb0'][left:left + size] if a['cb0'] is not None and len(a['cb0']) >= left + size else None
        y = b['cb0'][right:right + size] if b['cb0'] is not None and len(b['cb0']) >= right + size else None
        fields[name] = dict(status=comparison(x, y), values=[x.hex() if x is not None else None,
                                                          y.hex() if y is not None else None])
    observed = depth['objectInputs']['fields']['perInstanceModelMatrix']
    observed_matches_gpu = [value == observed if value is not None else None
                           for value in fields['perInstanceModelMatrix']['values']]
    consumed = {}
    for semantic in SEMANTICS:
        la, lb = inputs[0][0], inputs[1][0]
        va, vb = inputs[0][1], inputs[1][1]
        compatible = la is not None and lb is not None and la[semantic][0] == lb[semantic][0]
        consumed[semantic] = comparison(va.get(semantic) if compatible and va else None,
                                        vb.get(semantic) if compatible and vb else None)
    samples = [sample_value(queries[(draw['scene'], draw['pass'], draw['ordinal'])]) for draw in (depth, colour)]
    x, y = a['cb3'], b['cb3']
    projection = comparison(x[64:128] if x is not None and len(x) >= 128 else None,
                            y[64:128] if y is not None and len(y) >= 128 else None)
    surfaces = [draw.get('surface') for draw in (depth, colour)]
    prior_clears = []
    for draw, surface in zip((depth, colour), surfaces):
        matching = [row for row in clears if surface and len(surface) == 7 and
                    row.get('scene') == draw['scene'] and row.get('pass') == draw['pass'] and
                    row.get('resource') == surface[2] and row.get('flags', 0) & 1 and
                    0 <= row.get('afterOrdinal', -1) < draw['ordinal']]
        prior_clears.append(max(matching, key=lambda row: row['afterOrdinal']) if matching else None)
    return dict(scene=depth['scene'], eye=depth['pass'], ordinals=[depth['ordinal'], colour['ordinal']],
                readbackStatus=statuses, layouts=layout_status, deformation=fields,
                observedModelMatchesGpu=observed_matches_gpu,
                buffers={name: comparison(a[name], b[name]) for name in ('vertex0', 'index', 'cb3')},
                viewProjection=projection, consumedVertexInputs=consumed,
                observedAlphaReference=dict(status=comparison(alpha_reference(depth), alpha_reference(colour)),
                                            values=[alpha_reference(depth), alpha_reference(colour)]),
                depthSurface=comparison(surfaces[0][1:] if surfaces[0] else None,
                                        surfaces[1][1:] if surfaces[1] else None),
                priorDepthClears=prior_clears,
                viewport=comparison(depth.get('viewport'), colour.get('viewport')),
                raster=comparison(depth.get('raster'), colour.get('raster')),
                depthTest=[depth.get('depthTest'), colour.get('depthTest')], samples=samples,
                depthNonzeroColourZero=(samples[0]['status'] == samples[1]['status'] == 'ok' and
                                       samples[0]['passed'] > 0 and samples[1]['passed'] == 0))


def analyze_records(records):
    layouts, queries, draws, clears = {}, defaultdict(list), [], []
    for row in records:
        if row.get('type') == 'layout':
            identity = row.get('id')
            if identity in layouts and layouts[identity] != row:
                layouts[identity] = dict(known=False)
            else:
                layouts[identity] = row
        elif row.get('type') == 'samples':
            queries[(row.get('scene'), row.get('pass'), row.get('ordinal'))].append(row)
        elif row.get('type') == 'draw':
            draws.append(row)
        elif row.get('type') == 'clearDepth':
            clears.append(row)
    groups, unavailable, totals = defaultdict(lambda: [[], []]), [], defaultdict(list)
    for draw in draws:
        key = (draw.get('scene'), draw.get('pass'), draw.get('ordinal'))
        if key in queries:
            totals[(draw.get('pass'), draw.get('vs'), draw.get('slot'))].append(sample_value(queries[key]))
        if (draw.get('vs'), draw.get('slot')) not in ((DEPTH_VS, 5), (COLOUR_VS, 6)):
            continue
        identity = model_key(draw)
        if identity is None:
            unavailable.append(dict(scene=draw.get('scene'), eye=draw.get('pass'), ordinal=draw.get('ordinal'),
                                    reason='current_model_or_geometry_unavailable'))
            continue
        groups[(draw['scene'], draw['pass'], identity)][draw['vs'] == COLOUR_VS].append(draw)
    pairs, unmatched, ambiguous = [], [], []
    for (scene, eye, _), (depth, colour) in groups.items():
        item = dict(scene=scene, eye=eye, depth=[d['ordinal'] for d in depth], colour=[d['ordinal'] for d in colour])
        if len(depth) > 1 or len(colour) > 1:
            ambiguous.append(item)
        elif not depth or not colour:
            unmatched.append(item)
        else:
            pairs.append(compare_pair(depth[0], colour[0], layouts, queries, clears))
    summaries = []
    for (eye, shader, stage), values in sorted(totals.items()):
        good = [v['passed'] for v in values if v['status'] == 'ok']
        summaries.append(dict(eye=eye, shader=f'{shader:016x}', stage=stage, queries=len(values), known=len(good),
                              unavailable=len(values) - len(good), zero=good.count(0), totalPassed=sum(good),
                              minPassed=min(good) if good else None, maxPassed=max(good) if good else None))
    field_summary = {name: dict(Counter(pair['deformation'][name]['status'] for pair in pairs)) for name in FIELDS}
    return dict(schema=1, flagPairs=pairs, pairCount=len(pairs), unmatched=unmatched, ambiguous=ambiguous,
                unavailable=unavailable, deformationSummary=field_summary, sampleSummary=summaries,
                nativeQueries=native_query_summary(records),
                alphaSampling=alpha_sampling_summary(draws),
                queryRecords=sum(len(rows) for rows in queries.values()),
                depthNonzeroColourZero=sum(pair['depthNonzeroColourZero'] for pair in pairs),
                limits=('Only the two fingerprinted flag shaders are paired, using unique scene/eye/kind/args/'
                        'geometry/current observed model equality. Other streams are not declared valid. '
                        'GPU buffer bytes and selected vertex semantics are compared without emulating shaders. '
                        'Whole CB3 differences can include unused fields; viewProjection is separate. '
                        'PS CB5 is an observed upload, not proof both pixel shaders use its alpha reference. '
                        'Queries report samples passing the existing draw, not the cause of zero samples. '
                        'No result establishes visual flicker, a fix, or headset acceptance.'))


def analyze(run):
    path = Path(run) / 'flicker-draws.jsonl'
    if path.is_symlink() or path.stat().st_size > MAX_LOG:
        raise ValueError('Linked or oversized flicker log.')
    raw = path.read_bytes()
    if len(raw) > MAX_LOG:
        raise ValueError('Flicker log exceeds its capture bound.')
    records, malformed = [], 0
    for line in raw.splitlines():
        try:
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError('Non-object record.')
            records.append(row)
        except (ValueError, UnicodeDecodeError):
            malformed += 1
    result = analyze_records(records)
    result.update(malformedLines=malformed, logTruncated=any(r.get('type') == 'truncated' for r in records),
                  burstEnded=any(r.get('type') == 'end' for r in records))
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    target = args.output or args.run / 'flicker-inputs-summary.json'
    target.write_text(json.dumps(result, indent=2) + '\n')
    print(target)
