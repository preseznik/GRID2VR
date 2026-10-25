"""Independent input and ordering checks for frozen, per-frame eye views."""
import math
import numpy as np
from analyze_engine_trace import fields
from analyze_eye_offset import pose_rotation

POSE=('qx','qy','qz','qw','x','y','z')
FOV=('left','right','down','up')

def dynamic_inputs(run,lines):
    recorded=run/'recorded-views.request';xr=run/'xr-projection.request'
    if not recorded.exists() and not xr.exists():return None
    if recorded.exists() and xr.exists():raise ValueError('Conflicting view sources')
    count=int((run/'eye-sequence.request').read_text())
    events=[fields(line) for line in lines if 'views frozen ' in line]
    if recorded.exists():
        values=np.array([float(v) for v in recorded.read_text().split()],dtype='<f4')
        if values.size!=count*22:raise ValueError('Recorded view count mismatch')
        inputs=values.reshape(count,2,11)
    else:
        logs=(run/'openxr-session.log').read_text().splitlines()
        by_frame={}
        for line in logs:
            row=fields(line);by_frame.setdefault(row.get('frame'),[]).append(row)
        recentered=[fields(l) for l in (run/'trace.log').read_text().splitlines() if 'openxr recentered ' in l]
        if len(recentered)!=1 or not events or recentered[0]['frame']!=events[0]['frame']:raise ValueError('Missing recenter sample')
        scale=float(np.float32(recentered[0]['units_per_metre']));reference=None;inputs=[]
        def runtime_pose(row):
            # OpenXR supplies float32; %.9g logs round-trip those values. Do
            # not treat their decimal text as the original double inputs.
            return [float(np.float32(row[k])) for k in POSE]
        def normalized(q):
            norm=sum(v*v for v in q)
            if not all(math.isfinite(v) for v in q) or abs(norm-1)>.001:raise ValueError('Invalid runtime orientation')
            return [v/math.sqrt(norm) for v in q]
        for event in events:
            rows=by_frame.get(event['frame'],[])
            heads=[r for r in rows if r.get('kind')=='head']
            eyes=[r for r in rows if r.get('kind')=='eye']
            fovs=[r for r in rows if all(k in r for k in FOV)]
            samples=[r for r in rows if 'tracked' in r]
            if len(heads)!=1 or len(eyes)!=2 or len(fovs)!=2 or len(samples)!=1 or samples[0].get('tracked')!=samples[0].get('should_render') or samples[0].get('tracked')!='1':raise ValueError('Incomplete tracked sample')
            head=runtime_pose(heads[0])
            if reference is None:reference=head
            q=normalized(reference[:4])
            # The native reference basis passes through ScenePose's float32
            # orientation before PoseAxes normalizes it. Match that boundary
            # and scalar operation order while keeping exact pose comparisons.
            rotation=pose_rotation(np.r_[np.array(q,dtype='<f4'),0.,0.,0.])
            x,y,z,w=-q[0],-q[1],-q[2],q[3]
            pair=[]
            for eye in range(2):
                if eyes[eye]['eye']!=str(eye) or fovs[eye]['eye']!=str(eye):raise ValueError('Crossed runtime eyes')
                p=runtime_pose(eyes[eye]);b=normalized(p[:4])
                relative=[w*b[0]+x*b[3]+y*b[2]-z*b[1],w*b[1]-x*b[2]+y*b[3]+z*b[0],
                          w*b[2]+x*b[1]-y*b[0]+z*b[3],w*b[3]-x*b[0]-y*b[1]-z*b[2]]
                position=[sum((p[4+i]-reference[4+i])*float(rotation[local,i]) for i in range(3))*scale for local in range(3)]
                pair.append(np.r_[relative,position,np.tan([float(np.float32(fovs[eye][k])) for k in FOV])])
            inputs.append(pair)
        inputs=np.array(inputs,dtype='<f4')
    for pair in inputs:
        for view in pair:
            pose_rotation(view[:7]);f=view[7:]
            if not np.isfinite(view).all() or np.max(np.abs(f))>4 or f[0]>=0 or f[1]<=0 or f[2]>=0 or f[3]<=0:raise ValueError('Invalid dynamic view')
    return inputs

def frozen_frame_valid(lines,graphics,frame,tid,key,index,poses,frusta):
    rows=[(i,fields(l)) for i,l in enumerate(lines) if fields(l).get('frame')==frame]
    frozen=[(i,r) for i,r in rows if 'views frozen ' in lines[i]]
    inputs=[(i,r) for i,r in rows if 'views input ' in lines[i]]
    done=[(i,r) for i,r in rows if 'views complete ' in lines[i]]
    visibility=[i for i,r in rows if 'visibility before_jobs ' in lines[i] and r.get('applied')=='1']
    restored=[i for i,r in rows if 'visibility restore ' in lines[i] and r.get('restored')=='1']
    output=[fields(l) for l in graphics if 'views output ' in l and fields(l).get('frame')==frame]
    valid=len(frozen)==len(done)==len(visibility)==len(restored)==len(output)==1 and len(inputs)==2
    if not valid:return False
    valid=frozen[0][1].get('valid')=='1' and frozen[0][1].get('index')==str(index+1) and frozen[0][0]<inputs[0][0]<inputs[1][0]<visibility[0]<restored[0]<done[0][0]
    valid&=all(r.get('tid')==tid for _,r in frozen+inputs+done) and output[0].get('tid')==tid
    valid&=done[0][1].get('id')==key and done[0][1].get('accepted')==done[0][1].get('visibility_restored')=='1'
    valid&=output[0].get('scene')==key and output[0].get('accepted')==output[0].get('ready')=='1'
    for eye,(_,row) in enumerate(inputs):
        valid&=row.get('eye')==str(eye) and np.array([float(row[k]) for k in POSE],dtype='<f4').tobytes()==poses[eye].tobytes() and np.allclose([float(row[k]) for k in FOV],frusta[eye],rtol=0,atol=2e-6)
    return bool(valid)
