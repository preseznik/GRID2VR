"""Evaluate one translated native geometry view, not calibrated stereo or VR."""
import argparse
import json
import hashlib
import math
from pathlib import Path
import numpy as np
from analyze_engine_trace import fields
from analyze_replay import analyze as analyze_replay
from analyze_replay_work import analyze as analyze_work


def requested_yaw(run):
    path=run/'eye-yaw.request'
    if not path.exists():return 0.
    degrees=int(path.read_text())
    if degrees not in (-90,-60,-15,15,60,90):raise ValueError('Unsupported requested yaw.')
    if abs(degrees)>15 and not (run/'expand-visibility.request').exists():raise ValueError('Wide yaw requires visibility expansion.')
    return float(np.float32(degrees)*np.float32(np.pi/180))


def pose_rotation(pose):
    q=list(map(float,pose[:4]));position=np.asarray(pose[4:],dtype=float)
    norm=sum(v*v for v in q)
    if len(pose)!=7 or not np.isfinite(pose).all() or abs(norm-1)>.001 or float(position@position)>.25:
        raise ValueError('Invalid full pose.')
    inv=1/math.sqrt(norm)
    x,y,z,w=[v*inv for v in q]
    return np.array([[1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)],
        [2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)],
        [2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)]])


def requested_pose(run):
    path=run/'eye-pose.request'
    if not path.exists():return None
    if (run/'eye-yaw.request').exists() or not (run/'expand-visibility.request').exists():
        raise ValueError('Full pose requires expanded visibility and no yaw request.')
    pose=np.array([float(v) for v in path.read_text().split()],dtype='<f4')
    pose_rotation(pose)
    return pose


def pose_log_valid(rows,pose):
    return bool(len(rows)==1 and rows[0].get('camera_builder')=='0x25bf00' and
        np.array([float(rows[0].get(k,'nan')) for k in ('qx','qy','qz','qw','x','y','z')],dtype='<f4').tobytes()==pose.tobytes())


def projection_uploads_valid(projections, frame):
    return bool(projections and any(p.get('operation')=='write' for p in projections) and
        all(p.get('applied')=='1' and p.get('frame')==frame and
            (p.get('caller_rva'),p.get('operation')) in (('0x25cccd','write'),('0x32c2ec','verify')) for p in projections))


def expected_eye_view(camera, view, distance, yaw=0., pose=None):
    result=bytearray(view)
    if pose is not None:
        rotation=pose_rotation(pose)
        basis=np.array([np.frombuffer(view,'<f4',3,offset)*sign for offset,sign in ((0x20,-1),(0x10,1),(0x30,-1))],dtype=float)
        for row,(offset,sign) in enumerate(((0x20,-1),(0x10,1),(0x30,-1))):
            result[offset:offset+12]=np.array([sign*sum(float(rotation[row,k])*float(basis[k,i]) for k in range(3)) for i in range(3)],dtype='<f4').tobytes()
        position=np.frombuffer(view,'<f4',3,0x40)
        translated=[]
        for i in range(3):
            value=float(position[i])
            for k in range(3):value+=float(pose[4+k])*float(basis[k,i])
            translated.append(value)
        result[0x40:0x4c]=np.array(translated,dtype='<f4').tobytes();result[0x8c]=0
        return result
    right=np.frombuffer(camera,'<f4',3,0x70).copy()
    if yaw:
        c,s=np.float32(np.cos(yaw)),np.float32(np.sin(yaw))
        negative_right=np.frombuffer(view,'<f4',3,0x20)
        forward=np.frombuffer(view,'<f4',3,0x30)
        rotated_right=c*negative_right-s*forward
        result[0x20:0x2c]=rotated_right.tobytes()
        result[0x30:0x3c]=(s*negative_right+c*forward).tobytes()
        right=-rotated_right
    position=np.frombuffer(view,'<f4',3,0x40)
    result[0x40:0x4c]=(position+right*np.float32(distance)).tobytes()
    result[0x8c]=0
    return result


def asymmetric_camera(camera,frustum=None):
    result=bytearray(camera)
    p=np.frombuffer(camera,'<f4',16,0x110).reshape(4,4).copy()
    v=np.frombuffer(camera,'<f4',16,0x150).reshape(4,4).astype(float)
    left,right,down,up=np.array([-1,.8,-.7,.6] if frustum is None else frustum,dtype='<f4')
    p[0,0]=np.float32(2)/(right-left);p[1,1]=np.float32(2)/(up-down)
    p[2,0]=(right+left)/(right-left);p[2,1]=(up+down)/(up-down)
    result[0x110:0x150]=p.tobytes();result[0x190:0x1d0]=(v@p.astype(float)).astype('<f4').tobytes()
    return result


def camera_geometry(source, shifted, original_view, shifted_view, distance, yaw=0., asymmetric=False, pose=None, frustum=None):
    if any(len(b)!=size for b,size in ((source,512),(shifted,512),(original_view,144),(shifted_view,144))):
        return {"complete":False}
    matrix=lambda b,o:np.frombuffer(b,dtype='<f4',count=16,offset=o).reshape(4,4).astype(float)
    w,p,v,vp=(matrix(source,o) for o in (0x70,0x110,0x150,0x190))
    sw,sp,sv,svp=(matrix(shifted,o) for o in (0x70,0x110,0x150,0x190))
    a=np.frombuffer(original_view,dtype='<f4',count=3,offset=0x40).astype(float)
    b=np.frombuffer(shifted_view,dtype='<f4',count=3,offset=0x40).astype(float)
    rotation=pose_rotation(pose) if pose is not None else np.array([[np.cos(yaw),0,-np.sin(yaw)],[0,1,0],[np.sin(yaw),0,np.cos(yaw)]])
    expected_orientation=rotation@w[:3,:3]
    desired=np.asarray(pose[4:],dtype=float)@w[:3,:3] if pose is not None else expected_orientation[0]*distance
    changed=[i for i,(x,y) in enumerate(zip(original_view,shifted_view)) if x!=y]
    finite=all(np.all(np.isfinite(m)) for m in (w,p,v,vp,sw,sp,sv,svp,a,b)) and np.isfinite(distance) and np.isfinite(yaw)
    if not finite: return {"complete":False}
    # Native affine matrices retain float error in the homogeneous W lane too.
    # Camera origins require XYZ/W, even when W is only one ULP away from 1.
    if any(np.max(np.abs(m[:3,3]))>1e-7 or abs(m[3,3]-1)>1e-4 for m in (w,v,sw,sv)):
        return {"complete":False}
    try:
        original_h=np.linalg.inv(v)[3]; shifted_h=np.linalg.inv(sv)[3]
    except np.linalg.LinAlgError:
        return {"complete":False}
    if any(not np.isfinite(h).all() or abs(h[3])<1e-6 for h in (original_h,shifted_h)):return {"complete":False}
    origin=original_h[:3]/original_h[3];shifted_origin=shifted_h[:3]/shifted_h[3]
    if not all(np.all(np.isfinite(m)) for m in (origin,shifted_origin)):return {"complete":False}
    original_residual=float(np.max(np.abs(w[3,:3]/w[3,3]-origin)))
    shifted_residual=float(np.max(np.abs(sw[3,:3]/sw[3,3]-shifted_origin)))
    # Retain the historical center-relative rounding explanation for reports;
    # acceptance below uses the absolute requested eye origin instead.
    # The native builder's center and eye inversions round independently. At
    # large coordinates their opposite residuals can exceed the old 1e-4
    # delta threshold even with an exact requested SceneView. Permit that
    # case only when BOTH absolute rendered origins stay within a bounded
    # float32 translation residual of their exact inputs. A position ULP alone
    # misses rounding of the three products and translation in the view matrix.
    # Propagate that four-term dot-product bound through the recorded rotation;
    # cap each origin at 1e-4 and the relative delta at 2e-4 native units.
    origin_bounds=[2*float(np.max(np.abs(np.spacing(point.astype('<f4'))))) for point in (a,b)]
    unit_roundoff=np.finfo(np.float32).eps/2
    gamma4=(4*unit_roundoff)/(1-4*unit_roundoff)
    translation_rounding=True
    for index,(point,view_matrix) in enumerate(((a,v),(b,sv))):
        rotation_matrix=view_matrix[:3,:3];translation=view_matrix[3,:3]
        bound=gamma4*(np.abs(point)@np.abs(rotation_matrix)+np.abs(translation))
        error=np.abs(point@rotation_matrix+translation)
        translation_rounding&=bool(np.all(error<=bound))
        try:propagated=float(np.max(bound@np.abs(np.linalg.inv(rotation_matrix))))
        except np.linalg.LinAlgError:return {"complete":False}
        origin_bounds[index]=min(.0001,max(origin_bounds[index],propagated))
    absolute_origin_errors=[float(np.max(np.abs(origin-a))),float(np.max(np.abs(shifted_origin-b)))]
    quantization_error=float(np.max(np.abs(b-a-desired)))
    rendered_error=float(np.max(np.abs(shifted_origin-origin-desired)))
    # The requested eye is based on SceneView's position, not the independently
    # rounded inverse of the mono center. Test that absolute target directly;
    # subtracting the center inverse can both invent and cancel eye errors.
    requested_rendered_error=float(np.max(np.abs(shifted_origin-a-desired)))
    rounding_explained=bool(translation_rounding and all(error<=bound for error,bound in zip(absolute_origin_errors,origin_bounds)) and
        rendered_error<=sum(origin_bounds)+quantization_error and rendered_error<.0002)
    expected=expected_eye_view(source,original_view,distance,yaw,pose)
    expected_projection=asymmetric_camera(source,frustum)[0x110:0x150] if asymmetric else source[0x110:0x150]
    depth_exact=all(source[0x110+4*i:0x114+4*i]==shifted[0x110+4*i:0x114+4*i] for i in range(16) if i not in (0,5,8,9))
    # These tolerances describe native matrix rounding, never image equality.
    return dict(complete=True,viewOnlyPositionAndFlag=all(0x40<=i<0x4c or i==0x8c for i in changed) and original_view[0x8c]==1 and shifted_view[0x8c]==0,
        requestedOffset=float(distance) if pose is None else None,requestedYawRadians=float(yaw),requestedPose=pose.tolist() if pose is not None else None,requestedViewExact=expected==shifted_view,
        observedWorldDelta=(sw[3,:3]-w[3,:3]).tolist(),desiredWorldDelta=desired.tolist(),
        viewPositionError=float(np.max(np.abs(b-a-desired))),worldPositionError=float(np.max(np.abs(sw[3,:3]-w[3,:3]-desired))),
        requestedPositionExact=expected[0x40:0x4c]==shifted_view[0x40:0x4c],
        renderedPositionError=rendered_error,nativePositionRoundingExplained=rounding_explained,
        renderedRequestedPositionError=requested_rendered_error,
        nativeOriginPositionBounds=origin_bounds,nativeOriginPositionErrors=absolute_origin_errors,
        originalOriginResidual=original_residual,shiftedOriginResidual=shifted_residual,
        nativeOriginResidual=max(original_residual,shifted_residual),originResidualIncrease=max(0.,shifted_residual-original_residual),
        orientationError=float(np.max(np.abs(sw[:3,:3]-expected_orientation))),projectionExact=source[0x110:0x150]==shifted[0x110:0x150],
        asymmetricProjection=asymmetric,projectionMatchesRequest=shifted[0x110:0x150]==expected_projection,projectionDepthExact=depth_exact,
        worldViewError=float(max(np.max(np.abs(w@v-np.eye(4))),np.max(np.abs(sw@sv-np.eye(4))))),viewProjectionError=float(np.max(np.abs(sv@sp-svp))),
        vpChanged=source[0x190:0x1d0]!=shifted[0x190:0x1d0])


def geometry_valid(geometry):
    return bool(geometry.get('complete') and geometry['requestedViewExact'] and geometry['requestedPositionExact'] and geometry['projectionMatchesRequest'] and geometry['projectionDepthExact'] and geometry['vpChanged'] and
        max(geometry[k] for k in ('viewPositionError','originResidualIncrease','orientationError','renderedRequestedPositionError'))<.0001 and
        geometry['nativeOriginPositionErrors'][1]<.0001 and
        max(geometry['worldViewError'],geometry['viewProjectionError'])<.0002)


def eye_history(previous, previous_view, entry, prepared, distance, yaw=0., asymmetric=False, pose=None, frustum=None):
    if any(len(b)!=s for b,s in ((previous,512),(previous_view,144),(entry,512),(prepared,512))):
        return dict(complete=False)
    projected=asymmetric_camera(previous,frustum) if asymmetric else previous
    vp=np.frombuffer(projected,dtype='<f4',count=16,offset=0x190).reshape(4,4)
    right=np.frombuffer(previous,dtype='<f4',count=3,offset=0x70)
    position=np.frombuffer(previous_view,dtype='<f4',count=3,offset=0x40)
    if not all(np.all(np.isfinite(a)) for a in (vp,right,position)) or not np.isfinite(distance):return dict(complete=False)
    transformed=expected_eye_view(previous,previous_view,distance,yaw,pose)
    delta=np.frombuffer(transformed,'<f4',3,0x40).astype(float)-position.astype(float)
    expected=vp.copy()
    if yaw or pose is not None:
        v=np.frombuffer(projected,'<f4',16,0x150).reshape(4,4).astype(float)
        p=np.frombuffer(projected,'<f4',16,0x110).reshape(4,4).astype(float)
        if not all(np.all(np.isfinite(a)) for a in (v,p)) or not np.isfinite(yaw):return dict(complete=False)
        v[3]-=delta@v[:3]
        inverse_rotation=np.array([[np.cos(yaw),0,np.sin(yaw),0],[0,1,0,0],[-np.sin(yaw),0,np.cos(yaw),0],[0,0,0,1]])
        if pose is not None:inverse_rotation[:3,:3]=pose_rotation(pose).T
        expected=(v@inverse_rotation@p).astype('<f4')
    else:expected[3]=(vp[3].astype(float)-delta@vp[:3].astype(float)).astype('<f4')
    observed=np.frombuffer(prepared,dtype='<f4',count=16,offset=0x50).reshape(4,4)
    if not np.all(np.isfinite(observed)):return dict(complete=False)
    return dict(complete=True,sourceMatchesNativeHistory=previous[0x190:0x1d0]==entry[0x10:0x50],
        matrixError=float(np.max(np.abs(observed.astype(float)-expected))),
        previousWorldDelta=delta.tolist(),translated=prepared[0x50:0x90]!=entry[0x10:0x50])


def analyze(run, scene_id=None):
    if (run/'capture-cleanup.json').exists(): raise ValueError('Read retained-evidence.json after cleanup.')
    result=dict(run=run.name,accepted=False,limits='One original and one translated view in native world units. Not two calibrated eyes, simulation isolation, head tracking, culling coverage, sustained rendering or headset acceptance.')
    if not (run/'eye-offset.request').exists():return result
    base=analyze_replay(run,scene_id); result['replayMode']=base['mode']
    if base['attempts']!=1 or base['completions']!=1:return result
    lines=(run/'engine.log').read_text().splitlines()
    starts=[fields(l) for l in lines if 'replay begin ' in l and (scene_id is None or fields(l).get('id')==str(scene_id))];start=starts[0];key=start['id']
    offsets=[fields(l) for l in lines if 'eye offset ' in l and fields(l).get('id')==key]
    yaw=requested_yaw(run)
    pose=requested_pose(run)
    asymmetric=(run/'eye-asymmetric-fov.request').exists()
    restores=[fields(l) for l in lines if 'eye restored ' in l and fields(l).get('id')==key]
    g=result['gates']={}
    different={'context_same','history_slot1_current','secondaryHistorySeed','secondaryHistoryMatchesInput','viewCopyOnlyFlag','contextUnchanged','identicalReadback','identicalSourceReadback'}
    g.update({k:v for k,v in base['gates'].items() if k not in different})
    if pose is None:g['offsetRequest']=len(offsets)==1 and np.float32(offsets[0].get('world_units','nan')).tobytes()==np.float32(.064).tobytes()
    else:g['fullPoseRequest']=not offsets and pose_log_valid([fields(l) for l in lines if 'eye fullpose ' in l and fields(l).get('id')==key],pose)
    if yaw:
        poses=[fields(l) for l in lines if 'eye pose ' in l and fields(l).get('id')==key]
        g['yawRequest']=len(poses)==1 and np.float32(poses[0].get('yaw_radians','nan')).tobytes()==np.float32(yaw).tobytes()
    if asymmetric:
        projections=[fields(l) for l in lines if 'eye projection ' in l and fields(l).get('id')==key]
        g['projectionUploads']=projection_uploads_valid(projections,start.get('frame'))
    g['nativeCameraRestored']=len(restores)==1 and restores[0].get('camera_same')=='1'
    if (run/'eye-textures.request').exists():
        graphics=(run/'trace.log').read_text().splitlines()
        copies=[fields(l) for l in graphics if 'eye texture ' in l and fields(l).get('id')==key]
        pairs=[fields(l) for l in graphics if 'eye pair ' in l and fields(l).get('id')==key]
        hashes=[r.get('source_sha256','') for r in copies]
        g['ownedEyeTextures']=bool(len(copies)==2 and [r.get('eye') for r in copies]==['0','1'] and
            all(r.get('captured')=='1' and r.get('tid')==start.get('tid') for r in copies) and
            all(len(h)==64 and all(c in '0123456789abcdef' for c in h) for h in hashes) and hashes[0]!=hashes[1] and
            len(pairs)==1 and pairs[0].get('ready')=='1' and pairs[0].get('left')==hashes[0] and pairs[0].get('right')==hashes[1])
        result['ownedEyeTextures']=dict(copies=copies,pairs=pairs)
    def read(phase,field):
        p=run/f'stage-{key}-{phase}-{field}.bin'
        return p.read_bytes() if p.exists() else b''
    source=read('scene-complete','context'); shifted=read('replay-complete','context')
    original_view=read('scene-complete','view'); shifted_view=read('replay-entry','view')
    distance=.064 if pose is not None else float(offsets[0]['world_units']) if len(offsets)==1 else float('nan')
    geometry=camera_geometry(source,shifted,original_view,shifted_view,distance,yaw,asymmetric,pose)
    result['geometry']=geometry
    history_rows=[fields(l) for l in lines if 'eye history ' in l and fields(l).get('id')==key]
    history=eye_history(read('eye-previous','context'),read('eye-previous','view'),read('scene-entry','manager'),read('replay-entry','manager'),distance,yaw,asymmetric,pose)
    result['previousEyeHistory']=history
    g['previousEyeHistory']=bool(len(history_rows)==1 and history_rows[0].get('saved')=='1' and
        all(history_rows[0].get(f)==start.get(f) for f in ('tid','frame')) and
        int(history_rows[0].get('previous_frame',-2))+1==int(start['frame']) and
        history.get('complete') and history['sourceMatchesNativeHistory'] and history['translated'] and history['matrixError']<0.000001)
    g['geometryCamera']=geometry_valid(geometry)
    manager=read('replay-complete','manager')
    g['offsetHistoryMatchesCamera']=len(manager)==512 and len(shifted)==512 and manager[0x50:0x90]==shifted[0x190:0x1d0]
    uploads=[]
    for index,(label,cpu) in enumerate((('original',source),('offset',shifted))):
        names=[]
        for path in sorted(run.glob(f'work-{key}-pass{index}-slot6-draw*-v3.buffer')):
            payload=path.read_bytes()
            if len(cpu)!=512 or len(payload)<236:continue
            w=np.frombuffer(cpu,dtype='<f4',count=16,offset=0x70).reshape(4,4)
            v=np.frombuffer(cpu,dtype='<f4',count=16,offset=0x150).reshape(4,4)
            if (cpu[0x110:0x150]==payload[:64] and cpu[0x190:0x1d0]==payload[64:128] and
                v.T[:3].tobytes()==payload[128:176] and w.T[:3].tobytes()==payload[176:224] and cpu[0xa0:0xac]==payload[224:236]):
                names.append(dict(file=path.name,sha256=hashlib.sha256(payload).hexdigest()))
        uploads.append(dict(view=label,snapshots=names))
    result['gpuCameraUploads']=uploads;g['gpuCameraMatches']=all(u['snapshots'] for u in uploads)
    work=analyze_work(run); result['images']=work.get('images',{})
    for image in ('slot5Depth','complete'):
        pair=result['images'].get(image,{})
        g[image+'GeometryChanged']=bool(pair.get('valid') and pair.get('equal') is False and pair.get('changedPixelFraction',0)>.001)
    result['accepted']=all(g.values());result['failedGates']=[k for k,v in g.items() if not v]
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('run',type=Path);args=parser.parse_args()
    result=analyze(args.run);(args.run/'eye-offset-summary.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps(result,indent=2,allow_nan=False))
