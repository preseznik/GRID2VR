"""Build the bounded per-car cockpit asset bank without editing the game."""
from game_executable import validate_avx
import argparse
import hashlib
import json
from pathlib import Path
import re
import tempfile
import xml.etree.ElementTree as ET

from cockpit_camera import (CONVERTER_SHA256, SUPPORTED_AVX_SHA256, convert, digest,
                            patch, plain_path, read_file, seat_position, verify_roundtrip)

MODES = {'HeadOnly': 'head-only', 'HideDriver': 'hide-driver'}
INTRO_PATH = 'cutscene/intro_buildup.xml'
INTRO_SHA256 = 'b421f3141a4a5edde7d5c5c0c194b90daa826c55894abc2fc29e406f6d960f96'


def patch_intro(source):
    # Preserve every byte except the first-career handoff's destination/options.
    if hashlib.sha256(source).hexdigest() != INTRO_SHA256:
        raise ValueError('First-career intro changed')
    root = ET.fromstring(source)
    events = root.findall(".//Event[@type='CameraTransition']")
    if len(events) != 1 or events[0].get('endCamera') != 'chase_close' or events[0].get('toDefaultCamera') != 'true':
        raise ValueError('Unexpected first-career camera handoff')
    before = b'endCamera="chase_close" transitionDuration="1.000000" toDefaultCamera="true"'
    if source.count(before) != 1:
        raise ValueError('Ambiguous first-career camera handoff')
    return source.replace(before, b'endCamera="bonnet" transitionDuration="1.000000" toDefaultCamera="false"')


def profiles(path):
    data = json.loads(read_file(path, 1024 * 1024))
    if data.get('schema') != 1 or not isinstance(data.get('cars'), list) or not 1 <= len(data['cars']) <= 64:
        raise ValueError('Invalid cockpit calibration catalog')
    seen = set()
    for row in data['cars']:
        car = row.get('car')
        if not isinstance(car, str) or not re.fullmatch('[a-z0-9]{3}', car) or car in seen:
            raise ValueError('Invalid or duplicate cockpit car')
        seen.add(car)
        seat_position(row['position'])
        camera = row['camera']
        if camera['path'] != f'cars/models/{car}/cameras.xml':
            raise ValueError('Camera path is outside selected car')
        for name in ('sha256', 'decodedSha256', 'semanticSha256'):
            if not re.fullmatch('[0-9a-f]{64}', camera[name]):
                raise ValueError('Invalid camera fingerprint')
    return data


def inputs(game, converter, rows):
    validate_avx(game/'grid2_avx.exe', SUPPORTED_AVX_SHA256)
    if digest(converter,32*1024*1024) != CONVERTER_SHA256:
        raise ValueError('Unsupported XML converter')
    if digest(game/INTRO_PATH) != INTRO_SHA256:
        raise ValueError('Unsupported first-career intro')
    for row in rows:
        if digest(game/row['camera']['path']) != row['camera']['sha256']:
            raise ValueError(f"Installed camera changed: {row['car']}")


def build(game_root, converter, catalog_path, output, progress=None):
    game, converter, catalog_path, output = map(plain_path,(game_root,converter,catalog_path,output))
    if output.exists() or output.is_relative_to(game):
        raise ValueError('Choose a new output folder outside the installation')
    data = profiles(catalog_path)
    inputs(game,converter,data['cars'])
    output.mkdir(parents=True)
    records=[]
    for index, row in enumerate(data['cars']):
        if progress:
            progress(index, len(data['cars']), row['car'])
        folder=output/row['car']; folder.mkdir()
        record=dict(car=row['car'],originalSha256=row['camera']['sha256'],position=row['position'])
        with tempfile.TemporaryDirectory(prefix='encode-',dir=folder) as scratch:
            scratch=Path(scratch)
            source=scratch/'source.xml'
            convert(converter,'decode',game/row['camera']['path'],source)
            if digest(source) != row['camera']['decodedSha256']:
                raise ValueError('Decoded source changed')
            tree=ET.fromstring(read_file(source))
            for mode,native in MODES.items():
                expected,changes=patch(tree,row['position'],native,car=row['car'],source_semantic=row['camera']['semanticSha256'],start_cockpit=True,near_plane=.02)
                xml=scratch/f'{mode}.xml'
                ET.ElementTree(expected).write(xml,encoding='utf-8',xml_declaration=True)
                binary=folder/f'{mode}.bin'
                convert(converter,'encode',xml,binary)
                decoded=scratch/f'{mode}-roundtrip.xml'
                convert(converter,'decode',binary,decoded)
                verify_roundtrip(expected,ET.fromstring(read_file(decoded)))
                record[mode]=digest(binary)
        records.append(record)
    if progress:
        progress(len(records), len(records), None)
    inputs(game,converter,data['cars'])
    intro = output/'intro.xml'
    intro.write_bytes(patch_intro(read_file(game/INTRO_PATH)))
    manifest=dict(schema=1,cars=records,calibrationSha256=digest(catalog_path),
        executableSha256=validate_avx(game/'grid2_avx.exe', SUPPORTED_AVX_SHA256),converterSha256=CONVERTER_SHA256,
        startView='bonnet',nearPlaneMetres=.02,intro=dict(name=INTRO_PATH,originalSha256=INTRO_SHA256,sha256=digest(intro)),
        semanticRoundtrip=True,installed=False)
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    return manifest


def verify(game_root,converter,catalog_path,bank):
    """Independently re-encode each catalog-derived patch and compare exact bytes."""
    game,converter,catalog_path,bank=map(plain_path,(game_root,converter,catalog_path,bank))
    if bank.is_relative_to(game):
        raise ValueError('Asset bank must be outside the installation')
    manifest=json.loads(read_file(bank/'manifest.json',256*1024))
    with tempfile.TemporaryDirectory(prefix='verify-cockpit-',dir=bank.parent) as temporary:
        expected=build(game,converter,catalog_path,Path(temporary)/'expected')
        if manifest != expected:
            raise ValueError('Cockpit manifest does not match independently encoded assets')
        for row in expected['cars']:
            for mode in MODES:
                if digest(bank/row['car']/f'{mode}.bin') != row[mode]:
                    raise ValueError(f"Changed cockpit asset: {row['car']} {mode}")
        if digest(bank/'intro.xml') != expected['intro']['sha256']:
            raise ValueError('Changed first-career camera handoff')
    return manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root',type=Path,required=True)
    parser.add_argument('--converter',type=Path,required=True)
    parser.add_argument('--catalog',type=Path,required=True)
    parser.add_argument('--progress',action='store_true')
    action=parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--output',type=Path)
    action.add_argument('--verify',type=Path)
    args=parser.parse_args()
    def report(completed, total, car):
        message = (f'Preparing cockpit camera {completed+1} of {total}: {car}' if car else
                   f'Prepared all {total} cockpit cameras; checking originals…')
        print(f'GRID2VR_PROGRESS|{28+60*completed//total}|{message}',flush=True)
    result=(verify(args.game_root,args.converter,args.catalog,args.verify) if args.verify else
            build(args.game_root,args.converter,args.catalog,args.output,report if args.progress else None))
    print(json.dumps(dict(cars=len(result['cars']),semanticRoundtrip=True,installed=False)))
