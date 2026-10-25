"""Map the fingerprinted flag mesh into captured final images, without a visibility verdict."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct

from analyze_flicker_inputs import (COLOUR_VS, FORMAT_BYTES, MAX_LOG, consumed_layout, model_key,
                                   readback, sample_value, vertex_inputs)

MAX_IMAGE_BYTES = 32 * 1024 * 1024
MAX_IMAGE_PIXELS = 4096 * 4096
MAX_PROJECTIONS = 96
MAX_FLAG_PROJECTIONS = 24
MESH_VS = 0xd946f27a1c896b78
MESH_PS = 0x938d132c1aa95cbe
PIXEL_BYTES = {10: 8, 11: 8, 24: 4, 26: 4, 28: 4, 29: 4, 87: 4, 91: 4}
LIMITS = ('Positions emulate only SV_Position of flag VS 5365b368abb08738 and object VS '
          'd946f27a1c896b78 using captured GPU bytes and a CPU sine for flags. They are approximate, '
          'not bit-exact GPU rasterization. '
          'The padded rectangle includes background and does not model triangles, clipping, '
          'alpha discard, depth, occlusion, post-processing or later overlays. Final-region '
          'statistics and draw sample counts cannot establish object visibility, flicker, '
          'a fix, or headset acceptance. Captures sample eight separated scenes, not every frame.')


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def mul(a, b):
    return f32(a * b)


def add(a, b):
    return f32(a + b)


def mad(a, b, c):
    # refactoringAllowed permits differing GPU rounding; the ROI has a margin.
    return add(mul(a, b), c)


def matrix_position(columns, position):
    return [mad(columns[3][axis], position[3],
                mad(columns[2][axis], position[2],
                    mad(columns[0][axis], position[0], mul(position[1], columns[1][axis]))))
            for axis in range(4)]


def flag_position(position, red, cb0, cb3):
    """Position-only translation of the retained colour VS assembly, not lighting/UVs."""
    model = [cb0[start:start + 4] for start in range(44, 60, 4)]
    view_projection = [cb3[start:start + 4] for start in range(16, 32, 4)]
    phase = mul(model[3][2], model[3][0])
    phase = f32(phase - math.floor(phase))
    phase = mad(position[0], cb0[43], mad(phase, 256.0, cb0[41]))
    wave = mul(f32(math.sin(phase)), cb0[42])
    wind = f32(sum(mul(model[2][axis], cb0[36 + axis]) for axis in range(3)))
    wave = mad(wind, cb0[40], wave)
    displacement = mul(wave, red)
    deformed = (position[0], add(position[1], abs(displacement)),
                mad(wave, red, position[2]), 1.0)
    world = matrix_position(model, deformed)
    return matrix_position(view_projection, world)


def project_draw(draw, layouts):
    """Return a conservative screen rectangle only for complete, supported inputs."""
    def unknown(reason):
        return dict(status='unavailable', reason=reason)
    if draw.get('vs') != COLOUR_VS or draw.get('slot') != 6:
        return unknown('unsupported_shader_or_stage')
    if model_key(draw) is None:
        return unknown('missing_current_model_identity')
    layout, status = consumed_layout(draw, layouts)
    if status != 'ok':
        return unknown(status)
    if layout['POSITION'][0] != 6 or layout['COLOR'][0] not in (28, 87):
        return unknown('unsupported_position_or_colour_format')
    buffers = {}
    for name in ('cb0', 'cb3', 'vertex0', 'index'):
        buffers[name], status = readback(draw, name)
        if status != 'ok':
            return unknown(name + ':' + status)
    if len(buffers['cb0']) != 240 or len(buffers['cb3']) < 128 or len(buffers['cb3']) % 4:
        return unknown('unexpected_constant_buffer_size')
    observed = draw['objectInputs']['fields']['perInstanceModelMatrix']
    if buffers['cb0'][176:240].hex() != observed:
        return unknown('observed_model_disagrees_with_gpu')
    cb0 = struct.unpack('<60f', buffers['cb0'])
    cb3 = struct.unpack('<' + 'f' * (len(buffers['cb3']) // 4), buffers['cb3'])
    used = (*cb0[36:60], *cb3[16:32])
    if not all(math.isfinite(value) for value in used):
        return unknown('nonfinite_constants')
    values, status = vertex_inputs(draw, layout, buffers['vertex0'], buffers['index'])
    if status != 'ok':
        return unknown(status)
    viewport = draw.get('viewport', [])
    if (not isinstance(viewport, list) or len(viewport) != 7 or viewport[0] != 1 or
            not all(type(x) in (int, float) and math.isfinite(x) for x in viewport) or
            not 0 < viewport[3] <= 4096 or not 0 < viewport[4] <= 4096):
        return unknown('unsupported_viewport')
    clips = []
    red_byte = 2 if layout['COLOR'][0] == 87 else 0
    try:
        for offset in range(draw['args'][0]):
            position = struct.unpack_from('<3f', values['POSITION'], offset * 12)
            if not all(math.isfinite(x) for x in position):
                return unknown('nonfinite_vertex')
            red = f32(values['COLOR'][offset * 4 + red_byte] / 255.0)
            clips.append(flag_position(position, red, cb0, cb3))
    except (OverflowError, ValueError):
        return unknown('invalid_arithmetic')
    return project_clips(clips, viewport, cb0[56:59])


def project_clips(clips, viewport, translation):
    def unknown(reason):
        return dict(status='unavailable', reason=reason)
    if not clips or not all(math.isfinite(x) for clip in clips for x in clip):
        return unknown('nonfinite_clip')
    # No guessed near-plane clipping: a box from projected vertices is invalid there.
    if any(clip[3] <= 0 or clip[2] < 0 or clip[2] > clip[3] for clip in clips):
        return unknown('depth_or_w_clipping_required')
    points = [(viewport[1] + (clip[0] / clip[3] + 1) * viewport[3] / 2,
               viewport[2] + (1 - clip[1] / clip[3]) * viewport[4] / 2) for clip in clips]
    bounds = [min(p[0] for p in points), min(p[1] for p in points),
              max(p[0] for p in points), max(p[1] for p in points)]
    padding = 2
    roi = [math.floor(bounds[0]) - padding, math.floor(bounds[1]) - padding,
           math.ceil(bounds[2]) + padding + 1, math.ceil(bounds[3]) + padding + 1]
    return dict(status='projected', boundsPixels=bounds, paddedRoi=roi, paddingPixels=padding,
                indexedVertices=len(clips), uniquePositions=len(set(points)),
                wRange=[min(c[3] for c in clips), max(c[3] for c in clips)],
                depthRange=[min(c[2] / c[3] for c in clips), max(c[2] / c[3] for c in clips)],
                modelTranslation=list(translation), viewport=viewport)


def project_mesh(draw, layouts):
    """Project ranged vertex/index readbacks for the exact static object shader pair."""
    def unknown(reason):
        return dict(status='unavailable', reason=reason)
    if draw.get('vs') != MESH_VS or draw.get('ps') != MESH_PS or draw.get('slot') != 6:
        return unknown('unsupported_shader_or_stage')
    if model_key(draw) is None:
        return unknown('missing_current_model_identity')
    args, geometry = draw.get('args', []), draw.get('geometry', [])
    if (draw.get('kind') != 'Indexed' or len(args) != 3 or len(geometry) != 6 or
            not all(type(n) is int for n in (*args, *geometry)) or not 0 < args[0] <= 4096 or
            args[1] < 0 or not -0x80000000 <= args[2] <= 0xffffffff):
        return unknown('unsupported_draw')
    _, stride, vertex_offset, _, fmt, index_offset = geometry
    index_bytes = {57: 2, 42: 4}.get(fmt)
    if not index_bytes or stride < 12 or min(vertex_offset, index_offset) < 0:
        return unknown('unsupported_geometry')
    layout = layouts.get(draw.get('inputLayout'))
    if not layout or not layout.get('known'):
        return unknown('unknown_layout')
    position_offset, next_offsets = None, {}
    for element in layout.get('elements', []):
        if not isinstance(element, list) or len(element) != 7:
            return unknown('invalid_layout')
        name, index, format_id, slot, offset, classification, step = element
        if not all(type(n) is int and n >= 0 for n in element[1:]):
            return unknown('invalid_layout')
        if offset == 0xffffffff:
            offset = next_offsets.get(slot, 0)
        width = FORMAT_BYTES.get(format_id)
        next_offsets[slot] = offset + width if offset is not None and width else None
        if isinstance(name, str) and name.upper() == 'POSITION' and index == 0:
            if position_offset is not None:
                return unknown('duplicate_position')
            if format_id != 6 or slot != 0 or classification or step or offset is None or offset + 12 > stride:
                return unknown('unsupported_position_layout')
            position_offset = offset
    if position_offset is None:
        return unknown('missing_position')
    inputs = draw.get('meshInputs', {})
    proxy = dict(draw, flagInputs=inputs)
    buffers = {}
    for name in ('cb0', 'cb3', 'vertex0', 'index'):
        buffers[name], status = readback(proxy, name)
        if status != 'ok':
            return unknown(name + ':' + status)
    if any(len(buffers[name]) % 4 for name in ('cb0', 'cb3')) or len(buffers['cb0']) < 208 or len(buffers['cb3']) < 128:
        return unknown('unexpected_constant_buffer_size')
    for name in ('cb0', 'cb3'):
        if inputs[name].get('offset', 0) != 0:
            return unknown('ranged_constant_buffer')
    for name in ('vertex0', 'index'):
        item = inputs[name]
        if (not all(type(item.get(k)) is int and 0 <= item[k] <= 0xffffffff for k in ('offset', 'resourceBytes')) or
                item['offset'] + len(buffers[name]) > item['resourceBytes']):
            return unknown(name + ':invalid_range_metadata')
    index_start = index_offset + args[1] * index_bytes
    if inputs['index']['offset'] != index_start or len(buffers['index']) != args[0] * index_bytes:
        return unknown('index_range_mismatch')
    base = args[2] - 0x100000000 if args[2] >= 0x80000000 else args[2]
    indices = struct.unpack('<' + ('H' if index_bytes == 2 else 'I') * args[0], buffers['index'])
    actual = [index + base for index in indices]
    first, last = min(actual), max(actual)
    if first < 0:
        return unknown('negative_vertex_index')
    if type(inputs.get('vertexFirst')) is not int or inputs['vertexFirst'] != first:
        return unknown('vertex_first_mismatch')
    if (inputs['vertex0']['offset'] != vertex_offset + first * stride or
            len(buffers['vertex0']) != (last - first + 1) * stride):
        return unknown('vertex_range_mismatch')
    if buffers['cb0'][144:208].hex() != draw['objectInputs']['fields']['perInstanceModelMatrix']:
        return unknown('observed_model_disagrees_with_gpu')
    model = struct.unpack_from('<16f', buffers['cb0'], 144)
    vp = struct.unpack_from('<16f', buffers['cb3'], 64)
    if not all(math.isfinite(n) for n in (*model, *vp)):
        return unknown('nonfinite_constants')
    viewport = draw.get('viewport', [])
    if (not isinstance(viewport, list) or len(viewport) != 7 or viewport[0] != 1 or
            not all(type(x) in (int, float) and math.isfinite(x) for x in viewport) or
            not 0 < viewport[3] <= 4096 or not 0 < viewport[4] <= 4096):
        return unknown('unsupported_viewport')
    clips = []
    try:
        for index in actual:
            position = struct.unpack_from('<3f', buffers['vertex0'], (index - first) * stride + position_offset)
            if not all(math.isfinite(n) for n in position):
                return unknown('nonfinite_vertex')
            # The shader forms world.xyz and explicitly adds VP column3: world.w is1.
            world = [add(mad(model[8 + axis], position[2],
                             mad(model[axis], position[0], mul(position[1], model[4 + axis]))),
                         model[12 + axis]) for axis in range(3)]
            clips.append(matrix_position([vp[i:i + 4] for i in range(0, 16, 4)], [*world, 1.0]))
    except (OverflowError, ValueError):
        return unknown('invalid_arithmetic')
    result = project_clips(clips, viewport, model[12:15])
    result['modelTranslation'] = list(model[12:15])
    result['clipSummary'] = dict(vertices=len(clips), behindOrOnEye=sum(c[3] <= 0 for c in clips),
                                 wRange=[min(c[3] for c in clips), max(c[3] for c in clips)])
    result['bufferRange'] = dict(vertexFirst=first, vertexLast=last,
                                 vertexOffset=inputs['vertex0']['offset'], indexOffset=index_start,
                                 signedBaseVertex=base)
    return result


def image_region(path, projection):
    from PIL import Image, ImageStat
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_IMAGE_BYTES:
        return dict(status='missing_linked_or_oversized')
    with Image.open(path) as source:
        if source.format != 'PNG' or source.width * source.height > MAX_IMAGE_PIXELS:
            return dict(status='unsupported_image')
        viewport = projection['viewport']
        if viewport[1:3] != [0, 0] or list(source.size) != viewport[3:5]:
            return dict(status='viewport_image_mismatch', imageSize=list(source.size))
        box = projection['paddedRoi']
        clipped = [max(0, box[0]), max(0, box[1]), min(source.width, box[2]), min(source.height, box[3])]
        if clipped[0] >= clipped[2] or clipped[1] >= clipped[3]:
            return dict(status='offscreen')
        region = source.convert('RGB').crop(clipped)
        stats = ImageStat.Stat(region)
        return dict(status='sampled', clippedRoi=clipped, pixels=region.width * region.height,
                    imageSha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                    regionSha256=hashlib.sha256(region.tobytes()).hexdigest(),
                    meanRgb=stats.mean, deviationRgb=stats.stddev,
                    minMaxRgb=[list(v) for v in region.getextrema()])


def pixel_witness(rows, path=None):
    """Validate the actual RGB-write mask separately from approximate mesh projection."""
    if len(rows) != 1:
        return dict(status='missing' if not rows else 'ambiguous')
    row = rows[0]
    if row.get('status') != 'ok':
        return dict(status='unavailable', captureStatus=row.get('status'))
    def invalid(reason):
        return dict(status='invalid', reason=reason)
    integer = lambda value: type(value) is int and value >= 0
    width, height, fmt = row.get('width'), row.get('height'), row.get('format')
    if (not integer(width) or not integer(height) or not width or not height or
            fmt not in PIXEL_BYTES or width * height * PIXEL_BYTES[fmt] > 8 * 1024 * 1024):
        return invalid('dimensions_or_format')
    fields = ('changedPixels', 'retainedPixels', 'revertedPixels', 'otherPixels')
    if not all(integer(row.get(field)) for field in fields):
        return invalid('pixel_counts')
    if (row['changedPixels'] > width * height or
            row['changedPixels'] != sum(row[field] for field in fields[1:])):
        return invalid('pixel_count_sum')
    target, final = row.get('target'), row.get('finalSource')
    if (not integer(target) or not target or type(row.get('finalSourceSame')) is not bool or
            not isinstance(final, list) or len(final) != 4 or not all(map(integer, final))):
        return invalid('target_metadata')
    if row['finalSourceSame'] != bool(final[0] and final[0] == target):
        return invalid('final_source_identity')
    if row['finalSourceSame'] and final[1:] != [width, height, fmt]:
        return invalid('final_source_descriptor')
    bounds, runs = row.get('bounds'), row.get('maskRuns')
    if (not isinstance(bounds, list) or len(bounds) != 4 or not all(map(integer, bounds)) or
            bounds[0] > bounds[2] or bounds[1] > bounds[3] or bounds[2] > width or bounds[3] > height or
            not isinstance(runs, list) or len(runs) > 4096 or type(row.get('maskTruncated')) is not bool):
        return invalid('mask_metadata')
    observed, previous, derived = 0, None, [width, height, 0, 0]
    for run in runs:
        if not isinstance(run, list) or len(run) != 3 or not all(map(integer, run)):
            return invalid('mask_run')
        y, x, count = run
        if not count or y >= height or x + count > width:
            return invalid('mask_run_bounds')
        if previous and (y < previous[0] or (y == previous[0] and x < previous[1])):
            return invalid('mask_run_overlap_or_order')
        if not bounds[0] <= x < x + count <= bounds[2] or not bounds[1] <= y < bounds[3]:
            return invalid('mask_outside_declared_bounds')
        previous = (y, x + count)
        observed += count
        derived = [min(derived[0], x), min(derived[1], y), max(derived[2], x + count), max(derived[3], y + 1)]
    if not runs:
        derived = [0, 0, 0, 0]
    if observed > row['changedPixels'] or (not row['maskTruncated'] and
                                          (observed != row['changedPixels'] or bounds != derived)):
        return invalid('mask_count_or_bounds')
    result = dict(status='captured', **{field: row[field] for field in fields}, bounds=bounds,
                  target=target, finalSourceSame=row['finalSourceSame'], finalSource=final,
                  maskRuns=len(runs), observedMaskPixels=observed, maskTruncated=row['maskTruncated'],
                  interpretation=('RGB changes caused by this draw and later equality on its original target. '
                                  'Equality is not object survival, and a different final target is not compared.'),
                  finalPngMask=dict(status='unavailable_capture'))
    if row['maskTruncated']:
        result['finalPngMask'] = dict(status='truncated_mask')
        return result
    if not row['finalSourceSame']:
        result['finalPngMask'] = dict(status='different_final_source')
        return result
    if path is None:
        return result
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_IMAGE_BYTES:
        result['finalPngMask'] = dict(status='missing_linked_or_oversized')
        return result
    from PIL import Image
    with Image.open(path) as image:
        if image.format != 'PNG' or list(image.size) != [width, height]:
            result['finalPngMask'] = dict(status='target_image_mismatch')
            return result
        rgb = image.convert('RGB')
        histograms = [[0] * 256 for _ in range(3)]
        digest = hashlib.sha256()
        for y, x, count in runs:
            strip = rgb.crop((x, y, x + count, y + 1))
            digest.update(strip.tobytes())
            histogram = strip.histogram()
            for channel in range(3):
                for value in range(256):
                    histograms[channel][value] += histogram[channel * 256 + value]
        means, deviations, extrema = [], [], []
        for histogram in histograms:
            mean = sum(value * count for value, count in enumerate(histogram)) / observed if observed else None
            means.append(mean)
            deviations.append(math.sqrt(sum((value - mean) ** 2 * count for value, count in enumerate(histogram)) / observed)
                              if observed else None)
            values = [value for value, count in enumerate(histogram) if count]
            extrema.append([min(values), max(values)] if values else None)
        result['finalPngMask'] = dict(status='sampled' if observed else 'empty_mask', pixels=observed,
                                     imageSha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                     orderedMaskRgbSha256=digest.hexdigest(), meanRgb=means,
                                     deviationRgb=deviations, minMaxRgb=extrema)
    return result


def analyze(run):
    run = Path(run)
    path = run / 'flicker-draws.jsonl'
    if path.is_symlink() or path.stat().st_size > MAX_LOG:
        raise ValueError('Linked or oversized flicker log.')
    raw = path.read_bytes()
    if len(raw) > MAX_LOG:
        raise ValueError('Oversized flicker log.')
    rows, malformed = [], 0
    for line in raw.splitlines():
        try:
            row = json.loads(line)
            if not isinstance(row, dict):
                raise ValueError('Non-object record.')
            rows.append(row)
        except (ValueError, UnicodeDecodeError):
            malformed += 1
    layouts = {}
    for row in rows:
        if row.get('type') == 'layout':
            identity = row.get('id')
            layouts[identity] = row if identity not in layouts else None
    images, completed, samples, draw_keys = {}, {}, {}, Counter()
    witnesses = {'flagPixels': {}, 'meshPixels': {}}
    for row in rows:
        kind = row.get('type')
        key = (row.get('scene'), row.get('pass'), row.get('ordinal'))
        if kind == 'draw':
            draw_keys[key] += 1
        if kind in witnesses:
            witnesses[kind].setdefault(key, []).append(row)
        if kind == 'image':
            images.setdefault((row.get('scene'), row.get('eye')), []).append(row)
        if kind == 'complete':
            completed.setdefault(row.get('scene'), []).append(row)
        if kind == 'samples':
            samples.setdefault(key, []).append(row)
    projected, meshes, matched_draws, matched_meshes, mesh_inputs, unmasked_mesh_inputs = [], [], 0, 0, 0, 0
    for draw in rows:
        if draw.get('type') != 'draw':
            continue
        scene, eye, ordinal = draw.get('scene'), draw.get('pass'), draw.get('ordinal')
        key = (scene, eye, ordinal)
        is_flag = draw.get('vs') == COLOUR_VS and draw.get('slot') == 6
        is_mesh = key in witnesses['meshPixels']
        if 'meshInputs' in draw:
            mesh_inputs += 1
            unmasked_mesh_inputs += not is_mesh
        if not is_flag and not is_mesh:
            continue
        if is_flag:
            matched_draws += 1
            if len(projected) == MAX_FLAG_PROJECTIONS:
                continue
        else:
            matched_meshes += 1
            if len(meshes) == MAX_PROJECTIONS:
                continue
        result = dict(kind='flag' if is_flag else 'mesh', scene=scene, eye=eye, ordinal=ordinal,
                      drawable=draw.get('drawable'), vs=draw.get('vs'), ps=draw.get('ps'), args=draw.get('args'),
                      geometry=draw.get('geometry'), projection=(project_draw if is_flag else project_mesh)(draw, layouts),
                      samples=sample_value(samples.get((scene, eye, ordinal), [])))
        image_rows = images.get((scene, eye), [])
        completion = completed.get(scene, [])
        result['finalImage'] = dict(status='unavailable_capture')
        expected = f'stage-{scene}-flicker-eye{eye}.ppm'
        image_valid = (type(scene) is int and eye in (0, 1, 2) and len(image_rows) == 1 and
                image_rows[0].get('status') == 'saved' and image_rows[0].get('file') == expected and
                len(completion) == 1 and completion[0].get('accepted') == 1 and
                completion[0].get('truncated') == [0, 0, 0])
        image_path = run / expected.replace('.ppm', '.png') if image_valid else None
        if image_valid and result['projection']['status'] == 'projected':
            result['finalImage'] = image_region(image_path, result['projection'])
        witness_kind = 'flagPixels' if is_flag else 'meshPixels'
        result['pixelWitness'] = (pixel_witness(witnesses[witness_kind].get(key, []), image_path)
                                  if draw_keys[key] == 1 else dict(status='ambiguous_draw'))
        (projected if is_flag else meshes).append(result)
    return dict(schema=2, logSha256=hashlib.sha256(raw).hexdigest(), malformedLines=malformed,
                logTruncated=any(r.get('type') == 'truncated' for r in rows),
                burstEnded=any(r.get('type') == 'end' for r in rows),
                drawCount=matched_draws, limitReached=matched_draws > MAX_FLAG_PROJECTIONS,
                projectionCounts=dict(Counter(p['projection']['status'] for p in projected)),
                finalImageCounts=dict(Counter(p['finalImage']['status'] for p in projected)),
                pixelWitnessCounts=dict(Counter(p['pixelWitness']['status'] for p in projected)),
                meshDrawCount=matched_meshes, meshLimitReached=matched_meshes > MAX_PROJECTIONS,
                meshInputsDrawCount=mesh_inputs, meshInputsWithoutWitness=unmasked_mesh_inputs,
                meshProjectionCounts=dict(Counter(p['projection']['status'] for p in meshes)),
                meshFinalImageCounts=dict(Counter(p['finalImage']['status'] for p in meshes)),
                meshPixelWitnessCounts=dict(Counter(p['pixelWitness']['status'] for p in meshes)),
                flags=projected, meshes=meshes, limits=LIMITS)


def contact_sheet(run, result, target, kind='flags'):
    """A fixed-size contextual crop for human inspection; no automatic object classifier."""
    from PIL import Image, ImageDraw
    if kind == 'mixed':
        entries = []
        for index in range(24):
            for name in ('flags', 'meshes'):
                if index < len(result.get(name, [])):
                    entries.append(result[name][index])
    elif kind in ('flags', 'meshes'):
        entries = result.get(kind, [])
    else:
        raise ValueError('Unknown contact-sheet kind.')
    entries = entries[:24]
    sheet = Image.new('RGB', (720, max(1, (len(entries) + 2) // 3) * 180), '#171717')
    pen = ImageDraw.Draw(sheet)
    for index, item in enumerate(entries):
        x, y = index % 3 * 240, index // 3 * 180
        pen.text((x + 4, y + 3), f"{item.get('kind', 'flag')} {item['scene']} e{item['eye']} #{item['ordinal']} n{item['samples']['passed']}", fill='white')
        if item['finalImage']['status'] != 'sampled':
            pen.text((x + 4, y + 30), item['finalImage']['status'], fill='white')
            continue
        box = item['projection']['boundsPixels']
        center_x, center_y = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        crop_box = [int(center_x) - 56, int(center_y) - 38, int(center_x) + 56, int(center_y) + 38]
        source = Path(run) / f"stage-{item['scene']}-flicker-eye{item['eye']}.png"
        with Image.open(source) as original:
            crop = original.convert('RGB').crop(crop_box)
        marked = ImageDraw.Draw(crop)
        marked.rectangle((box[0] - crop_box[0], box[1] - crop_box[1],
                          box[2] - crop_box[0], box[3] - crop_box[1]), outline='#ff4040')
        sheet.paste(crop.resize((224, 152), Image.Resampling.NEAREST), (x + 4, y + 22))
    sheet.save(target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True, help='Explicit new evidence path; never overwrite retained summaries.')
    parser.add_argument('--contact-sheet', type=Path)
    parser.add_argument('--contact-kind', choices=('flags', 'meshes', 'mixed'), default='flags')
    args = parser.parse_args()
    result = analyze(args.run)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    if args.contact_sheet:
        contact_sheet(args.run, result, args.contact_sheet, args.contact_kind)
    print(args.output)
