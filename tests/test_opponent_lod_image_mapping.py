"""Observe Windows image-header rebasing without executing any target code.

The OS mapping observation and owned x86 runtime integration are distinct:
an arbitrary nonexecuting alias need not be at the section's assigned base.
Never infer runtime body qualification from that alias or remove live guards.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys


def verify(fixture, game):
    raw = game.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
    nt = struct.unpack_from('<I', raw, 0x3c)[0]
    preferred = struct.unpack_from('<I', raw, nt + 24 + 28)[0]
    assert preferred == 0x400000
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.CreateFileMappingW.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, w.DWORD, w.DWORD, w.LPCWSTR]
    kernel.CreateFileMappingW.restype = w.HANDLE
    kernel.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, c.c_size_t]
    kernel.MapViewOfFile.restype = c.c_void_p
    kernel.UnmapViewOfFile.argtypes = [c.c_void_p]
    kernel.UnmapViewOfFile.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    file = kernel.CreateFileW(str(game), 0x80000000, 1, None, 3, 0, None)
    assert file != c.c_void_p(-1).value, c.get_last_error()
    mapping = view = None
    try:
        # PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, FILE_MAP_READ. No LoadLibrary,
        # entry-point call, permission change, native pointer or game-file write.
        mapping = kernel.CreateFileMappingW(file, None, 0x11000002, 0, 0, None)
        assert mapping, c.get_last_error()
        view = kernel.MapViewOfFile(mapping, 4, 0, 0, 0)
        assert view, c.get_last_error()
        header = c.string_at(view, 4096)
        assert struct.unpack_from('<H', header)[0] == 0x5a4d
        mapped_nt = struct.unpack_from('<I', header, 0x3c)[0]
        assert mapped_nt == nt
        assert struct.unpack_from('<I', header, nt)[0] == 0x4550
        declared = struct.unpack_from('<I', header, nt + 24 + 28)[0]
        assert declared >= 0x10000 and not declared & 0xffff
        assert struct.unpack_from('<I', header, nt + 24 + 56)[0] == 0x100e000
        # Only the header's ImageBase differs from these file header bytes.
        normalized = bytearray(header[:0x400])
        struct.pack_into('<I', normalized, nt + 24 + 28, preferred)
        assert normalized == raw[:0x400]
        print(json.dumps(dict(operation='Windows SEC_IMAGE_NO_EXECUTE header observation',
                              fileImageBase=hex(preferred), mappedAlias=hex(view),
                              sectionImageBase=hex(declared), headerRebased=declared != preferred,
                              gameExecuted=False, fullRuntimeQualificationOnAlias=False)), flush=True)
    finally:
        if view:
            assert kernel.UnmapViewOfFile(view), c.get_last_error()
        if mapping:
            assert kernel.CloseHandle(mapping), c.get_last_error()
        assert kernel.CloseHandle(file), c.get_last_error()
    # Exercise the actual production qualification on an owned, nonexecutable
    # image having the observed base relationship, including all real body guards.
    # Native calls/transaction remain isolated fixture boundaries, not game calls.
    subprocess.run([str(fixture), 'header-rebased', str(game)], check=True)


if __name__ == '__main__':
    verify(Path(sys.argv[1]).resolve(), Path(sys.argv[2]) if len(sys.argv) > 2 else
           Path(r'E:\SteamLibrary\steamapps\common\grid 2\grid2_avx.exe'))
