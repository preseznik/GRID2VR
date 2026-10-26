"""Temporarily suppress SteamVR's automatic desktop theater for native VR tests.

Uses only IVRSettings_003's single dashboard/autoShowGameTheater key. No UI,
input, compositor or application-control calls. A background client cannot
start SteamVR; interrupted changes retain a journal for explicit recovery.
ABI source: https://github.com/ValveSoftware/openvr/blob/master/headers/openvr_capi.h
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import re

from session_config import plain_path

ROOT = Path(__file__).resolve().parent.parent
SECTION, KEY = 'dashboard', 'autoShowGameTheater'
JOURNAL = Path(os.environ.get('GRID2VR_DATA_ROOT', str(ROOT/'artifacts')))/'steamvr-theater-pending.json'


def runtime_paths():
    # A no-op recovery must also work without a running/available Steam installation.
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as steam_key:
        steam = Path(winreg.QueryValueEx(steam_key, 'SteamPath')[0])
    runtime = steam/'steamapps/common/SteamVR'
    libraries = steam/'steamapps/libraryfolders.vdf'
    if libraries.is_file():
        for value in re.findall(r'"path"\s*"([^"]+)"', libraries.read_text(encoding='utf-8-sig')):
            candidate = Path(value.replace('\\\\', '\\'))/'steamapps/common/SteamVR'
            if (candidate/'steamxr_win32.json').is_file():
                runtime = candidate
                break
    return steam, runtime


def read_json(path, limit=1024*1024):
    path = plain_path(path)
    if path.stat().st_size > limit:
        raise ValueError('Settings/journal exceeds size bound')
    return json.loads(path.read_text(encoding='utf-8-sig'))


def configured():
    steam, runtime = runtime_paths()
    default = read_json(runtime/'resources/settings/default.vrsettings')[SECTION][KEY]
    user = read_json(steam/'config/steamvr.vrsettings').get(SECTION, {})
    before = user.get(KEY, default)
    if type(default) is not bool or type(before) is not bool:
        raise ValueError('Theater setting is not a boolean')
    return dict(before=before, hadOverride=KEY in user, default=default)


class Settings:
    def __enter__(self):
        _, runtime = runtime_paths()
        dll = runtime/('bin/win64' if C.sizeof(C.c_void_p) == 8 else 'bin/win32')/'openvr_api.dll'
        self.dll = C.CDLL(str(plain_path(dll)))
        self.dll.VR_InitInternal.argtypes = [C.POINTER(C.c_int), C.c_int]
        self.dll.VR_InitInternal.restype = C.c_uint32
        self.dll.VR_ShutdownInternal.argtypes = []
        self.dll.VR_ShutdownInternal.restype = None
        self.dll.VR_GetGenericInterface.argtypes = [C.c_char_p, C.POINTER(C.c_int)]
        self.dll.VR_GetGenericInterface.restype = C.c_void_p
        error = C.c_int()
        self.dll.VR_InitInternal(C.byref(error), 3)  # VRApplication_Background
        if error.value:
            raise RuntimeError(f'OpenVR background connection failed: {error.value}; start SteamVR before testing')
        try:
            address = self.dll.VR_GetGenericInterface(b'FnTable:IVRSettings_003', C.byref(error))
            if error.value or not address:
                raise RuntimeError(f'IVRSettings_003 unavailable: {error.value}')
            table = C.cast(address, C.POINTER(C.c_void_p * 11)).contents
            args = (C.c_char_p, C.c_char_p)
            self._get = C.WINFUNCTYPE(C.c_bool, *args, C.POINTER(C.c_int))(table[5])
            self._set = C.WINFUNCTYPE(None, *args, C.c_bool, C.POINTER(C.c_int))(table[1])
            self._remove = C.WINFUNCTYPE(None, *args, C.POINTER(C.c_int))(table[10])
            return self
        except BaseException:
            self.dll.VR_ShutdownInternal()
            raise

    def __exit__(self, *_):
        self.dll.VR_ShutdownInternal()  # Release this client, not the VR server.

    def call(self, function, *args):
        error = C.c_int()
        result = function(SECTION.encode(), KEY.encode(), *args, C.byref(error))
        if error.value:
            raise RuntimeError(f'OpenVR theater setting operation failed: {error.value}')
        return result

    def get(self):
        return self.call(self._get)

    def set(self, value):
        self.call(self._set, value)

    def remove(self):
        self.call(self._remove)


def save_new(path, value):
    with plain_path(path).open('x', encoding='utf-8') as output:
        json.dump(value, output, indent=2)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())


def validate(state):
    if (state.get('schema') != 1 or state.get('section') != SECTION or state.get('key') != KEY or
            state.get('requested') is not False or
            any(type(state.get(k)) is not bool for k in ('before', 'hadOverride', 'default')) or
            not state['hadOverride'] and state['before'] != state['default']):
        raise ValueError('Unrecognized theater recovery journal')


def begin(api, journal, original):
    journal = plain_path(journal)
    if journal.exists():
        raise ValueError('Restore the pending theater transaction first')
    state = dict(schema=1, section=SECTION, key=KEY, requested=False, **original)
    validate(state)
    if api.get() is not state['before']:
        raise ValueError('Live theater preference differs from saved configuration; no change made')
    if not state['before']:
        return dict(**state, result='already_disabled')
    save_new(journal, state)  # Durable recovery ownership precedes the setting change.
    api.set(False)
    if api.get() is not False:
        raise RuntimeError('Theater disable verification failed; journal retained')
    return dict(**state, result='temporarily_disabled')


def restore(api, journal):
    journal = plain_path(journal)
    if not journal.exists():
        return dict(result='no_pending_change')
    state = read_json(journal, 4096)
    validate(state)
    if state['before'] is not True:
        raise ValueError('Journal does not describe an owned theater change')
    # A true value already restores the original preference; preserve any
    # external edit instead of removing that user's newly explicit override.
    if api.get() is state['before']:
        result = 'original_value_already_present'
    else:
        if state['hadOverride']:
            api.set(state['before'])
        else:
            api.remove()
        if api.get() is not state['before']:
            raise RuntimeError('Theater restoration verification failed; journal retained')
        result = 'restored_override' if state['hadOverride'] else 'restored_default'
    journal.unlink()
    return dict(**state, result=result)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inspect', 'query', 'begin', 'restore'))
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args(argv)
    if args.receipt:
        receipt = plain_path(args.receipt)
        data = plain_path(JOURNAL.parent)
        backup = receipt.parent
        legacy = backup.parent == data and re.fullmatch(r'stereo-probe-\d{8}-\d{6}-\d{3}', backup.name)
        grouped = (backup.name == 'backup' and backup.parent.parent == data/'logs' and
                   re.fullmatch(r'\d{8}-\d{6}-\d{3}', backup.parent.name))
        if (not (legacy or grouped) or
                receipt.name not in ('theater-begin.json', 'theater-restored.json') or receipt.exists()):
            raise ValueError('Receipt must be a new fixed-name file in an owned session backup directory')
    if args.action == 'inspect':
        result = configured()
    elif args.action == 'restore' and not JOURNAL.exists():
        result = dict(result='no_pending_change')
    else:
        with Settings() as api:
            if args.action == 'query':
                result = dict(configured(), live=api.get())
            elif args.action == 'begin':
                result = begin(api, JOURNAL, configured())
            else:
                result = restore(api, JOURNAL)
    if args.receipt:
        save_new(receipt, result)
    print(json.dumps(result))


if __name__ == '__main__':
    main()
