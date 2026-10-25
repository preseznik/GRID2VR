import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from package_stage import SCRIPTS


class PackageDependencies(unittest.TestCase):
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
