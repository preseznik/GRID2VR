"""Check retained CPU visibility bounds and their pre-job/restore ordering."""
import itertools
import json
from pathlib import Path
import numpy as np
from dynamic_views import dynamic_inputs
from analyze_engine_trace import fields


def box_geometry(original, expanded, view, matrix, posed_context, render_view=None, frusta=None):
    if tuple(map(len,(original,expanded,view,matrix,posed_context)))!=(224,224,144,64,512):
        return dict(accepted=False,complete=False)
    native=np.frombuffer(original,'<f4').reshape(14,4).astype(float)
    box=np.frombuffer(expanded,'<f4').reshape(14,4).astype(float)
    center=np.frombuffer(view,'<f4',3,0x40).astype(float)
    m=np.frombuffer(matrix,'<f4').reshape(4,4).astype(float)
    context=np.frombuffer(posed_context,'<f4').astype(float)
    # The context also contains integer flags/pointers, including 0xffffffff at
    # +0xd4. Only projection/view words are floats for this calculation.
    if not all(np.isfinite(a).all() for a in (native[:6],native[6:,:3],box,center,m,context[0x110//4:0x190//4])):
        return dict(accepted=False,complete=True,finite=False)
    radius=float(np.nextafter(np.float32(np.linalg.norm(native[6:,:3]-center,axis=1).max()+8),np.float32(np.inf)))
    source_far=float((native[:6,:3]@center+native[:6,3]).max())
    if frusta is not None:
        if render_view is None or len(render_view)!=144:
            return dict(accepted=False,complete=False)
        basis=np.array([np.frombuffer(render_view,'<f4',3,o) for o in (0x20,0x10,0x30)],dtype=float)
        origin=np.frombuffer(render_view,'<f4',3,0x40).astype(float)
        fovs=np.asarray(frusta,dtype='<f4').astype(float)
        if (fovs.shape not in ((1,4),(2,4)) or not all(np.isfinite(a).all() for a in (basis,origin,fovs)) or
            np.abs(basis@basis.T-np.eye(3)).sum(axis=1).max()>.001 or
            np.abs(fovs).max()>4 or np.any(fovs[:,(0,2)]>=0) or np.any(fovs[:,(1,3)]<=0) or source_far<=0):
            return dict(accepted=False,complete=True,eyeBoundsValid=False)
        ray_length=np.sqrt(1+np.max(np.abs(fovs[:,:2]),axis=1)**2+np.max(np.abs(fovs[:,2:]),axis=1)**2).max()
        eye_radius=np.linalg.norm(origin-center)+(source_far*ray_length+.5)*np.sqrt(1.001)+8
        if not np.isfinite(eye_radius) or eye_radius>25008:
            return dict(accepted=False,complete=True,eyeBoundsValid=False)
        radius=max(radius,float(np.nextafter(np.float32(eye_radius),np.float32(np.inf))))
    if not np.isfinite(radius) or not 25<radius<25009:
        return dict(accepted=False,complete=True,boundedRadius=False)
    expected=np.zeros((4,4));expected[3,3]=1
    for axis in range(3):
        expected[axis,axis]=np.float32((-1 if axis==2 else 1)/np.float32(radius))
        expected[3,axis]=np.float32(-np.float32(center[axis])*np.float32(expected[axis,axis]))
    corner_error=float(np.abs(np.abs(box[6:,:3]-center)-radius).max())
    corner_keys={tuple(np.sign(row-center)) for row in box[6:,:3]}
    plane_error=float(np.abs(np.linalg.norm(box[:6,:3],axis=1)-1).max())
    planes_at_center=box[:6,:3]@center+box[:6,3]
    plane_center_error=float(np.abs(planes_at_center-radius).max())
    native_margin=float((np.c_[native[6:,:3],np.ones(8)]@box[:6].T).min())
    try:
        with np.errstate(divide='raise',invalid='raise',over='raise'):
            inverse_p=np.linalg.inv(context[0x110//4:0x150//4].reshape(4,4))
            inverse_v=np.linalg.inv(context[0x150//4:0x190//4].reshape(4,4))
            rays=np.array([[x,y,0,1] for x,y in itertools.product((-1,1),repeat=2)])@inverse_p
            rays=rays[:,:3]/rays[:,3,None]
            points=rays*(source_far/-rays[:,2,None])
            world=np.c_[points,np.ones(4)]@inverse_v
            world=world[:,:3]/world[:,3,None]
            posed_margin=float((np.c_[world,np.ones(4)]@box[:6].T).min())
            if not np.isfinite(posed_margin):posed_margin=None
    except (ValueError,np.linalg.LinAlgError,FloatingPointError):
        posed_margin=None
    checks=dict(nativePlanes=bool(np.abs(np.linalg.norm(native[:6,:3],axis=1)-1).max()<.001),
        boundedRadius=25<radius<25009,matrixExact=bool(np.array_equal(m,expected)),
        distinctBoxCorners=corner_error<.02 and len(corner_keys)==8,
        distinctBoxPlanes=plane_error<1e-4 and plane_center_error<.02 and len({tuple(np.round(p,4)) for p in box[:6,:3]})==6,
        nativeCornersContained=native_margin>=0,
        posedFarCornersContained=bool(posed_margin is not None and np.isfinite(posed_margin) and posed_margin>=0))
    return dict(accepted=all(checks.values()),complete=True,gates=checks,radius=radius,cornerError=corner_error,
        planeCenterError=plane_center_error,nativeCornerMargin=native_margin,sourceFar=source_far,posedFarCornerMargin=posed_margin)


def analyze(run):
    lines=(run/'engine.log').read_text().splitlines()
    event_cache={}
    def events(text):
        if text not in event_cache:event_cache[text]=[(i,fields(line)) for i,line in enumerate(lines) if text in line]
        return event_cache[text]
    applied=[(i,r) for i,r in events('visibility before_jobs ') if r.get('applied')=='1']
    pairs=events('sequence pair ');restores=events('visibility restore ');snapshots=events('visibility snapshot ')
    try:inputs=dynamic_inputs(run,lines)
    except ValueError as error:
        return dict(run=run.name,accepted=False,gates={'dynamicInputs':False},
                    failedGates=['dynamicInputs'],inputError=str(error),
                    limits='Invalid or incomplete eye input; no visibility acceptance. Geometry was not evaluated.')
    requested=int((run/'eye-sequence.request').read_text())
    def read(key,phase,field):
        p=run/f'stage-{key}-{phase}-{field}.bin';return p.read_bytes() if p.exists() else b''
    def eye_camera(key,prefix=''):
        # Test the main scene frustum even if its near-object pass later uses
        # a shorter far plane. Older captures contain only the final camera.
        return read(key,prefix+'replay-main','context') or read(key,prefix+'replay-complete','context')
    results=[]
    for position,event in applied:
        key=event['id'];frame=event['frame'];tid=event['tid']
        matching=[(i,r) for i,r in pairs if r.get('frame')==frame]
        restored=[(i,r) for i,r in restores if r.get('id')==key]
        prepared=[(i,r) for i,r in snapshots if r.get('frame')==frame and r.get('phase')=='prepare-before']
        frusta=((-1.,.8,-.7,.6),(-.8,1.,-.7,.6)) if event.get('eye_bounds')=='1' else None
        if inputs is not None:
            indices=[i for i,(_,r) in enumerate(pairs) if r.get('frame')==frame]
            if len(indices)==1 and indices[0]<len(inputs):frusta=inputs[indices[0],:,7:]
        render_view=read(key,'before-jobs','render-view')
        geometry=box_geometry(read(key,'visibility-original','volume'),read(key,'visibility-expanded','volume'),
            read(key,'before-jobs','visibility-view'),read(key,'visibility-expanded','matrix'),
            eye_camera(matching[0][1]['id']) if len(matching)==1 else b'',render_view,frusta)
        checks=dict(guarded=event.get('valid')=='1' and event.get('caller_rva')=='0x1176b4',
            matchingPair=len(matching)==1 and matching[0][1].get('accepted')=='1' and matching[0][1].get('tid')==tid,
            restoration=len(restored)==1 and restored[0][1].get('restored')=='1' and restored[0][1].get('reason')=='scene_complete' and restored[0][1].get('tid')==tid and restored[0][1].get('frame')==frame,
            originalExact=bool(read(key,'visibility-original','volume')) and read(key,'visibility-original','volume')==read(key,'before-jobs','visibility-volume')==read(key,'visibility-restored','volume'),
            geometry=geometry['accepted'])
        if (run/'stereo-eyes.request').exists():
            right_geometry=box_geometry(read(key,'visibility-original','volume'),read(key,'visibility-expanded','volume'),
                read(key,'before-jobs','visibility-view'),read(key,'visibility-expanded','matrix'),
                eye_camera(matching[0][1]['id'],'right-') if len(matching)==1 else b'',render_view,frusta)
            checks['rightGeometry']=right_geometry['accepted']
            geometry['rightEye']=right_geometry
        checks['earlyOrder']=bool(len(prepared)==len(matching)==len(restored)==1 and position<prepared[0][0]<matching[0][0]<restored[0][0])
        checks['preparedVolume']=bool(len(prepared)==1 and all(read(prepared[0][1]['id'],phase,'visibility-volume')==read(key,'visibility-expanded','volume') for phase in ('prepare-before','prepare-after')))
        checks['visibilityViewUnchanged']=bool(len(prepared)==1 and all(read(prepared[0][1]['id'],phase,'visibility-view')==read(key,'before-jobs','visibility-view') and len(read(key,'before-jobs','visibility-view'))==144 for phase in ('prepare-before','prepare-after')))
        # Main method 15 writes SceneView +0x84/+0x88 at RVAs 0x549f2b/31
        # between the early callback and Prepare. Require all other bytes, and
        # the entire render view through Prepare, to remain unchanged.
        early=read(key,'before-jobs','render-view')
        before=read(prepared[0][1]['id'],'prepare-before','render-view') if len(prepared)==1 else b''
        after=read(prepared[0][1]['id'],'prepare-after','render-view') if len(prepared)==1 else b''
        checks['renderViewPreserved']=bool(len(early)==len(before)==len(after)==144 and before==after and early[:0x84]==before[:0x84] and early[0x8c:]==before[0x8c:])
        results.append(dict(id=key,frame=frame,accepted=all(checks.values()),gates=checks,geometry=geometry))
    limit=900 if all((run/name).exists() for name in ('compact-stereo.request','packed-snapshots.request','stereo-eyes.request')) else 120
    gates=dict(count=2<=requested<=limit and len(applied)==len(pairs)==len(restores)==requested,
        hook=any(r.get('installed')=='1' and r.get('expansion')=='1' for _,r in events('visibility hook ')),
        allPairs=bool(results) and all(r['accepted'] for r in results))
    return dict(run=run.name,accepted=all(gates.values()),gates=gates,pairs=results,
        limits='CPU volume containment, ordering and exact restoration only. Does not prove all visibility consumers, distant-object LOD, occlusion behavior, arbitrary runtime FOV, visual completeness or performance.')


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);args=parser.parse_args()
    result=analyze(args.run)
    (args.run/'visibility-summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
