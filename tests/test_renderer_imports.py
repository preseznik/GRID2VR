"""Pin the renderer proxy's import table (read-only; no execution).

GRID 2 resolves its static d3d11.dll import to this proxy in the game folder.
If any proxy dependency or entry point is missing on a player's PC, Windows
loads its own d3d11.dll instead and the mod silently does nothing. A new
dependency (for example MSVCP140_2.dll, which only newer Visual C++ runtimes
ship) must therefore be a deliberate, reviewed change to this list.
"""
import re
import struct
import sys
from pathlib import Path

ALLOWED = {name.lower() for name in (
    'ADVAPI32.dll', 'bcrypt.dll', 'CRYPT32.dll', 'D3DCOMPILER_47.dll', 'DINPUT8.dll', 'KERNEL32.dll',
    'MSVCP140.dll', 'MSVCP140_ATOMIC_WAIT.dll', 'pdh.dll', 'USER32.dll', 'VCRUNTIME140.dll', 'WINTRUST.dll')}
UNIVERSAL_CRT = re.compile(r'api-ms-win-crt-[a-z]+-l1-1-0\.dll')

data = Path(sys.argv[1]).read_bytes()
pe = struct.unpack_from('<I', data, 0x3c)[0]
optional = pe + 24
assert struct.unpack_from('<H', data, optional)[0] == 0x10b, 'expected a 32-bit renderer'
size, count = struct.unpack_from('<H', data, pe + 20)[0], struct.unpack_from('<H', data, pe + 6)[0]
sections = [struct.unpack_from('<8s6I', data, optional + size + 40 * i) for i in range(count)]


def offset(rva):
    for _, virtual, address, raw_size, raw, *_ in sections:
        if address <= rva < address + max(virtual, raw_size):
            return raw + rva - address
    raise ValueError(hex(rva))


def names(directory, stride, name_field):
    rva = struct.unpack_from('<I', data, optional + 96 + 8 * directory)[0]
    found, at = [], offset(rva) if rva else None
    while at is not None:
        name = struct.unpack_from('<I', data, at + 4 * name_field)[0]
        if not name:
            break
        found.append(data[offset(name):].split(b'\0')[0].decode())
        at += stride
    return found


imports = names(1, 20, 3)
delayed = names(13, 32, 1)
unexpected = [name for name in imports + delayed if name.lower() not in ALLOWED and not UNIVERSAL_CRT.fullmatch(name.lower())]
assert not unexpected, 'Renderer imports outside the reviewed list: ' + ', '.join(unexpected)
assert 'msvcp140_2.dll' not in {name.lower() for name in imports + delayed}
print('PASS: renderer imports %d reviewed modules (%d delayed); no newer-runtime-only DLL' % (len(imports), len(delayed)))
