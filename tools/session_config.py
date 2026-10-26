"""Read installed GRID 2 content and prepare an owned selected-session configuration.

Preparation never launches the game or edits the installation. Human practice
and race require the exact guarded flow bundle; benchmark/attract retain AI/demo control.
"""
from game_executable import validate_avx
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import secrets
import stat
import subprocess
import xml.etree.ElementTree as ET

SUPPORTED_AVX_SHA256 = '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
SESSION_FILENAME = 'GRID2VR.session.xml'
TOKEN = re.compile(r'[a-z0-9_]+\Z')
DLC_MANIFEST = Path(__file__).parent/'data/dlc-content.json'
# Shipped example_benchmark.xml uses eight cars, grids 0..7 and these opponent
# presets. Opponent model choices retain those drivers and grid positions.
RACE_DRIVERS = ('Average2_1', 'Average3_1', 'Ayala_1_1', 'Alvarez_1_1',
                'Bakheet_1_1', 'Banerjee_1_1', 'Bantao_1_1')


def plain_path(path):
    """Reject symlinks and Windows junctions in every existing component."""
    path = Path(path).absolute()
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f'Linked input/output is unsupported: {part}')
    return path.resolve()


def children(path):
    plain_path(path)
    if not path.is_dir():
        return []
    result = []
    for child in sorted(path.iterdir()):
        plain_path(child)
        if child.is_dir() and TOKEN.fullmatch(child.name):
            result.append(child)
    return result


def regular(path):
    return plain_path(path).is_file()


def installed_dlc(root):
    """Recognize inspected DLC by full archive fingerprints, without extraction.

    The manifest contains identifiers and hashes only. Steam/the game retain
    ownership checks; detecting installed bytes does not grant a DLC license.
    Split car packs require both their shared and model archives.
    """
    manifest = json.loads(plain_path(DLC_MANIFEST).read_text(encoding='utf-8'))
    present = set()
    for archive in manifest['archives']:
        path = plain_path(root/archive['path'])
        if not path.is_file() or path.stat().st_size != archive['bytes']:
            continue
        with path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() == archive['sha256']:
                present.add(archive['path'])
    return {kind: [row for row in manifest[kind] if set(row['archives']) <= present]
            for kind in ('cars', 'tracks')}


def catalog(game_root):
    root = plain_path(game_root)
    if not regular(root/'grid2_avx.exe'):
        raise ValueError('GRID 2 AVX executable is missing')
    # Launcher vehicle categories, deliberately independent of performance tiers.
    groups = json.loads(plain_path(Path(__file__).parent/'data/car-classes.json').read_text(encoding='utf-8'))
    classes = {car: group for group in groups for car in group['cars']}
    cars = []
    for folder in children(root/'cars/models'):
        # Do not advertise incomplete loose cars or infer ownership from codes.
        if not all(regular(folder/name) for name in ('cameras.xml', f'{folder.name}_highLOD.pssg')):
            continue
        liveries = [p.name.removeprefix('livery_') for p in children(folder)
                    if p.name.startswith('livery_') and len(p.name) > len('livery_')]
        if liveries:
            group = classes.get(folder.name, {})
            cars.append(dict(id=folder.name, liveries=liveries, dlc=False, cockpitAvailable=True,
                             classId=group.get('id'), className=group.get('label')))
    tracks = []
    for category in ('circuits', 'cities', 'p2p'):
        for location in children(root/'tracks/locations'/category):
            for route in children(location):
                if not re.fullmatch(r'route_\d+', route.name):
                    continue
                if not all(regular(route/name) for name in ('track.jpk', 'route_objects.pssg', 'progress_track.xml')):
                    continue
                tracks.append(dict(id=f'{category}/{location.name}/{route.name}', country=category,
                                   name=location.name, route=route.name))
    dlc = installed_dlc(root)
    from cockpit_dlc import available as available_cockpits
    dlc_cockpits = available_cockpits(root, dlc)
    car_ids = {car['id'] for car in cars}
    for car in dlc['cars']:
        if car['id'] in car_ids:
            continue
        group = classes.get(car['id'], {})
        cars.append(dict(id=car['id'], label=car['label'], liveries=car['liveries'],
                         classId=group.get('id'), className=group.get('label'),
                         dlc=True, cockpitAvailable=car['id'] in dlc_cockpits, archives=car['archives']))
        car_ids.add(car['id'])
    track_ids = {track['id'] for track in tracks}
    for track in dlc['tracks']:
        if track['id'] in track_ids:
            continue
        category, location, route = track['id'].split('/')
        tracks.append(dict(id=track['id'], country=category, name=location, route=route,
                           dlc=True, archives=track['archives']))
        track_ids.add(track['id'])
    return dict(schema=1, gameRoot=str(root), cars=cars, tracks=tracks,
                limits='Loose content and fingerprint-verified installed DLC. Game ownership checks still apply. DLC cockpit cameras require the guarded native loader; presence does not establish headset visual acceptance.')


def configuration(installed, car, track, livery='user', mode='benchmark', lighting='day', weather='dry', laps=1, control='ai', cycle_camera=False, opponents=0, opponent_cars='same', opponent_seed=0):
    if (mode, control) not in (('benchmark', 'ai'), ('attract', 'ai'), ('practice', 'human'), ('race', 'human')):
        raise ValueError('Human control requires practice or race mode and its guarded flow bundle')
    if type(opponents) is not int or (mode == 'race' and not 1 <= opponents <= 7) or (mode != 'race' and opponents != 0):
        raise ValueError('Race requires 1 to 7 opponents; other modes require zero')
    if opponent_cars not in ('same', 'mixed', 'class'):
        raise ValueError('Unknown opponent car selection')
    if type(opponent_seed) is not int or not 0 <= opponent_seed < 2**64:
        raise ValueError('Invalid opponent grid seed')
    if lighting not in ('day', 'day_alt', 'night', 'night_alt') or weather not in ('dry', 'wet'):
        raise ValueError('Unsupported session mode or conditions')
    if type(laps) is not int or not 1 <= laps <= 20:
        raise ValueError('Laps must be an integer from 1 to 20')
    if type(cycle_camera) is not bool or (cycle_camera and (mode, control) != ('benchmark', 'ai')):
        raise ValueError('Automatic camera cycling is only supported for AI benchmarks')
    vehicle = next((v for v in installed['cars'] if v['id'] == car), None)
    route = next((v for v in installed['tracks'] if v['id'] == track), None)
    if vehicle is None or route is None or livery not in vehicle['liveries']:
        raise ValueError('Selected car, livery or route is not in the installed-content catalog')
    # Validate even caller-provided catalogs; do not allow path/argument injection.
    if not all(TOKEN.fullmatch(v) for v in (car, livery, route['country'], route['name'], route['route'])):
        raise ValueError('Invalid content identifier')
    config = ET.Element('config', cycle_camera=str(cycle_camera).lower(), infinite_loop='false', skipreplays='true',
                        outputfilename='GRID2VR_SelectedSession')
    entry = ET.SubElement(config, 'track', country=route['country'], name=route['name'], route=route['route'],
                          lighting=lighting, weather=weather, laps=str(laps), number_cars=str(opponents+1))
    # The native loader still needs its shipped driver presets. Human practice
    # comes from normal race entry, never from changing these XML attributes.
    ET.SubElement(entry, 'car', name=car, livery=livery, ai_driver='Cau_Demo_1_1', ai_driver0='Cau_Demo_1_0', grid='0')
    pool = []
    if mode == 'race' and opponent_cars != 'same':
        pool = sorted((v['id'], 'user') for v in installed['cars']
                      if v['id'] != car and 'user' in v['liveries'] and
                      (opponent_cars == 'mixed' or (vehicle.get('classId') and v.get('classId') == vehicle['classId'])))
        if any(not TOKEN.fullmatch(code) for code, _ in pool):
            raise ValueError('Invalid opponent content identifier')
    rng = random.Random(opponent_seed)
    remaining = []
    for grid, driver in enumerate(RACE_DRIVERS[:opponents], start=1):
        if pool and not remaining:
            remaining = pool.copy()
            rng.shuffle(remaining)
        code, skin = remaining.pop() if remaining else (car, livery)
        ET.SubElement(entry, 'car', name=code, livery=skin, ai_driver=driver, ai_driver0='Cau_Demo_1_0', grid=str(grid))
    ET.indent(config, space='  ')
    return ET.tostring(config, encoding='utf-8', xml_declaration=True)+b'\n'


def launch_flag(mode):
    return '-benchmarkAttract' if mode in ('attract', 'practice', 'race') else '-benchmark'


def car_content(installed, car):
    vehicle = next(v for v in installed['cars'] if v['id'] == car)
    return dict(dlc=vehicle.get('dlc', False), cockpitAvailable=vehicle.get('cockpitAvailable', True))


def check_practice_bundle(root, control, bundle):
    if control == 'human':
        if not isinstance(bundle, (str, Path)):
            raise ValueError('Human selected sessions require --practice-bundle')
        # Local import: the flow preparer shares our path/fingerprint guards.
        import practice_entry
        practice_entry.verify(root, bundle)
    elif bundle is not None:
        raise ValueError('Practice flow is only supported with human selected sessions')


def prepare(game_root, output, practice_bundle=None, **selection):
    installed = catalog(game_root)
    root = Path(installed['gameRoot'])
    destination = plain_path(output)
    if destination.is_relative_to(root):
        raise ValueError('Session output must be outside the installed game')
    executable = root/'grid2_avx.exe'
    digest = validate_avx(executable, SUPPORTED_AVX_SHA256)
    if selection.get('mode') == 'race' and selection.get('opponent_cars', 'same') in ('mixed', 'class'):
        # Persist once: verification/staging must reconstruct the very same grid.
        selection.setdefault('opponent_seed', secrets.randbits(64))
    payload = configuration(installed, **selection)
    mode = selection.get('mode', 'benchmark')
    control = selection.get('control', 'ai')
    check_practice_bundle(root, control, practice_bundle)
    path = destination/'session.xml'
    result = dict(schema=2, status='prepared_not_launched', gameRoot=str(root),
                  executableSha256=digest, configuration=str(path),
                  configurationSha256=hashlib.sha256(payload).hexdigest(), selection=selection,
                  carContent=car_content(installed, selection['car']),
                  control=control, playerControlValidated=False,
                  steamArguments=['-applaunch', '44350', launch_flag(mode), SESSION_FILENAME],
                  limits='Generated configuration only. Selected-content loading and player-control handoff require runtime evidence.')
    if practice_bundle is not None:
        result['practiceBundle'] = str(plain_path(practice_bundle))
    # Own a new directory. Never overwrite an existing prepared session or use
    # filenames/paths supplied by a catalog as output paths.
    destination.mkdir(parents=True, exist_ok=False)
    (destination/'session.xml').write_bytes(payload)
    (destination/'session.json').write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    return result


def verify(game_root, session):
    """Revalidate current installed content, fingerprint and exact generated XML."""
    root = plain_path(game_root)
    folder = plain_path(session)
    metadata, config = folder/'session.json', folder/'session.xml'
    for path, limit in ((metadata, 32768), (config, 16384)):
        if not regular(path) or path.stat().st_size > limit:
            raise ValueError('Missing or oversized prepared session')
    data = json.loads(metadata.read_text(encoding='utf-8'))
    if not isinstance(data, dict) or data.get('schema') not in (1, 2) or data.get('control') not in ('ai', 'human') or data.get('playerControlValidated') is not False:
        raise ValueError('Invalid prepared-session schema/control')
    if any(not isinstance(data.get(key), str) for key in ('gameRoot', 'configuration')):
        raise ValueError('Invalid prepared-session paths')
    if folder.is_relative_to(root) or Path(data['gameRoot']) != root or Path(data['configuration']) != config:
        raise ValueError('Prepared session belongs to a different installation or directory')
    installed = catalog(root)
    digest = validate_avx(root/'grid2_avx.exe', SUPPORTED_AVX_SHA256)
    if digest != data.get('executableSha256'):
        raise ValueError('Prepared executable fingerprint differs')
    selection = data.get('selection')
    if not isinstance(selection, dict):
        raise ValueError('Prepared selection is missing')
    try:
        expected = configuration(installed, **selection)
    except TypeError as error:
        raise ValueError('Invalid prepared selection fields') from error
    if data['control'] != selection.get('control', 'ai'):
        raise ValueError('Prepared control differs from selection')
    content = car_content(installed, selection['car'])
    if data.get('carContent', dict(dlc=False, cockpitAvailable=True)) != content:
        raise ValueError('Selected car content changed; regenerate the session')
    check_practice_bundle(root, data['control'], data.get('practiceBundle'))
    payload = config.read_bytes()
    flag = launch_flag(selection.get('mode', 'benchmark'))
    # Old prepared bundles still verify against their original receipt. Staging
    # always replaces the old absolute launch path with the owned native name.
    arguments = ['-applaunch', '44350', flag, str(config) if data['schema'] == 1 else SESSION_FILENAME]
    if payload != expected or hashlib.sha256(payload).hexdigest() != data.get('configurationSha256') or data.get('steamArguments') != arguments:
        raise ValueError('Prepared session was changed; regenerate it instead')
    return data


def stage(game_root, session, run_directory):
    """Copy a revalidated selection into an owned run; never deploy or launch."""
    data = verify(game_root, session)
    run = plain_path(run_directory)
    if not run.is_dir() or run.is_relative_to(Path(data['gameRoot'])):
        raise ValueError('Run directory must exist outside the installed game')
    output = plain_path(run/'session-config.xml')
    payload = plain_path(data['configuration']).read_bytes()
    if hashlib.sha256(payload).hexdigest() != data['configurationSha256']:
        raise ValueError('Session configuration changed during staging')
    # GRID 2's VFS rejects ':' and '\\' in the filename (validator RVA 0x564de0).
    # An absolute workspace path silently falls back to the shipped demo XML.
    # Deployment copies this snapshot to the owned game-root name before launch.
    arguments = [*data['steamArguments'][:3], SESSION_FILENAME]
    command_line = subprocess.list2cmdline(arguments)
    flow_payload = None
    flow_output = plain_path(run/'practice-flow.bin')
    if data['control'] == 'human':
        import practice_entry
        flow_payload = plain_path(Path(data['practiceBundle'])/'flow.bin').read_bytes()
        if hashlib.sha256(flow_payload).hexdigest() != practice_entry.PRACTICE_SHA256:
            raise ValueError('Practice flow changed during staging')
        if flow_output.exists():
            raise FileExistsError(flow_output)
    with output.open('xb') as stream:
        stream.write(payload)
    if flow_payload is not None:
        with flow_output.open('xb') as stream:
            stream.write(flow_payload)
    result = dict(source=str(plain_path(session)), configuration=str(output),
                configurationSha256=data['configurationSha256'], selection=data['selection'],
                carContent=data.get('carContent', dict(dlc=False, cockpitAvailable=True)),
                control=data['control'], playerControlValidated=False,
                steamArguments=arguments, steamArgumentLine=command_line,
                nativeConfigurationName=SESSION_FILENAME)
    if flow_payload is not None:
        result['practiceFlow'] = str(flow_output)
        result['practiceFlowSha256'] = practice_entry.PRACTICE_SHA256
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, default=Path(r'E:\SteamLibrary\steamapps\common\grid 2'))
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('catalog')
    check = commands.add_parser('verify')
    check.add_argument('--session', type=Path, required=True)
    copy = commands.add_parser('stage')
    copy.add_argument('--session', type=Path, required=True)
    copy.add_argument('--run-directory', type=Path, required=True)
    selected = commands.add_parser('prepare')
    selected.add_argument('--output', type=Path, required=True)
    selected.add_argument('--car', required=True)
    selected.add_argument('--track', required=True)
    selected.add_argument('--livery', default='user')
    selected.add_argument('--mode', choices=('benchmark', 'attract', 'practice', 'race'), default='benchmark')
    selected.add_argument('--lighting', choices=('day', 'day_alt', 'night', 'night_alt'), default='day')
    selected.add_argument('--weather', choices=('dry', 'wet'), default='dry')
    selected.add_argument('--laps', type=int, default=1)
    selected.add_argument('--opponents', type=int, default=0)
    selected.add_argument('--opponent-cars', choices=('same', 'mixed', 'class'), default='same')
    selected.add_argument('--control', choices=('ai', 'human'), default='ai')
    selected.add_argument('--cycle-camera', action='store_true')
    selected.add_argument('--practice-bundle', type=Path)
    args = vars(parser.parse_args())
    try:
        command = args.pop('command'); root = args.pop('game_root')
        action = dict(catalog=catalog, verify=verify, prepare=prepare, stage=stage)[command]
        result = action(root, **args)
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
