"""Summarize one bounded passive burst. Draw equality is not visual acceptance."""
import argparse
from collections import Counter, defaultdict
import hashlib
import io
import json
from pathlib import Path
import re

MAX_LOG = 32 * 1024 * 1024
MAX_PNG = 32 * 1024 * 1024
VISIBILITY_PREVIEW_EDGE = 640
OBJECT_FIELD_BYTES = dict(model=64, perInstanceModelMatrix=64, localMatrix=64,
                          winddirection=16, _timer=4, _WindMagnitude=4, _Magnitude=4, _SinScale=4)


def compare_object_inputs(passes, scene):
    """Compare observed non-camera upload fields, never whole CB0 hashes."""
    result = dict(eyeComparisons=dict(compared=0, changed=0, unavailable=0, examples=[]),
                  depthColourComparisons=dict(compared=0, changed=0, unavailable=0, examples=[]),
                  ambiguousDepthColourGroups=0,
                  capturedDraws=[sum('objectInputs' in d for d in p) for p in passes])

    def compare(a, b, summary):
        left, right = a.get('objectInputs', {}), b.get('objectInputs', {})
        lf, rf = left.get('fields', {}), right.get('fields', {})
        for name in sorted(lf.keys() | rf.keys()):
            size = OBJECT_FIELD_BYTES.get(name)
            if not size:  # Camera or unverified fields cannot become object drift.
                continue
            x, y = lf.get(name), rf.get(name)
            valid = (left.get('known') and right.get('known') and
                     left.get('scene') == right.get('scene') == scene and
                     isinstance(x, str) and isinstance(y, str) and
                     re.fullmatch('[0-9a-f]{%d}' % (size * 2), x) and
                     re.fullmatch('[0-9a-f]{%d}' % (size * 2), y))
            if not valid:
                summary['unavailable'] += 1
                continue
            summary['compared'] += 1
            if x != y:
                summary['changed'] += 1
                if len(summary['examples']) < 12:
                    summary['examples'].append(dict(field=name, passes=[a['pass'], b['pass']],
                        ordinals=[a['ordinal'], b['ordinal']], shaders=[a['vs'], b['vs']],
                        values=[x, y]))

    for eye in (1, 2):
        # Ordinal pairing alone is insufficient if membership or ordering changed.
        for a, b in zip(passes[0], passes[eye]):
            if identity(a) == identity(b):
                compare(a, b, result['eyeComparisons'])
    for draws in passes:
        groups = defaultdict(lambda: [[], []])
        for d in draws:
            # Native stage 5 is the main prepass; stage 0 contains other work.
            if 'objectInputs' not in d or d['slot'] not in (5, 6):
                continue
            key = json.dumps([d[k] for k in ('drawable', 'kind', 'args', 'geometry')]
                             + [d.get('instanceStreams')], separators=(',', ':'))
            groups[key][d['slot'] == 6].append(d)
        for depth, colour in groups.values():
            if not depth or not colour:
                continue
            if len(depth) != 1 or len(colour) != 1:
                result['ambiguousDepthColourGroups'] += 1
                continue
            compare(depth[0], colour[0], result['depthColourComparisons'])
    result['depthColourStages'] = [5, 6]
    result['limits'] = ('Observed CPU upload fields only; unobserved writes and instance-buffer contents '
                        'are outside coverage. Same geometry across stages is a candidate pairing. '
                        'A difference is a lead, not proof of a disappearing object or a safe substitution.')
    return result


def identity(draw):
    # Camera/material constants may legitimately differ between the eyes.
    # Compare draw membership separately from those recorded inputs.
    return json.dumps([draw[k] for k in
                       ('slot', 'kind', 'vs', 'ps', 'drawable', 'properties', 'args', 'geometry')]
                      + [draw.get('instanceStreams')], separators=(',', ':'))


def analyze(run):
    path = run / 'flicker-draws.jsonl'
    if path.stat().st_size > MAX_LOG:
        raise ValueError('Flicker log exceeds its capture bound.')
    records = []
    partial = False
    for line in path.read_text().splitlines():
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            partial = True
    draws = defaultdict(lambda: [[], [], []])
    scope = next((r.get('captureScope', 'draws') for r in records if r.get('type') == 'start'), 'draws')
    if scope not in ('draws', 'visibility'):
        raise ValueError('Unknown flicker capture scope.')
    for row in records:
        if row.get('type') == 'draw':
            draws[row['scene']][row['pass']].append(row)
    scenes = []
    for row in records:
        if row.get('type') != 'scene':
            continue
        scene = row['scene']
        passes = draws[scene]
        counts = [Counter(map(identity, p)) for p in passes]
        deltas = []
        for eye in (1, 2):
            missing, extra = counts[0] - counts[eye], counts[eye] - counts[0]
            deltas.append(dict(eye=eye, missing=sum(missing.values()), extra=sum(extra.values()),
                               missingExamples=list(missing.items())[:12], extraExamples=list(extra.items())[:12]))
        completed = next((r for r in records if r.get('type') == 'complete' and r['scene'] == scene), None)
        if scope == 'visibility':
            totals = completed.get('draws') if completed else None
            valid = (isinstance(totals, list) and len(totals) == 3 and
                     all(type(n) is int and 0 <= n <= 0xffffffff for n in totals))
            scenes.append(dict(scene=scene, completion=completed,
                drawCounts=totals if valid else [0, 0, 0], drawCountBasis='completion_counters',
                recordedDrawCounts=[len(p) for p in passes], membership=None, objectInputs=None,
                inputCoverage=None, predicatedDraws=None,
                unavailable='Per-draw membership, inputs and pixels were intentionally not captured.'))
            continue
        scenes.append(dict(scene=scene, completion=completed, membership=deltas,
                           predicatedDraws=[sum(bool(d['predicate'][0]) for d in p) for p in passes],
                           inputCoverage=dict(
                               instanceStreamDraws=[sum('instanceStreams' in d for d in p) for p in passes],
                               depthTestDraws=[sum('depthTest' in d for d in p) for p in passes],
                               currentPsCb5Draws=[sum(bool(d.get('pcb5') and d['pcb5'][1] and d['pcb5'][3] == scene) for d in p) for p in passes]),
                           drawCounts=[len(p) for p in passes],
                           objectInputs=compare_object_inputs(passes, scene)))
    # Keep review images before cleanup. Reduced visibility captures retain small
    # previews, not pixel-analysis inputs; original dimensions/hashes stay explicit.
    # Refuse oversized/escaping input, and cap all retained PNGs for this burst.
    from PIL import Image
    images, png_bytes = [], 0
    for row in records:
        if row.get('type') != 'image':
            continue
        item = dict(row)
        if row['status'] == 'saved':
            name = row['file']
            if not re.fullmatch(r'stage-\d+-flicker-eye[012]\.ppm', name):
                raise ValueError('Unexpected capture filename.')
            source = run / name
            if source.is_symlink() or source.stat().st_size > 96 * 1024 * 1024:
                raise ValueError('Linked or oversized capture.')
            with Image.open(source) as image:
                if image.width > 4096 or image.height > 4096 or image.mode != 'RGB':
                    raise ValueError('Unexpected capture dimensions/format.')
                item.update(width=image.width, height=image.height,
                            sourceSha256=hashlib.sha256(source.read_bytes()).hexdigest())
                if scope == 'visibility':
                    image.thumbnail((VISIBILITY_PREVIEW_EDGE, VISIBILITY_PREVIEW_EDGE), Image.Resampling.LANCZOS)
                resized = image.size != (item['width'], item['height'])
                item.update(retainedWidth=image.width, retainedHeight=image.height,
                            retainedLossless=not resized, retainedTransform='lanczos_preview' if resized else 'identity')
                encoded = io.BytesIO()
                image.save(encoded, format='PNG')
                data = encoded.getvalue()
            if png_bytes + len(data) <= MAX_PNG:
                target = source.with_suffix('.png')
                if target.is_symlink():
                    raise ValueError('Linked PNG output.')
                target.write_bytes(data)
                png_bytes += len(data)
                item.update(retained=target.name, pngSha256=hashlib.sha256(data).hexdigest())
            else:
                item['retained'] = None
                item['retentionLimit'] = 'png_budget'
        images.append(item)
    return dict(schema=1, captureScope=scope, scenes=scenes, images=images, retainedPngBytes=png_bytes,
                complete=any(r.get('type') == 'end' for r in records),
                malformedLines=partial, logTruncated=any(r.get('type') == 'truncated' for r in records),
                limits='Passive source images, not compositor output. Visibility-scope retained images are resized review previews, not full-resolution pixel evidence; original dimensions and source hashes are retained. GPU readback can affect timing. Equal draw membership does not prove visibility. Unknown or older constant-buffer hashes are not current contents; camera hashes normally differ between eyes. A burst without flicker does not disprove the report.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    result = analyze(args.run)
    target = args.run / 'flicker-summary.json'
    target.write_text(json.dumps(result, indent=2) + '\n')
    print(target)
