"""Summarize native runtime counters, never independent stereo/gameplay proof."""
import hashlib
import json
import re


def normal_user_process_exit(session):
    """Owned user-session process exit, not a claim about final GPU restoration."""
    if not session or session.get('untilExit') is not True or session.get('runtimeSession') is not True:
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
    unexpected = [line for line in faults.splitlines() if not re.match(
        r'^code=c0000005 .*game_rva=0084762b .*engine_installed=0$', line)]
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
                instanceCapacityRecoveryFiles=capacity_files, startupMoviesRecoveryFiles=startup_files, unexpectedFaults=unexpected,
                independentStereoValidated=False, gameplayValidated=False,
                limits='Native counters and explicit restored stop are separate from non-rendering process exit. Last recorded background observations and exit code do not prove quit intent or final render-thread restoration. Bounded logs are incomplete history. A healthy report is not headset or driving acceptance.')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    # Preserve the pre-cleanup report and its retained-evidence checksum.
    target = args.run / 'runtime-after-cleanup.json'
    target.write_text(json.dumps(analyze(args.run), indent=2) + '\n')
    print(target)
