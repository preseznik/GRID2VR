import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker_mips import (COLOUR_VS, DEPTH_VS, MAX_BYTES, MAX_EXAMPLES, MAX_LOG,
                                 SCOPE, analyze, analyze_records)


def fixture(scene_count=1):
    records = [dict(type='start', maxScenes=8)]
    ordinal_count = 0
    for scene in range(10, 10 + scene_count):
        records.append(dict(type='scene', scene=scene, sample=scene - 9))
        for eye in range(3):
            for side, shader in enumerate((DEPTH_VS, COLOUR_VS)):
                draw = dict(type='draw', scene=scene, ordinal=side + 1, slot=5 + side, vs=shader,
                            ps=(0x59bc124b8c63bdaa, 0xaa19112331ae0872)[side], kind='Indexed',
                            args=[3, 0, 0], geometry=[12, 24, 0, 13, 57, 0], drawable=side * 45,
                            properties=side * 55, instanceStreams=[], textures=[30, 0, 0, 0, 0, 0, 0, 0],
                            objectInputs=dict(known=1, scene=scene, fields=dict(perInstanceModelMatrix='06' * 64)),
                            alphaSampling=dict(slot=3 + side, bound=1, descriptor=[0] * 13, view=30,
                                               resource=31, viewFormat=71, viewDimension=4,
                                               mostDetailedMip=0, mipLevels=3, resourceMinLodBits=0,
                                               texture=[4, 4, 3, 1, 71]), **{'pass': eye})
                digest = dict(status='ok', width=4, height=4, format=71, mips=3, array=1,
                              samples=1, quality=0, bytes=24,
                              alphaCoverage=dict(threshold=0.5, comparison='greater_equal', scope=SCOPE),
                              subresources=[dict(mip=mip, slice=0, rowBytes=8, rows=1, sha256=f'{mip+1:064x}',
                                                 alpha=dict(status='ok', texels=texels, atLeastHalf=texels // 2,
                                                            belowHalf=texels - texels // 2))
                                            for mip, texels in enumerate((16, 4, 1))])
                ordinal_count += 1
                sample = dict(type='alphaTexture', scene=scene, ordinal=side + 1, view=30, resource=31,
                              remainingBudget=MAX_BYTES - ordinal_count * 24, digest=digest, **{'pass': eye})
                records += [draw, sample]
        records.append(dict(type='complete', scene=scene, accepted=1, draws=[2, 2, 2], truncated=[0, 0, 0]))
    return records + [dict(type='end', scenes=scene_count)]


def samples(records):
    return [row for row in records if row.get('type') == 'alphaTexture']


def draws(records):
    return [row for row in records if row.get('type') == 'draw']


class MipEvidence(unittest.TestCase):
    def test_complete_exact_draws_and_flags_are_equal(self):
        result = analyze_records(fixture())
        self.assertEqual(result['validatedSamples'], 6)
        self.assertEqual(result['validatedBytes'], 144)
        self.assertEqual(result['nativeEye']['equal'], 4)
        self.assertEqual(result['flagPrepassColour']['equal'], 3)
        self.assertEqual(result['fullyTransparentMipRecords'], 6)
        self.assertEqual(result['coverageExamples'][0]['fraction'], 0.5)
        self.assertIn('not object-UV-specific', result['limits'])
        self.assertIn('not retained for independent rehashing', result['limits'])

    def test_changed_mip_hash_with_same_pointer_is_detected(self):
        records = fixture()
        samples(records)[3]['digest']['subresources'][1]['sha256'] = 'ff' * 32
        result = analyze_records(records)
        self.assertEqual(result['nativeEye']['changed'], 1)
        self.assertEqual(result['flagPrepassColour']['changed'], 1)
        self.assertEqual(result['nativeEye']['examples'][0]['changedMips'], [(1, 0)])
        self.assertEqual(result['resourceSceneConflicts'], 1)

    def test_bc3_family_requires_sixteen_byte_blocks_and_decoded_alpha(self):
        for fmt in (76, 77, 78):
            records = fixture()
            for draw, sample in zip(draws(records), samples(records)):
                draw['alphaSampling']['texture'][-1] = fmt
                draw['alphaSampling']['viewFormat'] = 77 if fmt == 76 else fmt
                sample['digest'].update(format=fmt, bytes=48)
                for mip in sample['digest']['subresources']:
                    mip['rowBytes'] = 16
            with self.subTest(format=fmt):
                result = analyze_records(records)
                self.assertEqual(result['validatedSamples'], 6)
                self.assertEqual(result['validatedBytes'], 288)
                self.assertEqual(result['nativeEye']['equal'], 4)
                samples(records)[3]['digest']['subresources'][1]['alpha']['status'] = 'unsupported_format'
                result = analyze_records(records)
                self.assertEqual(result['validatedSamples'], 5)
                self.assertEqual(result['nativeEye']['unavailable'], 1)
                self.assertEqual(result['unavailableReasons'], {'alpha_unavailable': 1})

    def test_invalid_remaining_budget_is_unavailable(self):
        for remaining in (-1, MAX_BYTES + 1, True):
            records = fixture()
            samples(records)[3]['remainingBudget'] = remaining
            with self.subTest(remaining=remaining):
                result = analyze_records(records)
                self.assertEqual(result['validatedSamples'], 5)
                self.assertEqual(result['unavailableReasons'], {'invalid_budget': 1})

    def test_changed_alpha_counts_are_detected(self):
        records = fixture()
        samples(records)[3]['digest']['subresources'][1]['alpha'].update(atLeastHalf=0, belowHalf=4)
        result = analyze_records(records)
        self.assertEqual(result['nativeEye']['changed'], 1)
        self.assertEqual(result['fullyTransparentMipRecords'], 7)

    def test_missing_duplicate_or_budget_samples_never_compare_equal(self):
        for variation in ('missing', 'duplicate', 'budget', 'map_failed', 'unsupported'):
            records = fixture()
            row = samples(records)[3]
            if variation == 'missing':
                records.remove(row)
            elif variation == 'duplicate':
                records.insert(-1, copy.deepcopy(row))
            else:
                row['digest']['status'] = variation
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['validatedSamples'], 5)
                self.assertEqual(result['nativeEye']['unavailable'], 1)
                self.assertEqual(result['flagPrepassColour']['unavailable'], 1)
                self.assertEqual(result['nativeEye']['equal'], 3)

    def test_stale_bound_view_resource_and_descriptor_are_refused(self):
        for variation in ('t0', 'view', 'resource', 'descriptor', 'old_model'):
            records = fixture()
            draw, sample = draws(records)[3], samples(records)[3]
            if variation == 't0': draw['textures'][0] += 1
            if variation == 'view': sample['view'] += 1
            if variation == 'resource': sample['resource'] += 1
            if variation == 'descriptor': draw['alphaSampling']['texture'][0] += 1
            if variation == 'old_model': draw['objectInputs']['scene'] = 9
            with self.subTest(variation=variation):
                result = analyze_records(records)
                if variation == 'old_model':
                    self.assertEqual(result['flagPrepassColour']['equal'], 2)
                    self.assertEqual(result['invalidDrawIdentities'], 1)
                else:
                    self.assertEqual(result['validatedSamples'], 5)
                    self.assertEqual(result['nativeEye']['unavailable'], 1)

    def test_full_mip_layout_hash_counts_and_definition_are_required(self):
        for variation in ('hash', 'hash_case', 'rows', 'row_bytes', 'count', 'negative', 'texels',
                          'unsupported_alpha', 'missing_mip', 'duplicate_mip', 'definition', 'bytes', 'format'):
            records = fixture()
            digest = samples(records)[3]['digest']
            item = digest['subresources'][1]
            if variation == 'hash': item['sha256'] = 'abc'
            if variation == 'hash_case': item['sha256'] = 'AA' * 32
            if variation == 'rows': item['rows'] = 2
            if variation == 'row_bytes': item['rowBytes'] = 4
            if variation == 'count': item['alpha']['belowHalf'] = 0
            if variation == 'negative': item['alpha']['atLeastHalf'] = -1
            if variation == 'texels': item['alpha']['texels'] = 16
            if variation == 'unsupported_alpha': item['alpha']['status'] = 'unsupported_format'
            if variation == 'missing_mip': digest['subresources'].pop()
            if variation == 'duplicate_mip': digest['subresources'][2] = copy.deepcopy(item)
            if variation == 'definition': digest['alphaCoverage']['threshold'] = 0.25
            if variation == 'bytes': digest['bytes'] = 25
            if variation == 'format': digest['format'] = []
            with self.subTest(variation=variation):
                result = analyze_records(records)
                self.assertEqual(result['validatedSamples'], 5)
                self.assertEqual(result['nativeEye']['unavailable'], 1)

    def test_resource_changes_across_scenes_are_separate(self):
        records = fixture(2)
        for sample in samples(records)[6:]:
            sample['digest']['subresources'][2]['sha256'] = 'ff' * 32
        result = analyze_records(records)
        self.assertEqual(result['nativeEye']['changed'], 0)
        self.assertEqual(result['acrossScenes']['changed'], 1)
        self.assertEqual(result['resourceSceneConflicts'], 0)

    def test_flag_pair_requires_same_bound_resource(self):
        records = fixture()
        samples(records)[3]['resource'] = 32
        draws(records)[3]['alphaSampling']['resource'] = 32
        result = analyze_records(records)
        self.assertEqual(result['validatedSamples'], 6)
        self.assertEqual(result['nativeEye']['equal'], 4)
        self.assertEqual(result['flagPrepassColour']['unavailable'], 1)

    def test_duplicate_draw_identity_does_not_pair_by_ordinal(self):
        records = fixture()
        draw, sample = copy.deepcopy(draws(records)[0]), copy.deepcopy(samples(records)[0])
        draw['ordinal'] = sample['ordinal'] = 3
        records[-2]['draws'][0] = 3
        records[-2:-2] = [draw, sample]
        result = analyze_records(records)
        self.assertEqual(result['nativeEye']['ambiguous'], 2)
        self.assertEqual(result['flagPrepassColour']['ambiguous'], 1)

    def test_examples_are_bounded(self):
        result = analyze_records(fixture(8))
        self.assertEqual(len(result['coverageExamples']), MAX_EXAMPLES)
        self.assertEqual(len(result['fullyTransparentMipExamples']), MAX_EXAMPLES)
        self.assertTrue(result['coverageExamplesTruncated'])

    def test_incomplete_truncated_rejected_and_miscounted_logs_are_refused(self):
        for variation in ('end', 'start', 'rejected', 'draw_truncation', 'log_truncation', 'count', 'duplicate_draw'):
            records = fixture()
            if variation == 'end': records.pop()
            if variation == 'start': records.pop(0)
            if variation == 'rejected': records[-2]['accepted'] = 0
            if variation == 'draw_truncation': records[-2]['truncated'][0] = 1
            if variation == 'log_truncation': records.insert(-1, dict(type='truncated'))
            if variation == 'count': records[-2]['draws'][0] = 0
            if variation == 'duplicate_draw': draws(records)[1]['ordinal'] = 1
            with self.subTest(variation=variation), self.assertRaises(ValueError):
                analyze_records(records)

    def test_file_bounds_malformed_partial_and_nonfinite_json_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'flicker-draws.jsonl'
            content = '\n'.join(json.dumps(row) for row in fixture()) + '\n'
            path.write_text(content)
            self.assertEqual(analyze(directory)['validatedSamples'], 6)
            for payload in (content[:-1], content + 'bad\n', content.replace('"quality": 0', '"quality": NaN')):
                path.write_text(payload)
                with self.assertRaises(ValueError): analyze(directory)
            with path.open('wb') as stream:
                stream.truncate(MAX_LOG + 1)
            with self.assertRaises(ValueError): analyze(directory)


if __name__ == '__main__':
    unittest.main()
