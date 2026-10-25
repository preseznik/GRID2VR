import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker_inputs import fnv1a
from analyze_flicker_instances import (MAX_DRAWS, PS, TARGETS, VS, analyze, analyze_records, decode_draw)


def buffer(data, resource, offset=0, resource_bytes=None):
    return dict(resource=resource, bytes=len(data), status='ok', hash=fnv1a(data), hex=data.hex(),
                offset=offset, resourceBytes=resource_bytes if resource_bytes is not None else offset + len(data))


def encode_model(model, stride=48):
    # The unused fourth components deliberately differ from affine matrix values.
    return (b''.join(struct.pack('<4e', *model[n:n + 3], 7) for n in (0, 4, 8)) +
            struct.pack('<4f', *model[12:15], 91) + bytes(stride - 40))


def fixture():
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    shifted = identity.copy()
    shifted[12] = .125
    layout = dict(type='layout', id=100, known=1, elements=[
        ['POSITION', 0, 6, 0, 0, 0, 0],
        *[['TANGENT', n, 10, 1, (n - 1) * 8, 1, 1] for n in (1, 2, 3)],
        ['TANGENT', 4, 2, 1, 24, 1, 1]])
    cb3 = bytearray(320)
    cb3[64:128] = struct.pack('<16f', *identity)
    vertices = b''.join(struct.pack('<3f', *p) + bytes(12)
                        for p in [(-.5, 0, .5), (.5, 0, .5), (0, .5, .5)])
    draw = dict(type='draw', scene=10, ordinal=20, slot=6, kind='IndexedInstanced', vs=VS, ps=PS,
                args=[6, 2, 3, 0xfffffffe, 5], geometry=[12, 24, 20, 13, 57, 6],
                inputLayout=100, drawable=22, properties=23, viewport=[1, 0, 0, 1280, 720, 0, 1],
                instanceStreams=[[1, 14, 48, 12], [2, 99, 4, 0]],
                instanceInputs=dict(status='ok', vertexFirst=5,
                    cb3=buffer(cb3, 15),
                    index=buffer(struct.pack('<6H', 7, 8, 9, 9, 8, 7), 13, 12, 100),
                    vertex0=buffer(vertices, 12, 140, 300),
                    streams=[dict(slot=1, firstElement=5, elementCount=2, stride=48, bindingOffset=12,
                                  stepRate=1, buffer=buffer(encode_model(identity) + encode_model(shifted), 14, 252, 500))]))
    draw['pass'] = 0
    return draw, {100: layout}


class InstanceInputs(unittest.TestCase):
    def test_actual_half_columns_translation_and_camera_project(self):
        draw, layouts = fixture()
        result = decode_draw(draw, layouts)
        self.assertEqual(result['status'], 'decoded')
        self.assertEqual([i['streamElement'] for i in result['instances']], [5, 6])
        self.assertEqual(result['instances'][0]['projection']['boundsPixels'], [320, 180, 960, 360])
        self.assertEqual(result['instances'][1]['projection']['boundsPixels'], [400, 180, 1040, 360])
        self.assertEqual(result['instances'][0]['model'][3::4], [0, 0, 0, 1])
        self.assertEqual(result['consumedStream']['offset'], 252)

    def test_larger_face_and_support_batches_decode_actual_target_models(self):
        for count in (126, 210):
            with self.subTest(index_count=count):
                draw, layouts = fixture()
                draw['args'][0], draw['args'][4] = count, 422
                draw['instanceInputs']['index'] = buffer(
                    struct.pack('<6H', 7, 8, 9, 9, 8, 7) * (count // 6), 13, 12)
                stream = draw['instanceInputs']['streams'][0]
                stream['firstElement'] = 422
                stream['buffer'] = buffer(
                    b''.join(encode_model(m) for m in TARGETS.values()), 14, 12 + 422 * 48)
                result = decode_draw(draw, layouts)
                self.assertEqual(result['status'], 'decoded')
                self.assertEqual([i['streamElement'] for i in result['instances']], [422, 423])
                self.assertEqual(result['targetMatchCount'], 2)
                self.assertTrue(all(i['targetMatches'][0]['fullModelMatch'] for i in result['instances']))
                rows = [layouts[100], *[dict(copy.deepcopy(draw), **{'pass': eye}) for eye in range(3)]]
                report = analyze_records(rows)
                self.assertEqual(report['candidateDraws'], 3)
                self.assertEqual(report['statusCounts'], {'decoded': 3})
                self.assertEqual(report['viewComparisonCounts'], {'equal': 1})

    def test_new_batch_sizes_do_not_relax_range_or_shader_guards(self):
        for count in (126, 210):
            draw, layouts = fixture()
            draw['args'][0] = count  # Retain the old six-index readback deliberately.
            self.assertEqual(decode_draw(draw, layouts)['reason'], 'index_readback_range_mismatch')
            draw['vs'] += 1
            self.assertEqual(decode_draw(draw, layouts)['reason'], 'unsupported_shader_or_stage')
        draw, layouts = fixture()
        draw['args'][0] = 127
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'unsupported_draw_range')

    def test_half_rotation_and_nonuniform_scale_follow_shader_column_order(self):
        draw, layouts = fixture()
        model = [0, 2, 0, 0, -.5, 0, 0, 0, 0, 0, 1, 0, .25, -.25, 0, 1]
        stream = draw['instanceInputs']['streams'][0]
        stream['buffer'] = buffer(encode_model(model) * 2, 14, 252, 500)
        result = decode_draw(draw, layouts)
        self.assertEqual(result['instances'][0]['worldBounds'], [[0, -1.25, .5], [.25, .75, .5]])
        self.assertEqual(result['instances'][0]['model'], model)

    def test_reference_placements_require_basis_as_well_as_translation(self):
        draw, layouts = fixture()
        a, b = [list(m) for m in TARGETS.values()]
        b[0] = -b[0]
        draw['instanceInputs']['streams'][0]['buffer'] = buffer(encode_model(a) + encode_model(b), 14, 252, 500)
        result = decode_draw(draw, layouts)
        self.assertTrue(result['instances'][0]['targetMatches'][0]['fullModelMatch'])
        self.assertFalse(result['instances'][1]['targetMatches'][0]['fullModelMatch'])
        self.assertEqual(result['targetMatchCount'], 2)

    def test_unknown_inputs_do_not_fall_back_to_cpu_model_or_unused_stream(self):
        draw, layouts = fixture()
        draw['objectInputs'] = dict(known=1, scene=10, fields={'perInstanceModelMatrix': bytes(64).hex()})
        draw['instanceInputs'].pop('streams')
        result = decode_draw(draw, layouts)
        self.assertEqual(result['reason'], 'ambiguous_or_missing_consumed_stream')
        self.assertNotIn('instances', result)

    def test_unobserved_step_rate_format_layout_and_shader_are_rejected(self):
        for change, reason in [('step', 'unsupported_step_rate'), ('format', 'unsupported_layout'),
                               ('classification', 'unsupported_layout'), ('offset', 'unsupported_layout'),
                               ('duplicate', 'duplicate_consumed_semantic'), ('missing', 'unknown_layout'),
                               ('shader', 'unsupported_shader_or_stage')]:
            draw, layouts = fixture()
            if change == 'step': layouts[100]['elements'][1][-1] = 2
            if change == 'format': layouts[100]['elements'][1][2] = 11
            if change == 'classification': layouts[100]['elements'][1][-2] = 0
            if change == 'offset': layouts[100]['elements'][1][4] = 4
            if change == 'duplicate': layouts[100]['elements'].append(layouts[100]['elements'][1])
            if change == 'missing': layouts.clear()
            if change == 'shader': draw['vs'] += 1
            with self.subTest(change=change):
                self.assertEqual(decode_draw(draw, layouts)['reason'], reason)

    def test_bound_resource_range_and_start_instance_are_verified(self):
        for change, reason in [('resource', 'instance_resource_mismatch'),
                               ('start', 'instance_range_metadata_mismatch'),
                               ('stride', 'instance_range_metadata_mismatch'),
                               ('offset', 'instance_readback_range_mismatch'),
                               ('size', 'instances:invalid_buffer_range'),
                               ('hash', 'instances:hash_mismatch'),
                               ('duplicate', 'ambiguous_or_missing_consumed_stream')]:
            draw, layouts = fixture()
            stream = draw['instanceInputs']['streams'][0]
            if change == 'resource': stream['buffer']['resource'] += 1
            if change == 'start': draw['args'][4] += 1
            if change == 'stride': stream['stride'] += 1
            if change == 'offset': stream['buffer']['offset'] += 1
            if change == 'size': stream['buffer']['resourceBytes'] = 300
            if change == 'hash': stream['buffer']['hash'] += 1
            if change == 'duplicate': draw['instanceInputs']['streams'].append(copy.deepcopy(stream))
            with self.subTest(change=change):
                self.assertEqual(decode_draw(draw, layouts)['reason'], reason)

    def test_signed_base_vertex_and_index_offset_are_not_ignored(self):
        draw, layouts = fixture()
        draw['args'][3] = -2
        self.assertEqual(decode_draw(draw, layouts)['status'], 'decoded')
        draw['args'][3] = -8
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'negative_actual_vertex')
        draw, layouts = fixture()
        draw['args'][2] += 1
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'index_readback_range_mismatch')
        draw, layouts = fixture()
        draw['instanceInputs']['vertexFirst'] += 1
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'vertex_readback_range_mismatch')

    def test_32bit_indices_use_four_byte_start_and_binding_offset(self):
        draw, layouts = fixture()
        draw['geometry'][4] = 42
        draw['instanceInputs']['index'] = buffer(struct.pack('<6I', 7, 8, 9, 9, 8, 7), 13, 18, 100)
        self.assertEqual(decode_draw(draw, layouts)['status'], 'decoded')

    def test_nonfinite_consumed_data_is_unavailable(self):
        for name in ['instance', 'vertex', 'camera']:
            draw, layouts = fixture()
            if name == 'instance':
                item = draw['instanceInputs']['streams'][0]['buffer']
                data = bytearray.fromhex(item['hex']); struct.pack_into('<e', data, 0, float('nan'))
            else:
                item = draw['instanceInputs']['vertex0' if name == 'vertex' else 'cb3']
                data = bytearray.fromhex(item['hex']); struct.pack_into('<f', data, 0 if name == 'vertex' else 64, float('inf'))
            item.update(buffer(data, item['resource'], item['offset'], item['resourceBytes']))
            with self.subTest(name=name):
                self.assertEqual(decode_draw(draw, layouts)['status'], 'unavailable')

    def test_input_failure_count_and_duplicate_bindings_remain_explicit(self):
        draw, layouts = fixture()
        draw['instanceInputs']['status'] = 'draw_budget'
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'instanceInputs:draw_budget')
        draw, layouts = fixture()
        draw['args'][1] = 65
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'unsupported_draw_range')
        draw, layouts = fixture()
        draw['instanceStreams'].append(draw['instanceStreams'][0])
        self.assertEqual(decode_draw(draw, layouts)['reason'], 'ambiguous_or_missing_consumed_stream')

    def test_view_comparison_uses_actual_models_and_separates_reordering(self):
        draw, layouts = fixture()
        rows = [layouts[100]]
        for eye in range(3):
            d = copy.deepcopy(draw); d['pass'] = eye; rows.append(d)
        self.assertEqual(analyze_records(rows)['viewComparisonCounts'], {'equal': 1})
        item = rows[2]['instanceInputs']['streams'][0]['buffer']; data = bytes.fromhex(item['hex'])
        item.update(buffer(data[48:] + data[:48], 14, 252, 500))
        self.assertEqual(analyze_records(rows)['viewComparisonCounts'], {'same_models_different_order': 1})
        item = rows[3]['instanceInputs']['streams'][0]['buffer']; data = bytearray.fromhex(item['hex'])
        struct.pack_into('<f', data, 24, .5); item.update(buffer(data, 14, 252, 500))
        self.assertEqual(analyze_records(rows)['viewComparisonCounts'], {'different': 1})

    def test_duplicate_draw_or_layout_never_provides_trusted_models(self):
        draw, layouts = fixture()
        result = analyze_records([layouts[100], draw, copy.deepcopy(draw)])
        self.assertEqual(result['reasonCounts'], {'ambiguous_draw': 2})
        result = analyze_records([layouts[100], layouts[100], draw])
        self.assertEqual(result['reasonCounts'], {'unknown_layout': 1})

    def test_output_bound_is_explicit(self):
        draw, layouts = fixture(); draw.pop('instanceInputs')
        rows = [layouts[100], *[dict(draw, ordinal=i) for i in range(MAX_DRAWS + 1)]]
        result = analyze_records(rows)
        self.assertEqual(len(result['draws']), MAX_DRAWS)
        self.assertTrue(result['limitReached'])
        self.assertEqual(result['candidateDraws'], MAX_DRAWS + 1)

    def test_file_receipt_marks_malformed_lines_and_does_not_invent_capture(self):
        draw, layouts = fixture(); draw.pop('instanceInputs')
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'flicker-draws.jsonl'
            path.write_text('\n'.join([json.dumps(layouts[100]), json.dumps(draw), 'broken', json.dumps({'type': 'end'})]))
            result = analyze(temporary)
        self.assertEqual(result['malformedLines'], 1)
        self.assertTrue(result['burstEnded'])
        self.assertEqual(result['reasonCounts'], {'instanceInputs:missing': 1})


if __name__ == '__main__':
    unittest.main()
