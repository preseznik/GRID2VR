"""Prepare the guarded race-start handoff to the converted bonnet camera.

The vehicle manager's initial view is overwritten by the native saved-camera
handoff. This event runs later, after startsequence enables player cameras. Its
explicit destination bypasses current_camera without modifying the saved value.
Deployment is restricted to launcher Practice/Race VR sessions by the caller.
"""
import hashlib
import xml.etree.ElementTree as ET
from pathlib import Path

from cockpit_camera import plain_path, read_file

START_PATH = 'cutscene/startsequence.xml'
START_SHA256 = '9634f52d182a990aa3b86dedc4ea37aea62b2db163588dcec5c5cd3f825cdf97'
HANDOFF = (b'\t\t\t<Event type="CameraTransition" startCamera="chase_close" '
           b'endCamera="bonnet" transitionDuration="0.000000" '
           b'toDefaultCamera="false" inhibitViewActions="false" />\r\n')
ANCHOR = (b'\t\t</Keyframe>\r\n\t\t<Keyframe name="kf_light_sequence" time="0.000000" '
          b'timeType="relative" relativeTo="kf_light_sequence">')


def patch_start(source):
    """Add one instantaneous native handoff; preserve every original byte."""
    if hashlib.sha256(source).hexdigest() != START_SHA256:
        raise ValueError('Race-start sequence changed')
    root = ET.fromstring(source)
    frames = root.findall("./Timeline/Keyframe[@name='kf_light_sequence']")
    if (root.tag != 'Cutscene' or len(frames) != 2 or
            frames[0].get('relativeTo') != 'kf_wait_for_network' or
            frames[0][-1].tag != 'Event' or frames[0][-1].get('type') != 'Controller' or
            frames[0][-1].find('Camera').attrib != {'enabled': 'true'} or
            root.findall(".//Event[@type='CameraTransition']") or source.count(ANCHOR) != 1):
        raise ValueError('Unexpected race-start camera handoff')
    return source.replace(ANCHOR, HANDOFF + ANCHOR)


def prepare(game_root, output, verify=False):
    game, output = map(plain_path, (game_root, output))
    if output.is_relative_to(game):
        raise ValueError('Camera handoff bank must be outside the installation')
    expected = patch_start(read_file(game / START_PATH))
    asset = output / 'startsequence.xml'
    if verify:
        if read_file(plain_path(asset)) != expected:
            raise ValueError('Prepared race-start handoff changed')
    else:
        output.mkdir(parents=True, exist_ok=False)
        asset.write_bytes(expected)
    return hashlib.sha256(expected).hexdigest()


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--output', type=Path)
    action.add_argument('--verify', type=Path)
    args = parser.parse_args()
    print(prepare(args.game_root, args.verify or args.output, bool(args.verify)))
