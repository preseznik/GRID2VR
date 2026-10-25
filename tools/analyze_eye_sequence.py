"""Validate a bounded native original/offset sequence; not headset acceptance."""
import json
from pathlib import Path
from analyze_engine_trace import fields
from analyze_eye_offset import analyze as analyze_eye, camera_geometry, geometry_valid, eye_history, requested_yaw, projection_uploads_valid, requested_pose, pose_log_valid

def analyze(run):
    if (run/'capture-cleanup.json').exists():raise ValueError('Read retained-evidence.json after cleanup.')
    count=int((run/'eye-sequence.request').read_text())
    yaw=requested_yaw(run)
    pose=requested_pose(run)
    asymmetric=(run/'eye-asymmetric-fov.request').exists()
    lines=(run/'engine.log').read_text().splitlines()
    starts=[fields(l) for l in lines if 'replay begin ' in l]
    ends=[fields(l) for l in lines if 'replay end ' in l]
    pairs=[fields(l) for l in lines if 'sequence pair ' in l]
    result=dict(run=run.name,accepted=False,requested=count,completed=len(pairs),
        limits='Bounded same-frame geometry pairs with sampled CPU state and GPU ownership/restoration. Not calibrated eyes, tracked pose, complete simulation isolation, all temporal resources or headset rendering.')
    g=result['gates']={}
    g['completeCount']=2<=count<=120 and len(starts)==len(ends)==len(pairs)==count
    g['noRefusals']=not any('replay refused ' in l or 'sequence refused ' in l for l in lines)
    g['consecutiveFrames']=bool(starts and [int(r['frame']) for r in starts]==list(range(int(starts[0]['frame']),int(starts[0]['frame'])+count)) and len({r['tid'] for r in starts})==1)
    result['firstPair']=analyze_eye(run,starts[0]['id']) if starts else {}
    g['firstPairEvidence']=result['firstPair'].get('accepted') is True
    graphics=(run/'trace.log').read_text().splitlines()
    work=(run/'replay-work.txt').read_text().splitlines()
    results=[]
    previous_ao_digest=None
    previous_rendered=None
    for index,start in enumerate(starts):
        key=start['id'];checks={}
        def rows(source,text):return [fields(l) for l in source if text in l and fields(l).get('id',fields(l).get('scene'))==key]
        end=rows(lines,'replay end ');pair=rows(lines,'sequence pair ')
        checks['completion']=len(end)==len(pair)==1 and pair[0].get('accepted')=='1' and pair[0].get('index')==str(index+1) and pair[0].get('target')==str(count) and all(end[0].get(f)==start.get(f) for f in ('tid','frame'))
        def read(phase,field,size):
            p=run/f'stage-{key}-{phase}-{field}.bin';b=p.read_bytes() if p.exists() else b''
            return b if len(b)==size else b''
        context=read('scene-complete','context',512);shifted=read('replay-complete','context',512)
        geometry=camera_geometry(context,shifted,read('scene-complete','view',144),read('replay-entry','view',144),.064,yaw,asymmetric,pose)
        checks['geometry']=geometry_valid(geometry)
        if pose is not None:checks['fullPoseRequest']=pose_log_valid(rows(lines,'eye fullpose '),pose)
        if yaw:
            poses=rows(lines,'eye pose ')
            checks['yawRequest']=len(poses)==1 and abs(float(poses[0].get('yaw_radians','inf'))-yaw)<1e-8
        if asymmetric:
            projections=rows(lines,'eye projection ')
            checks['projectionUploads']=projection_uploads_valid(projections,start.get('frame'))
        if index and (run/'eye-camera-history.request').exists():
            source=read('eye-previous-rendered','context',512);prepared=read('replay-entry','manager',512)
            center=read('eye-previous','context',512);entry=read('scene-entry','manager',512)
            history_rows=rows(lines,'eye history ')
            history=dict(mode='owned_previous',sourceMatchesPreviousRender=bool(source and source==previous_rendered),
                exactSeed=bool(source and prepared and source[0x190:0x1d0]==prepared[0x50:0x90]),
                nativeHistoryExact=bool(center and entry and center[0x190:0x1d0]==entry[0x10:0x50]))
            checks['historySeed']=all(v for k,v in history.items() if k!='mode') and len(history_rows)==1 and history_rows[0].get('mode')=='owned_previous' and history_rows[0].get('saved')=='1' and int(history_rows[0].get('previous_frame',-2))+1==int(start['frame'])
        else:
            history=eye_history(read('eye-previous','context',512),read('eye-previous','view',144),read('scene-entry','manager',512),read('replay-entry','manager',512),.064,yaw,asymmetric,pose)
            checks['historySeed']=bool(history.get('complete') and history['sourceMatchesNativeHistory'] and history['translated'] and history['matrixError']<.000001)
        previous_rendered=shifted
        checks['restoration']=all(read('scene-complete',f,n) and read('scene-complete',f,n)==read('replay-restored',f,n) for f,n in (('context',512),('manager',512),('object',352),('render-data',512),('packet',296)))
        checks['inputsPreserved']=all(read('scene-complete',f,n) and read('scene-complete',f,n)==read('replay-complete',f,n) for f,n in (('object',352),('render-data',512),('packet',296)))
        copies=rows(graphics,'eye texture ');ready=rows(graphics,'eye pair ')
        checks['ownedImages']=len(copies)==2 and [r.get('eye') for r in copies]==['0','1'] and all(r.get('captured')=='1' for r in copies) and len(ready)==1 and ready[0].get('ready')=='1'
        gpu=rows(work,'pair_gpu ');ao=rows(work,'ao_eye ')
        checks['gpuRestoration']=len(gpu)==1 and gpu[0].get('healthy')=='1' and gpu[0].get('dispatch0')==gpu[0].get('dispatch1')=='4'
        checks['ownAoHistory']=len(ao)==2 and sum(r.get('prepared')=='1' and r.get('previous_eye')==('0' if index==0 else '1') for r in ao)==1 and sum(r.get('retained')=='1' for r in ao)==1
        audit=rows(work,'ao_verify ')
        if (run/'eye-ao-audit.request').exists() and (index<2 or index>=count-2):
            phases={r.get('phase'):r for r in audit}
            checks['aoGpuHashes']=bool(len(audit)==3 and set(phases)=={'seed','retain','restore'} and
                all(len(r.get('source',''))==64 and r.get('source')==r.get('actual') for r in audit))
            if index in (1,count-1):
                seed=phases.get('seed',{})
                checks['aoGpuHandoff']=bool(previous_ao_digest and seed.get('source')==previous_ao_digest and seed.get('prior_sampled')==seed.get('prior_matched')=='1')
            previous_ao_digest=phases.get('retain',{}).get('actual')
            if index==count-1:
                hashes=[r.get('source_sha256','') for r in copies]
                checks['finalColorHashes']=bool(len(hashes)==2 and all(len(h)==64 for h in hashes) and hashes[0]!=hashes[1] and
                    len(ready)==1 and ready[0].get('left')==hashes[0] and ready[0].get('right')==hashes[1])
        else: previous_ao_digest=None
        results.append(dict(id=key,frame=start['frame'],accepted=all(checks.values()),gates=checks,geometry=geometry,history=history,aoAudit=audit))
    raw_scenes={p.name.split('-')[1] for p in run.glob('stage-*-replay-complete.raw')}
    expected_raw={starts[0]['id']} if starts else set()
    if starts and len(starts)==count and (run/'eye-ao-audit.request').exists():expected_raw.add(starts[-1]['id'])
    g['boundedRawCapture']=raw_scenes==expected_raw
    result['pairs']=results;g['everyPair']=bool(results) and all(r['accepted'] for r in results)
    if (run/'expand-visibility.request').exists():
        from analyze_visibility import analyze as analyze_visibility
        result['visibility']=analyze_visibility(run)
        g['visibility']=result['visibility']['accepted']
    result['accepted']=all(g.values());result['failedGates']=[k for k,v in g.items() if not v]
    return result

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);args=parser.parse_args()
    result=analyze(args.run);(args.run/'eye-sequence-summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
