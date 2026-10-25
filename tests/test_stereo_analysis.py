"""A three-pass receipt must reject partial output and crossed eye histories."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import numpy as np
sys.path.insert(0,str(Path(__file__).parents[1]/'tools'))
from analyze_stereo_sequence import analyze, stereo_poses, FRUSTA
from analyze_eye_offset import expected_eye_view, asymmetric_camera, camera_geometry, geometry_valid
from make_recorded_views import generate
from dynamic_views import dynamic_inputs, frozen_frame_valid
from analyze_engine_trace import fields
from analyze_eye_offset import pose_log_valid


def fixture(run,dynamic=False):
    center=np.array([0,0,0,1,0,0,0],dtype='<f4');poses=stereo_poses(center)
    for name in ('stereo-eyes','eye-camera-history','eye-asymmetric-fov','expand-visibility','eye-textures','fresh-replay-color'):
        (run/(name+'.request')).touch()
    (run/'eye-pose.request').write_text(' '.join(map(str,center)));(run/'eye-sequence.request').write_text('2')
    lines=[];graphics=[];work=[];previous=[None,None]
    recorded=np.array([float(v) for v in generate(4).split()],dtype='<f4').reshape(4,2,11)[1:3]
    if dynamic:(run/'recorded-views.request').write_text(' '.join(map(str,recorded.ravel())))
    def camera(position,frustum=None,basis=None):
        w=np.eye(4,dtype='<f4');w[3,:3]=position;v=np.linalg.inv(w).astype('<f4')
        if basis is not None:w[:3,:3]=basis;v=np.linalg.inv(w).astype('<f4')
        p=np.array([[1.5,0,0,0],[0,2,0,0],[0,0,-(15000+.2)/(15000-.2),-1],[0,0,-2*15000*.2/(15000-.2),0]],dtype='<f4')
        b=bytearray(512)
        struct.pack_into('<2f',b,0xe8,.2,15000.)
        for offset,m in ((0x70,w),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
        return asymmetric_camera(b,frustum) if frustum is not None else b
    def view(position):
        v=bytearray(144);v[0x8c]=1
        for offset,row in ((0x10,[0,1,0]),(0x20,[-1,0,0]),(0x30,[0,0,-1]),(0x40,position)):struct.pack_into('<3f',v,offset,*row)
        return v
    def upload(camera):
        b=bytearray(320);b[:64]=camera[0x110:0x150];b[64:128]=camera[0x190:0x1d0]
        b[128:176]=np.frombuffer(camera,'<f4',16,0x150).reshape(4,4).T[:3].tobytes()
        b[176:224]=np.frombuffer(camera,'<f4',16,0x70).reshape(4,4).T[:3].tobytes()
        b[224:236]=camera[0xa0:0xac];return b
    for index in range(2):
        key=str(9+index);frame=20+index
        prefix=f'tid=7 frame={frame}'
        frusta=FRUSTA
        if dynamic:
            poses=recorded[index,:,:7];frusta=recorded[index,:,7:]
            lines.append(f'{prefix} pass=0 views frozen valid=1 index={index+1}')
            for eye in range(2):
                values=dict(zip(('qx','qy','qz','qw','x','y','z','left','right','down','up'),recorded[index,eye]))
                lines.append(f'{prefix} pass=0 views input eye={eye} '+' '.join(f'{k}={v}' for k,v in values.items()))
            lines.append(f'{prefix} pass=0 visibility before_jobs id={key} applied=1')
        native=camera([3+index,4,5]);original=view([3+index,4,5]);prior=camera([2+index,4,5]);prior_view=view([2+index,4,5])
        entry=bytearray(512);entry[0x10:0x50]=prior[0x190:0x1d0]
        complete=bytearray(entry);complete[0x10:0x50]=native[0x190:0x1d0]
        def save(phase,field,data): (run/f'stage-{key}-{phase}-{field}.bin').write_bytes(data)
        for phase in ('scene-entry','scene-complete'):
            for field,data in (('context',native),('view',original),('manager',entry if phase=='scene-entry' else complete),('object',bytes(352)),('packet',bytes(296)),('render-data',bytes(512)),('nodes',bytes(32))):save(phase,field,data)
        hashes=['1'*64,'2'*64,'3'*64]
        for render_pass in range(3):graphics.append(f'tid=7 eye texture id={key} eye={render_pass} captured=1 source_sha256={hashes[render_pass]}')
        graphics.append(f'tid=7 eye pair id={key} ready=1 left={hashes[1]} right={hashes[2]}')
        for render_pass in range(3):
            work.extend([f'vpl_order scene={key} pass={render_pass} applied=1 restored=1',f'shadow_unused scene={key} pass={render_pass} applied=1',f'shadow_unused scene={key} pass={render_pass} restored=1'])
        if not index:
            (run/f'work-{key}-pass0-slot6-draw1-v3.buffer').write_bytes(upload(native))
            for render_pass,color in enumerate((0xfff003ff,0xc00ffc00,0xfff00000)):
                name=f'work-{key}-pass{render_pass}-slot6-draw1'
                graphics.append(f"fresh_color name={name} pass={render_pass} applied=1 saved=1 seed={('magenta','green','blue')[render_pass]}")
                (run/(name+'-fresh-seed.raw')).write_bytes(struct.pack('<8I',2,2,24,1,*([color]*4)))
        for eye,render_pass in enumerate((1,2)):
            name_prefix='right-' if eye else ''
            current_view=expected_eye_view(native,original,0,pose=poses[eye]);position=np.frombuffer(current_view,'<f4',3,0x40)
            basis=lambda value:np.array([np.frombuffer(value,'<f4',3,offset)*sign for offset,sign in ((0x20,-1),(0x10,1),(0x30,-1))])
            shifted=camera(position,frusta[eye],basis(current_view));prepared=bytearray(complete)
            save(name_prefix+'replay-main','context',shifted)
            bootstrap=camera(np.array([2+index,4,5],dtype='<f4')+poses[eye][4:],frusta[eye],basis(current_view))
            prepared[0x50:0x90]=(previous[eye] if index else bootstrap)[0x190:0x1d0]
            after=bytearray(complete);after[0x50:0x90]=shifted[0x190:0x1d0]
            for phase in ('replay-entry','replay-complete','replay-restored'):
                for field,data in (('context',shifted if phase=='replay-complete' else native),('view',current_view),('manager',prepared if phase=='replay-entry' else after if phase=='replay-complete' else complete),('object',bytes(352)),('packet',bytes(296)),('render-data',bytes(512)),('nodes',bytes(32))):save(name_prefix+phase,field,data)
            save(name_prefix+'eye-previous','context',prior);save(name_prefix+'eye-previous','view',prior_view)
            if index:save(name_prefix+'eye-previous-rendered','context',previous[eye])
            previous[eye]=shifted
            lines.append(f'{prefix} pass={render_pass} replay begin id={key} counter=1 history_counter=1')
            pose_values=' '.join(f'{k}={float(v):.9g}' for k,v in zip(('qx','qy','qz','qw','x','y','z'),poses[eye]))
            lines.extend([f'{prefix} pass={render_pass} eye fullpose id={key} {pose_values} camera_builder=0x25bf00',
                f'{prefix} pass={render_pass} eye projection id={key} applied=1 caller_rva=0x25cccd operation=write',
                f"{prefix} pass={render_pass} eye history id={key} saved=1 previous_frame={frame-1} mode={'owned_previous' if index else 'posed_previous'}",
                f'{prefix} pass={render_pass} eye restored id={key} camera_same=1',
                f'{prefix} pass={render_pass} eye clip id={key} main_history=1 sequence_same=1',
                f'{prefix} pass={render_pass} replay end id={key} same_frame=1 image_saved=1 packet_same=1 history_other_same=1 history_restored=1 near_same=1 counter_before=1 counter_after=1 history_counter_after=1',
                f'{prefix} pass={render_pass} stereo eye id={key} eye={eye} accepted=1 gpu_restored=1'])
            digest=str(4+2*index+eye)*64;prior_digest=str(2+2*index+eye)*64 if index else 'a'*64
            work.extend([f'pair_gpu scene={key} pass={render_pass} healthy=1 dispatch0=4 dispatch1=4',
                f'ao_eye scene={key} pass={render_pass} previous_eye={int(bool(index))} prepared=1 history_resource={"eye"+str(eye) if index else "incoming"}',
                f'ao_eye scene={key} pass={render_pass} retained=1 resource=eye{eye}',
                f'compute_state scene={key} pass={render_pass} prepared=1',f'compute_state scene={key} pass={render_pass} restored=1',
                f'shadow_sampler scene={key} pass={render_pass} applied=1',f'shadow_sampler scene={key} pass={render_pass} restored=1'])
            for phase,h in (('seed',prior_digest),('retain',digest),('restore','b'*64)):
                work.append(f'ao_verify scene={key} pass={render_pass} phase={phase} source={h} actual={h} prior_sampled={int(bool(index))} prior_matched=1')
            if not index:(run/f'work-{key}-pass{render_pass}-slot6-draw1-v3.buffer').write_bytes(upload(shifted))
            (run/f'stage-{key}-{name_prefix}replay-complete.raw').write_bytes(struct.pack('<8I',2,2,28,1,*([0xff000000+render_pass]*4)))
        lines.append(f'{prefix} pass=0 sequence pair id={key} index={index+1} target=2 accepted=1 gpu_restored=1')
        if dynamic:
            lines.extend((f'{prefix} pass=0 visibility restore id={key} restored=1',f'{prefix} pass=0 views complete id={key} accepted=1 visibility_restored=1'))
            graphics.append(f'tid=7 views output frame={frame} scene={key} accepted=1 ready=1')
    (run/'engine.log').write_text('\n'.join(lines));(run/'trace.log').write_text('\n'.join(graphics));(run/'replay-work.txt').write_text('\n'.join(work))


class StereoEvidenceTests(unittest.TestCase):
    def test_native_xr_float32_reconstruction_and_one_ulp_refusal(self):
        source=json.loads((Path(__file__).parent/'fixtures/native-xr-view-mapping.json').read_text())
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory)
            (run/'xr-projection.request').write_text('1280 720 1')
            (run/'eye-sequence.request').write_text('2')
            (run/'trace.log').write_text('\n'.join(source['trace']))
            runtime=run/'openxr-session.log';runtime.write_text('\n'.join(source['runtime']))
            inputs=dynamic_inputs(run,source['engine'])
            pairs=[fields(l) for l in source['engine'] if 'sequence pair ' in l]
            for index,pair in enumerate(pairs):
                frame=pair['frame'];poses=inputs[index,:,:7];frusta=inputs[index,:,7:]
                self.assertTrue(frozen_frame_valid(source['engine'],source['trace'],frame,pair['tid'],pair['id'],index,poses,frusta))
                for eye in range(2):
                    rows=[fields(l) for l in source['engine'] if 'eye fullpose ' in l and fields(l).get('frame')==frame and fields(l).get('pass')==str(eye+1)]
                    self.assertTrue(pose_log_valid(rows,poses[eye]))
                    # Do not turn float32 input reconstruction into a tolerance
                    # exemption: even one changed output ULP must still fail.
                    bad=[dict(rows[0])];bad[0]['x']=str(np.nextafter(np.float32(bad[0]['x']),np.float32(np.inf)))
                    self.assertFalse(pose_log_valid(bad,poses[eye]))
            self.assertEqual(len(pairs),2)
            changed=list(source['runtime'])
            i=next(i for i,l in enumerate(changed) if 'pose kind=eye eye=0 ' in l)
            old=fields(changed[i])['x'];new=str(np.nextafter(np.float32(old),np.float32(np.inf)))
            changed[i]=changed[i].replace(' x='+old+' ',' x='+new+' ')
            runtime.write_text('\n'.join(changed))
            altered=dynamic_inputs(run,source['engine']);pair=pairs[0]
            self.assertFalse(frozen_frame_valid(source['engine'],source['trace'],pair['frame'],pair['tid'],pair['id'],0,altered[0,:,:7],altered[0,:,7:]))

    def test_missing_xr_recenter_is_a_rejected_report(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            (run/'xr-projection.request').write_text('1280 720 1')
            (run/'openxr-session.log').write_text('tid=7 frame=20 sample tracked=1 should_render=0\n')
            (run/'engine.log').write_text('tid=7 frame=20 views frozen valid=0 index=1\n')
            for inspect in (analyze,__import__('analyze_visibility').analyze):
                result=inspect(run)
                self.assertFalse(result['accepted'])
                self.assertEqual(result['failedGates'],['dynamicInputs'])
                self.assertEqual(result['inputError'],'Missing recenter sample')

    def test_blur_request_requires_known_shader_and_restored_binding(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run);(run/'disable-motion-blur.request').touch()
            log=run/'replay-work.txt';original=log.read_text()
            rows='\n'.join(line for key in (9,10) for p in range(3) for line in (
                f'motion_blur scene={key} pass={p} shader=6e44f31d1ebda08d applied=1',f'motion_blur scene={key} pass={p} restored=1'))
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                self.assertFalse(analyze(run)['accepted'])
                log.write_text(original+'\n'+rows);self.assertTrue(analyze(run)['accepted'])
                for old,new in [('applied=1','applied=0'),('restored=1','restored=0'),('shader=6e44f31d1ebda08d','shader=123')]:
                    log.write_text(original+'\n'+rows.replace(old,new,1));self.assertFalse(analyze(run)['accepted'])

    def test_native_independent_inverse_rounding_is_bounded(self):
        root=Path(__file__).parent/'fixtures'
        center=(root/'native-moving-center-context.bin').read_bytes();view=(root/'native-moving-center-view.bin').read_bytes()
        eye=(root/'native-moving-right-context.bin').read_bytes();posed=(root/'native-moving-right-view.bin').read_bytes()
        pose=np.array([.013753200881183147,.034383002668619156,.008595750667154789,.9992771148681641,.04776349291205406,.015286925248801708,-.006720684003084898],dtype='<f4')
        # Sample 13 of the 120-input trajectory, retained from the live game.
        values=np.array([float(v) for v in generate(120).split()],dtype='<f4').reshape(120,2,11)
        match=[row for row in values[:,1] if row[:7].tobytes()==pose.tobytes()]
        self.assertEqual(len(match),1)
        result=camera_geometry(center,eye,view,posed,0,asymmetric=True,pose=pose,frustum=match[0][7:])
        self.assertGreater(result['renderedPositionError'],.0001)
        self.assertTrue(result['nativePositionRoundingExplained'])
        self.assertTrue(geometry_valid(result))
        # A coherent displaced camera is still wrong: inverse and VP agreement
        # alone cannot make an unrequested position acceptable.
        wrong=bytearray(eye);w=np.frombuffer(eye,'<f4',16,0x70).reshape(4,4).copy();w[3,2]+=.001
        v=np.linalg.inv(w).astype('<f4');p=np.frombuffer(eye,'<f4',16,0x110).reshape(4,4)
        for offset,m in ((0x70,w),(0x150,v),(0x190,(v.astype(float)@p.astype(float)).astype('<f4'))):wrong[offset:offset+64]=m.tobytes()
        result=camera_geometry(center,wrong,view,posed,0,asymmetric=True,pose=pose,frustum=match[0][7:])
        self.assertFalse(result['nativePositionRoundingExplained']);self.assertFalse(geometry_valid(result))

    def test_moving_views_require_frozen_input_and_correct_history(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run,dynamic=True)
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                result=analyze(run);self.assertTrue(result['accepted'],json.dumps(result,indent=2))
                log=run/'engine.log';saved=log.read_text()
                log.write_text(saved.replace('views frozen valid=1','views frozen valid=0',1))
                self.assertFalse(analyze(run)['accepted']);log.write_text(saved)
                source=run/'recorded-views.request';values=source.read_text().split();values[7]='-1.25';source.write_text(' '.join(values))
                self.assertFalse(analyze(run)['accepted'])

    def test_headset_eye_origin_uses_requested_center_not_mono_inverse(self):
        root=Path(__file__).parent/'fixtures'
        metadata=json.loads((root/'native-headset-6136.json').read_text())
        read=lambda label:(root/metadata['files'][label]['file']).read_bytes()
        center,view=read('center-context'),read('center-view')
        for eye,label in enumerate(('left','right')):
            values=metadata['views'][eye]
            pose=np.array([values[k] for k in ('qx','qy','qz','qw','x','y','z')],dtype='<f4')
            frustum=np.array([values[k] for k in ('left','right','down','up')],dtype='<f4')
            result=camera_geometry(center,read(label+'-context'),view,read(label+'-view'),0,asymmetric=True,pose=pose,frustum=frustum)
            self.assertTrue(result['requestedViewExact'])
            self.assertTrue(result['projectionMatchesRequest'])
            self.assertGreater(result['nativeOriginPositionErrors'][0],.0001)
            self.assertLess(result['nativeOriginPositionErrors'][1],.0001)
            self.assertFalse(result['nativePositionRoundingExplained'])
            # Preserve evidence of the old rejection; its center-relative
            # comparison is not the requested eye position.
            self.assertEqual(result['renderedPositionError']<.0001,
                             metadata['expectedCurrentGeometryAccepted'][eye])
            self.assertLess(result['renderedRequestedPositionError'],.0001)
            self.assertTrue(geometry_valid(result))
            if eye:
                self.assertGreater(result['renderedPositionError'],.0001)
                self.assertLess(result['renderedPositionError'],.0002)
                self.assertLess(result['renderedRequestedPositionError'],.000015)

            # The exact request remains mandatory, including one position ULP.
            wrong_view=bytearray(read(label+'-view'))
            position=np.frombuffer(wrong_view,'<f4',3,0x40)
            position[0]=np.nextafter(position[0],np.float32(np.inf))
            self.assertFalse(geometry_valid(camera_geometry(center,read(label+'-context'),
                view,wrong_view,0,asymmetric=True,pose=pose,frustum=frustum)))

            # Coherently move W, V and VP just beyond the existing origin cap.
            # Matching matrix inverses must not mask an unrequested camera.
            camera=read(label+'-context');posed=read(label+'-view')
            for axis in range(3):
                for sign in (-1,1):
                    with self.subTest(eye=label,axis=axis,sign=sign):
                        wrong=bytearray(camera)
                        w=np.frombuffer(camera,'<f4',16,0x70).reshape(4,4).copy()
                        point=np.frombuffer(posed,'<f4',3,0x40).astype(float)
                        point[axis]+=sign*.00015
                        w[3,:3]=point*float(w[3,3])
                        v=np.linalg.inv(w.astype(float)).astype('<f4')
                        p=np.frombuffer(camera,'<f4',16,0x110).reshape(4,4)
                        for offset,m in ((0x70,w),(0x150,v),(0x190,(v.astype(float)@p.astype(float)).astype('<f4'))):
                            wrong[offset:offset+64]=m.tobytes()
                        altered=camera_geometry(center,wrong,view,posed,0,
                            asymmetric=True,pose=pose,frustum=frustum)
                        self.assertGreater(altered['renderedRequestedPositionError'],.0001)
                        self.assertFalse(geometry_valid(altered))

    def test_long_run_native_translation_rounding_and_wrong_origin(self):
        root=Path(__file__).parent/'fixtures'
        values=np.array([float(v) for v in generate(900).split()],dtype='<f4').reshape(900,2,11)
        for key,index,eye_index in [('743',11,0),('14834',242,1)]:
            center,view,eye,posed=[(root/f'native-long-{key}-{label}.bin').read_bytes() for label in ('center-context','center-view','eye-context','eye-view')]
            pose=values[index,eye_index,:7];frustum=values[index,eye_index,7:]
            result=camera_geometry(center,eye,view,posed,0,asymmetric=True,pose=pose,frustum=frustum)
            self.assertGreater(result['renderedPositionError'],.0001)
            self.assertTrue(geometry_valid(result),result)
            self.assertLessEqual(max(result['nativeOriginPositionBounds']),.0001)
            wrong=bytearray(eye);w=np.frombuffer(eye,'<f4',16,0x70).reshape(4,4).copy();w[3,2]+=.0003
            v=np.linalg.inv(w).astype('<f4');p=np.frombuffer(eye,'<f4',16,0x110).reshape(4,4)
            for offset,m in ((0x70,w),(0x150,v),(0x190,(v.astype(float)@p.astype(float)).astype('<f4'))):wrong[offset:offset+64]=m.tobytes()
            self.assertFalse(geometry_valid(camera_geometry(center,wrong,view,posed,0,asymmetric=True,pose=pose,frustum=frustum)))

    def test_fresh_color_requires_same_first_material_when_requested(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run);(run/'fresh-color-identity.request').touch()
            path=run/'trace.log';original=path.read_text()
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                self.assertFalse(analyze(run)['accepted'])
                lines=[line+' ps=1234567890abcdef vs=9876543210abcdef identity=1' if 'fresh_color ' in line else line for line in original.splitlines()]
                valid='\n'.join(lines);path.write_text(valid);self.assertTrue(analyze(run)['accepted'])
                for bad in (valid.replace('identity=1','identity=0',1),valid.replace('ps=1234567890abcdef','ps=1234567890abcdee',1)):
                    path.write_text(bad);self.assertFalse(analyze(run)['accepted'])

    def test_shadow_identity_is_required_but_global_draw_number_can_change(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            (run/'shadow-pass-identity.request').touch()
            log=run/'replay-work.txt';lines=log.read_text().splitlines()
            lines=[line+' identity=1 ordinal=399 source_ordinal=400' if line.startswith('shadow_sampler ') and 'applied=1' in line else line for line in lines]
            lines.extend(f'shadow_sampler scene={key} ordinal=400 captured=1 identity=1' for key in (9,10))
            saved='\n'.join(lines);log.write_text(saved)
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                self.assertTrue(analyze(run)['accepted'])
                log.write_text(saved.replace('source_ordinal=400','source_ordinal=401',1))
                self.assertFalse(analyze(run)['accepted'])
                log.write_text(saved.replace('identity=1','identity=0',1))
                self.assertFalse(analyze(run)['accepted'])

    def test_compact_capture_keeps_all_stereo_gates(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            (run/'compact-stereo.request').touch()
            log=run/'replay-work.txt';original=log.read_text()
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                self.assertFalse(analyze(run)['accepted'])
                log.write_text('capture_mode compact=1\n'+original)
                self.assertTrue(analyze(run)['accepted'])
                extra=run/'work-9-pass1-slot6-draw1-vertex0.buffer';extra.write_bytes(b'bulky')
                self.assertFalse(analyze(run)['accepted']);extra.unlink()
                (run/'work-9-pass2-slot6-draw1-v3.buffer').unlink()
                self.assertFalse(analyze(run)['accepted'])

    def test_near_subpass_keeps_main_eye_history(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            def near_camera(data):
                data=bytearray(data);struct.pack_into('<2f',data,0xe8,.2,100.)
                p=np.frombuffer(data,'<f4',16,0x110).reshape(4,4).copy()
                p[2,2]=-(100+.2)/(100-.2);p[3,2]=-2*100*.2/(100-.2)
                v=np.frombuffer(data,'<f4',16,0x150).reshape(4,4)
                data[0x110:0x150]=p.tobytes();data[0x190:0x1d0]=(v.astype(float)@p.astype(float)).astype('<f4').tobytes()
                return data
            for phase in ('scene-complete','replay-complete','right-replay-complete','replay-restored','right-replay-restored'):
                path=run/f'stage-9-{phase}-context.bin';path.write_bytes(near_camera(path.read_bytes()))
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                result=analyze(run);self.assertTrue(result['accepted'],json.dumps(result,indent=2))
                (run/'wait-clip-transition.request').touch()
                log=run/'engine.log';original=log.read_text()
                trigger='tid=7 frame=19 capture trigger observed=clip_transition context=1234\n'
                clips='\n'.join(f'tid=7 frame=20 pass={p} replay clip id=9 clip_changed=1' for p in (1,2))+'\n'
                log.write_text(trigger+clips+original)
                self.assertTrue(analyze(run)['accepted'])
                for bad in (clips.replace('clip_changed=1','clip_changed=0'),clips.replace('id=9','id=10'),clips.splitlines()[0]+'\n'):
                    log.write_text(trigger+bad+original)
                    self.assertFalse(analyze(run)['accepted'])
                log.write_text(trigger.replace('frame=19','frame=18')+clips+original)
                self.assertFalse(analyze(run)['accepted'])
                log.write_text(trigger+clips+original)
                # Storing the final near-object VP as next-frame history is wrong
                # even when the camera restore and both output images pass.
                path=run/'stage-10-eye-previous-rendered-context.bin'
                path.write_bytes((run/'stage-9-replay-complete-context.bin').read_bytes())
                self.assertFalse(analyze(run)['accepted'])

    def test_crossed_or_partial_eye_evidence_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            # Volume construction/order has its own independent regression suite.
            with patch('analyze_stereo_sequence.analyze_visibility',return_value={'accepted':True}):
                result=analyze(run)
                self.assertTrue(result['accepted'],json.dumps(result,indent=2))
                path=run/'stage-10-right-eye-previous-rendered-context.bin';saved=path.read_bytes()
                path.write_bytes((run/'stage-10-eye-previous-rendered-context.bin').read_bytes())
                self.assertFalse(analyze(run)['accepted']);path.write_bytes(saved)
                for name,before,after in [('replay-work.txt','history_resource=eye1','history_resource=eye0'),('trace.log','eye=2 captured=1','eye=2 captured=0'),('engine.log','pass=2 stereo eye id=9 eye=1 accepted=1','pass=2 stereo eye id=9 eye=1 accepted=0')]:
                    path=run/name;saved=path.read_text();path.write_text(saved.replace(before,after))
                    self.assertFalse(analyze(run)['accepted']);path.write_text(saved)
                for name in ('work-9-pass2-slot6-draw1-v3.buffer','stage-9-right-replay-restored-context.bin','stage-9-right-replay-main-context.bin','stage-9-right-replay-complete.raw','work-9-pass2-slot6-draw1-fresh-seed.raw'):
                    path=run/name;saved=path.read_bytes();path.unlink()
                    self.assertFalse(analyze(run)['accepted']);path.write_bytes(saved)


if __name__=='__main__':unittest.main()
