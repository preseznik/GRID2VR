import re
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from package_stage import SCRIPTS, DATA_FILES, copy


class PackageDependencies(unittest.TestCase):
    def test_packaged_catalog_has_vehicle_categories(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            copy(ROOT/'tools/session_config.py', root/'tools/session_config.py')
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
