"""Freeze and validate the optional artwork shipped alongside each core release."""
import argparse
import hashlib
import json
import re
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'preseznik/GRID2VR'
WEB = 'https://github.com/' + REPOSITORY


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')


def identity(version, channel):
    require(re.fullmatch(r'\d+\.\d+\.\d+', version), 'Invalid core release version.')
    require(channel in ('Stable', 'Experimental'), 'Invalid release channel.')
    return 'v'+version


def asset(path, tag):
    require(path.is_file() and not path.is_symlink(), f'Missing or linked release asset: {path.name}')
    return dict(name=path.name, size=path.stat().st_size, sha256=sha(path), url=f'{WEB}/releases/download/{tag}/{path.name}')


def verify_file(path, expected):
    require(path.is_file() and not path.is_symlink(), f'Missing or linked release asset: {path.name}')
    require(path.stat().st_size == expected['size'] and sha(path) == expected['sha256'], f'Changed release asset: {path.name}')


def verify_native_pack(path, catalog):
    require(path.is_file(), 'Build the native renderer before packaging.')
    binary = path.read_bytes()
    pack_path = ('GRID2VR/addons/cockpit-textures/'+catalog['version']).encode('utf-16-le')
    require(pack_path in binary and all(digest.encode('ascii') in binary for name, digest in catalog['files'].items() if name.endswith('.dds')),
            'Native renderer does not contain this texture catalog; rebuild it before packaging.')


NATIVE_LOD_MODE = b'grid2vr_opponent_lod_mode=controlled_activation'


def verify_native_mode(path):
    # Releases must carry the accepted nearby-opponent LOD fix, whatever build
    # folder or cache produced the renderer.
    binary = path.read_bytes()
    require(NATIVE_LOD_MODE in binary and b'grid2vr_opponent_lod_mode=strict_ordinary' not in binary and
            b'grid2vr_opponent_lod_mode=single_attempt_diagnostic' not in binary,
            'Native renderer lacks the nearby-opponent LOD fix; build with GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION=ON.')


def native_path(root, native=None):
    return native if native is not None else root/'build/win32/bin/RelWithDebInfo/d3d11.dll'


def prepare(root, out, version, channel, revision, pack=None, native=None, native_sha256=None):
    tag = identity(version, channel)
    require(re.fullmatch('[a-f0-9]{40}', revision), 'A full source commit is required.')
    require((native is None) == (native_sha256 is None), 'Native path and SHA-256 must be supplied together.')
    selected_native = native_path(root, native)
    if native is not None:
        require(re.fullmatch('[a-f0-9]{64}', native_sha256), 'Invalid native SHA-256.')
        require(selected_native.is_file() and not selected_native.is_symlink(), 'Missing or linked native renderer.')
        require(sha(selected_native) == native_sha256, 'Native renderer does not match the requested SHA-256.')
    catalog_path = root/'tools/data/cockpit-texture-pack.json'
    catalog = read(catalog_path)
    require(catalog['schema'] == 1 and re.fullmatch(r'\d+\.\d+\.\d+-preview\.\d+', catalog['version']), 'Invalid texture pack catalog.')
    name = f'GRID2VR-CockpitTextures-{catalog["version"]}.zip'
    pack = pack or root/'artifacts/cockpit-packs/all-cars'/name
    expected = dict(size=catalog['zipBytes'], sha256=catalog['zipSha256'])
    verify_file(pack, expected)  # Fail before the launcher build, never omit artwork.
    with zipfile.ZipFile(pack) as archive:
        prefix = f'cockpit-textures/{catalog["version"]}/'
        require(len(archive.infolist()) == len(catalog['files']) and set(archive.namelist()) == {prefix+n for n in catalog['files']}, 'Texture ZIP contents differ from the catalog.')
        for entry, digest in catalog['files'].items():
            require(hashlib.sha256(archive.read(prefix+entry)).hexdigest() == digest, f'Changed texture ZIP entry: {entry}')
    native = selected_native
    verify_native_pack(native, catalog)
    verify_native_mode(native)
    out.mkdir(parents=True, exist_ok=True)
    require(not (out/name).exists() and not (out/'release-inputs.json').exists(), 'Release inputs already exist; choose a new package folder.')
    shutil.copyfile(pack, out/name)
    verify_file(out/name, expected)
    frozen = dict(schema=1, repository=REPOSITORY, tag=tag, version=version, channel=channel, revision=revision,
                  catalogSha256=sha(catalog_path), nativeSha256=sha(native), cockpitTextures=dict(version=catalog['version'], **asset(out/name, tag)))
    write(out/'release-inputs.json', frozen)
    return frozen


def check_inputs(root, out, version, channel, native=None):
    frozen = read(out/'release-inputs.json')
    require(frozen['tag'] == identity(version, channel) and frozen['version'] == version and frozen['channel'] == channel, 'Release inputs belong to another build.')
    require(sha(root/'tools/data/cockpit-texture-pack.json') == frozen['catalogSha256'], 'Texture catalog changed during packaging; rebuild the package.')
    require(sha(native_path(root, native)) == frozen['nativeSha256'], 'Native renderer changed during packaging; rebuild the package.')
    verify_file(out/frozen['cockpitTextures']['name'], frozen['cockpitTextures'])
    return frozen


def finalize(out, package, setup, portable):
    frozen = read(out/'release-inputs.json')
    require(package.get('releaseTag') == frozen['tag'] and package['version'] == frozen['version'] and package['channel'] == frozen['channel'] and
            package.get('cockpitTextures') == frozen['cockpitTextures'], 'Packaged core does not match its frozen texture release.')
    require(sha(out/'stage/GRID2VR/tools/data/cockpit-texture-pack.json') == frozen['catalogSha256'], 'Packaged texture catalog differs from the build input.')
    require(sha(out/'stage/GRID2VR/payload/d3d11.dll') == frozen['nativeSha256'], 'Packaged native renderer differs from the build input.')
    texture = out/frozen['cockpitTextures']['name']
    verify_file(texture, frozen['cockpitTextures'])
    files = [asset(p, frozen['tag']) for p in (setup, portable, texture)]
    checksums = out/'SHA256SUMS.txt'
    checksums.write_text(''.join(f'{f["sha256"]}  {f["name"]}\n' for f in files), encoding='ascii')
    files.append(asset(checksums, frozen['tag']))
    notes = out/'ReleaseNotes.md'
    require(notes.is_file(), 'Release notes are missing.')
    release = dict(frozen, assets=files, notesSha256=sha(notes))
    write(out/'release-assets.json', release)
    return validate(out)


def validate(out):
    release = read(out/'release-assets.json')
    tag = identity(release['version'], release['channel'])
    require(release['schema'] == 1 and release['repository'] == REPOSITORY and release['tag'] == tag, 'Invalid release identity.')
    require(re.fullmatch('[a-f0-9]{40}', release['revision']), 'Invalid release commit.')
    texture = release['cockpitTextures']
    require(re.fullmatch(r'\d+\.\d+\.\d+-preview\.\d+', texture['version']), 'Invalid texture version.')
    require(texture['name'] == f'GRID2VR-CockpitTextures-{texture["version"]}.zip', 'Invalid texture asset name.')
    names = {f'GRID2VR-{release["version"]}-Setup.exe', f'GRID2VR-{release["version"]}.zip', texture['name'], 'SHA256SUMS.txt'}
    require(len(release['assets']) == 4 and {a['name'] for a in release['assets']} == names, 'Release must include installer, portable ZIP, textures and checksums.')
    for item in release['assets']:
        require(re.fullmatch('[a-f0-9]{64}', item['sha256']) and item['size'] > 0, 'Invalid asset checksum/size.')
        require(item['url'] == f'{WEB}/releases/download/{tag}/{item["name"]}', 'Release asset points at another build.')
        verify_file(out/item['name'], item)
    expected = next(a for a in release['assets'] if a['name'] == texture['name'])
    require({k:texture[k] for k in expected} == expected, 'Texture download pin differs from uploaded artwork.')
    require(sha(out/'ReleaseNotes.md') == release['notesSha256'], 'Release notes changed after packaging.')
    sums = ''.join(f'{a["sha256"]}  {a["name"]}\n' for a in release['assets'] if a['name'] != 'SHA256SUMS.txt')
    require((out/'SHA256SUMS.txt').read_text(encoding='ascii') == sums, 'Release checksums omit or differ from an asset.')
    return release


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--channel', required=True)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--pack', type=Path)
    parser.add_argument('--native', type=Path)
    parser.add_argument('--native-sha256')
    args = parser.parse_args()
    print(json.dumps(prepare(ROOT, args.output.resolve(), args.version, args.channel, args.revision, args.pack, args.native, args.native_sha256), indent=2))
