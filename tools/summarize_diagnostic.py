"""Preserve analysis before disposable GPU readbacks are removed."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import json
from pathlib import Path
from unpack_snapshots import unpack


def summarize(run):
    if (run / "capture-cleanup.json").exists():
        raise ValueError("Captures were pruned or cleanup was interrupted; use retained-evidence.json.")
    session = json.loads((run / "session.json").read_text(encoding="utf-8-sig"))
    if session.get('runtimeSession'):
        from analyze_runtime import analyze
        result = analyze(run)
        path = run/'runtime-summary.json'
        path.write_text(json.dumps(result, indent=2)+'\n')
        retained = dict(schema=1, run=str(run.resolve()), recordedUtc=datetime.now(timezone.utc).isoformat(),
                        session=session, reports={path.name: dict(origin='native-runtime-counters',
                        sha256=hashlib.sha256(path.read_bytes()).hexdigest(), result=result)}, limits=result['limits'])
        if (run/'flicker-draws.jsonl').exists():
            from analyze_flicker import analyze as analyze_flicker
            flicker = analyze_flicker(run)
            flicker_path = run/'flicker-summary.json'
            flicker_path.write_text(json.dumps(flicker, indent=2)+'\n')
            retained['reports'][flicker_path.name] = dict(origin='passive-flicker-burst',
                sha256=hashlib.sha256(flicker_path.read_bytes()).hexdigest(), result=flicker)
            from analyze_flicker_inputs import analyze as analyze_inputs
            omitted = dict(status='unavailable', reason='Visibility-only capture omits per-draw payloads.',
                           limits='No draw membership, object, visibility or texture equality conclusion.')
            inputs = omitted if flicker['captureScope'] == 'visibility' else analyze_inputs(run)
            inputs_path = run/'flicker-inputs-summary.json'
            inputs_path.write_text(json.dumps(inputs, indent=2)+'\n')
            retained['reports'][inputs_path.name] = dict(origin='passive-flicker-inputs',
                sha256=hashlib.sha256(inputs_path.read_bytes()).hexdigest(), result=inputs)
            # Keep bounded diagnostic gaps explicit; incomplete captures must
            # still be summarizable and recoverable without retaining raw dumps.
            additional = [
                    ('flicker-mips-summary.json', 'analyze_flicker_mips', 'passive-texture-mips'),
                    ('flicker-visibility-summary.json', 'analyze_flicker_visibility', 'passive-colour-writes'),
                    ('flicker-instances-summary.json', 'analyze_flicker_instances', 'passive-instance-inputs')]
            if (run/'flicker-watch.jsonl').exists():
                additional.append(('flicker-watch-summary.json', 'analyze_flicker_watch', 'passive-moving-draw-samples'))
            for name, module, origin in additional:
                try:
                    result = (omitted if flicker['captureScope'] == 'visibility' and module != 'analyze_flicker_watch'
                              else importlib.import_module(module).analyze(run))
                except (ValueError, OSError) as exc:
                    result = dict(status='unavailable', reason=str(exc), limits='No visibility or texture equality conclusion.')
                report_path = run / name
                report_path.write_text(json.dumps(result, indent=2)+'\n')
                retained['reports'][name] = dict(origin=origin,
                    sha256=hashlib.sha256(report_path.read_bytes()).hexdigest(), result=result)
        (run/'retained-evidence.json').write_text(json.dumps(retained, indent=2)+'\n')
        return retained
    unpack(run)
    reports = {}
    legacy = not (run / "stereo-eyes.request").exists()
    specifications = (
        ("openxr-session-summary.json", "analyze_xr_session", (run / "xr-session.request").exists()),
        ("camera-binding-evidence.json", "analyze_camera_capture", (run / "trace.log").exists()),
        ("engine-summary.json", "analyze_engine_trace", (run / "engine.log").exists() and
         any((run / name).exists() for name in ("engine-vtable-frames.csv", "engine-frames.csv"))),
        ("scene-boundary-summary.json", "analyze_scene_boundary", (run / "engine.log").exists()),
        ("replay-summary.json", "analyze_replay", legacy and session.get("replayOnce") and (run / "engine.log").exists()),
        ("replay-work-summary.json", "analyze_replay_work", legacy and (run / "replay-work.txt").exists()),
        ("eye-offset-summary.json", "analyze_eye_offset", legacy and (run / "eye-offset.request").exists() and (run / "engine.log").exists()),
        ("eye-sequence-summary.json", "analyze_eye_sequence", legacy and (run / "eye-sequence.request").exists() and (run / "engine.log").exists()),
        ("stereo-sequence-summary.json", "analyze_stereo_sequence", not legacy and (run / "engine.log").exists()),
        ("visibility-summary.json", "analyze_visibility", (run / "expand-visibility.request").exists() and (run / "engine.log").exists()),
        ("producer-input-summary.json", "analyze_producer_inputs", (run / "producer-inputs.jsonl").exists()),
        ("msaa-summary.json", "analyze_msaa", (run / "trace.log").exists() and
         "msaa_readback name=" in (run / "trace.log").read_text()),
        ("ao-repeat-summary.json", "analyze_ao_repeat", (run / "repeat-ao.request").exists()),
        ("shadow-repeat-summary.json", "analyze_shadow_repeat", (run / "repeat-shadow.request").exists()),
    )
    for name, module, applicable in specifications:
        path = run / name
        if path.exists():
            result = json.loads(path.read_text(encoding="utf-8-sig"))
            origin = "existing-summary"
        elif applicable:
            result = importlib.import_module(module).analyze(run)
            path.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")
            origin = "computed-before-cleanup"
        else:
            continue
        reports[name] = dict(origin=origin, sha256=hashlib.sha256(path.read_bytes()).hexdigest(), result=result)
    retained = dict(schema=1, run=str(run.resolve()), recordedUtc=datetime.now(timezone.utc).isoformat(),
                    session=session, reports=reports,
                    limits="Historical analysis, not a new validation. Raw comparisons cannot be rerun after cleanup; hashes do not replace source data. Missing or failed replay evidence is not acceptance.")
    (run / "retained-evidence.json").write_text(json.dumps(retained, indent=2, allow_nan=False) + "\n")
    return retained


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    args = parser.parse_args()
    result = summarize(args.run)
    print(f"Preserved {len(result['reports'])} analysis reports: {args.run / 'retained-evidence.json'}")
