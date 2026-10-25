import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from analyze_xr_session import analyze


def fixture(run):
    (run/'xr-session.request').write_text('C:\\runtime\\steamxr_win32.json')
    (run/'trace.log').write_text('tid=7 device=1234 feature_level=0xb100\n')
    lines=['tid=7 frame=0 runtime_selection process_only=1 environment_restored=1',
        'tid=7 frame=0 runtime name=SteamVR/test',
        'tid=7 frame=0 device game=1234 compatible=1 luid=0:17 required=0:17']
    for op in ('create_instance','runtime_properties','get_system','graphics_requirements','create_game_session','create_local_space','create_head_space','begin_session'):
        lines.append(f'tid=7 frame=0 xr operation={op} result=0')
    for frame in (1,2):
        for op in ('wait_frame','begin_frame','locate_views','locate_head'):
            lines.append(f'tid=7 frame={frame} xr operation={op} result=0')
        lines.append(f'tid=7 frame={frame} sample tracked=1 should_render=1 display_time={frame*1000}')
        for kind,eye,x in [('head',0,0),('eye',0,-.032),('eye',1,.032)]:
            lines.append(f'tid=7 frame={frame} pose kind={kind} eye={eye} qx=0 qy=0 qz=0 qw=1 x={x} y=0 z=0')
        for eye in (0,1):lines.append(f'tid=7 frame={frame} fov eye={eye} left=-.8 right=.8 down=-.7 up=.7')
        lines.append(f'tid=7 frame={frame} xr operation=end_frame_zero_layers result=0')
    for op in ('destroy_head_space','destroy_local_space','destroy_session','destroy_instance'):
        lines.append(f'tid=7 frame=2 xr operation={op} result=0')
    lines.append('tid=7 frame=2 session shutdown=1 samples=2 failed=0 images_submitted=0')
    (run/'openxr-session.log').write_text('\n'.join(lines))


class XrEvidenceTests(unittest.TestCase):
    def test_selected_runtime_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run)
            log=run/'openxr-session.log';steam=log.read_text()
            request=run/'xr-session.request'
            request.write_text('C:\\runtime\\virtualdesktop-openxr-32.json')
            self.assertFalse(analyze(run)['accepted'])
            log.write_text(steam.replace('SteamVR/test','VirtualDesktopXR'))
            self.assertTrue(analyze(run)['accepted'])
            for name in ('virtualdesktop-openxr.json','steamxr_win32.json','unknown.json'):
                request.write_text('C:\\runtime\\'+name)
                self.assertFalse(analyze(run)['accepted'])

    def test_projection_requires_accepted_restored_pair_and_same_time(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run);(run/'xr-projection.request').write_text('1280 720 1')
            log=run/'openxr-session.log'
            text=log.read_text().replace('end_frame_zero_layers','end_frame_projection').replace('images_submitted=0','images_submitted=2')
            text+='\ntid=7 frame=0 images initialized=1\n'
            text+='\n'.join(f'tid=7 frame={f} images submitted=1 display_time={f*1000}' for f in (1,2))
            log.write_text(text)
            trace=run/'trace.log';trace.write_text(trace.read_text()+'\n'.join(f'tid=7 openxr pair frame={f} scene={f+9} accepted=1 ended=1' for f in (1,2)))
            (run/'engine.log').write_text('\n'.join(f'tid=7 frame={f} views complete id={f+9} accepted=1 visibility_restored=1' for f in (1,2)))
            self.assertTrue(analyze(run)['accepted'])
            # Startup can close untracked and tracked zero-layer frames before
            # the first image. The same fallback during rendering must fail.
            shift=lambda s:s.replace('frame=2','frame=4').replace('frame=1','frame=3')
            warm=[]
            for frame,tracked in ((1,0),(2,1)):
                warm.extend(f'tid=7 frame={frame} xr operation={op} result=0' for op in ('wait_frame','begin_frame','locate_views','locate_head'))
                warm.append(f'tid=7 frame={frame} sample tracked={tracked} should_render={tracked} display_time={frame*200}')
                warm.append(f'tid=7 frame={frame} xr operation=end_frame_zero_layers result=0')
            warm_text=shift(text).replace('samples=2','samples=3').replace('tid=7 frame=3 xr operation=wait_frame','\n'.join(warm)+'\ntid=7 frame=3 xr operation=wait_frame')
            trace_text=trace.read_text();engine=run/'engine.log';engine_text=engine.read_text()
            log.write_text(warm_text);trace.write_text(shift(trace_text));engine.write_text(shift(engine_text))
            self.assertTrue(analyze(run)['accepted'])
            self.assertEqual(analyze(run)['observations']['warmupCount'],2)
            log.write_text(warm_text.replace('frame=1 ','frame=5 '));self.assertFalse(analyze(run)['accepted'])
            log.write_text(text);trace.write_text(trace_text);engine.write_text(engine_text)
            for old,new in [('submitted=1','submitted=0'),('display_time=2000','display_time=1000'),('images initialized=1','images initialized=0'),('frame=2 images','frame=3 images')]:
                log.write_text(text.replace(old,new));self.assertFalse(analyze(run)['accepted'])
            log.write_text(text);engine=run/'engine.log';engine.write_text(engine.read_text().replace('visibility_restored=1','visibility_restored=0'))
            self.assertFalse(analyze(run)['accepted'])

    def test_render_eligibility_loss_is_explicit(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run);log=run/'openxr-session.log'
            log.write_text(log.read_text().replace('frame=2 sample tracked=1 should_render=1','frame=2 sample tracked=1 should_render=0'))
            report=analyze(run)
            self.assertFalse(report['accepted'])
            self.assertEqual(report['observations']['trackedSampleCount'],2)
            self.assertEqual(report['observations']['renderEligibleCount'],1)
            self.assertFalse(report['observations']['allSamplesRenderEligible'])

    def test_complete_and_corrupted_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run)
            self.assertTrue(analyze(run)['accepted'])
            for old,new in [('game=1234','game=5678'),('environment_restored=1','environment_restored=0'),
                ('tracked=1','tracked=0'),('should_render=1','should_render=0'),('display_time=2000','display_time=1000'),
                ('qx=0 qy=0 qz=0 qw=1','qx=0 qy=0 qz=0 qw=0'),('x=0.032','x=-0.032'),('left=-.8','left=.8'),
                ('operation=end_frame_zero_layers','operation=missing'),('operation=destroy_session','operation=missing'),
                ('images_submitted=0','images_submitted=1'),('tid=7 frame=2','tid=8 frame=2')]:
                fixture(run);log=run/'openxr-session.log';log.write_text(log.read_text().replace(old,new))
                with self.subTest(old=old):self.assertFalse(analyze(run)['accepted'])

    def test_inactive_tracking_still_closes_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            run=Path(folder);fixture(run);log=run/'openxr-session.log'
            lines=[line for line in log.read_text().splitlines() if ' pose ' not in line and ' fov ' not in line]
            log.write_text('\n'.join(lines).replace('tracked=1','tracked=0').replace('should_render=1','should_render=0').replace('samples=2','samples=0'))
            report=analyze(run)
            self.assertFalse(report['accepted'])
            self.assertTrue(all(report['gates'][k] for k in ('frameLoop','lifecycle','shutdown')))
            self.assertFalse(report['gates']['poseData'])


if __name__=='__main__':unittest.main()
