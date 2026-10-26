"""Supported-build validation; returned hashes identify the player's actual file."""
import hashlib
from pathlib import Path
import shutil
import stat
import subprocess

AVX_BASELINE = '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
_validated = set()


def validate_avx(path, baseline=AVX_BASELINE):
    path = Path(path)
    if not path.is_file() or path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError('Missing or oversized GRID 2 executable')
    for part in (path, *path.absolute().parents):
        attributes = part.lstat()
        if stat.S_ISLNK(attributes.st_mode) or getattr(attributes, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError('Linked GRID 2 executable path is unsupported')
    with path.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    # Exact known bytes remain supported. Tests replace only this baseline or mock
    # this boundary; never pass a receipt/caller-provided hash as the baseline.
    if actual == baseline or actual in _validated:
        return actual
    helper = Path(__file__).with_name('Game-Executable.ps1')
    bundled = helper.parent.parent/'payload/powershell/pwsh.exe'
    shell = str(bundled) if bundled.is_file() else shutil.which('pwsh')
    if not shell:
        raise ValueError('PowerShell is required to authenticate this Steam executable')
    result = subprocess.run([shell, '-NoProfile', '-NonInteractive', '-File', str(helper),
                             '-Grid2ExecutablePath', str(path.resolve()), '-Grid2ExecutableVariant', 'Avx'],
                            capture_output=True, text=True, timeout=60,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if result.returncode or result.stdout.strip() != actual:
        raise ValueError('Unsupported or changed GRID 2 AVX executable: ' + result.stderr.strip())
    _validated.add(actual)  # Cache by complete bytes, never just path/mtime.
    return actual
