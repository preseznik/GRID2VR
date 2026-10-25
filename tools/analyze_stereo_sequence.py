"""Verify center/left/right geometry renders, restoration and independent history."""
import hashlib
import json
import re
import struct
from pathlib import Path
import numpy as np
from analyze_engine_trace import fields
from analyze_eye_offset import (requested_pose, pose_rotation, pose_log_valid, camera_geometry,
    geometry_valid, eye_history, projection_uploads_valid)
from analyze_replay_work import raw_pair
from analyze_visibility import analyze as analyze_visibility
from dynamic_views import dynamic_inputs, frozen_frame_valid


FRUSTA=((-1.,.8,-.7,.6),(-.8,1.,-.7,.6))


def clip_path_valid(main,complete):
    if len(main)!=512 or len(complete)!=512:return False
    if main==complete:return True
    near,far=struct.unpack_from('<2I',main,0xe8)
    final_near,final_far=struct.unpack_from('<2I',complete,0xe8)
    if near not in (0x3dcccccd,0x3e4ccccd) or far!=0x466a6000 or final_near!=0x3e4ccccd or final_far!=0x42c80000:return False
    offsets=(0xe8,0xec,0x138,0x148,0x198,0x1a8,0x1b8,0x1c8)
    owned={i for o in offsets for i in range(o,o+4)}
    return all(main[i]==complete[i] for i in range(512) if i not in owned) and all(np.isfinite(struct.unpack_from('<f',data,o)[0]) for data in (main,complete) for o in offsets)


def stereo_poses(center):
    right=pose_rotation(center)[0]
    result=[]
    for sign in (-1,1):
        pose=center.copy()
        pose[4:]=[float(center[4+i])+sign*.5*float(np.float32(.064))*right[i] for i in range(3)]
        pose_rotation(pose)
        result.append(pose)
    return result


def sha(value):
    return bool(re.fullmatch('[0-9a-f]{64}',value or ''))


def gpu_camera_uploads(run,key,render_pass,camera):
    if len(camera)!=512:return []
    w=np.frombuffer(camera,'<f4',16,0x70).reshape(4,4)
    v=np.frombuffer(camera,'<f4',16,0x150).reshape(4,4)
    matches=[]
    for path in sorted(run.glob(f'work-{key}-pass{render_pass}-slot6-draw*-v3.buffer')):
        data=path.read_bytes()
        if len(data)>=236 and camera[0x110:0x150]==data[:64] and camera[0x190:0x1d0]==data[64:128] and v.T[:3].tobytes()==data[128:176] and w.T[:3].tobytes()==data[176:224] and camera[0xa0:0xac]==data[224:236]:
            matches.append(dict(file=path.name,sha256=hashlib.sha256(data).hexdigest()))
    return matches


def fresh_seeds(run,key,graphics):
    rows=[fields(l) for l in graphics if 'fresh_color name=' in l]
    valid=len(rows)==3 and [r.get('pass') for r in rows]==['0','1','2']
    if (run/'fresh-color-identity.request').exists():
        valid&=all(r.get('identity')=='1' and all(re.fullmatch(r'[0-9a-f]{16}',r.get(k,'')) and int(r[k],16) for k in ('ps','vs')) for r in rows) and len({(r.get('ps'),r.get('vs')) for r in rows})==1
    layouts=[]
    for render_pass,row in enumerate(rows):
        name=row.get('name','')
        if render_pass>2 or not re.fullmatch(rf'work-{key}-pass{render_pass}-slot6-draw\d+',name):
            valid=False;continue
        valid&=row.get('applied')==row.get('saved')=='1' and row.get('seed')==('magenta','green','blue')[render_pass]
        path=run/(name+'-fresh-seed.raw');data=path.read_bytes() if path.exists() else b''
        if len(data)<16 or len(data)%4:valid=False;continue
        width,height,format,samples=struct.unpack_from('<4I',data)
        layouts.append((width,height,format,samples,name.split('-slot6-')[-1]))
        valid&=bool(width and height and width*height<=3840*2160 and format==24 and samples==1 and len(data)==16+width*height*4 and
            np.all(np.frombuffer(data,'<u4',offset=16)==(0xfff003ff,0xc00ffc00,0xfff00000)[render_pass]))
    return dict(accepted=bool(valid and len(layouts)==3 and len(set(layouts))==1),rows=rows)


def analyze(run):
    if (run/'capture-cleanup.json').exists():raise ValueError('Use retained-evidence.json after cleanup.')
    count=int((run/'eye-sequence.request').read_text())
    center=requested_pose(run)
    if center is None:raise ValueError('Stereo requires a full pose request.')
    poses=stereo_poses(center)
    lines=(run/'engine.log').read_text().splitlines()
    try:inputs=dynamic_inputs(run,lines)
    except ValueError as error:
        return dict(run=run.name,accepted=False,requested=count,gates={'dynamicInputs':False},
                    failedGates=['dynamicInputs'],inputError=str(error),frames=[],
                    limits='Invalid or incomplete eye input; no stereo acceptance. Other frame gates were not evaluated.')
    graphics=(run/'trace.log').read_text().splitlines()
    work=(run/'replay-work.txt').read_text().splitlines()
    event_cache={}
    def events(source,text):
        cache_key=(id(source),text)
        if cache_key not in event_cache:
            event_cache[cache_key]=[(i,fields(l)) for i,l in enumerate(source) if text in l]
        return event_cache[cache_key]
    def group_frames(source):
        grouped={}
        for line in source:grouped.setdefault(fields(line).get('frame'),[]).append(line)
        return grouped
    engine_frames=group_frames(lines);graphics_frames=group_frames(graphics)
    pairs=events(lines,'sequence pair ')
    result=dict(run=run.name,accepted=False,requested=count,completed=len(pairs),
        limits='Fixed native-unit stereo geometry, not calibrated runtime tracking, all engine/temporal state, sustained performance or headset acceptance.')
    gates=result['gates']={}
    gates['requests']=all((run/name).exists() for name in ('stereo-eyes.request','eye-camera-history.request','eye-asymmetric-fov.request','expand-visibility.request','eye-textures.request','fresh-replay-color.request'))
    limit=900 if all((run/name).exists() for name in ('compact-stereo.request','packed-snapshots.request')) else 120
    gates['count']=2<=count<=limit and len(pairs)==count
    if inputs is not None:
        gates['dynamicInputCount']=len(inputs)==count
        result['limits']='Per-frame native-unit stereo geometry; world scale, all engine state, sustained performance and headset appearance remain unverified.'
    gates['consecutiveFrames']=bool(pairs and [int(r['frame']) for _,r in pairs]==list(range(int(pairs[0][1]['frame']),int(pairs[0][1]['frame'])+count)) and len({r['tid'] for _,r in pairs})==1)
    gates['noRefusals']=not any('replay refused ' in l or 'sequence refused ' in l for l in lines) and not any('refused=' in l for l in work)
    if (run/'compact-stereo.request').exists():
        modes=[fields(line) for line in work if line.startswith('capture_mode ')]
        gates['compactCaptureMode']=len(modes)==1 and modes[0].get('compact')=='1'
        captures=[p for p in run.glob('work-*') if p.suffix in ('.raw','.ppm','.buffer','.surface','.texture')]
        gates['compactCaptureBound']=all(re.fullmatch(r'work-\d+-pass[012]-slot6-draw\d+-(v3\.buffer|fresh-seed\.(raw|ppm))',p.name) for p in captures)
        result['limits']+=' Compact capture omits full per-draw meshes, textures and producer readbacks; the stereo acceptance gates are unchanged.'
    previous_cameras=[None,None];previous_ao=[None,None];owned_ao=[None,None]
    results=[]
    for index,(pair_position,pair) in enumerate(pairs):
        key=pair['id'];frame=pair['frame'];tid=pair['tid']
        if inputs is not None and index<len(inputs):poses=inputs[index,:,:7];frusta=inputs[index,:,7:]
        else:frusta=FRUSTA
        def rows(source,text,render_pass=None):
            return [r for _,r in events(source,text) if r.get('id',r.get('scene'))==key and (render_pass is None or r.get('pass')==str(render_pass))]
        def read(phase,field,render_pass=0):
            if render_pass==2 and (phase.startswith('replay-') or phase.startswith('eye-previous')):phase='right-'+phase
            path=run/f'stage-{key}-{phase}-{field}.bin'
            return path.read_bytes() if path.exists() else b''
        source=read('scene-complete','context');main_source=read('scene-entry','context');original_view=read('scene-complete','view')
        checks=dict(completion=pair.get('accepted')==pair.get('gpu_restored')=='1' and pair.get('index')==str(index+1) and pair.get('target')==str(count))
        if inputs is not None:checks['frozenViews']=frozen_frame_valid(engine_frames.get(frame,[]),graphics_frames.get(frame,[]),frame,tid,key,index,poses,frusta)
        starts=rows(lines,'replay begin ');ends=rows(lines,'replay end ');eyes=rows(lines,'stereo eye ')
        checks['orderedEyes']=len(starts)==len(ends)==len(eyes)==2 and all([r.get('pass') for r in records]==['1','2'] for records in (starts,ends,eyes)) and [r.get('eye') for r in eyes]==['0','1']
        checks['sameFrameThread']=all(r.get('frame')==frame and r.get('tid')==tid for r in starts+ends+eyes) and len(starts)==2
        copies=rows(graphics,'eye texture ');ready=rows(graphics,'eye pair ')
        checks['ownedImages']=len(copies)==3 and [r.get('eye') for r in copies]==['0','1','2'] and all(r.get('captured')=='1' and r.get('tid')==tid for r in copies) and len(ready)==1 and ready[0].get('ready')=='1'
        if (run/'disable-motion-blur.request').exists():
            blur=rows(work,'motion_blur ')
            checks['motionBlurDisabled']=len(blur)==6 and [r.get('pass') for r in blur]==['0','0','1','1','2','2'] and all(
                (row.get('applied')=='1' and row.get('shader') in ('6e44f31d1ebda08d','ed4ddaac90aff677','b02f3b98b3596660','cfc3cf441f08e879')) if i%2==0 else row.get('restored')=='1'
                for i,row in enumerate(blur))
        sampled=index in (0,count-1)
        if sampled:
            hashes=[r.get('source_sha256','') for r in copies]
            checks['imageHashes']=len(hashes)==3 and all(map(sha,hashes)) and len(set(hashes))==3 and len(ready)==1 and ready[0].get('left')==hashes[1] and ready[0].get('right')==hashes[2]
        details=[]
        ordered=[]
        for eye,render_pass in enumerate((1,2)):
            shifted=read('replay-complete','context',render_pass);posed_view=read('replay-entry','view',render_pass)
            main=read('replay-main','context',render_pass)
            clip=rows(lines,'eye clip ',render_pass)
            # Old unchanged-clip captures predate the separate main-camera
            # snapshot. New receipts must provide it, including unchanged frames.
            clip_aware=bool(main or clip)
            if not clip_aware and main_source==source:main=shifted
            geometry=camera_geometry(source,shifted,original_view,posed_view,0,asymmetric=True,pose=poses[eye],frustum=frusta[eye])
            e=dict(geometry=geometry_valid(geometry),pose=pose_log_valid(rows(lines,'eye fullpose ',render_pass),poses[eye]),
                projectionUploads=projection_uploads_valid(rows(lines,'eye projection ',render_pass),frame))
            main_geometry=camera_geometry(main_source,main,original_view,posed_view,0,asymmetric=True,pose=poses[eye],frustum=frusta[eye])
            e['mainCamera']=geometry_valid(main_geometry)
            e['clipSequence']=bool(clip_path_valid(main_source,source) and clip_path_valid(main,shifted) and
                (not clip_aware or (len(clip)==1 and clip[0].get('main_history')==clip[0].get('sequence_same')=='1')))
            end=rows(lines,'replay end ',render_pass);begin=rows(lines,'replay begin ',render_pass);status=rows(lines,'stereo eye ',render_pass);restored=rows(lines,'eye restored ',render_pass)
            e['nativeRestoration']=bool(len(end)==len(begin)==len(status)==len(restored)==1 and status[0].get('accepted')==status[0].get('gpu_restored')==restored[0].get('camera_same')=='1' and
                all(end[0].get(k)=='1' for k in ('same_frame','image_saved','packet_same','history_other_same','history_restored','near_same')) and
                end[0].get('counter_before')==end[0].get('counter_after')==begin[0].get('counter') and end[0].get('history_counter_after')==begin[0].get('history_counter'))
            e['restoredBytes']=all(len(read('scene-complete',field))==size and read('scene-complete',field)==read('replay-restored',field,render_pass) for field,size in (('context',512),('manager',512),('object',352),('render-data',512),('packet',296)))
            e['sharedInputs']=all(read('scene-complete',field)==read('replay-complete',field,render_pass) and bool(read('scene-complete',field)) for field in ('object','render-data','packet'))
            manager=read('replay-complete','manager',render_pass);prepared=read('replay-entry','manager',render_pass)
            entry=read('scene-entry','manager');prior=read('eye-previous','context',render_pass)
            previous=read('eye-previous-rendered','context',render_pass)
            e['currentEyeHistory']=len(manager)==len(main)==len(main_source)==512 and manager[0x50:0x90]==main[0x190:0x1d0] and manager[0x10:0x50]==main_source[0x190:0x1d0]
            history_rows=rows(lines,'eye history ',render_pass)
            if index:
                history=dict(mode='owned_previous',sourceMatchesPreviousRender=bool(previous and previous==previous_cameras[eye]),exactSeed=bool(previous and len(prepared)==512 and previous[0x190:0x1d0]==prepared[0x50:0x90]),nativeHistoryExact=bool(len(prior)==len(entry)==512 and prior[0x190:0x1d0]==entry[0x10:0x50]))
                history_ok=all(v for k,v in history.items() if k!='mode')
            else:
                history=eye_history(prior,read('eye-previous','view',render_pass),entry,prepared,0,asymmetric=True,pose=poses[eye],frustum=frusta[eye])
                history_ok=history.get('complete') and history['sourceMatchesNativeHistory'] and history['matrixError']<1e-6
            e['history']=bool(history_ok and len(history_rows)==1 and history_rows[0].get('saved')=='1' and int(history_rows[0].get('previous_frame',-1))+1==int(frame) and history_rows[0].get('mode')==('owned_previous' if index else 'posed_previous'))
            previous_cameras[eye]=main
            gpu=rows(work,'pair_gpu ',render_pass);ao=rows(work,'ao_eye ',render_pass)
            e['gpuRestoration']=len(gpu)==1 and gpu[0].get('healthy')=='1' and gpu[0].get('dispatch0')==gpu[0].get('dispatch1')=='4'
            seeds=[r for r in ao if r.get('prepared')=='1'];retains=[r for r in ao if r.get('retained')=='1']
            e['aoOwnership']=len(ao)==2 and len(seeds)==len(retains)==1 and seeds[0].get('previous_eye')==('1' if index else '0') and bool(retains[0].get('resource')) and retains[0]['resource']!='00000000'
            if index:e['aoOwnership']&=len(seeds)==len(retains)==1 and seeds[0].get('history_resource')==retains[0].get('resource')==owned_ao[eye]
            owned_ao[eye]=retains[0].get('resource') if len(retains)==1 else None
            audit=rows(work,'ao_verify ',render_pass)
            if index<2 or index>=count-2:
                phases={r.get('phase'):r for r in audit}
                e['aoGpuHashes']=len(audit)==3 and set(phases)=={'seed','retain','restore'} and all(sha(r.get('source')) and r.get('source')==r.get('actual') for r in audit)
                if index in (1,count-1):
                    seed=phases.get('seed',{})
                    e['aoGpuHandoff']=bool(previous_ao[eye] and seed.get('source')==previous_ao[eye] and seed.get('prior_sampled')==seed.get('prior_matched')=='1')
                previous_ao[eye]=phases.get('retain',{}).get('actual')
            else:previous_ao[eye]=None
            compute=rows(work,'compute_state ',render_pass)
            e['computeBindings']=len(compute)==2 and sum(r.get('prepared')=='1' for r in compute)==1 and sum(r.get('restored')=='1' for r in compute)==1
            shadow=rows(work,'shadow_sampler ',render_pass)
            e['shadowBindings']=len(shadow)==2 and sum(r.get('applied')=='1' for r in shadow)==1 and sum(r.get('restored')=='1' for r in shadow)==1
            if (run/'shadow-pass-identity.request').exists():
                originals=[r for _,r in events(work,'shadow_sampler ') if r.get('scene')==key and r.get('captured')=='1']
                applied=[r for r in shadow if r.get('applied')=='1']
                e['shadowPassIdentity']=bool(len(originals)==len(applied)==1 and originals[0].get('identity')==applied[0].get('identity')=='1' and
                    originals[0].get('ordinal')==applied[0].get('source_ordinal'))
            if index==0:
                uploads=gpu_camera_uploads(run,key,render_pass,main);e['gpuCamera']=bool(uploads)
                nodes=read('replay-complete','nodes',render_pass);e['packetNodes']=bool(nodes) and nodes==read('scene-complete','nodes')
            else:uploads=[]
            positions=[i for i,r in events(lines,'replay begin ') if r.get('id')==key and r.get('pass')==str(render_pass)]+[i for i,r in events(lines,'replay end ') if r.get('id')==key and r.get('pass')==str(render_pass)]
            ordered.extend(positions)
            details.append(dict(eye=eye,accepted=all(e.values()),gates=e,geometry=geometry,mainGeometry=main_geometry,history=history,aoAudit=audit,gpuCameraUploads=uploads))
        checks['independentAoResources']=bool(owned_ao[0] and owned_ao[1] and owned_ao[0]!=owned_ao[1])
        checks['renderOrder']=len(ordered)==4 and ordered==sorted(set(ordered)) and ordered[-1]<pair_position
        for text,field,n in (('vpl_order ','restored',3),('shadow_unused ','applied',3),('shadow_unused ','restored',3)):
            records=[r for r in rows(work,text) if r.get(field)=='1']
            checks[text.strip()+field]=len(records)==n and [r.get('pass') for r in records]==['0','1','2']
        checks['bothEyes']=all(d['accepted'] for d in details)
        if sampled:
            images=raw_pair(run/f'stage-{key}-replay-complete.raw',run/f'stage-{key}-right-replay-complete.raw')
            checks['distinctEyeImages']=bool(images.get('valid') and images.get('equal') is False and images.get('changedPixelFraction',0)>.001)
        else:images={}
        if index==0:
            checks['centerGpuCamera']=bool(gpu_camera_uploads(run,key,0,main_source))
            result['freshColor']=fresh_seeds(run,key,graphics);checks['freshColor']=result['freshColor']['accepted']
        results.append(dict(id=key,frame=frame,accepted=all(checks.values()),gates=checks,eyes=details,images=images))
    result['frames']=results;gates['everyFrame']=bool(results) and all(r['accepted'] for r in results)
    if (run/'wait-clip-transition.request').exists():
        triggers=events(lines,'capture trigger observed=clip_transition ')
        gates['requestedClipTransitionObserved']=bool(len(triggers)==1 and pairs and
            triggers[0][0]<pairs[0][0] and int(triggers[0][1]['frame'])+1==int(pairs[0][1]['frame']) and
            triggers[0][1].get('tid')==pairs[0][1].get('tid'))
        changed=[]
        for index,item in enumerate(results):
            clips=[r for _,r in events(lines,'replay clip ') if r.get('id')==item['id']]
            cameras=[]
            for prefix in ('','right-'):
                paths=[run/f"stage-{item['id']}-{prefix}replay-{phase}-context.bin" for phase in ('main','complete')]
                cameras.append([p.read_bytes() if p.exists() else b'' for p in paths])
            if (len(clips)==2 and [r.get('pass') for r in clips]==['1','2'] and
                all(r.get('clip_changed')=='1' and r.get('frame')==item['frame'] for r in clips) and
                all(len(main)==len(final)==512 and main!=final for main,final in cameras)):
                changed.append(index)
        # The next accepted group checks that each eye retained its main scene
        # history, rather than the shorter near-object projection.
        gates['requestedClipTransitionReplayed']=any(i+1<len(results) and results[i]['accepted'] and results[i+1]['accepted'] for i in changed)
    result['visibility']=analyze_visibility(run);gates['visibility']=result['visibility']['accepted']
    expected={f"stage-{pairs[i][1]['id']}-{prefix}replay-complete.raw" for i in ({0,len(pairs)-1} if pairs else set()) for prefix in ('','right-')}
    actual={p.name for p in run.glob('stage-*-*replay-complete.raw')}
    gates['boundedRawCapture']=actual==expected
    result['accepted']=all(gates.values());result['failedGates']=[k for k,v in gates.items() if not v]
    return result


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);args=parser.parse_args()
    result=analyze(args.run);(args.run/'stereo-sequence-summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
