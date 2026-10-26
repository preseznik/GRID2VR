"""Prepare bounded in-memory camera requests for fingerprinted owned DLC."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re

CATALOG = Path(__file__).parent / 'data/cockpit-dlc.json'
CATALOG_SHA256 = 'ba998d34619256bd3a3d214c1854c8140f05118a9df02ebf1fd1d04b4df4b69d'


def calibrations():
    from session_config import plain_path
    path = plain_path(CATALOG)
    if path.stat().st_size > 32768:
        raise ValueError('DLC cockpit catalog exceeds its bound')
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != CATALOG_SHA256:
        raise ValueError('DLC cockpit catalog changed')
    data = json.loads(raw)
    if data.get('schema') != 1 or len(data.get('cars', [])) != 25:
        raise ValueError('Invalid DLC cockpit catalog')
    result = {}
    for row in data['cars']:
        code = row['car']
        if not re.fullmatch('[a-z0-9]{3}', code) or code in result:
            raise ValueError('Invalid DLC cockpit car')
        if len(row['position']) != 3 or not all(type(v) in (int, float) and math.isfinite(v) and abs(v) < 3 for v in row['position']):
            raise ValueError('Invalid DLC cockpit position')
        result[code] = row
    return data, result


def available(root, installed):
    from session_config import plain_path
    data, rows = calibrations()
    embedded_ok = None
    result = {}
    for car in installed['cars']:
        row = rows.get(car['id'])
        if row is None or set(row['archives']) != set(car['archives']):
            continue
        if row['embeddedSource']:
            if embedded_ok is None:
                archive = data['embeddedArchive']
                path = plain_path(root / archive['path'])
                embedded_ok = path.is_file() and path.stat().st_size == archive['bytes']
                if embedded_ok:
                    with path.open('rb') as stream:
                        embedded_ok = hashlib.file_digest(stream, 'sha256').hexdigest() == archive['sha256']
            if not embedded_ok:
                continue
        result[car['id']] = row
    return result


def request(game, mode, car=None, normal=False, alignments=None):
    from session_config import installed_dlc, plain_path
    from game_executable import validate_avx
    game = plain_path(game)
    validate_avx(game / 'grid2_avx.exe')
    if mode not in ('head-only', 'hide-driver') or normal == bool(car):
        raise ValueError('Choose a DLC car or Normal mode')
    rows = available(game, installed_dlc(game))
    if not normal:
        if car not in rows:
            raise ValueError('DLC cockpit source is missing or changed')
        rows = {car: rows[car]}
    seats = {}
    if alignments:
        path = plain_path(alignments)
        if path.stat().st_size > 32768:
            raise ValueError('Seat settings exceed their bound')
        data = json.loads(path.read_text(encoding='utf-8-sig'))
        seats = data.get('Cars')
        if data.get('Version') != 1 or not isinstance(seats, dict) or len(seats) > 96:
            raise ValueError('Invalid seat settings')
        for code, offset in seats.items():
            if not re.fullmatch('[a-z0-9]{3}', code) or not isinstance(offset, dict):
                raise ValueError('Invalid seat car')
            if any(type(offset.get(k)) is not int or not -50 <= offset[k] <= 50 for k in ('Height', 'Forward', 'Right')):
                raise ValueError('Seat adjustments must be whole centimetres within -50..50')
    selected = []
    for code, row in sorted(rows.items()):
        x, y, z = row['position']
        seat = seats.get(code, dict(Height=0, Forward=0, Right=0))
        selected.append(dict(car=code, position=[round(x-seat['Right']/100,4), round(y+seat['Height']/100,4), round(z+seat['Forward']/100,4)]))
    text = 'dlc-cockpit-v1 ' + mode + '\n'
    text += ''.join(r['car']+' '+ ' '.join(f'{v:.4f}' for v in r['position'])+'\n' for r in selected)
    return dict(schema=1, cars=selected, text=text if selected else '')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--mode', choices=('head-only', 'hide-driver'), required=True)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--car')
    choice.add_argument('--normal', action='store_true')
    parser.add_argument('--alignments', type=Path)
    args = parser.parse_args()
    print(json.dumps(request(args.game_root, args.mode, args.car, args.normal, args.alignments)))
