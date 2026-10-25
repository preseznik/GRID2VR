import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_flicker import analyze, compare_object_inputs
from summarize_diagnostic import summarize


class FlickerEvidence(unittest.TestCase):
    def test_visibility_previews_keep_every_eye_within_the_existing_budget(self):
        from PIL import Image
        import random
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            pixels = random.Random(732).randbytes(1280 * 720 * 3)
            source = b'P6\n1280 720\n255\n' + pixels
            rows = [dict(type='start', captureScope='visibility')]
            for scene in range(8):
                for eye in range(3):
                    name = f'stage-{scene}-flicker-eye{eye}.ppm'
                    (run/name).write_bytes(source)
                    rows.append(dict(type='image', scene=scene, eye=eye, file=name, status='saved'))
            rows.append(dict(type='end', scenes=8))
            (run/'flicker-draws.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            report = analyze(run)
            self.assertEqual(len(report['images']), 24)
            self.assertLess(report['retainedPngBytes'], 32 * 1024 * 1024)
            for item in report['images']:
                self.assertEqual((item['width'], item['height']), (1280, 720))
                self.assertEqual(item['sourceSha256'], hashlib.sha256(source).hexdigest())
                self.assertEqual(item['retainedTransform'], 'lanczos_preview')
                self.assertFalse(item['retainedLossless'])
                with Image.open(run/item['retained']) as preview:
                    self.assertEqual(preview.size, (640, 360))
            # Ordinary per-draw investigations still retain exact pixels.
            (run/'flicker-draws.jsonl').write_text(json.dumps(rows[1]))
            item = analyze(run)['images'][0]
            self.assertTrue(item['retainedLossless'])
            self.assertEqual(item['retainedTransform'], 'identity')
            with Image.open(run/item['retained']) as image:
                self.assertEqual(image.size, (1280, 720))
                self.assertEqual(image.tobytes(), pixels)

    def test_visibility_scope_counts_are_not_draw_membership_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            for totals, expected in (([5000, 5100, 5101], [5000, 5100, 5101]),
                                     ([True, 1, 1], [0, 0, 0]), ([1, 2], [0, 0, 0]),
                                     ([-1, 2, 3], [0, 0, 0]), (None, [0, 0, 0])):
                rows = [dict(type='start', captureScope='visibility'), dict(type='scene', scene=20),
                        dict(type='complete', scene=20, accepted=1, draws=totals, truncated=[0, 0, 0]),
                        dict(type='end', scenes=1)]
                (run / 'flicker-draws.jsonl').write_text('\n'.join(map(json.dumps, rows)))
                report = analyze(run)
                self.assertEqual(report['captureScope'], 'visibility')
                scene = report['scenes'][0]
                self.assertEqual(scene['drawCounts'], expected)
                self.assertEqual(scene['recordedDrawCounts'], [0, 0, 0])
                self.assertIsNone(scene['membership'])
                self.assertIsNone(scene['objectInputs'])
            (run / 'session.json').write_text(json.dumps(dict(runtimeSession=True, result='process_exited')))
            retained = summarize(run)
            for name in ('inputs', 'mips', 'visibility', 'instances'):
                omitted = retained['reports'][f'flicker-{name}-summary.json']['result']
                self.assertEqual(omitted['status'], 'unavailable')
                self.assertIn('Visibility-only', omitted['reason'])

    def object_draw(self, eye, ordinal=1, slot=6, value='00', source_scene=20):
        return dict(type='draw', scene=20, **{'pass': eye}, ordinal=ordinal, slot=slot,
                    kind='Indexed', vs=1, ps=2, drawable=3, properties=4, args=[6, 0, 0],
                    geometry=[7, 8, 9], predicate=[0, 0],
                    objectInputs=dict(known=1, scene=source_scene, uploadHash=eye + 10,
                                      fields=dict(perInstanceModelMatrix=value * 64,
                                                  modelViewProj=('%02x' % eye) * 64)))

    def test_expected_camera_changes_do_not_mask_object_drift(self):
        passes = [[self.object_draw(0)], [self.object_draw(1)], [self.object_draw(2, value='01')]]
        result = compare_object_inputs(passes, 20)['eyeComparisons']
        self.assertEqual(result['compared'], 2)
        self.assertEqual(result['changed'], 1)
        self.assertEqual(result['examples'][0]['field'], 'perInstanceModelMatrix')
        self.assertEqual(result['examples'][0]['passes'], [0, 2])

    def test_stale_unknown_short_or_mismatched_draw_is_not_object_drift(self):
        native = self.object_draw(0)
        for variant in ('stale', 'unknown', 'short', 'malformed', 'other_mesh'):
            eye = self.object_draw(1, value='01')
            if variant == 'stale': eye['objectInputs']['scene'] = 19
            if variant == 'unknown': eye['objectInputs']['known'] = 0
            if variant == 'short': eye['objectInputs']['fields']['perInstanceModelMatrix'] = None
            if variant == 'malformed': eye['objectInputs']['fields']['perInstanceModelMatrix'] = 'ff'
            if variant == 'other_mesh': eye['geometry'][0] = 77
            with self.subTest(variant=variant):
                result = compare_object_inputs([[native], [eye], []], 20)['eyeComparisons']
                self.assertEqual(result['changed'], 0)
                self.assertEqual(result['compared'], 0)
                self.assertEqual(result['unavailable'], 0 if variant == 'other_mesh' else 1)

    def test_depth_colour_pairing_refuses_ambiguous_geometry(self):
        depth = self.object_draw(0, ordinal=1, slot=5)
        colour = self.object_draw(0, ordinal=3, value='01')
        colour['vs'] = 42  # Same input field through a different shader layout.
        result = compare_object_inputs([[depth, colour], [], []], 20)
        self.assertEqual(result['depthColourComparisons']['changed'], 1)
        result = compare_object_inputs([[depth, dict(depth), colour], [], []], 20)
        self.assertEqual(result['depthColourComparisons']['compared'], 0)
        self.assertEqual(result['ambiguousDepthColourGroups'], 1)
        other = dict(depth, slot=0)
        result = compare_object_inputs([[other, colour], [], []], 20)
        self.assertEqual(result['depthColourComparisons']['compared'], 0)
        self.assertEqual(result['depthColourStages'], [5, 6])

    def test_instance_binding_and_alpha_upload_age_are_not_hidden(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            rows = [dict(type='scene', scene=2)]
            for eye in range(3):
                rows.append(dict(type='draw', scene=2, **{'pass': eye}, slot=6, kind='Indexed',
                                 vs=1, ps=2, drawable=3, properties=5, args=[6, 0, 0],
                                 geometry=[7, 8, 9], predicate=[0, 0],
                                 instanceStreams=[[1, 100 if eye != 1 else 101, 64, 0]],
                                 depthTest=[1, 0, 3], pcb5=[8, 1, 123, 2 if eye == 0 else 1, 16]))
            (run/'flicker-draws.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            scene = analyze(run)['scenes'][0]
            self.assertEqual(scene['membership'][0]['missing'], 1)
            self.assertEqual(scene['membership'][1]['missing'], 0)
            self.assertEqual(scene['inputCoverage']['instanceStreamDraws'], [1, 1, 1])
            self.assertEqual(scene['inputCoverage']['currentPsCb5Draws'], [1, 0, 0])

    def test_equal_count_different_mesh_is_not_equal_membership(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            rows = [dict(type='scene', scene=1, sample=1)]
            for eye in range(3):
                rows.append(dict(type='draw', scene=1, **{'pass': eye}, slot=6, kind='Indexed',
                                 vs=1, ps=2, drawable=3 if eye != 1 else 4, properties=5,
                                 args=[6, 0, 0], geometry=[7, 8, 9], predicate=[0, 0]))
            rows += [dict(type='complete', scene=1, frame=20, accepted=1, draws=[1, 1, 1], truncated=[0, 0, 0])]
            (run/'flicker-draws.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            result = analyze(run)
            self.assertFalse(result['complete'])
            self.assertEqual(result['scenes'][0]['drawCounts'], [1, 1, 1])
            self.assertEqual(result['scenes'][0]['membership'][0]['missing'], 1)
            self.assertEqual(result['scenes'][0]['membership'][1]['missing'], 0)

    def test_lossless_evidence_survives_runtime_summarization(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            name = 'stage-5-flicker-eye1.ppm'
            (run/name).write_bytes(b'P6\n1 1\n255\n\xff\x00\x00')
            rows = [dict(type='image', scene=5, eye=1, file=name, status='saved'), dict(type='end', scenes=8)]
            (run/'flicker-draws.jsonl').write_text('\n'.join(map(json.dumps, rows)))
            (run/'session.json').write_text(json.dumps(dict(runtimeSession=True, result='process_exited')))
            retained = summarize(run)
            result = retained['reports']['flicker-summary.json']['result']
            self.assertEqual(result['images'][0]['retained'], name.replace('.ppm', '.png'))
            self.assertTrue(result['images'][0]['retainedLossless'])
            self.assertEqual(result['images'][0]['retainedTransform'], 'identity')
            self.assertTrue((run/name.replace('.ppm', '.png')).is_file())
            self.assertLess(result['retainedPngBytes'], 1000)
            self.assertFalse(retained['reports']['runtime-summary.json']['result']['gameplayValidated'])
            self.assertEqual(retained['reports']['flicker-mips-summary.json']['result']['status'], 'unavailable')
            self.assertEqual(retained['reports']['flicker-visibility-summary.json']['result']['drawCount'], 0)
            self.assertEqual(retained['reports']['flicker-instances-summary.json']['result']['candidateDraws'], 0)

    def test_partial_truncated_or_escaping_capture_is_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            run = Path(folder)
            path = run/'flicker-draws.jsonl'
            path.write_text('{"type":"truncated"}\n{"type":')
            result = analyze(run)
            self.assertTrue(result['malformedLines'])
            self.assertTrue(result['logTruncated'])
            self.assertFalse(result['complete'])
            path.write_text(json.dumps(dict(type='image', file='../outside.ppm', status='saved')))
            with self.assertRaises(ValueError):
                analyze(run)


if __name__ == '__main__':
    unittest.main()
