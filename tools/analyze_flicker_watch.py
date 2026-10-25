"""Summarize bounded passive draw-query timelines, not final object visibility."""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re
import struct

MAX_LOG = 16 * 1024 * 1024
MAX_EXAMPLES = 32
LIMITS = dict(version=1, acceptedLimit=180, sceneLimit=240, drawsPerPass=24,
              genericPerPass=23, flagsPerPass=1, pendingFrames=3, identityLimit=4096, maxBytes=MAX_LOG)
FIELDS = ('slot', 'kind', 'vs', 'ps', 'source', 'args', 'geometry', 'texture', 'target', 'depth', 'viewportBits')
MODEL_FIELDS = ('modelKnown', 'modelHex', 'modelStatus', 'instanceStreams')
MODEL_STATUSES = {'current_cpu_upload', 'unsupported_shader', 'no_constant_buffer', 'unobserved', 'stale', 'short'}
STATUSES = {'ok', 'pending', 'unavailable', 'context', 'identity_budget', 'query_creation',
            'missing_after_draw', 'context_mismatch', 'incomplete_scene', 'stopped'}


def integer(value, low=0, high=0xffffffff):
    return type(value) is int and low <= value <= high


def words(value, length):
    return isinstance(value, list) and len(value) == length and all(integer(item) for item in value)


def model_status(row):
    if not any(name in row for name in MODEL_FIELDS):
        return 'legacy_unknown'
    if not all(name in row for name in MODEL_FIELDS):
        return 'invalid_model_fields'
    status, known, encoded = row['modelStatus'], row['modelKnown'], row['modelHex']
    if not isinstance(status, str) or status not in MODEL_STATUSES or type(known) is not bool:
        return 'invalid_model_status'
    if known:
        if status != 'current_cpu_upload' or not isinstance(encoded, str) or not re.fullmatch('[0-9a-f]{128}', encoded):
            return 'invalid_current_model'
        if not all(math.isfinite(value) for value in struct.unpack('<16f', bytes.fromhex(encoded))):
            return 'invalid_nonfinite_model'
    elif status == 'current_cpu_upload' or encoded is not None:
        return 'invalid_unknown_model'
    streams = row['instanceStreams']
    if (not isinstance(streams, list) or len(streams) != 3 or
            not all(words(stream, 4) and stream[0] == index for index, stream in enumerate(streams, 1))):
        return 'invalid_instance_streams'
    return status


def identity(row):
    if not all(name in row for name in FIELDS):
        return None, 'missing_binding_fields'
    if (not integer(row['slot'], 6, 6) or not integer(row['vs'], 0, 2**64 - 1) or
            not integer(row['ps'], 0, 2**64 - 1) or not words(row['source'], 2) or not words(row['geometry'], 6) or
            not all(integer(row[name]) for name in ('texture', 'target', 'depth'))):
        return None, 'invalid_binding_fields'
    kind = row['kind']
    if not isinstance(kind, str) or kind not in ('Indexed', 'IndexedInstanced') or not words(row['args'], 3 if kind == 'Indexed' else 5):
        return None, 'invalid_draw_arguments'
    count = row['args'][0]
    flag = row['vs'] == 0x5365b368abb08738 and row['ps'] == 0xaa19112331ae0872
    generic = (row['ps'] == 0x938d132c1aa95cbe and count in (6, 12, 36, 54, 72, 126) and
               ((row['vs'] == 0xd946f27a1c896b78 and kind == 'Indexed') or
                (row['vs'] == 0x13dd94df551eb6d9 and kind == 'IndexedInstanced')))
    if not count or not (flag or generic):
        return None, 'outside_watch_selection'
    viewports = row['viewportBits']
    if not isinstance(viewports, list) or len(viewports) > 16 or not all(words(viewport, 6) for viewport in viewports):
        return None, 'invalid_viewport_bits'
    if any(not math.isfinite(struct.unpack('<f', struct.pack('<I', value))[0]) for viewport in viewports for value in viewport):
        return None, 'nonfinite_viewport'
    model = model_status(row)
    if model.startswith('invalid_'):
        return None, model
    fields = FIELDS if model == 'legacy_unknown' else FIELDS + MODEL_FIELDS
    return json.dumps([row[name] for name in fields], separators=(',', ':')), 'ok'


def sample_status(row):
    status = row.get('status')
    if not isinstance(status, str) or status not in STATUSES:
        return 'unknown_status'
    if status == 'ok':
        return 'ok' if integer(row.get('samples'), 0, 2**64 - 1) and 'hresult' not in row else 'invalid_result'
    if 'samples' in row:
        return 'invalid_result'
    if status == 'unavailable':
        if not integer(row.get('hresult'), -(2**31), 2**31 - 1) or row['hresult'] in (0, 1):
            return 'invalid_hresult'
    elif 'hresult' in row:
        return 'invalid_hresult'
    return status


def analyze_records(records):
    if (not records or not all(isinstance(row, dict) and isinstance(row.get('type'), str) for row in records) or
            records[0].get('type') != 'begin' or records[-1].get('type') != 'end'):
        raise ValueError('Missing or malformed watch boundary.')
    types = Counter(row['type'] for row in records)
    if types['begin'] != 1 or types['end'] != 1 or types['truncated'] or set(types) - {'begin', 'end', 'object', 'scene', 'sample'}:
        raise ValueError('Duplicate, truncated or unknown watch record.')
    begin, end = records[0], records[-1]
    if any(not integer(begin.get(name), value, value) for name, value in LIMITS.items()):
        raise ValueError('Unsupported watch limits/version.')
    if (begin.get('scope') != 'passive_draw_occlusion_samples_not_final_object_visibility' or
            begin.get('identityScope') != 'binding_identity_not_resource_contents_or_model_location'):
        raise ValueError('Unknown watch evidence scope.')
    if 'modelScope' in begin and begin['modelScope'] != 'fresh_observed_cpu_model_when_known_not_gpu_contents_proof':
        raise ValueError('Unknown watch model scope.')
    if end.get('reason') != 'complete' or end.get('truncated') is not False or not integer(end.get('accepted'), 180, 180):
        raise ValueError('Watch did not complete 180 accepted scenes without log truncation.')
    scenes = [row for row in records if row['type'] == 'scene']
    if (not 180 <= len(scenes) <= 240 or not integer(end.get('scenes'), len(scenes), len(scenes)) or
            any(not integer(row.get('scene'), 1, 2**64 - 1) or type(row.get('accepted')) is not bool for row in scenes) or
            len({row['scene'] for row in scenes}) != len(scenes) or sum(row['accepted'] for row in scenes) != 180):
        raise ValueError('Inconsistent or duplicate scene coverage.')
    scene_map = {row['scene']: row for row in scenes}
    for row in scenes:
        if (not words(row.get('candidates'), 3) or not words(row.get('draws'), 3) or
                not isinstance(row.get('truncated'), list) or len(row['truncated']) != 3 or
                not all(type(value) is bool for value in row['truncated']) or
                any(candidate > draw for candidate, draw in zip(row['candidates'], row['draws']))):
            raise ValueError('Invalid full-pass/candidate/truncation counts.')
    objects = defaultdict(list)
    for row in records:
        if row['type'] == 'object':
            if not integer(row.get('id'), 1, 4096):
                raise ValueError('Object ID exceeds capture bounds.')
            objects[row['id']].append(row)
    if sum(map(len, objects.values())) > 4096:
        raise ValueError('Object records exceed capture bounds.')
    object_status, definitions, binding_ids = {}, {}, defaultdict(list)
    for object_id, rows in objects.items():
        if len(rows) != 1:
            object_status[object_id] = 'duplicate_definition'
            continue
        binding, status = identity(rows[0])
        object_status[object_id] = status
        if binding is not None:
            definitions[object_id] = rows[0]
            binding_ids[binding].append(object_id)
    for ids in binding_ids.values():
        if len(ids) > 1:
            for object_id in ids:
                object_status[object_id] = 'duplicate_binding_ids'
    samples = [row for row in records if row['type'] == 'sample']
    per_scene, per_group = defaultdict(list), defaultdict(list)
    for row in samples:
        if (not integer(row.get('scene'), 1, 2**64 - 1) or row['scene'] not in scene_map or
                not integer(row.get('pass'), 0, 2) or not integer(row.get('ordinal'), 1) or
                not integer(row.get('drawOrdinal'), 1) or not integer(row.get('object'), 0, 4096)):
            raise ValueError('Invalid or orphan query record location.')
        per_scene[(row['scene'], row['pass'])].append(row)
        per_group[(row['object'], row['pass'])].append(row)
    if any(len(rows) > 24 for rows in per_scene.values()):
        raise ValueError('Query records exceed 24 per scene/pass.')
    missing, capped, contradictions, duplicate_rows = 0, 0, [], set()
    count_status = {}
    passes = [dict(eye=eye, fullDraws=0, candidates=0, recordedQueries=0, truncatedScenes=0,
                   missingQueries=0, unselectedOrMissing=0, countContradictions=0) for eye in range(3)]
    for scene in scenes:
        for eye in range(3):
            key, summary = (scene['scene'], eye), passes[eye]
            rows = per_scene[key]
            candidates, truncated = scene['candidates'][eye], scene['truncated'][eye]
            summary['fullDraws'] += scene['draws'][eye]
            summary['candidates'] += candidates
            summary['recordedQueries'] += len(rows)
            summary['truncatedScenes'] += truncated
            summary['unselectedOrMissing'] += max(0, candidates - len(rows))
            by_ordinal, by_draw = defaultdict(list), defaultdict(list)
            invalid = False
            for row in rows:
                by_ordinal[row['ordinal']].append(row)
                by_draw[row['drawOrdinal']].append(row)
                if row['ordinal'] > candidates or row['drawOrdinal'] > scene['draws'][eye]:
                    invalid = True
            for duplicates in (*by_ordinal.values(), *by_draw.values()):
                if len(duplicates) > 1:
                    duplicate_rows.update(id(row) for row in duplicates)
                    invalid = True
            if len(rows) > candidates or (not truncated and candidates > 24):
                invalid = True
            if not truncated:
                absent = max(0, candidates - len(by_ordinal))
                missing += absent
                summary['missingQueries'] += absent
                if not invalid and absent:
                    count_status[key] = 'missing_scene_queries'
            else:
                capped += 1
            if invalid:
                count_status[key] = 'contradictory_scene_counts'
                summary['countContradictions'] += 1
                if len(contradictions) < MAX_EXAMPLES:
                    contradictions.append(dict(scene=key[0], eye=eye, candidates=candidates, records=len(rows)))
    states, transitions, positive_changes, group_summaries = Counter(), [], [], []
    query_statuses = Counter(sample_status(row) for row in samples)
    ranges = []
    zero_to_nonzero = nonzero_to_zero = positive_comparisons = 0
    for (object_id, eye), observations in sorted(per_group.items()):
        by_scene = defaultdict(list)
        for row in observations:
            by_scene[row['scene']].append(row)
        counts, values, previous = Counter(), [], None
        group_zero_up = group_zero_down = 0
        largest = None
        for scene in scenes:
            rows = by_scene.get(scene['scene'], [])
            value = None
            if not scene['accepted']:
                state = 'rejected_scene'
            elif object_status.get(object_id) != 'ok':
                state = object_status.get(object_id, 'unknown_binding')
            elif len(rows) > 1:
                state = 'ambiguous_binding_observation'
            elif any(id(row) in duplicate_rows for row in rows):
                state = 'duplicate_query_record'
            elif count_status.get((scene['scene'], eye)) == 'contradictory_scene_counts':
                state = 'contradictory_scene_counts'
            elif not rows:
                state = count_status.get((scene['scene'], eye),
                                         'not_observed_capped' if scene['truncated'][eye] else 'absent_observation')
            else:
                state = sample_status(rows[0])
                if state == 'ok':
                    value = rows[0]['samples']
                    values.append(value)
            counts[state] += 1
            states[state] += 1
            if previous is not None and value is not None:
                old_scene, old_value = previous
                if bool(old_value) != bool(value):
                    if old_value == 0:
                        zero_to_nonzero += 1
                        group_zero_up += 1
                        direction = 'zero_to_nonzero'
                    else:
                        nonzero_to_zero += 1
                        group_zero_down += 1
                        direction = 'nonzero_to_zero'
                    if len(transitions) < MAX_EXAMPLES:
                        transitions.append(dict(object=object_id, eye=eye, scenes=[old_scene, scene['scene']],
                                                samples=[old_value, value], direction=direction))
                elif old_value and value:
                    positive_comparisons += 1
                    ratio = max(old_value, value) / min(old_value, value)
                    item = dict(object=object_id, eye=eye, scenes=[old_scene, scene['scene']],
                                samples=[old_value, value], ratio=ratio)
                    if largest is None or ratio > largest['ratio']:
                        largest = item
                    if ratio > 1:
                        positive_changes.append(item)
                        positive_changes.sort(key=lambda item: item['ratio'], reverse=True)
                        del positive_changes[MAX_EXAMPLES:]
            # Missing/failed/absent/rejected evidence always breaks adjacency.
            previous = (scene['scene'], value) if value is not None else None
        if values:
            ranges.extend((min(values), max(values)))
        definition = definitions.get(object_id, {})
        group_summaries.append(dict(object=object_id, eye=eye, bindingStatus=object_status.get(object_id, 'unknown_binding'),
            modelStatus=model_status(definition) if definition else 'unknown_binding',
            modelKnown=definition.get('modelKnown'), modelHex=definition.get('modelHex'),
            states=dict(counts), queryRecords=len(observations), knownSamples=len(values), zeroSamples=values.count(0),
            minSamples=min(values) if values else None, maxSamples=max(values) if values else None,
            zeroToNonzero=group_zero_up, nonzeroToZero=group_zero_down, largestPositiveRatio=largest,
            shader=f"{definition['vs']:016x}" if 'vs' in definition else None,
            target=definition.get('target'), depth=definition.get('depth'), texture=definition.get('texture')))
    group_summaries.sort(key=lambda group: (group['zeroToNonzero'] + group['nonzeroToZero'],
        (group['largestPositiveRatio'] or {}).get('ratio', 0), group['knownSamples']), reverse=True)
    return dict(schema=1, complete=True, scenes=len(scenes), acceptedScenes=180, rejectedScenes=len(scenes) - 180,
                logTruncated=False, objectDefinitions=sum(map(len, objects.values())), identities=len(objects),
                bindingStatuses=dict(Counter(object_status.values())), queryRecords=len(samples),
                declaredModelScope=begin.get('modelScope'),
                modelEvidenceDefinitions=dict(Counter(model_status(rows[0]) for rows in objects.values() if len(rows) == 1)),
                unresolvedBindingQueryRecords=dict(Counter(object_status.get(row['object'], 'unknown_binding')
                    for row in samples if object_status.get(row['object']) != 'ok')),
                queryStatuses=dict(query_statuses), missingQueryRecords=missing, cappedScenePasses=capped,
                duplicateQueryRecords=len(duplicate_rows), countContradictions=sum(p['countContradictions'] for p in passes),
                countContradictionExamples=contradictions, passCoverage=passes,
                observationStates=dict(states), groups=len(group_summaries), groupExamples=group_summaries[:MAX_EXAMPLES],
                groupExamplesTruncated=len(group_summaries) > MAX_EXAMPLES,
                minKnownSamples=min(ranges) if ranges else None, maxKnownSamples=max(ranges) if ranges else None,
                zeroToNonzero=zero_to_nonzero, nonzeroToZero=nonzero_to_zero,
                transitionExamples=transitions,
                transitionExamplesTruncated=zero_to_nonzero + nonzero_to_zero > len(transitions),
                positiveAdjacentComparisons=positive_comparisons, largestPositiveChanges=positive_changes,
                limits=('Only existing selected draw occlusion samples are measured, not final object visibility. '
                        'Zero does not establish flicker; occlusion, viewport, motion and other conditions can change samples. '
                        'Object IDs identify recorded bindings and optional CPU-model bytes, not resource contents or '
                        'a proven same-object lifetime. Target/depth/viewport changes can produce another ID. '
                        'Optional current model bytes are CPU upload evidence, not GPU readback; instance-stream '
                        'bindings do not establish instance-buffer contents. Legacy logs have unknown model evidence. '
                        'Transitions require adjacent captured scene records, both accepted with one valid query for that '
                        'binding/pass. Missing, pending, duplicate, absent or rejected observations break adjacency. '
                        'Per-pass truncation means unselected draws exist; absent observations do not establish absence '
                        'from the full render. Ratios exclude zero denominators. No result establishes a fix or headset acceptance.'))


def analyze(run):
    path = Path(run) / 'flicker-watch.jsonl'
    if path.is_symlink() or path.stat().st_size > MAX_LOG:
        raise ValueError('Linked or oversized watch log.')
    raw = path.read_bytes()
    if len(raw) > MAX_LOG or not raw.endswith(b'\n'):
        raise ValueError('Oversized or incomplete final watch line.')
    def reject_constant(value):
        raise ValueError('Nonfinite JSON: ' + value)
    try:
        records = [json.loads(line, parse_constant=reject_constant) for line in raw.splitlines()]
    except (ValueError, UnicodeDecodeError) as exc:
        raise ValueError('Malformed watch log.') from exc
    return analyze_records(records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    target = args.output or args.run / 'flicker-watch-summary.json'
    target.write_text(json.dumps(analyze(args.run), indent=2) + '\n', encoding='utf-8')
    print(target)
