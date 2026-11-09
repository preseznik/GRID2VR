"""Locate the Steam GRID 2 installation for development tools and tests.

GRID2VR_GAME_ROOT overrides the lookup. Otherwise Steam's libraryfolders.vdf
lists the libraries, and the one holding appmanifest_44350.acf names the game
folder. tools/Paths.ps1 and the launcher's Installation.Discover use the same
rule. Run as a script to print the folder.
"""
import os
from pathlib import Path
import re
import sys

APP_ID = '44350'
OVERRIDE = 'GRID2VR_GAME_ROOT'


def steam_root():
    path = None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            path = winreg.QueryValueEx(key, 'SteamPath')[0]
    except OSError:
        pass
    if not path:
        path = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))/'Steam'
    return Path(os.path.abspath(path))


def libraries(steam):
    found = [steam]
    vdf = steam/'steamapps'/'libraryfolders.vdf'
    if vdf.is_file() and vdf.stat().st_size < 1024*1024:
        for value in re.findall(r'"path"\s*"([^"]+)"', vdf.read_text(encoding='utf-8-sig', errors='replace')):
            library = Path(value.replace('\\\\', '\\'))
            if all(os.path.normcase(library) != os.path.normcase(known) for known in found):
                found.append(library)
    return found


def discover():
    for library in libraries(steam_root()):
        manifest = library/'steamapps'/f'appmanifest_{APP_ID}.acf'
        if not manifest.is_file() or manifest.stat().st_size > 65536:
            continue
        match = re.search(r'"installdir"\s*"([^"]+)"', manifest.read_text(encoding='utf-8-sig', errors='replace'))
        if not match or Path(match.group(1)).name != match.group(1):
            continue
        game = library/'steamapps'/'common'/match.group(1)
        if (game/'grid2_avx.exe').is_file():
            return game
    return None


def game_root():
    override = os.environ.get(OVERRIDE)
    if override:
        return Path(os.path.abspath(override))
    game = discover()
    if game is None:
        raise FileNotFoundError(f'GRID 2 (Steam app {APP_ID}) was not found in any Steam library; set {OVERRIDE}.')
    return game


def game_executable():
    return game_root()/'grid2_avx.exe'


if __name__ == '__main__':
    try:
        print(game_root())
    except FileNotFoundError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
