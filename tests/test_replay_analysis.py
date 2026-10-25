"""Replay evidence must fail closed on missing, mismatched or changed state."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).parents[1] / "tools"))
from analyze_replay import analyze
from analyze_eye_offset import camera_geometry, geometry_valid


def fixture(run):
    sizes = {"manager": 0x200, "context": 0x200, "view": 0x90, "packet": 0x128, "object": 0x160, "render-data": 0x200}
    phases = ("scene-entry", "scene-complete", "replay-entry", "replay-complete", "replay-restored")
    lines = ["tid=7 frame=12 replay begin id=9 manager=ABCD history_counter=1",
             "tid=7 frame=12 replay end id=9 same_frame=1 image_saved=1 packet_same=1 context_same=1 history_other_same=1 history_restored=1 counter_before=1 counter_after=1 history_counter_after=1 history_slot1_current=1"]
    graphics = []
    for i, phase in enumerate(phases):
        lines.append(f"tid=7 frame=12 scene state id=9 phase={phase} manager=ABCD manager_valid=1 manager_saved=1")
        graphics.append(f"stage boundary id=9 slot=100 phase={phase} draws={(i+1)//2*10} dispatches=0")
        graphics.append(f"stage target id=9 phase={phase} kind=color index=0 resource=1234 size=2x2 format=24 samples=1")
        for field, size in sizes.items():
            data = bytearray(size)
            if field == "view" and not phase.startswith("replay-"): data[0x8c] = 1
            if field == "manager": struct.pack_into("<I", data, 0x94, 1)
            (run / f"stage-9-{phase}-{field}.bin").write_bytes(data)
    for phase, counter in (("entry", 1), ("complete", 2)):
        data = bytearray(0x200)
        struct.pack_into("<I", data, 0x94, counter)
        # Independent next-frame inputs are allowed; all history/identity bytes are not.
        struct.pack_into("<4f", data, 0x1dc, .016, .016, .02, .02)
        (run / f"stage-10-scene-{phase}-manager.bin").write_bytes(data)
        (run / f"stage-10-scene-{phase}-context.bin").write_bytes(bytes(0x200))
        lines.append(f"tid=7 frame=13 scene state id=10 phase=scene-{phase} manager=ABCD manager_valid=1 manager_saved=1")
    lines += ["tid=7 frame=13 scene candidate id=10 valid=1", "tid=7 frame=13 scene complete id=10 stage6_seen=1 native_stereo=0"]
    (run / "engine.log").write_text("\n".join(lines))
    (run / "trace.log").write_text("\n".join(graphics))
    for phase in ("scene-complete", "replay-complete"):
        (run / f"stage-9-{phase}.ppm").write_bytes(b"P6\n2 2\n255\n" + bytes(12))


class ReplayEvidenceTests(unittest.TestCase):
    def test_full_pose_geometry_and_independent_history(self):
        from analyze_eye_offset import eye_history, expected_eye_view, requested_pose, pose_log_valid
        import numpy as np
        w=np.array([[0,0,-1,0],[0,1,0,0],[1,0,0,0],[3,4,5,1]],dtype='<f4')
        p=np.array([[1.5,0,0,0],[0,2,0,0],[0,0,-1.0002,-1],[0,0,-.20002,0]],dtype='<f4')
        def camera(world):
            b=bytearray(512);v=np.linalg.inv(world).astype('<f4')
            for offset,m in ((0x70,world),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
            return b
        source=camera(w);view=bytearray(144);view[0x8c]=1
        for offset,row in ((0x10,[0,1,0]),(0x20,[0,0,1]),(0x30,[-1,0,0]),(0x40,[3,4,5])):struct.pack_into('<3f',view,offset,*row)
        pose=np.array([.5,.5,.5,.5,.125,.25,-.125],dtype='<f4')
        eye=bytearray(view);eye[0x8c]=0
        for offset,row in ((0x10,[1,0,0]),(0x20,[0,-1,0]),(0x30,[0,0,1]),(0x40,[2.875,4.25,4.875])):struct.pack_into('<3f',eye,offset,*row)
        generated=expected_eye_view(source,view,0,pose=pose)
        # Zero signs are immaterial to the geometry but exact native-byte
        # ownership is checked independently by the generated-view gate.
        self.assertTrue(np.array_equal(np.frombuffer(generated,'<f4'),np.frombuffer(eye,'<f4')))
        world=np.array([[0,1,0,0],[1,0,0,0],[0,0,-1,0],[2.875,4.25,4.875,1]],dtype='<f4')
        shifted=camera(world)
        self.assertTrue(geometry_valid(camera_geometry(source,shifted,view,generated,0,pose=pose)))
        wrong=pose.copy();wrong[4]=-.125
        self.assertFalse(geometry_valid(camera_geometry(source,shifted,view,generated,0,pose=wrong)))
        changed=bytearray(generated);changed[0x10]^=1
        self.assertFalse(geometry_valid(camera_geometry(source,shifted,view,changed,0,pose=pose)))
        entry=bytearray(512);entry[0x10:0x50]=source[0x190:0x1d0]
        prepared=bytearray(512);prepared[0x50:0x90]=shifted[0x190:0x1d0]
        self.assertLess(eye_history(source,view,entry,prepared,0,pose=pose)['matrixError'],1e-6)
        self.assertGreater(eye_history(source,view,entry,prepared,0,pose=wrong)['matrixError'],.1)
        row=dict(zip(('qx','qy','qz','qw','x','y','z'),map(str,pose)));row['camera_builder']='0x25bf00'
        self.assertTrue(pose_log_valid([row],pose))
        self.assertFalse(pose_log_valid([dict(row,x='0.1')],pose))
        self.assertFalse(pose_log_valid([row,row],pose))
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);path=run/'eye-pose.request';path.write_text(' '.join(map(str,pose)))
            with self.assertRaises(ValueError):requested_pose(run)
            (run/'expand-visibility.request').touch()
            self.assertTrue(np.array_equal(requested_pose(run),pose))
            for bad in ('0 0 0 0 0 0 0','0 0 0 1 .4 .4 0','nan 0 0 1 0 0 0','0 0 0 1 0 0 0 1'):
                path.write_text(bad)
                with self.assertRaises(ValueError):requested_pose(run)

    def test_projection_rebind_requires_verified_context_and_guarded_caller(self):
        from analyze_eye_offset import projection_uploads_valid
        write=dict(applied='1',frame='42',caller_rva='0x25cccd',operation='write')
        verify=dict(applied='1',frame='42',caller_rva='0x32c2ec',operation='verify')
        self.assertTrue(projection_uploads_valid([write,verify],'42'))
        for rows in ([verify], [write,dict(verify,applied='0')], [write,dict(verify,caller_rva='0x123')], [write,dict(verify,operation='write')]):
            self.assertFalse(projection_uploads_valid(rows,'42'))
        self.assertFalse(projection_uploads_valid([write,verify],'43'))

    def test_rotated_geometry_and_bootstrap_history(self):
        from analyze_eye_offset import eye_history, expected_eye_view
        import numpy as np
        yaw=float(np.float32(15)*np.float32(np.pi/180));c,s=np.cos(yaw),np.sin(yaw)
        w=np.eye(4,dtype='<f4');w[3,:3]=[3,4,5]
        p=np.array([[1.5,0,0,0],[0,2,0,0],[0,0,-1.0002,-1],[0,0,-.20002,0]],dtype='<f4')
        def camera(world):
            b=bytearray(512);v=np.linalg.inv(world).astype('<f4')
            for offset,m in ((0x70,world),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
            return b
        source=camera(w);view=bytearray(144);view[0x8c]=1
        for offset,row in ((0x10,[0,1,0]),(0x20,[-1,0,0]),(0x30,[0,0,-1]),(0x40,[3,4,5])):struct.pack_into('<3f',view,offset,*row)
        eye=expected_eye_view(source,view,.064,yaw)
        world=w.copy();world[:3,:3]=[[c,0,-s],[0,1,0],[s,0,c]];world[3,:3]=np.frombuffer(eye,'<f4',3,0x40)
        shifted=camera(world)
        self.assertTrue(geometry_valid(camera_geometry(source,shifted,view,eye,.064,yaw)))
        self.assertFalse(geometry_valid(camera_geometry(source,shifted,view,eye,.064,-yaw)))
        wrong=bytearray(eye);wrong[0x10]^=1
        self.assertFalse(geometry_valid(camera_geometry(source,shifted,view,wrong,.064,yaw)))
        entry=bytearray(512);entry[0x10:0x50]=source[0x190:0x1d0]
        prepared=bytearray(512);prepared[0x50:0x90]=shifted[0x190:0x1d0]
        self.assertLess(eye_history(source,view,entry,prepared,.064,yaw)['matrixError'],1e-6)
        self.assertGreater(eye_history(source,view,entry,prepared,.064,-yaw)['matrixError'],.1)
        from analyze_eye_offset import asymmetric_camera
        asymmetric=asymmetric_camera(shifted)
        self.assertTrue(geometry_valid(camera_geometry(source,asymmetric,view,eye,.064,yaw,True)))
        self.assertFalse(geometry_valid(camera_geometry(source,asymmetric,view,eye,.064,yaw)))
        prepared[0x50:0x90]=asymmetric[0x190:0x1d0]
        self.assertLess(eye_history(source,view,entry,prepared,.064,yaw,True)['matrixError'],1e-6)
        asymmetric[0x148]^=1
        self.assertFalse(geometry_valid(camera_geometry(source,asymmetric,view,eye,.064,yaw,True)))

    def test_camera_origin_divides_homogeneous_w(self):
        import numpy as np
        def context(x,homogeneous):
            b=bytearray(512);v=np.eye(4,dtype='<f4');v[3,:]=[-x,-5,20,homogeneous]
            w=np.linalg.inv(v.astype(float)).astype('<f4');p=np.eye(4,dtype='<f4')
            for offset,m in ((0x70,w),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
            return b
        source=context(1000,1-2**-23);shifted=context(np.float32(1000)+np.float32(.064),1+2**-23)
        view=bytearray(144);view[0x8c]=1;struct.pack_into('<3f',view,0x40,1000,5,-20)
        eye=bytearray(view);eye[0x8c]=0;struct.pack_into('<f',eye,0x40,np.float32(1000)+np.float32(.064))
        result=camera_geometry(source,shifted,view,eye,.064)
        self.assertGreater(result['worldPositionError'],.0001)
        self.assertLess(result['renderedPositionError'],.0001)
        self.assertTrue(geometry_valid(result))
        bad=bytearray(shifted);struct.pack_into('<f',bad,0x15c,.1)
        self.assertFalse(camera_geometry(source,bad,view,eye,.064)['complete'])

    def test_sequence_rejects_broken_state_or_history_between_pairs(self):
        from analyze_eye_sequence import analyze as sequence
        from unittest.mock import patch
        import numpy as np
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);(run/'eye-sequence.request').write_text('2')
            lines=[];graphics=[];work=[]
            for index,key in enumerate(('9','10')):
                frame=12+index
                lines.extend([f'tid=7 frame={frame} replay begin id={key}',f'tid=7 frame={frame} replay end id={key}',
                    f'tid=7 frame={frame} sequence pair id={key} index={index+1} target=2 accepted=1'])
                graphics.extend([f'eye texture id={key} eye={e} captured=1' for e in (0,1)]+[f'eye pair id={key} ready=1'])
                work.extend([f'pair_gpu scene={key} healthy=1 dispatch0=4 dispatch1=4',
                    f'ao_eye scene={key} previous_eye={index} prepared=1',f'ao_eye scene={key} retained=1'])
                w=np.eye(4,dtype='<f4');w[3,0]=1+index
                p=np.diag([1.5,2,1,1]).astype('<f4')
                def camera(world):
                    b=bytearray(512);v=np.linalg.inv(world).astype('<f4')
                    for offset,m in ((0x70,world),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
                    return b
                center=camera(w);right=w.copy();right[3,0]+=np.float32(.064);shifted=camera(right)
                original=bytearray(144);original[0x8c]=1;struct.pack_into('<3f',original,0x40,*w[3,:3])
                view=bytearray(original);view[0x8c]=0;struct.pack_into('<3f',view,0x40,*right[3,:3])
                entry=bytearray(512);entry[0x10:0x50]=center[0x190:0x1d0]
                prepared=bytearray(entry);prepared[0x50:0x90]=shifted[0x190:0x1d0]
                for phase in ('scene-entry','scene-complete','replay-entry','replay-complete','replay-restored','eye-previous'):
                    for field,size in (('object',352),('packet',296),('render-data',512),('manager',512),('context',512),('view',144)):
                        b=(shifted if phase=='replay-complete' else center) if field=='context' else (view if phase=='replay-entry' else original) if field=='view' else (prepared if phase=='replay-entry' else entry) if field=='manager' else bytes(size)
                        (run/f'stage-{key}-{phase}-{field}.bin').write_bytes(b)
            (run/'engine.log').write_text('\n'.join(lines));(run/'trace.log').write_text('\n'.join(graphics));(run/'replay-work.txt').write_text('\n'.join(work))
            (run/'stage-9-replay-complete.raw').touch()
            # First-pair GPU evidence has its own tests; isolate cross-frame gates here.
            with patch('analyze_eye_sequence.analyze_eye',return_value={'accepted':True}):
                self.assertTrue(sequence(run)['accepted'])
                # The exact prior eye render must supply subsequent history;
                # a plausible matrix copied from another frame is insufficient.
                prior=(run/'stage-9-replay-complete-context.bin').read_bytes()
                previous_path=run/'stage-10-eye-previous-rendered-context.bin';previous_path.write_bytes(prior)
                prepared_path=run/'stage-10-replay-entry-manager.bin';original_prepared=prepared_path.read_bytes()
                owned_prepared=bytearray(original_prepared);owned_prepared[0x50:0x90]=prior[0x190:0x1d0];prepared_path.write_bytes(owned_prepared)
                (run/'eye-camera-history.request').touch()
                (run/'engine.log').write_text('\n'.join(lines+['tid=7 frame=13 eye history id=10 previous_frame=12 saved=1 mode=owned_previous']))
                self.assertTrue(sequence(run)['accepted'])
                corrupt=bytearray(prior);corrupt[0x190]^=1;previous_path.write_bytes(corrupt)
                self.assertFalse(sequence(run)['accepted'])
                previous_path.write_bytes(prior);prepared_path.write_bytes(original_prepared)
                self.assertFalse(sequence(run)['accepted'])
                (run/'eye-camera-history.request').unlink();(run/'engine.log').write_text('\n'.join(lines))
                pth=run/'stage-10-replay-restored-manager.bin';good=pth.read_bytes();bad=bytearray(good);bad[0x10]^=1;pth.write_bytes(bad)
                self.assertFalse(sequence(run)['accepted']);pth.write_bytes(good)
                (run/'eye-ao-audit.request').touch()
                self.assertFalse(sequence(run)['accepted'])
                (run/'eye-ao-audit.request').unlink()
                (run/'replay-work.txt').write_text('\n'.join(work).replace('previous_eye=1','previous_eye=0'))
                self.assertFalse(sequence(run)['accepted'])

    def test_sequence_count_and_missing_evidence_fail_closed(self):
        from analyze_eye_sequence import analyze as sequence
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run)
            (run/'eye-sequence.request').write_text('16')
            (run/'replay-work.txt').write_text('')
            result=sequence(run)
            self.assertFalse(result['accepted'])
            self.assertFalse(result['gates']['completeCount'])
            self.assertFalse(result['gates']['everyPair'])
            (run/'capture-cleanup.json').touch()
            with self.assertRaisesRegex(ValueError,'retained-evidence'): sequence(run)

    def test_eye_history_uses_previous_rotation_projection_and_native_identity(self):
        from analyze_eye_offset import eye_history
        import numpy as np
        camera=bytearray(512);view=bytearray(144);entry=bytearray(512);prepared=bytearray(512)
        # 90-degree camera yaw: eye translation is along world Z. A changing
        # current FOV is irrelevant; only this recorded previous VP is used.
        struct.pack_into('<3f',camera,0x70,0,0,1)
        struct.pack_into('<3f',view,0x40,20,5,100)
        vp=np.array([[0,0,1,1],[0,2,0,0],[-1.5,0,0,0],[150,-10,-20.1,-20]],dtype='<f4')
        camera[0x190:0x1d0]=entry[0x10:0x50]=vp.tobytes()
        expected=vp.copy();expected[3]-=float(np.float32(100.064)-np.float32(100))*vp[2]
        prepared[0x50:0x90]=expected.tobytes()
        result=eye_history(camera,view,entry,prepared,.064)
        self.assertTrue(result['complete'] and result['translated'] and result['sourceMatchesNativeHistory'])
        self.assertEqual(result['matrixError'],0)
        prepared[0x50:0x90]=vp.tobytes()
        self.assertGreater(eye_history(camera,view,entry,prepared,.064)['matrixError'],.09)
        entry[0x10]^=1
        self.assertFalse(eye_history(camera,view,entry,prepared,.064)['sourceMatchesNativeHistory'])
        self.assertFalse(eye_history(camera[:-1],view,entry,prepared,.064)['complete'])

    def test_geometry_rejects_shared_origin_error_that_cancels_in_delta(self):
        import numpy as np
        # Use binary-exact positions to isolate the cap: a shared camera error
        # cancels in the legacy center/eye delta, even with coherent W/V/VP.
        def context(position):
            result=bytearray(512);w=np.eye(4,dtype='<f4');w[3,:3]=position
            v=np.linalg.inv(w.astype(float)).astype('<f4');p=np.eye(4,dtype='<f4')
            for offset,m in ((0x70,w),(0x110,p),(0x150,v),(0x190,v@p)):
                result[offset:offset+64]=m.tobytes()
            return result
        view=bytearray(144);view[0x8c]=1
        eye=bytearray(view);eye[0x8c]=0;struct.pack_into('<f',eye,0x40,.0625)
        for axis in range(3):
            for sign in (-1,1):
                for error,accepted in ((2**-14,True),(2**-13,False)):
                    with self.subTest(axis=axis,sign=sign,error=error):
                        position=np.zeros(3);position[axis]=sign*error
                        source=context(position);shifted=context(position+np.array([.0625,0,0]))
                        result=camera_geometry(source,shifted,view,eye,.0625)
                        self.assertEqual(result['renderedPositionError'],0)
                        self.assertEqual(result['renderedRequestedPositionError'],error)
                        self.assertEqual(geometry_valid(result),accepted)

    def test_geometry_camera_translation_and_rejection_evidence(self):
        import numpy as np
        def context(x):
            b=bytearray(512);w=np.eye(4,dtype='<f4');w[3,:3]=[x,5,-20]
            v=np.linalg.inv(w).astype('<f4');p=np.eye(4,dtype='<f4');p[0,0]=1.5
            for offset,m in ((0x70,w),(0x110,p),(0x150,v),(0x190,v@p)):b[offset:offset+64]=m.tobytes()
            return b
        source=context(100); shifted=context(100.064)
        view=bytearray(144);struct.pack_into('<3f',view,0x40,100,5,-20);view[0x8c]=1
        eye=bytearray(view);struct.pack_into('<f',eye,0x40,100.064);eye[0x8c]=0
        result=camera_geometry(source,shifted,view,eye,.064)
        self.assertTrue(result['complete'] and result['projectionExact'] and result['viewOnlyPositionAndFlag'] and result['vpChanged'])
        self.assertLess(result['worldPositionError'],.0001)
        self.assertTrue(geometry_valid(result))
        # Opposite rounding residuals in W can exceed the displacement tolerance
        # even when the actual view matrices describe the requested shift.
        rounded_source=bytearray(source);rounded_shifted=bytearray(shifted)
        struct.pack_into('<f',rounded_source,0xa0,100-.00006)
        struct.pack_into('<f',rounded_shifted,0xa0,100.064+.00006)
        rounded=camera_geometry(rounded_source,rounded_shifted,view,eye,.064)
        self.assertGreater(rounded['worldPositionError'],.0001)
        self.assertTrue(geometry_valid(rounded))
        struct.pack_into('<f',rounded_shifted,0xa0,100.064+.00018)
        self.assertFalse(geometry_valid(camera_geometry(rounded_source,rounded_shifted,view,eye,.064)))
        # A pre-existing native inverse residual is not a replay displacement.
        struct.pack_into('<f',rounded_source,0xa0,100+.00012)
        struct.pack_into('<f',rounded_shifted,0xa0,100.064+.00012)
        self.assertTrue(geometry_valid(camera_geometry(rounded_source,rounded_shifted,view,eye,.064)))
        wrong_position=bytearray(eye);struct.pack_into('<f',wrong_position,0x40,100.064+.00002)
        self.assertFalse(geometry_valid(camera_geometry(source,shifted,view,wrong_position,.064)))
        # Correct-looking world/SceneView positions cannot mask wrong geometry V.
        wrong_view=bytearray(shifted);wrong_view[0x150:0x190]=source[0x150:0x190]
        self.assertFalse(geometry_valid(camera_geometry(source,wrong_view,view,eye,.064)))
        wrong_view[0x150:0x190]=bytes(64)
        self.assertFalse(camera_geometry(source,wrong_view,view,eye,.064)['complete'])
        self.assertFalse(camera_geometry(source[:-1],shifted,view,eye,.064)['complete'])
        self.assertFalse(camera_geometry(source,shifted,view,eye,float('nan'))['complete'])
        wrong=bytearray(eye);wrong[0x20]^=1
        self.assertFalse(camera_geometry(source,shifted,view,wrong,.064)['viewOnlyPositionAndFlag'])
        self.assertGreater(camera_geometry(source,source,view,eye,.064)['worldPositionError'],.06)
        wrong=bytearray(shifted);wrong[0x110]^=1
        self.assertFalse(camera_geometry(source,wrong,view,eye,.064)['projectionExact'])

    def test_eye_mode_cannot_be_accepted_as_identical_view(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory);fixture(run);(run/'eye-offset.request').touch()
            self.assertFalse(analyze(run)['accepted'])
            self.assertEqual(analyze(run)['mode'],'geometry-offset')

    def test_clip_trigger_requires_observation_and_repeated_transition(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/'wait-clip-transition.request').touch()
            self.assertFalse(analyze(run)['accepted'])
            for phase in ('scene-entry','replay-entry','scene-complete','replay-complete','replay-restored'):
                p=run/f'stage-9-{phase}-context.bin'; b=bytearray(p.read_bytes())
                struct.pack_into('<ff',b,0xe8,*((.1,15000) if phase.endswith('entry') else (.2,100)))
                p.write_bytes(b)
            p=run/'engine.log'; original=p.read_text()
            evidence='\ntid=7 frame=11 capture trigger observed=clip_transition context=1234\n'
            evidence+='tid=7 frame=12 replay clip id=9 clip_changed=1\n'
            p.write_text(original+evidence)
            self.assertTrue(analyze(run)['accepted'])
            for bad in (evidence.replace('clip_changed=1','clip_changed=0'),evidence.replace('frame=11','frame=10'),evidence+evidence):
                p.write_text(original+bad)
                self.assertFalse(analyze(run)['accepted'])

    def test_camera_input_and_completed_state_are_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            # A native clip subpass may change the completed context; history
            # holds the incoming matrix. Both views must reproduce that change.
            for phase in ('scene-complete','replay-complete','replay-restored'):
                p=run/f'stage-9-{phase}-context.bin'; b=bytearray(p.read_bytes())
                struct.pack_into('<f',b,0x198,.25); p.write_bytes(b)
                p=run/f'stage-9-{phase}-object.bin'; b=bytearray(p.read_bytes())
                struct.pack_into('<f',b,0x40,.2); p.write_bytes(b)
            p=run/'stage-10-scene-complete-context.bin'; b=bytearray(p.read_bytes())
            struct.pack_into('<f',b,0x198,.25); p.write_bytes(b)
            self.assertTrue(analyze(run)['accepted'])
            for phase,field,offset in (('replay-entry','context',0x198),('replay-restored','context',0x198),
                                       ('replay-entry','object',0x40),('replay-restored','object',0x40),
                                       ('replay-complete','manager',0x58)):
                p=run/f'stage-9-{phase}-{field}.bin'; good=p.read_bytes(); bad=bytearray(good)
                bad[offset]^=1; p.write_bytes(bad)
                self.assertFalse(analyze(run)['accepted'],(phase,field,offset))
                p.write_bytes(good)

    def test_fresh_color_requires_distinct_exact_seeds_and_matching_draw(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/'fresh-replay-color.request').touch()
            log=run/'trace.log'; original=log.read_text()
            rows='\n'.join(f'fresh_color name=work-9-pass{p}-slot6-draw8 pass={p} applied=1 saved=1 seed={seed}'
                           for p,seed in enumerate(('magenta','green')))
            log.write_text(original+'\n'+rows)
            self.assertFalse(analyze(run)['accepted'])
            paths=[run/f'work-9-pass{p}-slot6-draw8-fresh-seed.raw' for p in (0,1)]
            for path,value in zip(paths,(0xfff003ff,0xc00ffc00)):
                path.write_bytes(struct.pack('<4I',2,2,24,1)+struct.pack('<4I',*([value]*4)))
            self.assertTrue(analyze(run)['accepted'])
            good=paths[1].read_bytes()
            for bad in (paths[0].read_bytes(),good[:-1],struct.pack('<4I',4,1,24,1)+good[16:],good[:-4]+bytes(4)):
                paths[1].write_bytes(bad)
                self.assertFalse(analyze(run)['accepted'])
            paths[1].write_bytes(good)
            for bad in (rows+'\n'+rows,rows.replace('saved=1','saved=0'),rows.replace('pass1-slot6-draw8','pass1-slot6-draw9'),rows.replace('work-9','work-10')):
                log.write_text(original+'\n'+bad)
                self.assertFalse(analyze(run)['accepted'])

    def test_unused_shadow_bindings_require_both_passes_and_restoration(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/"normalize-shadow-bindings.request").touch()
            self.assertFalse(analyze(run)["accepted"])
            good="\n".join(f"shadow_unused pass={p} ordinal=12 {flag}=1" for p in (0,1) for flag in ("applied","restored"))
            log=run/"replay-work.txt"; log.write_text(good)
            self.assertTrue(analyze(run)["accepted"])
            for bad in (good.replace("restored=1","restored=0"), good.replace("pass=1","pass=0"),
                        good.replace("pass=1 ordinal=12","pass=1 ordinal=13"), good+"\n"+good):
                log.write_text(bad); self.assertFalse(analyze(run)["accepted"])

    def test_pruned_capture_does_not_replace_historical_result(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory)
            (run/"capture-cleanup.json").write_text('{}')
            summary=run/"replay-summary.json"; summary.write_text('retained original result')
            with self.assertRaisesRegex(ValueError, "retained-evidence.json"):
                analyze(run)
            self.assertEqual(summary.read_text(), 'retained original result')

    def test_shadow_sampler_requires_matching_draw_and_restoration(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/"isolate-shadow-sampler.request").touch()
            self.assertFalse(analyze(run)["accepted"])
            good="shadow_sampler ordinal=12 captured=1\nshadow_sampler ordinal=12 applied=1 changed=1\nshadow_sampler ordinal=12 restored=1"
            log=run/"replay-work.txt"
            log.write_text(good); self.assertTrue(analyze(run)["accepted"])
            for bad in (good.replace("restored=1","restored=0"),
                        good.replace("ordinal=12 restored","ordinal=13 restored"),
                        good+"\nshadow_sampler ordinal=12 refused=draw_identity",
                        good+"\nshadow_sampler ordinal=12 restored=1"):
                log.write_text(bad); self.assertFalse(analyze(run)["accepted"])

    def test_requested_vpl_sort_needs_both_passes_and_content_proof(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/"sort-vpl.request").touch()
            self.assertFalse(analyze(run)["accepted"])
            (run/"replay-work.txt").write_text("vpl_order pass=0 applied=1 restored=1\nvpl_order pass=1 applied=1 restored=1")
            self.assertFalse(analyze(run)["accepted"])
            header=struct.pack("<6I",16,32,57,1,1,2)
            for p in (0,1):
                (run/f"work-9-pass{p}-compute4-uav1-native.texture").write_bytes(header+struct.pack("<512H",3,1,2,0,*([17]*508)))
                (run/f"work-9-pass{p}-compute4-uav1.texture").write_bytes(header+struct.pack("<512H",1,2,3,0,*([17]*508)))
            self.assertTrue(analyze(run)["accepted"])
            (run/"replay-work.txt").write_text("vpl_order pass=0 applied=1 restored=1\nvpl_order pass=1 applied=1 restored=0")
            self.assertFalse(analyze(run)["accepted"])

    def test_source_precision_and_requested_controls_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            run=Path(directory); fixture(run)
            (run/"replay-work.request").touch()
            self.assertFalse(analyze(run)["accepted"])
            header=struct.pack("<4I",2,2,24,1)
            a=run/"stage-9-scene-complete.raw"; b=run/"stage-9-replay-complete.raw"
            a.write_bytes(header+struct.pack("<4I",1,0,0,0)); b.write_bytes(header+struct.pack("<4I",2,0,0,0))
            self.assertTrue(analyze(run)["images"]["equal"])
            self.assertFalse(analyze(run)["accepted"])
            b.write_bytes(a.read_bytes()); self.assertTrue(analyze(run)["accepted"])
            (run/"isolate-models.request").touch(); self.assertFalse(analyze(run)["accepted"])
            (run/"replay-work.txt").write_text("model_isolation ordinal=1 captured=1\nmodel_isolation ordinal=1 applied=1\nmodel_isolation ordinal=2 restored=1")
            self.assertFalse(analyze(run)["accepted"])
            (run/"replay-work.txt").write_text("model_isolation ordinal=1 captured=1\nmodel_isolation ordinal=1 applied=1\nmodel_isolation ordinal=1 restored=1")
            self.assertTrue(analyze(run)["accepted"])
            (run/"restore-compute-state.request").touch(); self.assertFalse(analyze(run)["accepted"])
            with (run/"replay-work.txt").open("a") as f:
                f.write("\ncompute_state prepared=1\ncompute_state restored=1\nao_history captured=1\nao_history prepared=1\nao_history completed_restored=1")
            self.assertFalse(analyze(run)["accepted"])
            texture=struct.pack("<6I",2,2,49,1,1,2)+bytes(8)
            for phase in ("scene-complete","replay-restored"):
                (run/f"stage-9-{phase}-ao.texture").write_bytes(texture)
            self.assertTrue(analyze(run)["accepted"])
            (run/"stage-9-replay-restored-ao.texture").write_bytes(texture[:-1]+b"\1")
            self.assertFalse(analyze(run)["accepted"])

    def test_complete_and_corrupt_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            fixture(run)
            self.assertTrue(analyze(run)["accepted"])
            path = run / "stage-9-replay-complete.ppm"
            path.write_bytes(b"P6\n2 2\n255\n" + b"\1" + bytes(11))
            self.assertFalse(analyze(run)["accepted"])
            self.assertEqual(analyze(run)["images"]["changedPixelFraction"], .25)
            path.unlink()
            self.assertFalse(analyze(run)["gates"]["identicalReadback"])
            fixture(run)
            path = run / "stage-10-scene-entry-manager.bin"
            data = bytearray(path.read_bytes()); data[0x10] = 1; path.write_bytes(data)
            self.assertFalse(analyze(run)["gates"]["followingHistoryContinuity"])
            fixture(run)
            path = run / "engine.log"
            path.write_text(path.read_text().replace("tid=7 frame=12 replay end", "tid=8 frame=12 replay end"))
            self.assertFalse(analyze(run)["gates"]["matchingReturn"])
            fixture(run)
            path.write_text(path.read_text().replace("tid=7 frame=13", "tid=8 frame=13"))
            self.assertFalse(analyze(run)["gates"]["followingHistoryContinuity"])
            fixture(run)
            (run / "stage-9-replay-complete-manager.bin").write_bytes(bytes(8))
            self.assertFalse(analyze(run)["gates"]["snapshotsComplete"])
            fixture(run)
            path.write_text(path.read_text().replace("replay end", "incomplete replay"))
            self.assertFalse(analyze(run)["accepted"])


if __name__ == "__main__":
    unittest.main()
