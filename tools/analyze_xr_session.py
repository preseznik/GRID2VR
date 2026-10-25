"""Verify game-device OpenXR lifecycle and optional accepted-frame submission."""
import json
import math
from pathlib import Path
from analyze_engine_trace import fields


def analyze(run):
    projection=(run/'xr-projection.request').exists()
    log=run/'openxr-session.log'
    lines=log.read_text().splitlines() if log.exists() else []
    def rows(text):return [fields(line) for line in lines if text in line]
    operations=rows('xr operation=');samples=rows('sample tracked=');devices=rows('device game=')
    shutdown=rows('session shutdown=');selection=rows('runtime_selection ')
    request=run/'xr-session.request'
    manifest=request.read_text().strip().replace('\\','/').rsplit('/',1)[-1] if request.exists() else ''
    runtime=rows('runtime name=')
    runtime_matches=len(runtime)==1 and (
        (manifest=='steamxr_win32.json' and runtime[0].get('name','').startswith('SteamVR/')) or
        (manifest=='virtualdesktop-openxr-32.json' and runtime[0].get('name')=='VirtualDesktopXR'))
    trace=(run/'trace.log').read_text().splitlines() if (run/'trace.log').exists() else []
    attached={fields(line).get('device') for line in trace if 'device=' in line and 'feature_level=' in line}
    def operations_ok(name):return [r for r in operations if r.get('operation')==name]
    all_samples=samples;warmups=[]
    if projection:
        rendered={r.get('frame') for r in operations_ok('end_frame_projection')}
        samples=[s for s in all_samples if s.get('frame') in rendered]
        warmups=[s for s in all_samples if s.get('frame') not in rendered]
    required=('create_instance','runtime_properties','get_system','graphics_requirements','create_game_session',
              'create_local_space','create_head_space','begin_session','destroy_head_space','destroy_local_space','destroy_session','destroy_instance')
    gates=dict(request=(run/'xr-session.request').exists(),
        selectedRuntime=runtime_matches,
        processOnlyRuntime=len(selection)==1 and selection[0].get('process_only')==selection[0].get('environment_restored')=='1',
        sameGameDevice=len(devices)==1 and devices[0].get('compatible')=='1' and devices[0].get('luid')==devices[0].get('required') and devices[0].get('game') in attached,
        oneRenderThread=bool(lines) and len({fields(line).get('tid') for line in lines})==1,
        lifecycle=all(len(operations_ok(name))==1 for name in required) and all(r.get('result')=='0' for r in operations),
        trackedSamples=len(samples)>=2 and all(r.get('tracked')=='1' for r in samples),
        renderEligible=sum(r.get('should_render')=='1' for r in samples)>=2,
        shutdown=len(shutdown)==1 and shutdown[0].get('shutdown')=='1' and shutdown[0].get('failed')=='0' and shutdown[0].get('samples')==str(sum(r.get('tracked')=='1' for r in all_samples)) and shutdown[0].get('images_submitted')==str(len(samples) if projection else 0))
    poses=rows('pose kind=');fovs=rows('fov eye=');details=[]
    for sample in samples:
        frame=sample['frame'];current=[r for r in operations if r.get('frame')==frame]
        end_operation='end_frame_projection' if projection else 'end_frame_zero_layers'
        frame_ops=[r['operation'] for r in current if r['operation'] in ('wait_frame','begin_frame','locate_views','locate_head','end_frame_zero_layers','end_frame_projection')]
        p=[r for r in poses if r.get('frame')==frame];f=[r for r in fovs if r.get('frame')==frame]
        balanced=frame_ops==['wait_frame','begin_frame','locate_views','locate_head',end_operation]
        valid=balanced and len(p)==3 and [(r.get('kind'),r.get('eye')) for r in p]==[('head','0'),('eye','0'),('eye','1')] and len(f)==2 and [r.get('eye') for r in f]==['0','1']
        positions=[]
        for pose in p:
            q=[float(pose.get(k,'nan')) for k in ('qx','qy','qz','qw')];position=[float(pose.get(k,'nan')) for k in ('x','y','z')]
            valid&=all(map(math.isfinite,q+position)) and abs(sum(v*v for v in q)-1)<.001
            positions.append(position)
        tangents=[]
        for fov in f:
            a=[float(fov.get(k,'nan')) for k in ('left','right','down','up')]
            valid&=all(math.isfinite(v) and abs(v)<math.pi/2 for v in a) and a[0]<0<a[1] and a[2]<0<a[3]
            tangents.append([math.tan(v) for v in a] if all(map(math.isfinite,a)) else [])
        separation=math.dist(positions[1],positions[2]) if len(positions)==3 else None
        if separation is not None and not math.isfinite(separation):separation=None
        valid&=separation is not None and .04<separation<.09
        details.append(dict(frame=frame,accepted=bool(valid),frameLoop=balanced,separationMetres=separation,fovTangents=tangents,displayTime=sample['display_time']))
    times=[int(r['display_time']) for r in samples];frames=[int(r['frame']) for r in samples]
    gates['monotonicSamples']=len(times)>=2 and all(a<b for a,b in zip(times,times[1:])) and all(a+1==b for a,b in zip(frames,frames[1:]))
    gates['frameLoop']=bool(details) and all(r['frameLoop'] for r in details) and len(operations_ok('end_frame_projection' if projection else 'end_frame_zero_layers'))==len(samples)
    gates['poseData']=bool(details) and all(r['accepted'] for r in details)
    if projection:
        gates['warmupBeforeRendering']=len(operations_ok('end_frame_zero_layers'))==len(warmups) and all(
            samples and int(s['frame'])<int(samples[0]['frame']) and
            [r['operation'] for r in operations if r.get('frame')==s['frame'] and r['operation'] in ('wait_frame','begin_frame','locate_views','locate_head','end_frame_zero_layers','end_frame_projection')]==
            ['wait_frame','begin_frame','locate_views','locate_head','end_frame_zero_layers'] for s in warmups)
        images=rows('images initialized=');submissions=rows('images submitted=')
        pairs=[fields(l) for l in trace if 'openxr pair ' in l]
        engine=(run/'engine.log').read_text().splitlines() if (run/'engine.log').exists() else []
        completed=[fields(l) for l in engine if 'views complete ' in l]
        gates['eyeImagesInitialized']=len(images)==1 and images[0].get('initialized')=='1'
        gates['projectionCount']=len(submissions)==len(pairs)==len(completed)==len(samples) and all(r.get('submitted')=='1' for r in submissions)
        gates['acceptedRenderedFrames']=gates['projectionCount'] and all(
            s['frame']==p.get('frame')==c.get('frame')==sample['frame'] and s.get('display_time')==sample['display_time'] and
            s.get('tid')==p.get('tid')==c.get('tid') and p.get('scene')==c.get('id') and
            p.get('accepted')==p.get('ended')==c.get('accepted')==c.get('visibility_restored')=='1'
            for s,p,c,sample in zip(submissions,pairs,completed,samples))
    else:gates['noImageSubmission']=bool(shutdown) and all(r.get('images_submitted')=='0' for r in shutdown)
    observations=dict(sampleCount=len(samples),warmupCount=len(warmups),trackedSampleCount=sum(r.get('tracked')=='1' for r in samples),
        renderEligibleCount=sum(r.get('should_render')=='1' for r in samples),
        allSamplesRenderEligible=bool(samples) and all(r.get('should_render')=='1' for r in samples),
        predictedPeriodsNs=sorted({int(r['period']) for r in samples if 'period' in r}))
    return dict(run=run.name,accepted=all(gates.values()),gates=gates,failedGates=[k for k,v in gates.items() if not v],observations=observations,samples=details,
        limits=('Projection submission and frame association only; also require stereo-sequence-summary.json. Headset appearance, comfort and performance require user acceptance.' if projection else
            'Actual game-device session and pose sampling only. Zero layers submitted; tracked rendering, swapchains, headset images and performance remain unverified.'))


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);args=parser.parse_args()
    result=analyze(args.run);(args.run/'openxr-session-summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
