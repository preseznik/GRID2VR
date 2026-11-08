"""Bounded in-game seat request and conflict-checked settings commits. No game writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import uuid

from session_config import plain_path


def calibrated_cars():
    path = Path(__file__).parent / 'data/cockpit-assets.json'
    raw = plain_path(path).read_bytes()
    if hashlib.sha256(raw).hexdigest() != 'e57dde118abfdd0f6d81f98734e1799e40021cefdaf9c055699c168d3f240ec2':
        raise ValueError('Base cockpit catalog changed')
    from cockpit_dlc import calibrations
    cars = [row['car'] for row in json.loads(raw)['cars']] + list(calibrations()[1])
    if len(cars) != 76 or len(set(cars)) != len(cars):
        raise ValueError('Unexpected calibrated seat cars')
    return cars


def position(value):
    if not isinstance(value, dict):
        raise ValueError('Invalid seat position')
    result = tuple(value.get(key, 0) for key in ('Height', 'Forward', 'Right'))
    if any(type(v) is not int or not -50 <= v <= 50 for v in result):
        raise ValueError('Seat positions must be whole centimetres within -50..50')
    return result


def load_settings(path):
    path = plain_path(path)
    raw = path.read_bytes() if path.exists() else b'{"Version":1,"Cars":{}}'
    if len(raw) > 32768:
        raise ValueError('Seat settings exceed their bound')
    value = json.loads(raw)
    if type(value.get('Version')) is not int or value['Version'] != 1 or not isinstance(value.get('Cars'), dict) or len(value['Cars']) > 96:
        raise ValueError('Invalid seat settings')
    for car, seat in value['Cars'].items():
        if not re.fullmatch('[a-z0-9]{3}', car):
            raise ValueError('Invalid seat car')
        position(seat)
    fov = value.get('DesktopFov', 0)
    if type(fov) is not int or not -30 <= fov <= 30:
        raise ValueError('Invalid Desktop FOV')
    if any(key in value and type(value[key]) is not bool for key in ('ApplyToDesktop', 'InGameSeat')):
        raise ValueError('Invalid seat preference')
    return raw, value


def prepare(settings, cars, desktop, positions, fov, hide_driver):
    _, value = load_settings(settings)
    if any(type(v) is not bool for v in (desktop, positions, hide_driver)) or type(fov) is not int or not -30 <= fov <= 30 or (fov and not desktop):
        raise ValueError('Invalid seat session mode')
    if not 1 <= len(cars) <= 96 or len(set(cars)) != len(cars) or any(not re.fullmatch('[a-z0-9]{3}', car) for car in cars):
        raise ValueError('Invalid seat session cars')
    text = f'grid2vr-seat-menu-v1 {int(desktop)} {int(positions)} {fov} {int(hide_driver)}\n'
    for car in cars:
        seat = position(value['Cars'].get(car, {}))
        text += car + ' ' + ' '.join(str(v) for v in seat) + '\n'
    return text


def whole(text, low, high):
    if not re.fullmatch('-?[0-9]{1,3}', text):
        raise ValueError('Expected a bounded whole number')
    value = int(text)
    if not low <= value <= high:
        raise ValueError('Seat session value is outside its bound')
    return value


def request(raw):
    if len(raw) > 8192:
        raise ValueError('Seat request exceeds its bound')
    lines = raw.decode('ascii').splitlines()
    if not lines:
        raise ValueError('Empty seat request')
    header = lines[0].split()
    if len(header) != 5 or header[0] != 'grid2vr-seat-menu-v1':
        raise ValueError('Invalid seat request header')
    desktop, positions = whole(header[1], 0, 1), whole(header[2], 0, 1)
    fov, hide_driver = whole(header[3], -30, 30), whole(header[4], 0, 1)
    if fov and not desktop:
        raise ValueError('VR does not change headset FOV')
    cars = {}
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 4 or not re.fullmatch('[a-z0-9]{3}', fields[0]) or fields[0] in cars:
            raise ValueError('Invalid or duplicate seat request car')
        cars[fields[0]] = tuple(whole(v, -50, 50) for v in fields[1:])
    if not 1 <= len(cars) <= 96:
        raise ValueError('Invalid seat request size')
    return dict(desktop=bool(desktop), positions=bool(positions), fov=fov, hide_driver=bool(hide_driver), cars=cars)


def apply(settings, request_raw, commit_raw):
    settings = plain_path(settings)
    expected = request(request_raw)
    if len(commit_raw) > 8192:
        raise ValueError('Seat commit exceeds its bound')
    lines = commit_raw.decode('ascii').splitlines()
    header = lines[0].split() if lines else []
    if len(header) != 3 or header[:2] != ['grid2vr-seat-commit-v1', hashlib.sha256(request_raw).hexdigest()]:
        raise ValueError('Seat commit does not belong to this session')
    fov = whole(header[2], -30, 30)
    if fov and not expected['desktop']:
        raise ValueError('VR commit contains a Desktop FOV')
    changes = {}
    for line in lines[1:]:
        fields = line.split()
        if len(fields) != 4 or fields[0] not in expected['cars'] or fields[0] in changes or not expected['positions']:
            raise ValueError('Invalid seat commit car')
        changes[fields[0]] = tuple(whole(v, -50, 50) for v in fields[1:])
    before, value = load_settings(settings)
    previous_fov = value.get('DesktopFov', 0)
    if expected['desktop'] and fov != expected['fov']:
        if value.get('DesktopFov', 0) not in (expected['fov'], fov):
            raise ValueError('Desktop FOV changed elsewhere; session commit retained')
        value['DesktopFov'] = fov
    changed = []
    for car, seat in changes.items():
        current = position(value['Cars'].get(car, {}))
        if current not in (expected['cars'][car], seat):
            raise ValueError('Seat changed elsewhere; session commit retained')
        if current != seat:
            changed.append(car)
            if seat == (0, 0, 0):
                value['Cars'].pop(car, None)
            else:
                value['Cars'][car] = dict(zip(('Height', 'Forward', 'Right'), seat))
    fov_changed = expected['desktop'] and fov != expected['fov'] and fov != previous_fov
    if not changed and not fov_changed:
        return dict(changed=False, cars=[], fovChanged=False)
    temporary = settings.with_name(settings.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with plain_path(temporary).open('xb') as stream:
            stream.write((json.dumps(value, indent=2) + '\n').encode('utf-8'))
            stream.flush()
            os.fsync(stream.fileno())
        if (settings.read_bytes() if settings.exists() else b'{"Version":1,"Cars":{}}') != before:
            raise ValueError('Seat settings changed during commit; session commit retained')
        os.replace(temporary, settings)
    finally:
        if temporary.exists():
            temporary.unlink()
    return dict(changed=True, cars=changed, fovChanged=fov_changed)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', type=Path, required=True)
    parser.add_argument('--request', type=Path, required=True)
    parser.add_argument('--commit', type=Path)
    parser.add_argument('--prepare', action='store_true')
    parser.add_argument('--desktop', type=int, choices=(0, 1), default=0)
    parser.add_argument('--positions', type=int, choices=(0, 1), default=1)
    parser.add_argument('--fov', type=int, default=0)
    parser.add_argument('--hide-driver', type=int, choices=(0, 1), default=0)
    args = parser.parse_args()
    if args.prepare:
        _, settings = load_settings(args.settings)
        enabled = settings.get('InGameSeat', True)
        if enabled:
            raw = prepare(args.settings, calibrated_cars(), bool(args.desktop), bool(args.positions), args.fov, bool(args.hide_driver)).encode('ascii')
            with plain_path(args.request).open('xb') as stream:
                stream.write(raw)
        print(json.dumps(dict(enabled=enabled)))
    else:
        if not args.commit:
            parser.error('--commit is required unless --prepare is used')
        print(json.dumps(apply(args.settings, plain_path(args.request).read_bytes(), plain_path(args.commit).read_bytes())))
