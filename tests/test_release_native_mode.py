"""Release packaging must refuse a renderer without the nearby-opponent LOD fix."""
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import release_assets  # noqa: E402

with tempfile.TemporaryDirectory() as folder:
    native = Path(folder)/'d3d11.dll'
    native.write_bytes(b'MZ..grid2vr_opponent_lod_mode=controlled_activation\0..')
    release_assets.verify_native_mode(native)
    for marker in (b'grid2vr_opponent_lod_mode=strict_ordinary', b'grid2vr_opponent_lod_mode=single_attempt_diagnostic', b''):
        native.write_bytes(b'MZ..'+marker+b'\0..')
        try:
            release_assets.verify_native_mode(native)
        except Exception:
            continue
        raise SystemExit(f'accepted renderer without LOD fix: {marker!r}')
print('PASS: release packaging requires the controlled-activation nearby-opponent LOD renderer')
