import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker_watch import LIMITS, MAX_EXAMPLES, MAX_LOG, analyze, analyze_records


def fixture(rejected=0):
    begin = dict(type='begin', **LIMITS,
                 genericIndexCounts=[6, 12, 36, 54, 72, 126],
                 scope='passive_draw_occlusion_samples_not_final_object_visibility',
                 identityScope='binding_identity_not_resource_contents_or_model_location')
    obj = dict(type='object', id=1, slot=6, kind='Indexed', vs=0xd946f27a1c896b78, ps=0x938d132c1aa95cbe,
               source=[10, 11], args=[6, 0, 0], geometry=[12, 24, 0, 13, 57, 0],
               texture=14, target=15, depth=16, viewportBits=[[0, 0, 0x3f800000, 0x3f800000, 0, 0x3f800000]])
    records = [begin, obj]
    for index in range(180 + rejected):
        scene = 100 + index * 5  # Consecutive captures need not have consecutive integer scene IDs.
        records.append(dict(type='scene', scene=scene, accepted=index >= rejected,
                            candidates=[1, 1, 1], draws=[100, 100, 100], truncated=[False] * 3))
        for eye in range(3):
            records.append(dict(type='sample', scene=scene, ordinal=1, drawOrdinal=75, object=1,
                                status='ok', samples=100, **{'pass': eye}))
    records.append(dict(type='end', scenes=180 + rejected, accepted=180, reason='complete', truncated=False))
    return records


def scenes(records):
    return [row for row in records if row['type'] == 'scene']


def sample(records, index, eye=0):
    target = scenes(records)[index]['scene']
    return next(row for row in records if row['type'] == 'sample' and row['scene'] == target and row['pass'] == eye)


class DrawWatch(unittest.TestCase):
    def test_complete_constant_queries_preserve_scope_and_full_counts(self):
        result = analyze_records(fixture())
        self.assertTrue(result['complete'])
        self.assertEqual(result['acceptedScenes'], 180)
        self.assertEqual(result['queryStatuses'], {'ok': 540})
        self.assertEqual(result['positiveAdjacentComparisons'], 179 * 3)
        self.assertEqual(result['zeroToNonzero'] + result['nonzeroToZero'], 0)
        self.assertEqual(result['minKnownSamples'], 100)
        self.assertEqual(result['maxKnownSamples'], 100)
        self.assertEqual(result['passCoverage'][0]['fullDraws'], 18000)
        self.assertIn('Zero does not establish flicker', result['limits'])
        self.assertIn('same-object lifetime', result['limits'])

    def test_zero_transitions_and_positive_ratios_use_adjacent_capture_order(self):
        records = fixture()
        sample(records, 10)['samples'] = 0
        sample(records, 12)['samples'] = 1000
        result = analyze_records(records)
        self.assertEqual(result['zeroToNonzero'], 1)
        self.assertEqual(result['nonzeroToZero'], 1)
        self.assertEqual(result['transitionExamples'][0]['scenes'], [145, 150])
        self.assertEqual(result['largestPositiveChanges'][0]['ratio'], 10)
        self.assertEqual(result['maxKnownSamples'], 1000)

    def test_missing_pending_failed_and_absent_observations_break_adjacency(self):
        for variation in ('missing', 'pending', 'unavailable', 'absent', 'capped', 'invalid_payload'):
            records = fixture()
            sample(records, 9)['samples'] = 0
            row = sample(records, 10)
            if variation in ('missing', 'absent', 'capped'):
                records.remove(row)
                if variation == 'absent': scenes(records)[10]['candidates'][0] = 0
                if variation == 'capped':
                    scenes(records)[10]['candidates'][0] = 30
                    scenes(records)[10]['truncated'][0] = True
            elif variation == 'invalid_payload':
                row.update(status='pending', samples=0)
            else:
                row.pop('samples')
                row['status'] = variation
                if variation == 'unavailable': row['hresult'] = -2147467259
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['zeroToNonzero'], 0)
                self.assertEqual(result['nonzeroToZero'], 1)
                if variation == 'missing': self.assertEqual(result['missingQueryRecords'], 1)
                if variation == 'capped': self.assertEqual(result['cappedScenePasses'], 1)

    def test_duplicate_queries_and_binding_observations_are_unavailable(self):
        for variation in ('duplicate_query', 'same_binding_twice'):
            records = fixture()
            duplicate = copy.deepcopy(sample(records, 10))
            duplicate['samples'] = 0
            scenes(records)[10]['candidates'][0] = 2
            if variation == 'same_binding_twice':
                duplicate['ordinal'] = 2
                duplicate['drawOrdinal'] = 76
            records.insert(-1, duplicate)
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['zeroToNonzero'] + result['nonzeroToZero'], 0)
                self.assertEqual(result['observationStates']['ambiguous_binding_observation'], 1)
                self.assertEqual(result['duplicateQueryRecords'], 2 if variation == 'duplicate_query' else 0)

    def test_unknown_and_duplicate_identity_definitions_are_not_known_objects(self):
        for variation in ('unknown', 'duplicate', 'reused_binding', 'nonfinite_viewport'):
            records = fixture()
            if variation == 'unknown': records.pop(1)
            if variation == 'duplicate': records.insert(2, copy.deepcopy(records[1]))
            if variation == 'reused_binding': records.insert(2, dict(records[1], id=2))
            if variation == 'nonfinite_viewport': records[1]['viewportBits'][0][2] = 0x7fc00000
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertIsNone(result['minKnownSamples'])
                self.assertEqual(sum(result['unresolvedBindingQueryRecords'].values()), 540)
                self.assertEqual(result['positiveAdjacentComparisons'], 0)

    def test_optional_current_models_are_part_of_identity_and_legacy_remains_unknown(self):
        records = fixture()
        self.assertEqual(analyze_records(records)['modelEvidenceDefinitions'], {'legacy_unknown': 1})
        records[1].update(modelKnown=True, modelHex='00' * 64, modelStatus='current_cpu_upload',
                          instanceStreams=[[1, 0, 0, 0], [2, 500, 64, 0], [3, 0, 0, 0]])
        second = copy.deepcopy(records[1])
        second.update(id=2, modelHex='11' * 64)
        records.insert(2, second)
        result = analyze_records(records)
        self.assertEqual(result['bindingStatuses'], {'ok': 2})
        self.assertEqual(result['modelEvidenceDefinitions'], {'current_cpu_upload': 2})
        self.assertEqual(result['groupExamples'][0]['modelHex'], '00' * 64)
        second['modelHex'] = records[1]['modelHex']
        second['instanceStreams'][1][1] = 501
        self.assertEqual(analyze_records(records)['bindingStatuses'], {'ok': 2})

    def test_stale_and_malformed_model_evidence_are_distinguished(self):
        for variation in ('stale', 'short_hex', 'uppercase', 'missing', 'slot', 'unknown_hex', 'false_current', 'nonfinite'):
            records = fixture()
            obj = records[1]
            obj.update(modelKnown=True, modelHex='aa' * 64, modelStatus='current_cpu_upload',
                       instanceStreams=[[1, 0, 0, 0], [2, 500, 64, 0], [3, 0, 0, 0]])
            if variation == 'stale': obj.update(modelKnown=False, modelHex=None, modelStatus='stale')
            if variation == 'short_hex': obj['modelHex'] = '00'
            if variation == 'uppercase': obj['modelHex'] = 'AA' * 64
            if variation == 'missing': obj.pop('modelHex')
            if variation == 'slot': obj['instanceStreams'][1][0] = 1
            if variation == 'unknown_hex': obj.update(modelKnown=False, modelStatus='stale')
            if variation == 'false_current': obj.update(modelKnown=False, modelHex=None)
            if variation == 'nonfinite': obj['modelHex'] = 'ff' * 64
            with self.subTest(variation=variation):
                result = analyze_records(records)
                if variation == 'stale':
                    self.assertEqual(result['bindingStatuses'], {'ok': 1})
                    self.assertEqual(result['modelEvidenceDefinitions'], {'stale': 1})
                else:
                    self.assertEqual(sum(result['unresolvedBindingQueryRecords'].values()), 540)
                    self.assertIsNone(result['minKnownSamples'])

    def test_rejected_scene_does_not_form_a_comparison(self):
        records = fixture(1)
        sample(records, 0)['samples'] = 0
        result = analyze_records(records)
        self.assertEqual(result['rejectedScenes'], 1)
        self.assertEqual(result['zeroToNonzero'], 0)
        self.assertEqual(result['positiveAdjacentComparisons'], 179 * 3)

    def test_per_pass_selection_truncation_is_not_global_log_truncation(self):
        records = fixture()
        for scene in scenes(records):
            scene['candidates'][0] = 30
            scene['truncated'][0] = True
        result = analyze_records(records)
        self.assertTrue(result['complete'])
        self.assertEqual(result['cappedScenePasses'], 180)
        self.assertEqual(result['missingQueryRecords'], 0)
        self.assertEqual(result['passCoverage'][0]['unselectedOrMissing'], 29 * 180)

    def test_count_contradictions_are_explicit_and_not_usable(self):
        for variation in ('candidate_ordinal', 'draw_ordinal', 'too_many_candidates'):
            records = fixture()
            row = sample(records, 10)
            row['samples'] = 0
            if variation == 'candidate_ordinal': row['ordinal'] = 2
            if variation == 'draw_ordinal': row['drawOrdinal'] = 101
            if variation == 'too_many_candidates': scenes(records)[10]['candidates'][0] = 25
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['countContradictions'], 1)
                self.assertEqual(result['zeroToNonzero'] + result['nonzeroToZero'], 0)

    def test_malformed_result_status_and_nonfinite_or_negative_counts_are_unavailable(self):
        for variation in ('negative', 'float', 'boolean', 'unknown_status', 'unavailable_success', 'pending_samples'):
            records = fixture()
            row = sample(records, 10)
            if variation == 'negative': row['samples'] = -1
            if variation == 'float': row['samples'] = float('nan')
            if variation == 'boolean': row['samples'] = True
            if variation == 'unknown_status': row['status'] = 'new_status'
            if variation == 'unavailable_success':
                row.pop('samples')
                row.update(status='unavailable', hresult=0)
            if variation == 'pending_samples': row.update(status='pending', samples=0)
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['positiveAdjacentComparisons'], 179 * 3 - 2)

    def test_transition_and_ratio_examples_are_bounded(self):
        records = fixture()
        for index in range(180):
            sample(records, index, 0)['samples'] = 100 if index % 2 else 0
            sample(records, index, 1)['samples'] = 100 if index % 2 else 10000
        result = analyze_records(records)
        self.assertEqual(len(result['transitionExamples']), MAX_EXAMPLES)
        self.assertTrue(result['transitionExamplesTruncated'])
        self.assertEqual(len(result['largestPositiveChanges']), MAX_EXAMPLES)

    def test_incomplete_duplicate_or_over_budget_capture_is_refused(self):
        for variation in ('accepted', 'reason', 'truncated', 'missing_end', 'duplicate_scene', 'scene_count',
                          'record_cap', 'object_cap', 'full_draws', 'begin_limits'):
            records = fixture()
            if variation == 'accepted': records[-1]['accepted'] = 179
            if variation == 'reason': records[-1]['reason'] = 'stopped'
            if variation == 'truncated': records[-1]['truncated'] = True
            if variation == 'missing_end': records.pop()
            if variation == 'duplicate_scene': records.insert(-1, copy.deepcopy(scenes(records)[0]))
            if variation == 'scene_count': records[-1]['scenes'] = 181
            if variation == 'record_cap': records[-1:-1] = [copy.deepcopy(sample(records, 0)) for _ in range(24)]
            if variation == 'object_cap': records[1]['id'] = 4097
            if variation == 'full_draws': scenes(records)[0]['draws'][0] = 0
            if variation == 'begin_limits': records[0]['drawsPerPass'] = 25
            with self.subTest(variation=variation), self.assertRaises(ValueError):
                analyze_records(records)

    def test_file_bounds_partial_malformed_and_nonfinite_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'flicker-watch.jsonl'
            text = '\n'.join(json.dumps(row) for row in fixture()) + '\n'
            path.write_text(text)
            self.assertTrue(analyze(directory)['complete'])
            for payload in (text[:-1], text + 'bad\n', text.replace('"samples": 100', '"samples": NaN', 1)):
                path.write_text(payload)
                with self.assertRaises(ValueError): analyze(directory)
            with path.open('wb') as stream: stream.truncate(MAX_LOG + 1)
            with self.assertRaises(ValueError): analyze(directory)


if __name__ == '__main__':
    unittest.main()
