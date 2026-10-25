"""Map retained native camera inputs; this does not validate a camera mutation."""
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from analyze_engine_trace import fields

SUPPORTED = '519CD994D2FF92A2A7A2A784856854183F53E359B83F8B9563CC09A0AE91E502'


def analyze(run):
    session = json.loads((run / 'session.json').read_text(encoding='utf-8-sig'))
    if session.get('executableSha256', '').upper() != SUPPORTED:
        raise ValueError('Unsupported recorded executable.')
    starts = [fields(line) for line in (run / 'engine.log').read_text().splitlines() if 'replay begin ' in line]
    rows = []
    for start in starts:
        key = start['id']
        camera = (run / f'stage-{key}-scene-complete-context.bin').read_bytes()
        view = (run / f'stage-{key}-scene-complete-view.bin').read_bytes()
        if len(camera) != 512 or len(view) != 144:
            raise ValueError('Incomplete camera/SceneView snapshot.')
        floats = lambda data, offset, count: np.frombuffer(data, '<f4', count, offset).astype(float)
        world = floats(camera, 0x70, 16).reshape(4, 4)
        projection = floats(camera, 0x110, 16).reshape(4, 4)
        axes = np.array([-floats(view, 0x20, 3), floats(view, 0x10, 3), -floats(view, 0x30, 3)])
        fov = floats(view, 0x50, 1)[0]
        degrees, near, far, aspect = floats(camera, 0xe4, 4)
        if not all(np.isfinite(m).all() for m in (world, projection, axes, [fov, degrees, near, far, aspect])):
            raise ValueError('Nonfinite native camera.')
        perspective = view[0x70] == 0 and camera[0xe0] == 1 and 0 < fov < np.pi and 0 < near < far and aspect > 0
        expected = np.zeros((4, 4))
        if perspective:
            expected[0, 0] = 1 / (np.tan(fov / 2) * aspect)
            expected[1, 1] = 1 / np.tan(fov / 2)
            expected[2, 2] = -(far + near) / (far - near)
            expected[2, 3] = -1
            expected[3, 2] = -2 * far * near / (far - near)
        rows.append(dict(id=key, frame=start['frame'], perspective=bool(perspective),
            axisError=float(np.max(np.abs(world[:3, :3] - axes))),
            positionError=float(np.max(np.abs(world[3, :3] - floats(view, 0x40, 3)))),
            fovRadians=float(fov), fovDegrees=float(degrees), aspect=float(aspect), near=float(near), far=float(far),
            fovConversionError=float(abs(degrees - np.degrees(fov))),
            projectionError=float(np.max(np.abs(expected - projection))),
            cameraSha256=hashlib.sha256(camera).hexdigest(), viewSha256=hashlib.sha256(view).hexdigest()))
    return dict(run=run.name, executableSha256=SUPPORTED, snapshots=len(rows),
        limits='Correlation of retained native inputs and CPU matrices. No rotated/asymmetric view, culling, metre scale or headset rendering is established.',
        fields={'up': 'SceneView+0x10 xyz', 'negativeRight': 'SceneView+0x20 xyz',
                'forward': 'SceneView+0x30 xyz; native camera +Z is its negative',
                'position': 'SceneView+0x40 xyz', 'verticalFovRadians': 'SceneView+0x50',
                'fovDegrees': 'context+0xe4', 'nearFar': 'context+0xe8/+0xec', 'aspect': 'context+0xf0'},
        projection='Row-vector perspective, forward -Z, P22=-(far+near)/(far-near), P23=-1, P32=-2*far*near/(far-near). Preserve native depth terms when changing lateral FOV.',
        maxima={name: max((row[name] for row in rows), default=None) for name in
                ('axisError', 'positionError', 'fovConversionError', 'projectionError')},
        rows=rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.run)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    print(json.dumps({key: result[key] for key in ('snapshots', 'maxima', 'limits')}, indent=2))
