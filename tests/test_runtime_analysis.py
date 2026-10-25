import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).parents[1]/'tools'))
from analyze_runtime import analyze, handoff_observations, non_rendering_clean_exit, normal_user_process_exit
import practice_entry
import instance_capacity_config
import startup_movies
from summarize_diagnostic import summarize


class RuntimeEvidenceTests(unittest.TestCase):
    def test_startup_backups_require_exact_hashes_and_restored_ownership(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            original, modified = b'original startup fixture', b'modified startup fixture'
            before, after = (hashlib.sha256(v).hexdigest() for v in (original, modified))
            (run/'startup-movies-original.bin').write_bytes(original)
            (run/'startup-movies.bin').write_bytes(modified)
            self.assertFalse(analyze(run)['storageBounded'])
            entry = dict(name='system/states.bin', originalSha256=before, sha256=after)
            (run/'deployment-restored.json').write_text(json.dumps(dict(startupMovies=entry)))
            self.assertFalse(analyze(run)['storageBounded'])
            for hash_name in ('MODIFIED_SHA256', 'LEGACY_MODIFIED_SHA256'):
                with self.subTest(bundle=hash_name), patch.object(startup_movies, 'STATES_SHA256', before), patch.object(startup_movies, hash_name, after):
                    (run/'startup-movies.bin').write_bytes(modified)
                    report = analyze(run)
                    self.assertTrue(report['storageBounded'])
                    self.assertEqual(report['startupMoviesRecoveryFiles'], ['startup-movies-original.bin', 'startup-movies.bin'])
                    (run/'startup-movies.bin').write_bytes(b'foreign')
                    self.assertFalse(analyze(run)['storageBounded'])

    def test_capacity_backup_requires_fixed_hash_and_restored_journal(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            data = b'original capacity fixture'
            before = hashlib.sha256(data).hexdigest()
            backup = run/'instance-capacity-original.bin'
            backup.write_bytes(data)
            self.assertFalse(analyze(run)['storageBounded'])
            entry = dict(name='tracks/generic/ornament_system_settings.xml', originalSha256=before,
                         sha256=instance_capacity_config.MODIFIED_SHA256)
            (run/'deployment-restored.json').write_text(json.dumps(dict(instanceCapacity=entry)))
            self.assertFalse(analyze(run)['storageBounded'])
            with patch.object(instance_capacity_config, 'ORIGINAL_SHA256', before):
                self.assertEqual(analyze(run)['instanceCapacityRecoveryFiles'], [backup.name])
                self.assertTrue(analyze(run)['storageBounded'])
                backup.write_bytes(b'foreign')
                self.assertFalse(analyze(run)['storageBounded'])
                backup.write_bytes(b'x'*8193)
                self.assertFalse(analyze(run)['storageBounded'])

    def test_user_exit_keeps_process_and_render_stop_evidence_separate(self):
        session = dict(untilExit=True, runtimeSession=True, result='process_exited', terminatedOwnedProcessIds=[],
                       processes=[dict(id=42)], exits=[dict(id=42, code=0)])
        self.assertTrue(normal_user_process_exit(session))
        for change in (dict(untilExit=False), dict(runtimeSession=False), dict(result='session_end_requested'),
                       dict(terminatedOwnedProcessIds=[42]), dict(exits=[]), dict(exits=[dict(id=99, code=0)]),
                       dict(exits=[dict(id=42, code=1)]), dict(exits=[dict(id=42, code=False)]),
                       dict(processes=[dict(id=42), dict(id=42)])):
            self.assertFalse(normal_user_process_exit({**session, **change}))
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            (run/'session.json').write_text(json.dumps(session))
            state = dict(frame=100, epoch=1, state=0, attemptedPairs=50, acceptedPairs=50, failed=False)
            (run/'runtime-status.json').write_text(json.dumps(state))
            report = analyze(run)
            self.assertTrue(report['normalUserProcessExit'])
            self.assertTrue(report['lastRenderingHealthy'])
            self.assertFalse(report['nativeHealthy'])
            self.assertFalse(report['gameplayValidated'])
            for change in (dict(failed=True), dict(acceptedPairs=49), dict(state=3)):
                (run/'runtime-status.json').write_text(json.dumps({**state, **change}))
                self.assertFalse(analyze(run)['lastRenderingHealthy'])
            (run/'runtime-stopped').write_text('{"failed":true}')
            self.assertFalse(analyze(run)['normalUserProcessExit'])

    def test_practice_recovery_files_are_bounded_and_hash_checked(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            original, modified = b'original fixture', b'modified fixture'
            before, after = [hashlib.sha256(value).hexdigest() for value in (original, modified)]
            (run/'practice-flow-original.bin').write_bytes(original)
            (run/'practice-flow.bin').write_bytes(modified)
            self.assertFalse(analyze(run)['storageBounded'])
            entry = dict(name='system/flow.bin', originalSha256=before, sha256=after)
            (run/'deployment-restored.json').write_text(json.dumps(dict(practiceFlow=entry)))
            # Fake hashes in a manifest alone do not exempt these files.
            self.assertFalse(analyze(run)['storageBounded'])
            with patch.object(practice_entry, 'FLOW_SHA256', before), patch.object(practice_entry, 'PRACTICE_SHA256', after):
                report = analyze(run)
                self.assertTrue(report['storageBounded'])
                self.assertEqual(len(report['practiceRecoveryFiles']), 2)
                (run/'cpu-snapshots.bin').write_bytes(b'capture')
                self.assertFalse(analyze(run)['storageBounded'])
                (run/'cpu-snapshots.bin').unlink()
                (run/'practice-flow.bin').write_bytes(b'changed input')
                self.assertFalse(analyze(run)['storageBounded'])
                (run/'practice-flow.bin').write_bytes(modified)
                (run/'practice-flow-original.bin').write_bytes(b'x'*(2*1024*1024+1))
                self.assertFalse(analyze(run)['storageBounded'])

    def test_manual_non_rendering_exit_is_not_a_runtime_stop(self):
        session = dict(result='process_exited', terminatedOwnedProcessIds=[], runtimeSession=True,
                       playerControlOnly=True, playerControlProbe='Off', engineTrace=False,
                       captureStageTargets=False, replayOnce=False, replayStereo=False,
                       xrSessionProbe=False, xrProjection=False, processes=[dict(id=42)], exits=[dict(id=42, code=0)])
        self.assertTrue(non_rendering_clean_exit(session))
        for change in (dict(result='timeout_owned_game_terminated'), dict(terminatedOwnedProcessIds=[42]),
                       dict(playerControlOnly=False), dict(playerControlProbe='Human'), dict(engineTrace=True),
                       dict(captureStageTargets=True), dict(replayOnce=True), dict(replayStereo=True),
                       dict(xrSessionProbe=True), dict(xrProjection=True), dict(runtimeSession=False),
                       dict(exits=[]), dict(exits=[dict(id=99, code=0)]), dict(exits=[dict(id=42, code=1)]),
                       dict(exits=[dict(id=42, code=False)]), dict(processes=[dict(id=42),dict(id=42)])):
            with self.subTest(change=change):
                self.assertFalse(non_rendering_clean_exit({**session, **change}))
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            (run/'session.json').write_text(json.dumps(session))
            (run/'background-play.request').write_text('suppress')
            (run/'background-play.json').write_text(json.dumps(dict(requested='suppress', installed=True,
                enabled=True, failed=False, stopped=False, focusLossCalls=1, matchedCallerCalls=1,
                foregroundPresents=100, backgroundPresents=20, hiddenPresents=0)))
            report = analyze(run)
            self.assertTrue(report['backgroundPlayObservedHealthy'])
            self.assertTrue(report['nonRenderingCleanExit'])
            self.assertFalse(report['backgroundPlayProbeHealthy'])
            self.assertFalse(report['nativeHealthy'])
            self.assertFalse(report['gameplayValidated'])
            for contents in ('{"failed":true}', 'malformed'):
                (run/'runtime-stopped').write_text(contents)
                self.assertFalse(analyze(run)['nonRenderingCleanExit'])

    def test_background_play_requires_its_own_install_and_stop_evidence(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            self.assertIsNone(analyze(run)['backgroundPlayProbeHealthy'])
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'not_requested')
            (run/'background-play.request').write_text('suppress')
            self.assertFalse(analyze(run)['backgroundPlayProbeHealthy'])
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'not_observed')
            state = dict(requested='suppress', installed=True, enabled=True, failed=False, stopped=True,
                         focusLossCalls=2, matchedCallerCalls=2, foregroundPresents=100, backgroundPresents=200, hiddenPresents=1)
            (run/'background-play.json').write_text(json.dumps(state))
            self.assertFalse(analyze(run)['backgroundPlayProbeHealthy'])
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'observed')
            (run/'runtime-stopped').write_text('{"failed":false}')
            self.assertTrue(analyze(run)['backgroundPlayProbeHealthy'])
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'healthy')
            self.assertFalse(analyze(run)['gameplayValidated'])
            for change in (dict(installed=False), dict(enabled=False), dict(failed=True), dict(stopped=False),
                           dict(matchedCallerCalls=1), dict(focusLossCalls=True), dict(backgroundPresents=-1)):
                (run/'background-play.json').write_text(json.dumps({**state, **change}))
                self.assertFalse(analyze(run)['backgroundPlayProbeHealthy'])
            (run/'background-play.json').write_bytes(b'x'*8193)
            self.assertFalse(analyze(run)['backgroundPlayProbeHealthy'])
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')

    def test_unobserved_background_never_proves_background_or_weakens_renderer(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            state = dict(frame=200, state=2, epoch=2, attemptedPairs=45, acceptedPairs=45, failed=False)
            (run/'runtime-status.json').write_text(json.dumps(state))
            (run/'runtime-stopped').write_text('{"failed":false}')
            (run/'background-play.request').write_text('suppress')
            result = analyze(run)
            self.assertTrue(result['rendererHealthy'])
            self.assertFalse(result['nativeHealthy'])
            self.assertFalse(result['backgroundPlayProbeHealthy'])
            self.assertFalse(result['backgroundPlayObservedHealthy'])
            self.assertEqual(result['backgroundPlayVerification'], 'not_observed')
            self.assertFalse(result['gameplayValidated'])
            for change in (dict(state=0), dict(failed=True), dict(acceptedPairs=0), dict(attemptedPairs=46)):
                (run/'runtime-status.json').write_text(json.dumps({**state, **change}))
                self.assertFalse(analyze(run)['rendererHealthy'])
            (run/'runtime-status.json').write_text(json.dumps(state))
            for receipt in ('{}', '{"failed":true}', 'invalid', '[]', 'x'*8193):
                (run/'background-play.json').write_text(receipt)
                self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')
                self.assertFalse(analyze(run)['nativeHealthy'])
            (run/'background-play.json').unlink()
            (run/'background-play.json').mkdir()
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')
            (run/'background-play.json').rmdir()
            for request in ('invalid', 'x'*17):
                (run/'background-play.request').write_text(request)
                self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')
            (run/'background-play.request').unlink()
            (run/'background-play.request').mkdir()
            self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')
            (run/'background-play.request').rmdir()
            (run/'background-play.request').write_text('suppress')
            # Reject links even on hosts where creating symlinks needs privileges.
            with patch.object(Path, 'is_symlink', lambda p: p.name == 'background-play.json'):
                self.assertEqual(analyze(run)['backgroundPlayVerification'], 'failed')
            (run/'runtime-stopped').unlink()
            self.assertFalse(analyze(run)['rendererHealthy'])
            (run/'runtime-stopped').write_text('{"failed":true}')
            self.assertFalse(analyze(run)['rendererHealthy'])
            (run/'runtime-stopped').write_text('{"failed":false}')
            (run/'engine-faults.txt').write_text('code=c0000005 game_rva=00000000 engine_installed=1\n')
            self.assertFalse(analyze(run)['rendererHealthy'])
            (run/'engine-faults.txt').unlink()
            (run/'stage.raw').write_bytes(b'x')
            self.assertFalse(analyze(run)['rendererHealthy'])

    def test_handoff_observations_preserve_unknown_and_reject_inconsistent_counts(self):
        control = dict(installed=True, failed=False)
        self.assertIsNone(handoff_observations(control))
        observation = dict(writerCalls=30, writerIdentified=30, writerUnreadable=0, recordedFrameChanges=29,
                           inputEligibilityCalls=20, inputAllowed=0, inputDenied=20,
                           finish=dict(calls=1, singletonIdentity=True, readable=True, mode=2, active=1, flag94=0))
        control['replayObservation'] = observation
        result = handoff_observations(control)
        self.assertTrue(result['replayWriterRepeated'])
        self.assertEqual(result['physicalInputGate'], 'denied')
        self.assertTrue(result['attractExitConditionObserved'])
        for change in (dict(writerCalls=-1), dict(writerCalls=True), dict(writerIdentified=31),
                       dict(recordedFrameChanges=30), dict(inputAllowed=1)):
            self.assertIsNone(handoff_observations({**control, 'replayObservation': {**observation, **change}}))
        for change in (dict(writerIdentified=29), dict(writerUnreadable=1), dict(recordedFrameChanges=0)):
            self.assertFalse(handoff_observations({**control, 'replayObservation': {**observation, **change}})['replayWriterRepeated'])
        for allowed, denied, expected in ((0, 0, 'unobserved'), (10, 0, 'allowed'), (10, 10, 'mixed')):
            changed = {**observation, 'inputAllowed': allowed, 'inputDenied': denied, 'inputEligibilityCalls': allowed+denied}
            self.assertEqual(handoff_observations({**control, 'replayObservation': changed})['physicalInputGate'], expected)
        for change in (dict(calls=0), dict(singletonIdentity=False), dict(readable=False), dict(mode=1), dict(active=0), dict(flag94=1)):
            changed = {**observation, 'finish': {**observation['finish'], **change}}
            self.assertFalse(handoff_observations({**control, 'replayObservation': changed})['attractExitConditionObserved'])
        self.assertIsNone(handoff_observations({**control, 'failed': True}))

    def test_input_handoff_needs_restoration_and_never_proves_driving(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            self.assertIsNone(analyze(run)['playerControlProbeHealthy'])
            (run/'player-control.request').write_text('human')
            self.assertFalse(analyze(run)['playerControlProbeHealthy'])
            control = dict(requested='human', installed=True, stopped=True, failed=False,
                           calls=20, eligible=20, switched=20, restored=20, playbackChangedInput=0, refused=0)
            def write(value):
                (run/'player-control.json').write_text(json.dumps(value))
            write(control)
            self.assertFalse(analyze(run)['playerControlProbeHealthy'])
            (run/'runtime-stopped').write_text('{"failed":false}')
            self.assertTrue(analyze(run)['playerControlProbeHealthy'])
            self.assertFalse(analyze(run)['gameplayValidated'])
            for change in (dict(stopped=False), dict(failed=True), dict(restored=19), dict(switched=19),
                           dict(eligible=0), dict(refused=1), dict(calls='20'), dict(requested='observe')):
                write({**control, **change})
                self.assertFalse(analyze(run)['playerControlProbeHealthy'])
            (run/'player-control.request').write_text('observe')
            write({**control, 'requested': 'observe', 'switched': 0, 'restored': 0, 'playbackChangedInput': 12})
            self.assertTrue(analyze(run)['playerControlProbeHealthy'])
            write({**control, 'requested': 'observe', 'switched': 0, 'restored': 0})
            self.assertFalse(analyze(run)['playerControlProbeHealthy'])
            (run/'player-control.json').write_bytes(b'x'*32769)
            self.assertFalse(analyze(run)['playerControlProbeHealthy'])

    def test_restored_stop_is_native_evidence_only(self):
        with tempfile.TemporaryDirectory() as name:
            run = Path(name)
            (run/'session.json').write_text(json.dumps(dict(runtimeSession=True)))
            self.assertFalse(analyze(run)['nativeHealthy'])
            state = dict(frame=200, state=2, epoch=2, attemptedPairs=45, acceptedPairs=45, failed=False)
            (run/'runtime-status.json').write_text(json.dumps(state))
            self.assertFalse(analyze(run)['nativeHealthy'])
            (run/'runtime-stopped').write_text('{"failed":false}')
            report = analyze(run)
            self.assertTrue(report['nativeHealthy'])
            self.assertFalse(report['independentStereoValidated'])
            self.assertFalse(report['gameplayValidated'])
            (run/'runtime-readiness.json').write_text('{"reason":"foreground","sceneWait":"native_camera"}')
            self.assertEqual(analyze(run)['readiness']['reason'], 'foreground')
            (run/'runtime-readiness.json').write_bytes(b'x'*4097)
            self.assertIsNone(analyze(run)['readiness'])
            self.assertTrue(analyze(run)['nativeHealthy'])  # Readiness is explanatory, not acceptance.
            self.assertEqual(list(summarize(run)['reports']), ['runtime-summary.json'])
            for change in (dict(state=0), dict(failed=True), dict(acceptedPairs=0), dict(attemptedPairs=46), dict(acceptedPairs='45')):
                (run/'runtime-status.json').write_text(json.dumps({**state, **change}))
                self.assertFalse(analyze(run)['nativeHealthy'])
            (run/'runtime-status.json').write_text(json.dumps(state))
            (run/'trace.log').write_bytes(b'x'*(1024*1024+1))
            self.assertFalse(analyze(run)['nativeHealthy'])
            (run/'trace.log').unlink()
            (run/'stage-1.raw').write_bytes(b'x')
            self.assertFalse(analyze(run)['nativeHealthy'])
            (run/'stage-1.raw').unlink()
            (run/'engine-faults.txt').write_text('code=c0000005 game_rva=00000000 engine_installed=1\n')
            self.assertFalse(analyze(run)['nativeHealthy'])


if __name__ == '__main__':
    unittest.main()
