"""Recheck CPU camera geometry in memory; retain the original GPU/run verdicts."""
import argparse
import hashlib
import json
from pathlib import Path

from analyze_engine_trace import fields
from analyze_eye_offset import camera_geometry, geometry_valid
from dynamic_views import dynamic_inputs, frozen_frame_valid
from unpack_snapshots import read_archive


def audit(run):
    data, records, _ = read_archive(run)
    digest = hashlib.sha256(data).hexdigest()
    retired = json.loads((run/'cpu-snapshots-retired.json').read_text())
    if retired['status'] != 'complete' or retired['archiveSha256'] != digest:
        raise ValueError('Requires the exact retired CPU archive')
    report = json.loads((run/'stereo-sequence-summary.json').read_text())
    lines = (run/'engine.log').read_text().splitlines()
    graphics = (run/'trace.log').read_text().splitlines()
    inputs = dynamic_inputs(run, lines)
    pairs = [fields(line) for line in lines if 'sequence pair ' in line]
    if inputs is None or not 2 <= len(pairs) <= 900 or not len(inputs) == len(pairs) == len(report['frames']) == report['requested']:
        raise ValueError('Incomplete archived sequence')
    # GPU images were retired. These verdicts remain historical evidence;
    # only CPU geometry and frozen inputs are independently recomputed here.
    retained = all(v for k, v in report['gates'].items() if k != 'everyFrame')
    frozen = True
    accepted = 0
    comparisons = 0
    changes = []
    failures = []
    maxima = dict(renderedRequestedPositionError=0., eyeOriginError=0., centerOriginError=0., renderedPositionError=0.)
    def by_frame(source):
        grouped = {}
        for line in source:
            grouped.setdefault(fields(line).get('frame'), []).append(line)
        return grouped
    engine_frames, graphics_frames = by_frame(lines), by_frame(graphics)
    for index, (pair, previous) in enumerate(zip(pairs, report['frames'])):
        key, frame = pair['id'], pair['frame']
        if previous['id'] != key or previous['frame'] != frame or len(previous['eyes']) != 2:
            raise ValueError('Archived frame identity mismatch')
        frozen &= frozen_frame_valid(engine_frames[frame], graphics_frames[frame], frame,
            pair['tid'], key, index, inputs[index, :, :7], inputs[index, :, 7:])
        retained &= all(v for k, v in previous['gates'].items() if k != 'bothEyes')
        view = records[f'stage-{key}-scene-complete-view.bin']
        frame_ok = True
        for eye, prefix in enumerate(('', 'right-')):
            old = previous['eyes'][eye]
            if old['eye'] != eye:
                raise ValueError('Crossed archived eyes')
            retained &= all(v for k, v in old['gates'].items() if k not in ('geometry', 'mainCamera'))
            posed = records[f'stage-{key}-{prefix}replay-entry-view.bin']
            for phase, center_phase, field, gate in (('complete', 'complete', 'geometry', 'geometry'), ('main', 'entry', 'mainGeometry', 'mainCamera')):
                geometry = camera_geometry(records[f'stage-{key}-scene-{center_phase}-context.bin'],
                    records[f'stage-{key}-{prefix}replay-{phase}-context.bin'], view, posed,
                    0, asymmetric=True, pose=inputs[index, eye, :7], frustum=inputs[index, eye, 7:])
                # The fix changes the acceptance reference, not prior measurements.
                if any(geometry.get(k) != v for k, v in old[field].items()):
                    raise ValueError('Historical geometry measurements differ')
                valid = geometry_valid(geometry)
                comparisons += 1
                frame_ok &= valid
                for metric, value in dict(renderedRequestedPositionError=geometry['renderedRequestedPositionError'],
                        eyeOriginError=geometry['nativeOriginPositionErrors'][1],
                        centerOriginError=geometry['nativeOriginPositionErrors'][0],
                        renderedPositionError=geometry['renderedPositionError']).items():
                    maxima[metric] = max(maxima[metric], value)
                item = dict(frame=frame, scene=key, eye=eye, phase=phase, accepted=valid)
                if valid != old['gates'][gate]:
                    changes.append(dict(**item, geometry=geometry))
                if not valid:
                    failures.append(item)
        accepted += int(frame_ok)
    source_files = ['cpu-snapshots.bin', 'stereo-sequence-summary.json', 'retained-evidence.json',
        'engine.log', 'trace.log', 'eye-sequence.request']
    source_files += [name for name in ('openxr-session.log', 'recorded-views.request') if (run/name).exists()]
    return dict(run=run.name, originalReportAccepted=report['accepted'],
        frames=len(pairs), cameraComparisons=comparisons, cameraAcceptedFrames=accepted,
        cameraGeometryAccepted=accepted == len(pairs), frozenInputsAccepted=bool(frozen),
        retainedNonGeometryGatesPassed=bool(retained),
        combinedEvidenceAccepted=bool(accepted == len(pairs) and retained and frozen),
        originCap=.0001, maxima=maxima, changedComparisons=changes, failures=failures,
        sourceSha256={name: hashlib.sha256((run/name).read_bytes()).hexdigest() for name in source_files},
        analyzerSha256={name: hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest()
            for name in ('analyze_eye_offset.py', 'dynamic_views.py', 'audit_archived_camera_geometry.py')},
        limits='CPU geometry and frozen inputs rechecked from the unchanged archive. Other checks use historical receipts; no GPU replay, new game run or headset acceptance. Original reports are unchanged.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.run.resolve()):
        parser.error('Write a separate audit outside the original run')
    result = audit(args.run)
    with args.output.open('x') as output:
        output.write(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps({k: result[k] for k in ('run', 'frames', 'cameraComparisons', 'cameraAcceptedFrames', 'combinedEvidenceAccepted', 'maxima')}, indent=2))
    raise SystemExit(0 if result['combinedEvidenceAccepted'] else 1)
