"""Summarize native runtime counters, never independent stereo/gameplay proof."""
import hashlib
import json
import re


def normal_process_exit(session):
    """Matching owned processes exited 0, without harness termination."""
    if not isinstance(session, dict):
        return False
    if session.get('result') != 'process_exited' or session.get('terminatedOwnedProcessIds') != []:
        return False
    processes, exits = session.get('processes'), session.get('exits')
    if not isinstance(processes, list) or not processes or not isinstance(exits, list) or len(processes) != len(exits):
        return False
    if any(not isinstance(p, dict) or type(p.get('id')) is not int or p['id'] <= 0 for p in processes + exits):
        return False
    ids = {p['id'] for p in processes}
    return (len(ids) == len(processes) and len({p['id'] for p in exits}) == len(exits) and
            ids == {p['id'] for p in exits} and all(type(p.get('code')) is int and p['code'] == 0 for p in exits))


def normal_user_process_exit(session):
    """Owned user-session process exit, not a claim about final GPU restoration."""
    return (normal_process_exit(session) and session.get('untilExit') is True and
            session.get('runtimeSession') is True)


def classify_faults(lines, session):
    """Keep the game's deliberate SEH signal distinct from unexpected faults.

    The hash-pinned 0x847620 stub explicitly zeros EAX then writes [EAX]. Its
    native frame handler supplies a scratch address in CONTEXT.Eax and resumes.
    Renderer hook installation does not change this protocol. Require the exact
    captured null-write signature and completed owned execution; never handle
    exceptions, waive abnormal exits, or discard the original observations.
    """
    known, unexpected = [], []
    fingerprint = session.get('executableSha256') if isinstance(session, dict) else None
    supported = (isinstance(fingerprint, str) and fingerprint.lower() in {
                 '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502',
                 # PC4090's validated Steam variant, also observed in clean exits.
                 '55c3d5e68aa6074ea094f3ec7f0cb562b765c0d516ca03336097d7eacb0f5bf2'} and
                 session.get('executableBuild') == 'grid2-avx-1.0.85.8679')
    completed = normal_process_exit(session)
    # A bounded probe deliberately stops its owned game after the completion
    # marker. Preserve that existing workflow without accepting timeouts/crashes.
    if supported and session.get('runKind') == 'Probe' and session.get('result') == 'probe_window_completed':
        processes, exits = session.get('processes'), session.get('exits')
        stopped = session.get('terminatedOwnedProcessIds')
        if (isinstance(processes, list) and processes and isinstance(exits, list) and isinstance(stopped, list) and
                all(isinstance(p, dict) and type(p.get('id')) is int and p['id'] > 0 for p in processes + exits) and
                all(type(p) is int and p > 0 for p in stopped)):
            ids = {p['id'] for p in processes}
            completed = (len(processes) == len(ids) == len(exits) == len(stopped) and
                         ids == {p['id'] for p in exits} == set(stopped) and
                         all(type(p.get('code')) is int and p['code'] == -1 for p in exits))
    fields = ('code', 'address', 'game_rva', 'eip', 'esp', 'game_base', 'eax', 'ebx',
              'ecx', 'edx', 'esi', 'edi', 'ebp', 'access', 'location', 'frame', 'engine_installed')
    pattern = ' '.join(name + ('=([0-9]+)' if name in ('frame', 'engine_installed') else '=([0-9a-fA-F]+)') for name in fields)
    for line in lines:
        match = re.fullmatch(pattern, line)
        expected = False
        if supported and completed and match:
            values = dict(zip(fields, match.groups()))
            value = {key: int(text, 10 if key in ('frame', 'engine_installed') else 16) for key, text in values.items()}
            base = value['game_base']
            expected = (value['code'] == 0xc0000005 and value['game_rva'] == 0x84762b and
                        0x10000 <= base <= 0xffffffff - 0x84762b and base % 0x10000 == 0 and
                        value['address'] == value['eip'] == base + 0x84762b and
                        value['eax'] == value['ecx'] == value['edi'] == value['location'] == 0 and
                        value['access'] == 1 and value['frame'] <= 0xffffffffffffffff and
                        all(v <= 0xffffffff for k, v in value.items() if k != 'frame') and
                        values['engine_installed'] in ('0', '1'))
        (known if expected else unexpected).append(line)
    return dict(knownNativeExceptions=known, unexpectedFaults=unexpected)


def non_rendering_clean_exit(session):
    """Process exit is not a render-thread stop acknowledgment or quit intent."""
    if not session or session.get('result') != 'process_exited' or session.get('terminatedOwnedProcessIds') != []:
        return False
    if session.get('runtimeSession') is not True or session.get('playerControlOnly') is not True or session.get('playerControlProbe') != 'Off':
        return False
    if any(session.get(key) is not False for key in ('engineTrace', 'captureStageTargets', 'replayOnce',
                                                    'replayStereo', 'xrSessionProbe', 'xrProjection')):
        return False
    processes, exits = session.get('processes'), session.get('exits')
    if not isinstance(processes, list) or not processes or not isinstance(exits, list) or len(exits) != len(processes):
        return False
    if any(not isinstance(item, dict) or type(item.get('id')) is not int or item['id'] <= 0 for item in processes + exits):
        return False
    ids = {item['id'] for item in processes}
    return (len(ids) == len(processes) and ids == {item['id'] for item in exits} and
            all(type(item.get('code')) is int and item['code'] == 0 for item in exits))


def handoff_observations(control):
    """Explain native observations without treating them as a playable handoff."""
    if not control or control.get('installed') is not True or control.get('failed') is not False:
        return None
    value = control.get('replayObservation')
    if not isinstance(value, dict):
        return None
    names = ('writerCalls', 'writerIdentified', 'writerUnreadable', 'recordedFrameChanges',
             'inputEligibilityCalls', 'inputAllowed', 'inputDenied')
    if any(type(value.get(k)) is not int or value[k] < 0 for k in names):
        return None
    if (value['writerIdentified'] > value['writerCalls'] or value['writerUnreadable'] > value['writerCalls'] or
            value['recordedFrameChanges'] > max(0, value['writerCalls'] - 1) or
            value['inputAllowed'] + value['inputDenied'] != value['inputEligibilityCalls']):
        return None
    input_gate = 'unobserved'
    if value['inputEligibilityCalls']:
        input_gate = 'mixed' if value['inputAllowed'] and value['inputDenied'] else 'allowed' if value['inputAllowed'] else 'denied'
    finish = value.get('finish', {})
    exit_observed = (isinstance(finish, dict) and type(finish.get('calls')) is int and finish['calls'] > 0 and
                     finish.get('singletonIdentity') is True and finish.get('readable') is True and
                     type(finish.get('mode')) is int and finish['mode'] == 2 and
                     type(finish.get('active')) is int and finish['active'] == 1 and
                     type(finish.get('flag94')) is int and finish['flag94'] == 0)
    return dict(replayWriterRepeated=value['writerCalls'] > 1 and value['writerIdentified'] == value['writerCalls'] and
                value['writerUnreadable'] == 0 and value['recordedFrameChanges'] > 0,
                physicalInputGate=input_gate, attractExitConditionObserved=exit_observed,
                limits='Entry observations only. Input eligibility is not physical input acceptance; recorded state writes do not establish a safe physics handoff.')


def analyze(run):
    def read(name, limit=4096):
        path = run/name
        if not path.is_file() or path.stat().st_size > limit:
            return None
        try:
            value = json.loads(path.read_text(encoding='utf-8-sig'))
            return value if isinstance(value, dict) else None
        except (ValueError, OSError):
            return None

    status, stopped = read('runtime-status.json'), read('runtime-stopped')
    faults = (run/'engine-faults.txt').read_text() if (run/'engine-faults.txt').exists() else ''
    fault_classification = classify_faults(faults.splitlines(), read('session.json', 256*1024))
    unexpected = fault_classification['unexpectedFaults']
    # The fixed practice binary and its recovery backup are bounded inputs,
    # not renderer dumps. Never exempt arbitrary .bin files or supplied hashes.
    from practice_entry import FLOW_SHA256, PRACTICE_SHA256
    deployment = read('deployment-restored.json', 32768)
    expected_entry = dict(name='system/flow.bin', originalSha256=FLOW_SHA256, sha256=PRACTICE_SHA256)
    practice_files = []
    if deployment and deployment.get('practiceFlow') == expected_entry:
        for name, digest in (('practice-flow-original.bin', FLOW_SHA256), ('practice-flow.bin', PRACTICE_SHA256)):
            path = run/name
            if path.is_file() and not path.is_symlink() and path.stat().st_size <= 2*1024*1024 and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
                practice_files.append(name)
    from instance_capacity_config import ORIGINAL_SHA256, MODIFIED_SHA256
    capacity_files = []
    expected_capacity = dict(name='tracks/generic/ornament_system_settings.xml', originalSha256=ORIGINAL_SHA256, sha256=MODIFIED_SHA256)
    path = run/'instance-capacity-original.bin'
    if (deployment and deployment.get('instanceCapacity') == expected_capacity and path.is_file() and
            not path.is_symlink() and path.stat().st_size <= 8192 and hashlib.sha256(path.read_bytes()).hexdigest() == ORIGINAL_SHA256):
        capacity_files.append(path.name)
    from startup_movies import STATES_SHA256, MODIFIED_SHA256 as STARTUP_MODIFIED_SHA256, LEGACY_MODIFIED_SHA256
    startup_files = []
    startup = deployment.get('startupMovies') if deployment else None
    startup_hash = startup.get('sha256') if isinstance(startup, dict) else None
    expected_startup = dict(name='system/states.bin', originalSha256=STATES_SHA256, sha256=startup_hash)
    if startup_hash in (STARTUP_MODIFIED_SHA256, LEGACY_MODIFIED_SHA256) and startup == expected_startup:
        for name, digest in (('startup-movies-original.bin', STATES_SHA256), ('startup-movies.bin', startup_hash)):
            path = run/name
            if path.is_file() and not path.is_symlink() and path.stat().st_size <= 2*1024*1024 and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
                startup_files.append(name)
    raw = [p.name for p in run.iterdir() if p.suffix in ('.bin', '.raw', '.ppm', '.texture', '.surface', '.buffer') and p.name not in practice_files + capacity_files + startup_files]
    names = ('trace.log', 'engine.log', 'replay-work.txt', 'openxr-session.log', 'instance-capacity.jsonl')
    logs = [run/name for base in names for name in (base, base+'.previous') if (run/name).exists()]
    storage_ok = not raw and all(p.stat().st_size <= 1024*1024 for p in logs)
    valid = bool(status) and all(type(status.get(k)) is int and status[k] >= 0
                                for k in ('frame', 'state', 'epoch', 'attemptedPairs', 'acceptedPairs'))
    healthy = (valid and status.get('failed') is False and status['state'] == 2 and
               status['acceptedPairs'] > 0 and status['attemptedPairs'] == status['acceptedPairs'] and
               bool(stopped) and stopped.get('failed') is False and not unexpected and storage_ok)
    renderer_healthy = bool(healthy)
    control = read('player-control.json', 32768)
    background = read('background-play.json', 8192)
    background_path = run/'background-play.json'
    background_request = run/'background-play.request'
    background_ok = None
    background_observed = None
    background_verification = 'not_requested'
    if background_request.exists() or background_request.is_symlink():
        background_mode = None
        if background_request.is_file() and not background_request.is_symlink() and background_request.stat().st_size <= 16:
            try:
                background_mode = background_request.read_text().strip()
            except (OSError, UnicodeError):
                pass
        background_observed = (bool(background) and background_mode in ('observe', 'suppress') and
                         not background_path.is_symlink() and
                         background.get('requested') == background_mode and background.get('installed') is True and
                         background.get('enabled') is (background_mode == 'suppress') and
                         background.get('failed') is False and not unexpected and storage_ok)
        if background_observed:
            keys = ('focusLossCalls', 'matchedCallerCalls', 'foregroundPresents', 'backgroundPresents', 'hiddenPresents')
            background_observed = all(type(background.get(k)) is int and background[k] >= 0 for k in keys)
            background_observed = background_observed and background['matchedCallerCalls'] == background['focusLossCalls']
        background_ok = (background_observed and background.get('stopped') is True and
                         bool(stopped) and stopped.get('failed') is False)
        # An absent activation receipt is not a failed hook or a verified hook.
        # Keep malformed/oversized receipts distinct so they cannot be waived.
        background_verification = ('healthy' if background_ok else 'observed' if background_observed else
                                   'not_observed' if background_mode in ('observe', 'suppress') and
                                   not background_path.exists() and not background_path.is_symlink() else 'failed')
        healthy = healthy and background_ok
    request = run/'player-control.request'
    requested = request.read_text().strip() if request.is_file() and request.stat().st_size <= 16 else None
    control_ok = None
    if request.exists():
        counters = ('calls', 'eligible', 'switched', 'restored', 'playbackChangedInput', 'refused')
        control_ok = bool(control) and all(type(control.get(k)) is int and control[k] >= 0 for k in counters)
        if control_ok:
            control_ok = (requested in ('observe', 'human') and control.get('requested') == requested and
                          control.get('installed') is True and control.get('stopped') is True and
                          control.get('failed') is False and control['calls'] == control['eligible'] > 0 and
                          control['refused'] == 0 and bool(stopped) and stopped.get('failed') is False and
                          not unexpected and storage_ok)
            if requested == 'human':
                control_ok = control_ok and control['eligible'] == control['switched'] == control['restored']
            else:
                control_ok = control_ok and control['switched'] == control['restored'] == 0 and control['playbackChangedInput'] > 0
    return dict(nativeHealthy=bool(healthy), rendererHealthy=renderer_healthy,
                status=status, stopped=stopped, readiness=read('runtime-readiness.json'),
                normalUserProcessExit=(normal_user_process_exit(read('session.json', 256*1024)) and
                                      (not (run/'runtime-stopped').exists() or bool(stopped) and stopped.get('failed') is False)),
                lastRenderingHealthy=(valid and status.get('failed') is False and status['state'] in (0, 1, 2) and
                                      status['acceptedPairs'] > 0 and status['attemptedPairs'] == status['acceptedPairs'] and
                                      not unexpected and storage_ok),
                backgroundPlay=background, backgroundPlayProbeHealthy=bool(background_ok) if background_ok is not None else None,
                backgroundPlayObservedHealthy=bool(background_observed) if background_observed is not None else None,
                backgroundPlayVerification=background_verification,
                nonRenderingCleanExit=(non_rendering_clean_exit(read('session.json', 256*1024)) and
                                       (not (run/'runtime-stopped').exists() or bool(stopped) and stopped.get('failed') is False)),
                playerControl=control, playerControlProbeHealthy=bool(control_ok) if control_ok is not None else None,
                handoffObservations=handoff_observations(control),
                storageBounded=storage_ok, rawFiles=raw, practiceRecoveryFiles=practice_files,
                instanceCapacityRecoveryFiles=capacity_files, startupMoviesRecoveryFiles=startup_files, **fault_classification,
                independentStereoValidated=False, gameplayValidated=False,
                limits='Native counters and explicit restored stop are separate from non-rendering process exit. Last recorded background observations and exit code do not prove quit intent or final render-thread restoration. Bounded logs are incomplete history. A healthy report is not headset or driving acceptance.')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--classify-faults', action='store_true', help='Print read-only exception classification JSON.')
    args = parser.parse_args()
    result = analyze(args.run)
    if args.classify_faults:
        print(json.dumps({key: result[key] for key in ('knownNativeExceptions', 'unexpectedFaults')}))
    else:
        # Preserve the pre-cleanup report and its retained-evidence checksum.
        target = args.run / 'runtime-after-cleanup.json'
        target.write_text(json.dumps(result, indent=2) + '\n')
        print(target)
