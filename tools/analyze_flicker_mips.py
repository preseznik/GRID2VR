"""Validate bounded alpha-mip readbacks. Whole-mip coverage is not object visibility."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

from analyze_flicker_inputs import (ALPHA_SAMPLERS, COLOUR_VS, DEPTH_VS,
                                   alpha_sampling_state, model_key)

MAX_LOG = 32 * 1024 * 1024
MAX_BYTES = 64 * 1024 * 1024
MAX_EXAMPLES = 24
SCOPE = 'whole_mip_unfiltered_not_object_uv_or_pixel_visibility'
FORMATS = {28, 29, 70, 71, 72, 76, 77, 78, 87, 91}
IDENTITY = ('slot', 'kind', 'vs', 'ps', 'drawable', 'properties', 'args', 'geometry', 'instanceStreams')


def integer(value, low=0, high=0xffffffff):
    return type(value) is int and low <= value <= high


def location(row):
    return row.get('scene'), row.get('pass'), row.get('ordinal')


def validate_log(records):
    if not records or not all(isinstance(row, dict) and isinstance(row.get('type'), str) for row in records):
        raise ValueError('Missing or non-object records.')
    if records[0].get('type') != 'start' or records[-1].get('type') != 'end':
        raise ValueError('Missing capture boundary.')
    kinds = Counter(row.get('type') for row in records)
    if kinds['start'] != 1 or kinds['end'] != 1 or kinds['truncated'] or kinds['incomplete']:
        raise ValueError('Duplicate, truncated or incomplete capture boundary.')
    scenes = [row.get('scene') for row in records if row.get('type') == 'scene']
    if (not 1 <= len(scenes) <= 8 or not all(integer(scene, 1, 2**64 - 1) for scene in scenes) or
            len(set(scenes)) != len(scenes) or records[-1].get('scenes') != len(scenes)):
        raise ValueError('Invalid scene coverage.')
    completes, draws = defaultdict(list), defaultdict(list)
    for row in records:
        kind = row.get('type')
        if kind in ('draw', 'alphaTexture'):
            scene, eye, ordinal = location(row)
            if scene not in scenes or not integer(eye, 0, 2) or not integer(ordinal, 1, 4096):
                raise ValueError('Invalid draw/sample location.')
            if kind == 'draw':
                draws[(scene, eye)].append(ordinal)
        elif kind == 'complete':
            completes[row.get('scene')].append(row)
    if set(completes) != set(scenes):
        raise ValueError('Missing or extra scene completion.')
    for scene in scenes:
        if len(completes[scene]) != 1:
            raise ValueError('Duplicate scene completion.')
        row = completes[scene][0]
        if (row.get('accepted') not in (True, 1) or row.get('truncated') != [0, 0, 0] or
                row.get('draws') != [len(draws[(scene, eye)]) for eye in range(3)]):
            raise ValueError('Rejected, truncated or miscounted scene.')
        for eye in range(3):
            ordinals = draws[(scene, eye)]
            if sorted(ordinals) != list(range(1, len(ordinals) + 1)):
                raise ValueError('Missing or duplicate draw ordinal.')
    return scenes


def checked_digest(sample, draw):
    """Return only canonical validated contents; pointer identity is not a digest."""
    textures = draw.get('textures')
    if (not isinstance(textures, list) or len(textures) != 8 or not all(integer(x) for x in textures) or
            sample.get('view') != textures[0]):
        return None, 'draw_view_mismatch'
    if not integer(draw.get('ps'), 0, 2**64 - 1):
        return None, 'invalid_pixel_shader'
    state, reason = alpha_sampling_state(draw)
    if state is None:
        return None, 'sampling_' + reason
    if sample.get('view') != state['view'] or sample.get('resource') != state['resource']:
        return None, 'resource_or_view_mismatch'
    if not integer(sample.get('remainingBudget'), 0, MAX_BYTES):
        return None, 'invalid_budget'
    digest = sample.get('digest')
    if not isinstance(digest, dict):
        return None, 'missing_digest'
    if digest.get('status') != 'ok':
        return None, 'digest_' + str(digest.get('status', 'missing'))
    width, height, mips, layers, fmt = (digest.get(name) for name in ('width', 'height', 'mips', 'array', 'format'))
    if (not integer(width, 1, 16384) or not integer(height, 1, 16384) or
            not integer(mips, 1, max(width, height).bit_length()) or not integer(layers, 1, 2048) or
            not integer(fmt) or fmt not in FORMATS or not integer(digest.get('samples'), 1, 1) or
            not integer(digest.get('quality'), 0, 0)):
        return None, 'unsupported_descriptor'
    if state['texture'] != [width, height, mips, layers, fmt]:
        return None, 'stale_texture_descriptor'
    if digest.get('alphaCoverage') != dict(threshold=0.5, comparison='greater_equal', scope=SCOPE):
        return None, 'invalid_alpha_definition'
    entries = digest.get('subresources')
    if not isinstance(entries, list) or len(entries) != mips * layers:
        return None, 'missing_subresources'
    canonical, seen, total = [], set(), 0
    for item in entries:
        if not isinstance(item, dict):
            return None, 'invalid_subresource'
        mip, layer = item.get('mip'), item.get('slice')
        if not integer(mip, 0, mips - 1) or not integer(layer, 0, layers - 1) or (mip, layer) in seen:
            return None, 'duplicate_or_invalid_subresource'
        seen.add((mip, layer))
        w, h = max(1, width >> mip), max(1, height >> mip)
        if fmt in (70, 71, 72, 76, 77, 78):
            row_bytes, rows = ((w + 3) // 4) * (16 if fmt in (76, 77, 78) else 8), (h + 3) // 4
        else:
            row_bytes, rows = w * 4, h
        digest_hash = item.get('sha256')
        if (not integer(item.get('rowBytes'), row_bytes, row_bytes) or not integer(item.get('rows'), rows, rows) or
                not isinstance(digest_hash, str) or not re.fullmatch('[0-9a-f]{64}', digest_hash)):
            return None, 'invalid_hash_or_layout'
        alpha = item.get('alpha')
        if not isinstance(alpha, dict) or alpha.get('status') != 'ok':
            return None, 'alpha_unavailable'
        texels, above, below = (alpha.get(name) for name in ('texels', 'atLeastHalf', 'belowHalf'))
        if (not integer(texels, 1, 16384**2) or texels != w * h or not integer(above, 0, texels) or
                not integer(below, 0, texels) or above + below != texels):
            return None, 'invalid_alpha_counts'
        canonical.append((mip, layer, row_bytes, rows, digest_hash, texels, above, below))
        total += row_bytes * rows
    if not integer(digest.get('bytes'), 1, MAX_BYTES) or digest['bytes'] != total:
        return None, 'invalid_byte_count'
    return (width, height, mips, layers, fmt, tuple(sorted(canonical))), 'ok'


def draw_identity(draw):
    if (not all(name in draw for name in IDENTITY) or not isinstance(draw['kind'], str) or
            not all(integer(draw[name], 0, 2**64 - 1) for name in ('slot', 'vs', 'ps', 'drawable', 'properties')) or
            not isinstance(draw['args'], list) or not all(integer(x) for x in draw['args']) or
            not isinstance(draw['geometry'], list) or len(draw['geometry']) != 6 or
            not all(integer(x) for x in draw['geometry']) or not isinstance(draw['instanceStreams'], list) or
            not all(isinstance(stream, list) and len(stream) == 4 and all(integer(x) for x in stream)
                    for stream in draw['instanceStreams'])):
        return None
    return json.dumps([draw[name] for name in IDENTITY], separators=(',', ':'))


def summary():
    return dict(compared=0, equal=0, changed=0, unavailable=0, ambiguous=0, unmatched=0, examples=[])


def analyze_records(records):
    scenes = validate_log(records)
    draws = {location(row): row for row in records if row.get('type') == 'draw'}
    samples, valid, reasons = defaultdict(list), {}, Counter()
    for row in records:
        if row.get('type') == 'alphaTexture':
            samples[location(row)].append(row)
    coverage, transparent_examples, transparent, alpha_records, valid_bytes = [], [], 0, 0, 0
    variants = defaultdict(list)
    for key, rows in samples.items():
        if len(rows) != 1:
            reasons['duplicate_sample'] += 1
            continue
        draw = draws.get(key)
        if draw is None:
            reasons['orphan_sample'] += 1
            continue
        value, reason = checked_digest(rows[0], draw)
        if value is None:
            reasons[reason] += 1
            continue
        valid[key] = value
        variants[(rows[0]['resource'], value)].append(draw)
        valid_bytes += rows[0]['digest']['bytes']
        for mip, layer, _, _, _, texels, above, _ in value[5]:
            alpha_records += 1
            transparent += above == 0
            item = dict(scene=key[0], eye=key[1], ordinal=key[2], resource=rows[0]['resource'],
                        mip=mip, slice=layer, texels=texels, atLeastHalf=above, fraction=above / texels)
            if len(coverage) < MAX_EXAMPLES:
                coverage.append(item)
            if above == 0 and len(transparent_examples) < MAX_EXAMPLES:
                transparent_examples.append(item)
    if valid_bytes > MAX_BYTES:
        raise ValueError('Successful texture evidence exceeds the readback budget.')
    stereo, flags = summary(), summary()

    def compare(left, right, target, label):
        x, y = valid.get(location(left)), valid.get(location(right))
        if x is None or y is None:
            target['unavailable'] += 1
            return
        target['compared'] += 1
        status = 'equal' if x == y else 'changed'
        target[status] += 1
        if status == 'changed' and len(target['examples']) < MAX_EXAMPLES:
            left_mips = {(m[0], m[1]): m for m in x[5]}
            right_mips = {(m[0], m[1]): m for m in y[5]}
            changed = sorted(k for k in left_mips.keys() | right_mips.keys() if left_mips.get(k) != right_mips.get(k))
            target['examples'].append(dict(comparison=label, scene=left['scene'], passes=[left['pass'], right['pass']],
                ordinals=[left['ordinal'], right['ordinal']], descriptorChanged=x[:5] != y[:5], changedMips=changed[:MAX_EXAMPLES]))

    eye_groups, flag_groups = defaultdict(lambda: [[], [], []]), defaultdict(lambda: [[], []])
    invalid_identity, missing_flags = 0, 0
    for key, draw in draws.items():
        if not integer(draw.get('ps'), 0, 2**64 - 1) or draw.get('ps') not in ALPHA_SAMPLERS:
            continue
        identity = draw_identity(draw)
        if identity is None:
            invalid_identity += 1
        else:
            eye_groups[(key[0], identity)][key[1]].append(draw)
        if (draw.get('vs'), draw.get('slot')) in ((DEPTH_VS, 5), (COLOUR_VS, 6)):
            if key not in samples:
                missing_flags += 1
            observed = draw.get('objectInputs')
            identity = model_key(draw) if isinstance(observed, dict) and isinstance(observed.get('fields'), dict) else None
            if identity is not None:
                flag_groups[(key[0], key[1], identity)][draw['vs'] == COLOUR_VS].append(draw)
            else:
                invalid_identity += 1
    for passes in eye_groups.values():
        for eye in (1, 2):
            left, right = passes[0], passes[eye]
            if not left and not right:
                continue
            if len(left) > 1 or len(right) > 1:
                stereo['ambiguous'] += 1
            elif not left or not right:
                stereo['unmatched'] += 1
            else:
                compare(left[0], right[0], stereo, 'native_eye')
    for left, right in flag_groups.values():
        if len(left) > 1 or len(right) > 1:
            flags['ambiguous'] += 1
        elif not left or not right:
            flags['unmatched'] += 1
        elif (location(left[0]) not in valid or location(right[0]) not in valid or
              samples[location(left[0])][0]['resource'] != samples[location(right[0])][0]['resource']):
            flags['unavailable'] += 1
        else:
            compare(left[0], right[0], flags, 'flag_prepass_colour_same_resource')
    timeline, temporal = defaultdict(lambda: defaultdict(set)), summary()
    for key, value in valid.items():
        timeline[samples[key][0]['resource']][key[0]].add(value)
    conflicts = 0
    for resource, observed in timeline.items():
        previous = None
        for scene, values in sorted(observed.items()):
            if len(values) != 1:
                conflicts += 1
                previous = None
                continue
            value = next(iter(values))
            if previous is not None:
                temporal['compared'] += 1
                status = 'equal' if previous[1] == value else 'changed'
                temporal[status] += 1
                if status == 'changed' and len(temporal['examples']) < MAX_EXAMPLES:
                    temporal['examples'].append(dict(resource=resource, scenes=[previous[0], scene]))
            previous = scene, value
    variant_examples = []
    for (resource, value), observed in list(variants.items())[:MAX_EXAMPLES]:
        variant_examples.append(dict(resource=resource, width=value[0], height=value[1], mips=value[2],
            array=value[3], format=value[4], sampleCount=len(observed),
            firstScene=min(draw['scene'] for draw in observed), lastScene=max(draw['scene'] for draw in observed),
            shaders=sorted({f"{draw['vs']:016x}" for draw in observed})[:MAX_EXAMPLES],
            coverage=[dict(mip=m[0], slice=m[1], texels=m[5], atLeastHalf=m[6], fraction=m[6] / m[5])
                      for m in value[5][:MAX_EXAMPLES]], coverageTruncated=len(value[5]) > MAX_EXAMPLES))
    return dict(schema=1, complete=True, scenes=len(scenes), sampleRecords=sum(map(len, samples.values())),
                validatedSamples=len(valid), validatedBytes=valid_bytes, unavailableReasons=dict(reasons),
                missingFlagSamples=missing_flags, invalidDrawIdentities=invalid_identity,
                nativeEye=stereo, flagPrepassColour=flags, acrossScenes=temporal,
                resourceSceneConflicts=conflicts, mipRecords=alpha_records, fullyTransparentMipRecords=transparent,
                fullyTransparentMipExamples=transparent_examples,
                textureVariants=len(variants), textureVariantExamples=variant_examples,
                textureVariantExamplesTruncated=len(variants) > len(variant_examples),
                coverageExamples=coverage, coverageExamplesTruncated=alpha_records > len(coverage),
                limits=('Fresh recorded mip SHA256 and decoded whole-mip alpha >= 0.5 only. Coverage is unfiltered '
                        'and not object-UV-specific, sampled LOD, final visibility, flicker proof or headset acceptance. '
                        'Other alpha resources are sampled at most four per pass; missing samples stay unavailable. '
                        'Flag pairs require unique geometry/args/current observed model and the same bound resource. '
                        'Across-scene resource pointers may be reused; changes do not establish the same resource lifetime. '
                        'SHA256 syntax and metadata are checked; raw mip bytes are not retained for independent rehashing.'))


def analyze(run):
    path = Path(run) / 'flicker-draws.jsonl'
    if path.is_symlink() or path.stat().st_size > MAX_LOG:
        raise ValueError('Linked or oversized flicker log.')
    raw = path.read_bytes()
    if len(raw) > MAX_LOG or not raw.endswith(b'\n'):
        raise ValueError('Oversized or partial final log line.')
    def reject_constant(value):
        raise ValueError('Nonfinite JSON value: ' + value)
    try:
        records = [json.loads(line, parse_constant=reject_constant) for line in raw.splitlines()]
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError('Malformed flicker log.') from exc
    return analyze_records(records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    target = args.output or args.run / 'flicker-mips-summary.json'
    target.write_text(json.dumps(analyze(args.run), indent=2) + '\n', encoding='utf-8')
    print(target)
