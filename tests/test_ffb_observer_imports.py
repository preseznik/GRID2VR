"""Pin the game imports the force-feedback observer relies on to the exact PE."""
import hashlib
from pathlib import Path
import struct

game = Path(r'E:\SteamLibrary\steamapps\common\grid 2\grid2_avx.exe')
data = game.read_bytes()
assert hashlib.sha256(data).hexdigest() == '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
pe = struct.unpack_from('<I', data, 0x3c)[0]
optional = pe + 24
count, size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<8s4I', data, optional + size + i * 40) for i in range(count)]


def offset(rva):
    for _, virtual, address, raw_size, raw in sections:
        if address <= rva < address + max(virtual, raw_size):
            return raw + rva - address
    raise ValueError(hex(rva))


def text(rva):
    start = offset(rva)
    return data[start:data.index(b'\0', start)].decode()


imports = {}
at = offset(struct.unpack_from('<I', data, optional + 104)[0])
while True:
    lookup, _, _, name, first = struct.unpack_from('<5I', data, at)
    if not name:
        break
    entries, thunk = [], offset(lookup or first)
    while value := struct.unpack_from('<I', data, thunk)[0]:
        entries.append(('ordinal', value & 0xffff) if value & 0x80000000 else ('name', text(value + 2)))
        thunk += 4
    imports[text(name).lower()] = entries
    at += 20

# XInputGetState (2) and XInputSetState (3) are ordinal imports from XINPUT1_3.
assert imports['xinput1_3.dll'] == [('ordinal', 3), ('ordinal', 4), ('ordinal', 2)], imports['xinput1_3.dll']
assert imports['dinput8.dll'] == [('name', 'DirectInput8Create')]
print('PASS: exact PE imports XINPUT1_3 ordinals 2/3 and dinput8!DirectInput8Create for the observer; no execution')
