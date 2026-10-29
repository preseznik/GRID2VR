import re
import json
import subprocess
import sys
import tempfile
import unittest
import hashlib
import shutil
import zipfile
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from package_stage import SCRIPTS, DATA_FILES, copy, stage_powershell
import release_assets as assets
import publish_release as publisher


class PackageDependencies(unittest.TestCase):
    def test_powershell_shares_frameworks_but_keeps_compiler_and_app_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);source=root/'source';source.mkdir()
            frameworks=[dict(name=name,version='10.0.13') for name in ('Microsoft.NETCore.App','Microsoft.WindowsDesktop.App')]
            (source/'pwsh.runtimeconfig.json').write_text(json.dumps(dict(runtimeOptions=dict(includedFrameworks=frameworks))))
            packs={f"runtimepack.{f['name']}.Runtime.win-x64/10.0.13":dict(runtime={f['name']+'.dll':{}}) for f in frameworks}
            dependencies={name.rsplit('/',1)[0]:'10.0.13' for name in packs}
            deps=dict(targets={'portable':{'pwsh/1':dict(dependencies=dependencies.copy())},
                'windows':{'pwsh/1':dict(dependencies=dependencies.copy()),**packs,'Application/1':dict(runtime={'Application.dll':{}})}},
                libraries={**{name:{} for name in packs},'Application/1':{}})
            (source/'pwsh.deps.json').write_text(json.dumps(deps))
            for name in ['Application.dll','pwsh.dll','Microsoft.CodeAnalysis.CSharp.dll','ref/System.Runtime.dll',
                         'Modules/PSReadLine/unused.dll','Modules/Microsoft.PowerShell.Utility/required.dll',
                         'Microsoft.NETCore.App.dll','Microsoft.WindowsDesktop.App.dll']:
                target=source/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(b'fixture')
            with patch('package_stage.package_apphost') as host:
                stage_powershell(source,root/'package','10.0.13')
            host.assert_called_once()
            result=root/'package'
            for name in ['Application.dll','pwsh.dll','Microsoft.CodeAnalysis.CSharp.dll','ref/System.Runtime.dll','Modules/Microsoft.PowerShell.Utility/required.dll']:
                self.assertTrue((result/name).is_file(),name)
            for name in ['Microsoft.NETCore.App.dll','Microsoft.WindowsDesktop.App.dll','Modules/PSReadLine']:
                self.assertFalse((result/name).exists(),name)
            options=json.loads((result/'pwsh.runtimeconfig.json').read_text())['runtimeOptions']
            self.assertEqual(options['frameworks'],frameworks)
            self.assertNotIn('includedFrameworks',options)
            result_deps=json.loads((result/'pwsh.deps.json').read_text())
            self.assertEqual(result_deps['targets']['windows']['Application/1'],deps['targets']['windows']['Application/1'])
            for target in result_deps['targets'].values():
                self.assertEqual(target['pwsh/1']['dependencies'],{})
                self.assertFalse(set(target)&set(packs))

    def test_powershell_rejects_a_mismatched_private_runtime(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            frameworks=[dict(name=name,version='10.0.12') for name in ('Microsoft.NETCore.App','Microsoft.WindowsDesktop.App')]
            (root/'pwsh.runtimeconfig.json').write_text(json.dumps(dict(runtimeOptions=dict(includedFrameworks=frameworks))))
            with self.assertRaisesRegex(AssertionError,'versions differ'):
                stage_powershell(root,root/'package','10.0.13')

    def test_packaged_catalog_has_vehicle_categories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            copy(ROOT/'tools/session_config.py', root/'tools/session_config.py')
            copy(ROOT/'tools/game_executable.py', root/'tools/game_executable.py')
            copy(ROOT/'tools/cockpit_dlc.py', root/'tools/cockpit_dlc.py')
            copy(ROOT/'tools/data/cockpit-dlc.json', root/'tools/data/cockpit-dlc.json')
            for name in DATA_FILES: copy(ROOT/'tools/data'/name, root/'tools/data'/name)
            car = root/'game/cars/models/foc'
            (car/'livery_user').mkdir(parents=True)
            (car/'cameras.xml').write_bytes(b'camera')
            (car/'foc_highLOD.pssg').write_bytes(b'mesh')
            (root/'game/grid2_avx.exe').write_bytes(b'fixture')
            output = subprocess.check_output([sys.executable, '-B', str(root/'tools/session_config.py'),
                '--game-root', str(root/'game'), 'catalog'], text=True)
            self.assertEqual(json.loads(output)['cars'][0]['className'], 'Hot hatches')

    def test_shipped_scripts_include_their_local_helpers(self):
        shipped = {name + '.ps1' for name in SCRIPTS}
        for name in shipped:
            source = (ROOT / 'tools' / name).read_text(encoding='utf-8-sig')
            for literal in re.findall(r'''['"]([^'"\r\n]*\.ps1)['"]''', source):
                dependency = literal.replace('\\', '/').rsplit('/', 1)[-1]
                if (ROOT / 'tools' / dependency).is_file():
                    self.assertIn(dependency, shipped, f'{name} requires {dependency}')


class FakeGithub:
    """Models drafts being absent from the by-tag endpoint, plus upload failures."""
    def __init__(self, plan, out):
        self.plan = plan
        self.out = out
        self.remote = None
        self.tag_commit = None
        self.calls = []
        self.fail_upload = None
        self.bad_digest = None
        self.omit_digest = False
        self.payloads = {}

    def api(self, endpoint, missing=False):
        if '/releases/tags/' in endpoint:
            return self.remote if self.remote and not self.remote['draft'] else None
        if '/releases?' in endpoint:
            return [self.remote] if self.remote else []
        if '/releases/' in endpoint:
            return self.remote
        if endpoint.endswith('/commits/'+self.plan['revision']):
            return {'sha':self.plan['revision']}
        if endpoint.endswith('/commits/'+self.plan['tag']):
            return {'sha':self.tag_commit} if self.tag_commit else None
        raise AssertionError(endpoint)

    def release(self, *args):
        self.calls.append(args)
        self.assert_no_clobber(args)
        command, tag = args[:2]
        assert tag == self.plan['tag']
        if command == 'create':
            self.remote = dict(id=123, tag_name=tag, target_commitish=args[args.index('--target')+1],
                               draft=True, prerelease='--prerelease' in args,
                               body=(self.out/'ReleaseNotes.md').read_text(encoding='utf-8'), assets=[])
        elif command == 'upload':
            path = Path(args[2])
            if path.name == self.fail_upload:
                raise RuntimeError('simulated interrupted upload')
            item = assets.asset(path, tag)
            self.payloads[path.name] = path.read_bytes()
            self.remote['assets'].append(dict(name=path.name, size=item['size'], state='uploaded',
                digest=None if self.omit_digest else 'sha256:'+('0'*64 if path.name == self.bad_digest else item['sha256']),
                browser_download_url=item['url']))
        elif command == 'edit':
            assert '--draft=false' in args
            assert len(self.remote['assets']) == 4
            self.remote['draft'] = False
        elif command == 'download':
            name = args[args.index('--pattern')+1]
            (Path(args[args.index('--dir')+1])/name).write_bytes(self.payloads[name])
        else:
            raise AssertionError(args)

    @staticmethod
    def assert_no_clobber(args):
        assert '--clobber' not in args


class ReleasePublication(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.out = self.root/'release'
        self.catalog_path = self.root/'tools/data/cockpit-texture-pack.json'
        self.catalog_path.parent.mkdir(parents=True)
        self.pack_version = '0.2.0-preview.2'
        self.pack = self.root/f'GRID2VR-CockpitTextures-{self.pack_version}.zip'
        contents = {'pack.json':b'{}', 'textures/cat.dds':b'artwork'}
        with zipfile.ZipFile(self.pack, 'w') as archive:
            for name, value in contents.items():
                archive.writestr(f'cockpit-textures/{self.pack_version}/'+name, value)
        self.catalog = dict(schema=1, version=self.pack_version, zipBytes=self.pack.stat().st_size,
                            zipSha256=assets.sha(self.pack), files={n:hashlib.sha256(v).hexdigest() for n,v in contents.items()})
        assets.write(self.catalog_path, self.catalog)
        self.native = self.root/'build/win32/bin/RelWithDebInfo/d3d11.dll'
        self.native.parent.mkdir(parents=True)
        self.native.write_bytes(('GRID2VR/addons/cockpit-textures/'+self.pack_version).encode('utf-16-le')+
                                self.catalog['files']['textures/cat.dds'].encode('ascii')+assets.NATIVE_LOD_MODE)

    def prepare(self, version='0.1.10', channel='Experimental', out=None):
        return assets.prepare(self.root, out or self.out, version, channel, 'a'*40, self.pack)

    def complete(self, channel='Experimental'):
        frozen = self.prepare(channel=channel)
        for source, destination in [(self.catalog_path, 'tools/data/cockpit-texture-pack.json'),
                                    (self.native, 'payload/d3d11.dll')]:
            target = self.out/'stage/GRID2VR'/destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        setup = self.out/'GRID2VR-0.1.10-Setup.exe'; setup.write_bytes(b'installer')
        portable = self.out/'GRID2VR-0.1.10.zip'; portable.write_bytes(b'portable')
        (self.out/'ReleaseNotes.md').write_text('Improved cockpits \u2014 all cars.\n', encoding='utf-8')
        package = dict(version=frozen['version'], channel=channel, releaseTag=frozen['tag'], cockpitTextures=frozen['cockpitTextures'])
        plan = assets.finalize(self.out, package, setup, portable)
        return plan, FakeGithub(plan, self.out)

    def test_missing_corrupt_or_internally_mismatched_pack_stops_packaging(self):
        original = self.pack.read_bytes()
        self.pack.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.prepare()
        self.pack.write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'Changed'):
            self.prepare()
        self.pack.write_bytes(original)
        self.catalog['files']['textures/cat.dds'] = '0'*64
        assets.write(self.catalog_path, self.catalog)
        with self.assertRaisesRegex(ValueError, 'Changed texture ZIP entry'):
            self.prepare()
        self.assertFalse(self.out.exists())

    def test_stale_native_renderer_is_rejected(self):
        self.native.write_bytes(b'old renderer')
        with self.assertRaisesRegex(ValueError, 'rebuild'):
            self.prepare()

    def test_native_without_nearby_lod_fix_is_rejected(self):
        self.native.write_bytes(self.native.read_bytes().replace(assets.NATIVE_LOD_MODE, b'grid2vr_opponent_lod_mode=strict_ordinary'))
        with self.assertRaisesRegex(ValueError, 'nearby-opponent LOD'):
            self.prepare()
        self.assertFalse((self.out/'release-inputs.json').exists())

    def test_pinned_candidate_is_used_instead_of_default_native(self):
        candidate = self.root/'accepted.dll'
        candidate.write_bytes(self.native.read_bytes()+b'accepted camera controls')
        digest = assets.sha(candidate)
        frozen = assets.prepare(self.root, self.out, '0.1.10', 'Experimental', 'a'*40, self.pack, candidate, digest)
        self.assertEqual(frozen['nativeSha256'], digest)
        self.assertEqual(assets.check_inputs(self.root, self.out, '0.1.10', 'Experimental', candidate), frozen)
        with self.assertRaisesRegex(ValueError, 'renderer changed'):
            assets.check_inputs(self.root, self.out, '0.1.10', 'Experimental')
        candidate.write_bytes(candidate.read_bytes()+b'changed after acceptance')
        with self.assertRaisesRegex(ValueError, 'renderer changed'):
            assets.check_inputs(self.root, self.out, '0.1.10', 'Experimental', candidate)

    def test_native_override_requires_matching_pin_before_creating_output(self):
        for native, digest, message in [(self.native, None, 'supplied together'),
                                        (None, assets.sha(self.native), 'supplied together'),
                                        (self.native, 'invalid', 'Invalid native'),
                                        (self.native, '0'*64, 'requested SHA-256')]:
            with self.subTest(native=native, digest=digest), self.assertRaisesRegex(ValueError, message):
                assets.prepare(self.root, self.out, '0.1.10', 'Experimental', 'a'*40, self.pack, native, digest)
            self.assertFalse(self.out.exists())

    def test_finalization_rejects_a_different_staged_native(self):
        self.complete()
        staged = self.out/'stage/GRID2VR/payload/d3d11.dll'
        staged.write_bytes(b'another native build')
        frozen = assets.read(self.out/'release-inputs.json')
        package = dict(version=frozen['version'], channel=frozen['channel'], releaseTag=frozen['tag'], cockpitTextures=frozen['cockpitTextures'])
        with self.assertRaisesRegex(ValueError, 'Packaged native renderer differs'):
            assets.finalize(self.out, package, self.out/'GRID2VR-0.1.10-Setup.exe', self.out/'GRID2VR-0.1.10.zip')

    def test_each_core_build_owns_its_pack_url(self):
        older = self.prepare()
        newer = self.prepare(version='0.1.11', out=self.root/'next')
        self.assertIn('/v0.1.10/', older['cockpitTextures']['url'])
        self.assertIn('/v0.1.11/', newer['cockpitTextures']['url'])
        self.assertEqual(older['cockpitTextures']['sha256'], newer['cockpitTextures']['sha256'])
        self.assertEqual(assets.read(self.out/'release-inputs.json'), older)

    def test_changed_catalog_or_native_during_build_is_rejected(self):
        self.prepare()
        original = self.catalog_path.read_bytes()
        self.catalog_path.write_bytes(original+b' ')
        with self.assertRaisesRegex(ValueError, 'catalog changed'):
            assets.check_inputs(self.root, self.out, '0.1.10', 'Experimental')
        self.catalog_path.write_bytes(original)
        self.native.write_bytes(b'new renderer')
        with self.assertRaisesRegex(ValueError, 'renderer changed'):
            assets.check_inputs(self.root, self.out, '0.1.10', 'Experimental')

    def test_finalized_release_has_pack_and_checksums(self):
        plan, _ = self.complete()
        self.assertEqual(len(plan['assets']), 4)
        sums = (self.out/'SHA256SUMS.txt').read_text()
        self.assertEqual(len(sums.splitlines()), 3)
        self.assertIn(self.pack.name, sums)

    def test_missing_or_changed_pack_prevents_any_github_call(self):
        _, github = self.complete()
        target = self.out/self.pack.name
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'Changed'):
            publisher.publish(self.out, github)
        target.unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            publisher.publish(self.out, github)
        self.assertEqual(github.calls, [])
        self.assertIsNone(github.remote)

    def test_omitted_pack_or_old_release_url_prevents_publication(self):
        plan, github = self.complete()
        original = json.loads(json.dumps(plan))
        plan['assets'] = [a for a in plan['assets'] if a['name'] != self.pack.name]
        assets.write(self.out/'release-assets.json', plan)
        with self.assertRaisesRegex(ValueError, 'must include'):
            publisher.publish(self.out, github)
        original['assets'][0]['url'] = original['assets'][0]['url'].replace('/v0.1.10/', '/v0.1.6/')
        assets.write(self.out/'release-assets.json', original)
        with self.assertRaisesRegex(ValueError, 'another build'):
            publisher.publish(self.out, github)
        self.assertEqual(github.calls, [])

    def test_complete_experimental_release_uploads_all_assets_before_publish(self):
        plan, github = self.complete()
        receipt = publisher.publish(self.out, github)
        self.assertTrue(receipt['published'])
        self.assertEqual([c[0] for c in github.calls], ['create']+['upload']*4+['edit'])
        self.assertIn('--latest=false', github.calls[-1])
        self.assertEqual({a['browser_download_url'] for a in github.remote['assets']}, {a['url'] for a in plan['assets']})

    def test_interrupted_texture_upload_stays_draft_then_resumes_without_overwrite(self):
        _, github = self.complete()
        github.fail_upload = self.pack.name
        with self.assertRaisesRegex(RuntimeError, 'interrupted'):
            publisher.publish(self.out, github)
        self.assertTrue(github.remote['draft'])
        self.assertEqual(len(github.remote['assets']), 2)
        github.calls.clear(); github.fail_upload = None
        publisher.publish(self.out, github)
        self.assertEqual([c[0] for c in github.calls], ['upload', 'upload', 'edit'])
        self.assertEqual(Path(github.calls[0][2]).name, self.pack.name)

    def test_uploaded_corrupt_pack_never_publishes_or_overwrites(self):
        _, github = self.complete()
        github.bad_digest = self.pack.name
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            publisher.publish(self.out, github)
        self.assertTrue(github.remote['draft'])
        github.calls.clear()
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            publisher.publish(self.out, github)
        self.assertEqual(github.calls, [])

    def test_published_release_cannot_be_modified(self):
        _, github = self.complete()
        publisher.publish(self.out, github)
        github.calls.clear()
        with self.assertRaisesRegex(ValueError, 'already published'):
            publisher.publish(self.out, github)
        self.assertEqual(github.calls, [])

    def test_existing_tag_at_another_commit_is_not_reused(self):
        _, github = self.complete()
        github.tag_commit = 'b'*40
        with self.assertRaisesRegex(ValueError, 'another commit'):
            publisher.publish(self.out, github)
        self.assertEqual(github.calls, [])

    def test_unexpected_draft_asset_is_retained_and_blocks_publication(self):
        _, github = self.complete()
        github.fail_upload = self.pack.name
        with self.assertRaises(RuntimeError):
            publisher.publish(self.out, github)
        github.remote['assets'].append({'name':'foreign.zip'})
        github.calls.clear()
        with self.assertRaisesRegex(ValueError, 'Unexpected'):
            publisher.publish(self.out, github)
        self.assertTrue(github.remote['draft'])
        self.assertEqual(github.calls, [])

    def test_missing_server_digests_require_download_verification(self):
        _, github = self.complete(channel='Stable')
        github.omit_digest = True
        publisher.publish(self.out, github)
        self.assertEqual(sum(c[0] == 'download' for c in github.calls), 8)
        self.assertIn('--latest=true', next(c for c in github.calls if c[0] == 'edit'))
        self.assertFalse(github.remote['prerelease'])


if __name__ == '__main__':
    unittest.main()
