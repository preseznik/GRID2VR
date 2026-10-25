import sys
import tempfile
import unittest
from pathlib import Path
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from analyze_visibility import analyze,box_geometry


def volume(radius):
    data=np.zeros((14,4),dtype='<f4')
    for axis in range(3):
        for side,normal in enumerate((-1,1)):
            data[2*axis+side,axis]=normal;data[2*axis+side,3]=radius
    for corner in range(8):
        data[6+corner,:3]=[radius if corner&(1<<a) else -radius for a in range(3)]
    return data.tobytes()


def fixture(run):
    original=volume(100);radius=np.nextafter(np.float32(np.sqrt(30000)+8),np.float32(np.inf))
    expanded=volume(radius);view=bytes(144)
    matrix=np.diag(np.array([1/radius,1/radius,-1/radius,1],dtype='<f4')).tobytes()
    context=np.zeros(128,dtype='<f4')
    context[0x150//4:0x190//4]=np.eye(4,dtype='<f4').ravel()
    context[0x110//4:0x150//4]=[1,0,0,0,0,1,0,0,0,0,-1.002,-1,0,0,-.2002,0]
    (run/'eye-sequence.request').write_text('2')
    lines=['tid=7 frame=3 visibility hook installed=1 expansion=1']
    for n in range(2):
        key,prepared,pair,frame=10+n,20+n,30+n,4+n
        files={('before-jobs','visibility-view'):view,('before-jobs','render-view'):view,
            ('before-jobs','visibility-volume'):original,('visibility-original','volume'):original,
            ('visibility-expanded','volume'):expanded,('visibility-expanded','matrix'):matrix,
            ('visibility-restored','volume'):original}
        for (phase,field),data in files.items():(run/f'stage-{key}-{phase}-{field}.bin').write_bytes(data)
        for phase in ('prepare-before','prepare-after'):
            for field,data in [('visibility-view',view),('render-view',view),('visibility-volume',expanded)]:
                (run/f'stage-{prepared}-{phase}-{field}.bin').write_bytes(data)
        (run/f'stage-{pair}-replay-complete-context.bin').write_bytes(context.tobytes())
        lines.extend([f'tid=7 frame={frame} visibility before_jobs id={key} valid=1 applied=1 caller_rva=0x1176b4',
            f'tid=7 frame={frame} visibility snapshot id={prepared} phase=prepare-before',
            f'tid=7 frame={frame} sequence pair id={pair} accepted=1',
            f'tid=7 frame={frame} visibility restore id={key} restored=1 reason=scene_complete'])
    (run/'engine.log').write_text('\n'.join(lines))


class VisibilityTests(unittest.TestCase):
    def test_captured_asymmetric_right_eye_needs_larger_bounds(self):
        fixtures=Path(__file__).parent/'fixtures'
        original=(fixtures/'native-stereo-visibility-volume.bin').read_bytes()
        view=(fixtures/'native-stereo-visibility-view.bin').read_bytes()
        render=(fixtures/'native-stereo-render-view.bin').read_bytes()
        camera=(fixtures/'native-stereo-right-main-context.bin').read_bytes()
        center=np.frombuffer(view,'<f4',3,0x40)
        def bounds(radius):
            radius=np.float32(radius)
            data=np.frombuffer(volume(radius),'<f4').reshape(14,4).copy()
            data[:6,3]-=data[:6,:3]@center;data[6:,:3]+=center
            matrix=np.diag(np.array([1/radius,1/radius,-1/radius,1],dtype='<f4'))
            matrix[3,:3]=-center*np.diag(matrix)[:3]
            return data.tobytes(),matrix.tobytes()
        old,matrix=bounds(2196.847412109375)
        result=box_geometry(original,old,view,matrix,camera)
        self.assertTrue(result['gates']['matrixExact'])
        self.assertLess(result['posedFarCornerMargin'],-50)
        self.assertFalse(result['accepted'])
        expanded,matrix=bounds(2377.598876953125)
        frusta=((-1,.8,-.7,.6),(-.8,1,-.7,.6))
        result=box_geometry(original,expanded,view,matrix,camera,render,frusta)
        self.assertTrue(result['accepted'],result)
        self.assertGreater(result['posedFarCornerMargin'],100)
        self.assertFalse(box_geometry(original,old,view,matrix,camera,render,frusta)['accepted'])
        self.assertFalse(box_geometry(original,expanded,view,matrix,camera,bytes(144),frusta)['accepted'])
        self.assertFalse(box_geometry(original,expanded,view,matrix,camera,render,((float('nan'),1,-1,1),))['accepted'])

    def test_complete_and_restoration_corruption(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run)
            self.assertTrue(analyze(run)['accepted'])
            (run/'stage-10-visibility-restored-volume.bin').write_bytes(bytes(224))
            self.assertFalse(analyze(run)['accepted'])
            fixture(run)
            log=run/'engine.log';log.write_text(log.read_text().replace('reason=scene_complete','reason=present_fallback',1))
            self.assertFalse(analyze(run)['accepted'])
            fixture(run)
            lines=log.read_text().splitlines();lines[1],lines[2]=lines[2],lines[1];log.write_text('\n'.join(lines))
            self.assertFalse(analyze(run)['accepted'])

    def test_missing_or_invalid_geometry(self):
        self.assertFalse(box_geometry(b'',b'',b'',b'',b'')['accepted'])
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run)
            p=run/'stage-10-visibility-expanded-volume.bin';data=bytearray(p.read_bytes());data[16:32]=data[:16];p.write_bytes(data)
            self.assertFalse(analyze(run)['accepted'])
            fixture(run)
            (run/'stage-30-replay-complete-context.bin').write_bytes(bytes(512))
            self.assertFalse(analyze(run)['accepted'])
            fixture(run)
            p=run/'stage-20-prepare-before-render-view.bin';data=bytearray(p.read_bytes());data[0x40]=1;p.write_bytes(data)
            self.assertFalse(analyze(run)['accepted'])
            fixture(run)
            log=run/'engine.log';log.write_text(log.read_text().replace('tid=7 frame=4 visibility restore','tid=8 frame=4 visibility restore'))
            self.assertFalse(analyze(run)['accepted'])


if __name__=='__main__':unittest.main()
