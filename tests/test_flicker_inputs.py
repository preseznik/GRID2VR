import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker_inputs import (COLOUR_VS, DEPTH_VS, FIELDS, MAX_LOG,
                                   analyze, analyze_records, fnv1a)


def buffer(data, resource):
    return dict(resource=resource, bytes=len(data), status='ok', hash=fnv1a(data), hex=data.hex())


def fixture():
    layout = dict(type='layout', id=100, known=1, elements=[
        ['POSITION', 0, 6, 0, 0, 0, 0], ['TEXCOORD', 0, 16, 0, 12, 0, 0],
        ['COLOR', 0, 28, 0, 20, 0, 0]])
    vertex = bytes(range(72))
    index = struct.pack('<3H', 0, 1, 2)
    draws = []
    for side, shader in enumerate((DEPTH_VS, COLOUR_VS)):
        cb0 = bytearray(432 if side == 0 else 240)
        for n, (_, (left, right, size)) in enumerate(FIELDS.items(), 1):
            offset = (left, right)[side]
            cb0[offset:offset + size] = bytes([n]) * size
        draw = dict(type='draw', scene=10, ordinal=side + 1, slot=5 + side, vs=shader, ps=7,
                    kind='Indexed', args=[3, 0, 0], geometry=[12, 24, 0, 13, 57, 0],
                    drawable=0 if side == 0 else 45, properties=side * 55, inputLayout=100,
                    instanceStreams=[[2, 100 + side, 64, 0]],
                    objectInputs=dict(known=1, scene=10, fields=dict(perInstanceModelMatrix='06' * 64)),
                    flagInputs=dict(cb0=buffer(cb0, 14), cb3=buffer(bytes(368), 15),
                                    vertex0=buffer(vertex, 12), index=buffer(index, 13)),
                    pcb5=[55, 1, 0, 10, 16, 0, 123 + side, 0, 0],
                    surface=[90 + side, 91, 92, 45, 1280, 720, 1],
                    viewport=[1, 0, 0, 1280, 720, 0, 1], raster=[1, 3, 1, 0, 0, 0, 0, 1, 0, 0],
                    depthTest=[1, 1, 4])
        draw['pass'] = 1
        draws.append(draw)
    samples = [dict(type='samples', scene=10, ordinal=i + 1, hr=0, passed=20 if i == 0 else 0,
                    **{'pass': 1}) for i in range(2)]
    return [layout, *draws, *samples]


class FlagInputs(unittest.TestCase):
    def test_unique_pair_all_inputs_and_zero_samples_are_not_cause_claim(self):
        result = analyze_records(fixture())
        self.assertEqual(result['pairCount'], 1)
        pair = result['flagPairs'][0]
        self.assertTrue(pair['depthNonzeroColourZero'])
        self.assertEqual(pair['observedModelMatchesGpu'], [True, True])
        self.assertTrue(all(value['status'] == 'equal' for value in pair['deformation'].values()))
        self.assertEqual(set(pair['buffers'].values()), {'equal'})
        self.assertEqual(set(pair['consumedVertexInputs'].values()), {'equal'})
        self.assertEqual(pair['observedAlphaReference']['status'], 'equal')
        self.assertEqual(pair['depthSurface'], 'equal')
        self.assertIn('not the cause', result['limits'])

    def test_different_model_geometry_or_args_never_pairs(self):
        for change in ('model', 'geometry', 'args'):
            records = fixture()
            if change == 'model':
                records[2]['objectInputs']['fields']['perInstanceModelMatrix'] = '07' * 64
            elif change == 'geometry':
                records[2]['geometry'][0] += 1
            else:
                records[2]['args'][0] += 1
            with self.subTest(change=change):
                result = analyze_records(records)
                self.assertEqual(result['pairCount'], 0)
                self.assertEqual(len(result['unmatched']), 2)

    def test_ambiguous_model_mesh_does_not_pair_by_ordinal(self):
        records = fixture()
        records.append(dict(records[1], ordinal=5))
        result = analyze_records(records)
        self.assertEqual(result['pairCount'], 0)
        self.assertEqual(len(result['ambiguous']), 1)

    def test_stale_unknown_or_malformed_observed_model_refused(self):
        for change in ('stale', 'unknown', 'short'):
            records = fixture()
            observed = records[1]['objectInputs']
            if change == 'stale': observed['scene'] = 9
            if change == 'unknown': observed['known'] = 0
            if change == 'short': observed['fields']['perInstanceModelMatrix'] = '00'
            with self.subTest(change=change):
                result = analyze_records(records)
                self.assertEqual(result['pairCount'], 0)
                self.assertEqual(len(result['unavailable']), 1)

    def test_all_deformation_fields_are_compared_from_actual_bytes(self):
        for name, (_, offset, size) in FIELDS.items():
            records = fixture()
            data = bytearray.fromhex(records[2]['flagInputs']['cb0']['hex'])
            data[offset] ^= 1
            records[2]['flagInputs']['cb0'] = buffer(data, 14)
            with self.subTest(field=name):
                pair = analyze_records(records)['flagPairs'][0]
                self.assertEqual(pair['deformation'][name]['status'], 'changed')
                if name == 'perInstanceModelMatrix':
                    self.assertEqual(pair['observedModelMatchesGpu'], [True, False])

    def test_failed_stale_resource_short_or_corrupt_gpu_readback_is_unavailable(self):
        for change in ('failed', 'resource', 'short', 'hash'):
            records = fixture()
            item = records[2]['flagInputs']['vertex0']
            if change == 'failed': item['status'] = 'map'
            if change == 'resource': item['resource'] = 77
            if change == 'short': item['hex'] = '00'
            if change == 'hash': item['hash'] ^= 1
            with self.subTest(change=change):
                pair = analyze_records(records)['flagPairs'][0]
                self.assertEqual(pair['buffers']['vertex0'], 'unavailable')
                self.assertEqual(pair['consumedVertexInputs']['POSITION'], 'unavailable')

    def test_consumed_bytes_changed_without_pointer_change(self):
        records = fixture()
        data = bytearray.fromhex(records[2]['flagInputs']['vertex0']['hex'])
        data[0] ^= 1
        records[2]['flagInputs']['vertex0'] = buffer(data, 12)
        pair = analyze_records(records)['flagPairs'][0]
        self.assertEqual(pair['buffers']['vertex0'], 'changed')
        self.assertEqual(pair['consumedVertexInputs'], dict(POSITION='changed', TEXCOORD='equal', COLOR='equal'))

    def test_layout_must_establish_consumed_buffer_not_stale_extra_stream(self):
        for change in ('other_slot', 'instance', 'unknown', 'absent', 'conflict'):
            records = fixture()
            if change == 'other_slot': records[0]['elements'][0][3] = 2
            if change == 'instance': records[0]['elements'][0][5] = 1
            if change == 'unknown': records[0]['known'] = 0
            if change == 'absent': records[0]['elements'].pop()
            if change == 'conflict': records.append(dict(records[0], known=0))
            with self.subTest(change=change):
                pair = analyze_records(records)['flagPairs'][0]
                self.assertEqual(pair['consumedVertexInputs']['POSITION'], 'unavailable')

    def test_append_offset_supported_and_signed_base_vertex_bounds_checked(self):
        records = fixture()
        for entry in records[0]['elements'][1:]: entry[4] = 0xffffffff
        self.assertEqual(analyze_records(records)['flagPairs'][0]['consumedVertexInputs']['COLOR'], 'equal')
        for draw in records[1:3]: draw['args'][2] = 0xffffffff
        pair = analyze_records(records)['flagPairs'][0]
        self.assertEqual(pair['layouts'][0]['consumed'], 'negative_vertex_index')

    def test_unknown_missing_or_duplicate_query_is_not_zero(self):
        for change in ('pending', 'failed', 'missing', 'duplicate'):
            records = fixture()
            if change == 'pending': records[4]['hr'] = 1
            if change == 'failed': records[4]['hr'] = -1
            if change == 'missing': records.pop()
            if change == 'duplicate': records.append(copy.deepcopy(records[4]))
            with self.subTest(change=change):
                result = analyze_records(records)
                self.assertFalse(result['flagPairs'][0]['depthNonzeroColourZero'])
                self.assertIsNone(result['flagPairs'][0]['samples'][1]['passed'])

    def test_cb3_projection_is_separate_from_unused_bytes_and_alpha_age(self):
        records = fixture()
        data = bytearray(368)
        data[-1] = 1
        records[2]['flagInputs']['cb3'] = buffer(data, 15)
        records[2]['pcb5'][3] = 9
        pair = analyze_records(records)['flagPairs'][0]
        self.assertEqual(pair['buffers']['cb3'], 'changed')
        self.assertEqual(pair['viewProjection'], 'equal')
        self.assertEqual(pair['observedAlphaReference']['status'], 'unavailable')

    def test_depth_clears_match_resource_eye_scene_and_precede_draw(self):
        records = fixture()
        records[2]['surface'][1:3] = [101, 102]
        original = dict(type='clearDepth', scene=10, resource=92, view=91,
                        afterOrdinal=0, flags=3, depth=1, stencil=255, **{'pass': 1})
        colour = dict(original, resource=102, view=101, afterOrdinal=1, stencil=0)
        records += [original, colour, dict(colour, afterOrdinal=2, depth=0),
                    dict(original, **{'pass': 2}), dict(original, scene=9),
                    dict(colour, flags=2, depth=0)]  # Stencil-only clear is not a depth clear.
        pair = analyze_records(records)['flagPairs'][0]
        self.assertEqual(pair['depthSurface'], 'changed')
        self.assertEqual(pair['priorDepthClears'], [original, colour])

    def test_parser_reports_partial_truncated_log_and_enforces_size_bound(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'flicker-draws.jsonl'
            path.write_text('\n'.join(map(json.dumps, fixture())) + '\n{"type":"truncated"}\n{"type":')
            result = analyze(folder)
            self.assertEqual(result['malformedLines'], 1)
            self.assertTrue(result['logTruncated'])
            self.assertFalse(result['burstEnded'])
            with path.open('wb') as stream: stream.truncate(MAX_LOG + 1)
            with self.assertRaises(ValueError): analyze(folder)

    def test_native_query_groups_preserve_kind_eye_operation_caller_and_results(self):
        row = dict(type='nativeQuery', scene=10, afterOrdinal=3, query=90, kindKnown=1,
                   kind=1, operation='get', callerRva=123, hr=0, bytes=8, hex='00' * 8,
                   **{'pass': 0})
        records = [row, dict(row, hr=1), dict(row, kindKnown=0), dict(row, **{'pass': 1}),
                   dict(row, operation='begin', bytes=0), dict(row, callerRva=456)]
        result = analyze_records(records)['nativeQueries']
        self.assertEqual(result['observedRecords'], 6)
        self.assertEqual(len(result['groups']), 5)
        first = result['groups'][0]
        self.assertEqual(first['results'], {'0': 1, '1': 1})
        self.assertEqual(first['payloadsObserved'], 1)
        self.assertIsNone(result['groups'][1]['kind'])
        self.assertEqual(result['hookCoverage'], 'unknown')

    def test_zero_native_queries_does_not_establish_hook_coverage(self):
        records = [dict(type='end', scenes=8, nativeQueryRecords=0, nativeQueryLimitReached=False)]
        result = analyze_records(records)['nativeQueries']
        self.assertEqual(result['observedRecords'], 0)
        self.assertTrue(result['recordCountMatches'])
        self.assertFalse(result['limitReached'])
        self.assertEqual(result['hookCoverage'], 'unknown')
        self.assertIn('not absence of native query use', result['limits'])
        result = analyze_records([])['nativeQueries']
        self.assertIsNone(result['recordCountMatches'])
        self.assertIsNone(result['limitReached'])

    def test_native_query_count_and_payload_failures_are_explicit(self):
        row = dict(type='nativeQuery', kindKnown=1, kind=1, operation='get', callerRva=123,
                   hr=0, bytes=8, hex='00', **{'pass': 0})
        records = [row, dict(row, hex='00' * 8, hr=1),
                   dict(type='end', nativeQueryRecords=2048, nativeQueryLimitReached=True)]
        result = analyze_records(records)['nativeQueries']
        self.assertFalse(result['recordCountMatches'])
        self.assertTrue(result['limitReached'])
        self.assertEqual(result['groups'][0]['payloadsObserved'], 0)

    def alpha_fixture(self):
        native = fixture()[1]
        native['ps'] = 0x59bc124b8c63bdaa
        native['pass'] = 0
        native['alphaSampling'] = dict(slot=3, bound=1, descriptor=[21, 1, 1, 1, 0, 16, 1, 0, 0, 0, 0, 0, 0x7f7fffff],
                                       view=30, resource=31, viewFormat=71, viewDimension=4,
                                       mostDetailedMip=0, mipLevels=11, resourceMinLodBits=0,
                                       texture=[1024, 1024, 11, 1, 71])
        eye = copy.deepcopy(native)
        eye['pass'] = 1
        return [native, eye]

    def test_alpha_sampling_reports_exact_metadata_change(self):
        records = self.alpha_fixture()
        result = analyze_records(records)['alphaSampling']
        self.assertEqual(result['equal'], 1)
        records[1]['alphaSampling']['descriptor'][4] = 0x3f800000
        result = analyze_records(records)['alphaSampling']
        self.assertEqual(result['changed'], 1)
        self.assertEqual(result['examples'][0]['changedFields'], ['descriptor'])
        self.assertIn('does not prove equal texture contents', result['limits'])

    def test_alpha_sampling_missing_malformed_or_unsupported_is_unavailable(self):
        for variation in ('missing', 'slot', 'bound', 'descriptor', 'texture', 'dimension', 'format', 'mip', 'lod'):
            records = self.alpha_fixture()
            state = records[1]['alphaSampling']
            if variation == 'missing': del records[1]['alphaSampling']
            if variation == 'slot': state['slot'] = 4
            if variation == 'bound': state['bound'] = 1.0
            if variation == 'descriptor': state['descriptor'].pop()
            if variation == 'texture': state['texture'] = []
            if variation == 'dimension': state['viewDimension'] = 5
            if variation == 'format': state['viewFormat'] = 0
            if variation == 'mip': state['mostDetailedMip'] = 11
            if variation == 'lod': del state['resourceMinLodBits']
            with self.subTest(variation=variation):
                result = analyze_records(records)['alphaSampling']
                self.assertEqual(result['unavailable'], 1)
                self.assertEqual(result['changed'], 0)

    def test_alpha_sampling_null_sampler_is_explicit_not_a_guessed_descriptor(self):
        records = self.alpha_fixture()
        for draw in records:
            draw['alphaSampling'].update(bound=0, descriptor=[])
        self.assertEqual(analyze_records(records)['alphaSampling']['equal'], 1)
        records[1]['alphaSampling']['descriptor'] = [0] * 13
        self.assertEqual(analyze_records(records)['alphaSampling']['unavailable'], 1)

    def test_alpha_sampling_refuses_ambiguous_or_changed_draw_identity(self):
        records = self.alpha_fixture()
        records.append(copy.deepcopy(records[0]))
        result = analyze_records(records)['alphaSampling']
        self.assertEqual(result['matched'], 0)
        self.assertGreater(result['ambiguous'], 0)
        records = self.alpha_fixture()
        records[1]['geometry'][0] += 1
        result = analyze_records(records)['alphaSampling']
        self.assertEqual(result['matched'], 0)
        self.assertGreater(result['unmatched'], 0)


if __name__ == '__main__':
    unittest.main()
