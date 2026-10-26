import re
import json
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from package_stage import SCRIPTS, DATA_FILES, copy, stage_powershell


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


if __name__ == '__main__':
    unittest.main()
