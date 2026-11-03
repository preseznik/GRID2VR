"""A missing release tag must read as absent, so publishing can create it."""
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import publish_release  # noqa: E402


def lookup(code, stderr, stdout=''):
    publish_release.subprocess.run = lambda command, **_: subprocess.CompletedProcess(command, code, stdout, stderr)
    return publish_release.Github().api('repos/preseznik/GRID2VR/commits/v9.9.9', missing=True)


assert lookup(1, 'gh: Not Found (HTTP 404)') is None
assert lookup(1, 'gh: No commit found for SHA: v9.9.9 (HTTP 422)') is None
assert lookup(0, '', '{"sha": "abc"}') == {'sha': 'abc'}
for stderr in ('gh: Validation Failed (HTTP 422)', 'gh: Bad credentials (HTTP 401)', ''):
    try:
        lookup(1, stderr)
    except RuntimeError:
        continue
    raise SystemExit(f'treated a failed lookup as missing: {stderr!r}')
print('PASS: release publishing reads a missing tag as absent and still stops on other GitHub errors')
