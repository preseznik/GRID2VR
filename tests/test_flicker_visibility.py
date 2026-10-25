import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker_inputs import COLOUR_VS
from analyze_flicker_visibility import (MESH_PS, MESH_VS, analyze, contact_sheet, image_region,
                                       pixel_witness, project_draw, project_mesh)
from test_flicker_inputs import buffer, fixture


def supported():
    records = fixture()
    layout, draw = records[0], records[2]
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    cb0 = bytearray(240)
    cb3 = bytearray(320)
    cb0[176:240] = struct.pack('<16f', *identity)
    cb3[64:128] = struct.pack('<16f', *identity)
    vertex = b''.join(struct.pack('<5f4B', *position, 0, 0, 0, 0, 0, 255)
                      for position in ((-.5, 0, .5), (.5, 0, .5), (0, .5, .5)))
    draw.update(vs=COLOUR_VS, slot=6)
    draw['objectInputs']['fields']['perInstanceModelMatrix'] = cb0[176:240].hex()
    draw['flagInputs'].update(cb0=buffer(cb0, 14), cb3=buffer(cb3, 15), vertex0=buffer(vertex, 12))
    return draw, {layout['id']: layout}


def witness():
    return dict(type='flagPixels', scene=10, ordinal=2, status='ok', target=20, width=8, height=4,
                format=28, finalSourceSame=True, finalSource=[20, 8, 4, 28], changedPixels=5,
                retainedPixels=2, revertedPixels=1, otherPixels=2, bounds=[2, 1, 5, 3],
                maskTruncated=False, maskRuns=[[1, 2, 3], [2, 2, 2]], **{'pass': 1})


def supported_mesh():
    draw, layouts = supported()
    draw.update(vs=MESH_VS, ps=MESH_PS, ordinal=7, args=[6, 4, 0xfffffffe], geometry=[12, 24, 20, 13, 57, 6])
    inputs = draw.pop('flagInputs')
    cb0 = bytearray(224)
    cb0[144:208] = bytes.fromhex(draw['objectInputs']['fields']['perInstanceModelMatrix'])
    inputs['cb0'] = buffer(cb0, 14)
    inputs['vertexFirst'] = 5
    inputs['vertex0'].update(offset=140, resourceBytes=256)
    inputs['index'] = dict(buffer(struct.pack('<6H', 7, 8, 9, 9, 8, 7), 13), offset=14, resourceBytes=128)
    draw['meshInputs'] = inputs
    # A rigid object does not require the flag's COLOR semantic.
    layouts[100]['elements'] = [layouts[100]['elements'][0]]
    return draw, layouts


class FlagVisibility(unittest.TestCase):
    def test_identity_camera_and_model_produce_expected_screen_box(self):
        draw, layouts = supported()
        result = project_draw(draw, layouts)
        self.assertEqual(result['status'], 'projected')
        self.assertEqual(result['boundsPixels'], [320, 180, 960, 360])
        self.assertEqual(result['paddedRoi'], [318, 178, 963, 363])
        self.assertEqual(result['indexedVertices'], 3)
        self.assertEqual(result['uniquePositions'], 3)

    def test_actual_gpu_model_must_agree_with_observed_identity(self):
        draw, layouts = supported()
        draw['objectInputs']['fields']['perInstanceModelMatrix'] = '00' * 64
        self.assertEqual(project_draw(draw, layouts)['reason'], 'observed_model_disagrees_with_gpu')

    def test_exact_shader_layout_and_hash_are_required(self):
        for change, expected in (('shader', 'unsupported_shader_or_stage'),
                                 ('hash', 'cb0:hash_mismatch'),
                                 ('layout', 'unsupported_position_or_colour_format')):
            draw, layouts = supported()
            if change == 'shader': draw['vs'] += 1
            if change == 'hash': draw['flagInputs']['cb0']['hash'] += 1
            if change == 'layout': layouts[100]['elements'][0][2] = 7
            with self.subTest(change=change):
                self.assertEqual(project_draw(draw, layouts)['reason'], expected)

    def test_bgra_uses_red_byte_and_wave_affects_positive_y_and_z(self):
        draw, layouts = supported()
        cb0 = bytearray.fromhex(draw['flagInputs']['cb0']['hex'])
        struct.pack_into('<4f', cb0, 160, 0, 1.57079632679, .125, 0)
        draw['flagInputs']['cb0'] = buffer(cb0, 14)
        layouts[100]['elements'][2][2] = 87
        vertex = bytearray.fromhex(draw['flagInputs']['vertex0']['hex'])
        for offset in range(0, len(vertex), 24):
            vertex[offset + 22] = 255
        draw['flagInputs']['vertex0'] = buffer(vertex, 12)
        result = project_draw(draw, layouts)
        self.assertEqual(result['boundsPixels'], [320, 135, 960, 315])
        self.assertEqual(result['depthRange'], [.625, .625])
        # Blue does not displace the mesh, despite sharing its packed colour word.
        for offset in range(0, len(vertex), 24):
            vertex[offset + 22], vertex[offset + 20] = 0, 255
        draw['flagInputs']['vertex0'] = buffer(vertex, 12)
        self.assertEqual(project_draw(draw, layouts)['boundsPixels'], [320, 180, 960, 360])

    def test_near_plane_nonfinite_and_range_errors_are_not_guessed(self):
        for value, reason in ((-.5, 'depth_or_w_clipping_required'),
                              (float('nan'), 'nonfinite_vertex')):
            draw, layouts = supported()
            vertex = bytearray.fromhex(draw['flagInputs']['vertex0']['hex'])
            struct.pack_into('<f', vertex, 8, value)
            draw['flagInputs']['vertex0'] = buffer(vertex, 12)
            with self.subTest(value=value):
                self.assertEqual(project_draw(draw, layouts)['reason'], reason)
        draw, layouts = supported()
        draw['args'][1] = 300
        self.assertEqual(project_draw(draw, layouts)['reason'], 'index_range')

    def test_image_viewport_mapping_and_rgb_statistics_are_explicit(self):
        from PIL import Image
        draw, layouts = supported()
        projection = project_draw(draw, layouts)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'image.png'
            Image.new('RGB', (1280, 720), (25, 50, 75)).save(path)
            result = image_region(path, projection)
            self.assertEqual(result['status'], 'sampled')
            self.assertEqual(result['meanRgb'], [25, 50, 75])
            self.assertEqual(result['pixels'], 645 * 185)
            bad = copy.deepcopy(projection)
            bad['viewport'][3] = 640
            self.assertEqual(image_region(path, bad)['status'], 'viewport_image_mismatch')

    def test_final_png_requires_unique_accepted_complete_capture(self):
        from PIL import Image
        draw, layouts = supported()
        image = dict(type='image', scene=10, eye=1, file='stage-10-flicker-eye1.ppm', status='saved')
        complete = dict(type='complete', scene=10, accepted=1, truncated=[0, 0, 0])
        rows = [layouts[100], draw, image, complete, dict(type='end')]
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            Image.new('RGB', (1280, 720), 'red').save(run / 'stage-10-flicker-eye1.png')
            def inspect(records):
                (run / 'flicker-draws.jsonl').write_text('\n'.join(json.dumps(r) for r in records))
                return analyze(run)
            result = inspect(rows)
            self.assertEqual(result['finalImageCounts'], {'sampled': 1})
            self.assertIn('cannot establish object visibility', result['limits'])
            self.assertNotIn('visible', result['flags'][0])
            self.assertEqual(inspect([*rows, image])['finalImageCounts'], {'unavailable_capture': 1})
            complete['accepted'] = 0
            self.assertEqual(inspect(rows)['finalImageCounts'], {'unavailable_capture': 1})

    def test_oversized_or_ambiguous_layout_is_rejected(self):
        draw, layouts = supported()
        layouts[100] = None
        self.assertEqual(project_draw(draw, layouts)['reason'], 'unknown_layout')
        draw, layouts = supported()
        draw['viewport'][3] = 8192
        self.assertEqual(project_draw(draw, layouts)['reason'], 'unsupported_viewport')

    def test_exact_mask_samples_only_written_coordinates(self):
        from PIL import Image
        row = witness()
        with tempfile.TemporaryDirectory() as directory:
            image = Image.new('RGB', (8, 4), 'white')
            for y, x, count in row['maskRuns']:
                for column in range(x, x + count):
                    image.putpixel((column, y), (10, 30, 50))
            path = Path(directory) / 'mask.png'
            image.save(path)
            result = pixel_witness([row], path)
            self.assertEqual(result['status'], 'captured')
            self.assertEqual(result['observedMaskPixels'], 5)
            self.assertEqual(result['finalPngMask']['pixels'], 5)
            self.assertEqual(result['finalPngMask']['meanRgb'], [10, 30, 50])
            self.assertEqual(result['finalPngMask']['deviationRgb'], [0, 0, 0])
            self.assertIn('Equality is not object survival', result['interpretation'])

    def test_mask_sum_bounds_overlap_and_identity_must_be_consistent(self):
        for change, reason in (('sum', 'pixel_count_sum'), ('overlap', 'mask_run_overlap_or_order'),
                               ('extent', 'mask_outside_declared_bounds'),
                               ('total', 'mask_count_or_bounds'), ('identity', 'final_source_identity'),
                               ('descriptor', 'final_source_descriptor'), ('outside', 'mask_run_bounds')):
            row = witness()
            if change == 'sum': row['retainedPixels'] += 1
            if change == 'overlap': row['maskRuns'][1] = [1, 3, 2]
            if change == 'extent': row['bounds'][0] = 3
            if change == 'total': row['maskRuns'][1][2] = 1
            if change == 'identity': row['finalSourceSame'] = False
            if change == 'descriptor': row['finalSource'][1] = 9
            if change == 'outside': row['maskRuns'][1][0] = 4
            with self.subTest(change=change):
                self.assertEqual(pixel_witness([row])['reason'], reason)

    def test_truncated_mask_and_different_target_never_sample_final_png(self):
        row = witness()
        row['maskTruncated'] = True
        row['maskRuns'].pop()
        self.assertEqual(pixel_witness([row])['finalPngMask']['status'], 'truncated_mask')
        row = witness()
        row['finalSourceSame'] = False
        row['finalSource'][0] = 21
        self.assertEqual(pixel_witness([row])['finalPngMask']['status'], 'different_final_source')
        self.assertEqual(pixel_witness([])['status'], 'missing')
        self.assertEqual(pixel_witness([row, row])['status'], 'ambiguous')

    def test_zero_mask_is_valid_but_mismatched_final_dimensions_are_not_sampled(self):
        from PIL import Image
        row = witness()
        for name in ('changedPixels', 'retainedPixels', 'revertedPixels', 'otherPixels'):
            row[name] = 0
        row.update(maskRuns=[], bounds=[0, 0, 0, 0])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'mask.png'
            Image.new('RGB', (8, 4)).save(path)
            self.assertEqual(pixel_witness([row], path)['finalPngMask']['status'], 'empty_mask')
            Image.new('RGB', (4, 4)).save(path)
            self.assertEqual(pixel_witness([row], path)['finalPngMask']['status'], 'target_image_mismatch')

    def test_witness_matching_uses_scene_pass_ordinal_not_nearest_draw(self):
        draw, layouts = supported()
        row = witness()
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            def inspect(witness_row):
                (run / 'flicker-draws.jsonl').write_text('\n'.join(json.dumps(r) for r in (layouts[100], draw, witness_row)))
                return analyze(run)['flags'][0]['pixelWitness']
            self.assertEqual(inspect(row)['status'], 'captured')
            for field in ('scene', 'pass', 'ordinal'):
                altered = dict(row)
                altered[field] += 1
                with self.subTest(field=field):
                    self.assertEqual(inspect(altered)['status'], 'missing')

    def test_ranged_mesh_uses_signed_base_vertex_and_actual_buffer_offsets(self):
        draw, layouts = supported_mesh()
        result = project_mesh(draw, layouts)
        self.assertEqual(result['status'], 'projected')
        self.assertEqual(result['boundsPixels'], [320, 180, 960, 360])
        self.assertEqual(result['bufferRange'], dict(vertexFirst=5, vertexLast=7,
                                                   vertexOffset=140, indexOffset=14, signedBaseVertex=-2))
        draw['args'][2] = -2
        self.assertEqual(project_mesh(draw, layouts)['boundsPixels'], result['boundsPixels'])

    def test_mesh_shader_forms_world_xyz_and_forces_homogeneous_one(self):
        draw, layouts = supported_mesh()
        cb0 = bytearray.fromhex(draw['meshInputs']['cb0']['hex'])
        struct.pack_into('<f', cb0, 192, .25)
        struct.pack_into('<f', cb0, 204, 9.0)
        draw['meshInputs']['cb0'] = buffer(cb0, 14)
        draw['objectInputs']['fields']['perInstanceModelMatrix'] = cb0[144:208].hex()
        self.assertEqual(project_mesh(draw, layouts)['boundsPixels'], [480, 180, 1120, 360])

    def test_mesh_uint32_indices_use_four_byte_start_offset(self):
        draw, layouts = supported_mesh()
        draw['geometry'][4] = 42
        draw['meshInputs']['index'] = dict(buffer(struct.pack('<6I', 7, 8, 9, 9, 8, 7), 13),
                                           offset=22, resourceBytes=128)
        result = project_mesh(draw, layouts)
        self.assertEqual(result['boundsPixels'], [320, 180, 960, 360])
        self.assertEqual(result['bufferRange']['indexOffset'], 22)

    def test_mesh_range_metadata_hash_and_resource_are_checked(self):
        cases = (('first', 'vertex_first_mismatch'), ('voffset', 'vertex_range_mismatch'),
                 ('ioffset', 'index_range_mismatch'), ('resourcebytes', 'vertex0:invalid_range_metadata'),
                 ('hash', 'index:hash_mismatch'), ('resource', 'vertex0:resource_mismatch'),
                 ('short', 'vertex_range_mismatch'))
        for change, expected in cases:
            draw, layouts = supported_mesh()
            inputs = draw['meshInputs']
            if change == 'first': inputs['vertexFirst'] = 4
            if change == 'voffset': inputs['vertex0']['offset'] += 4
            if change == 'ioffset': inputs['index']['offset'] += 2
            if change == 'resourcebytes': inputs['vertex0']['resourceBytes'] = 145
            if change == 'hash': inputs['index']['hash'] += 1
            if change == 'resource': inputs['vertex0']['resource'] += 1
            if change == 'short':
                previous = inputs['vertex0']
                inputs['vertex0'] = dict(buffer(bytes.fromhex(previous['hex'])[:-1], 12), offset=140, resourceBytes=256)
            with self.subTest(change=change):
                self.assertEqual(project_mesh(draw, layouts)['reason'], expected)

    def test_mesh_position_layout_and_negative_actual_index_are_rejected(self):
        draw, layouts = supported_mesh()
        draw['args'][2] = -8
        self.assertEqual(project_mesh(draw, layouts)['reason'], 'negative_vertex_index')
        draw, layouts = supported_mesh()
        layouts[100]['elements'][0][3] = 1
        self.assertEqual(project_mesh(draw, layouts)['reason'], 'unsupported_position_layout')
        draw, layouts = supported_mesh()
        draw['ps'] += 1
        self.assertEqual(project_mesh(draw, layouts)['reason'], 'unsupported_shader_or_stage')

    def test_mesh_and_flag_lists_stay_separate_and_unmasked_meshes_are_counted(self):
        mesh, layouts = supported_mesh()
        flag, flag_layouts = supported()
        flag_layouts[200] = dict(flag_layouts[100], id=200)
        flag['inputLayout'] = 200
        row = dict(witness(), type='meshPixels', ordinal=7)
        unmasked = dict(mesh, ordinal=8)
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            records = [layouts[100], flag_layouts[200], mesh, row, flag, unmasked]
            (run / 'flicker-draws.jsonl').write_text('\n'.join(json.dumps(r) for r in records))
            result = analyze(run)
            self.assertEqual(len(result['flags']), 1)
            self.assertEqual(len(result['meshes']), 1)
            self.assertEqual(result['meshes'][0]['projection']['status'], 'projected')
            self.assertEqual(result['meshes'][0]['pixelWitness']['status'], 'captured')
            self.assertEqual(result['flags'][0]['pixelWitness']['status'], 'missing')
            self.assertEqual(result['meshInputsDrawCount'], 2)
            self.assertEqual(result['meshInputsWithoutWitness'], 1)
            contact_sheet(run, result, run / 'mesh.png', 'meshes')
            contact_sheet(run, result, run / 'mixed.png', 'mixed')
            self.assertTrue((run / 'mesh.png').is_file())
            with self.assertRaises(ValueError):
                contact_sheet(run, result, run / 'bad.png', 'unknown')

    def test_mesh_output_is_bounded_independently_of_flags(self):
        mesh, layouts = supported_mesh()
        records = [layouts[100]]
        for ordinal in range(1, 99):
            records.extend((dict(mesh, ordinal=ordinal), dict(witness(), type='meshPixels', ordinal=ordinal)))
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'flicker-draws.jsonl').write_text('\n'.join(json.dumps(r) for r in records))
            result = analyze(run)
            self.assertEqual(result['meshDrawCount'], 98)
            self.assertEqual(len(result['meshes']), 96)
            self.assertTrue(result['meshLimitReached'])
            self.assertEqual(len(result['flags']), 0)


if __name__ == '__main__':
    unittest.main()
