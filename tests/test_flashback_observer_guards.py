"""Pin the flashback observer's read layout to the exact local GRID 2 executable (read-only; no execution)."""
import hashlib
import re
import struct
from pathlib import Path

root = Path(__file__).resolve().parents[1]
game = Path(r'E:\SteamLibrary\steamapps\common\grid 2\grid2_avx.exe')
data = game.read_bytes()
assert hashlib.sha256(data).hexdigest() == '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
pe = struct.unpack_from('<I', data, 0x3c)[0]
optional = pe + 24
assert struct.unpack_from('<I', data, optional + 28)[0] == 0x400000
count, size = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<8s4I', data, optional + size + i * 40) for i in range(count)]


def offset(rva):
    for _, virtual, address, raw_size, raw in sections:
        if address <= rva < address + max(virtual, raw_size):
            return raw + rva - address
    raise ValueError(hex(rva))


def word(rva):
    return struct.unpack_from('<I', data, offset(rva))[0]


header = (root / 'src' / 'flashback_observer_guards.h').read_text()
code = re.search(r'codeGuards\[\]\{(.*?)\n\};', header, re.S).group(1)
guards = [(int(rva, 16), int(length), [int(b, 16) for b in body.split(',')])
          for rva, length, body in re.findall(r'\{(0x[0-9a-f]+),(\d+),\{([^}]*)\}\}', code)]
assert len(guards) == 28, len(guards)
for rva, length, expected in guards:
    assert len(expected) == length, hex(rva)
    actual = list(data[offset(rva):offset(rva) + length])
    assert actual == expected, (hex(rva), bytes(actual).hex(), bytes(expected).hex())

constants = dict((name, int(value, 16)) for name, value in re.findall(r'(\w+Rva)=(0x[0-9a-f]+)', header))
addresses = re.search(r'addressGuards\[\]\{(.*?)\n\};', header, re.S).group(1)
pairs = re.findall(r'\{(0x[0-9a-f]+),(\w+)\}', addresses)
assert len(pairs) == 8
for rva, target in pairs:
    target = constants[target] if not target.startswith('0x') else int(target, 16)
    assert word(int(rva, 16)) == 0x400000 + target, (rva, hex(word(int(rva, 16))))

# The vtable stored by the factory has the attach routine (which builds the read layout) in slot 6.
assert word(constants['monitorVtableRva'] + 6 * 4) == 0x400000 + 0x212ee0
# Attach registers the reset callback through the event registration routine with event 15.
call = 0x212f8b
assert call + 5 + struct.unpack_from('<i', data, offset(call) + 1)[0] == 0x83230
assert re.search(r'resetEvent=15,eventSlotsOffset=0x1adc,delegateBytes=12', header)

# The block flag is the benchmark settings object's mode word: parser -benchmark 1 / -benchmarkAttract 2.
assert re.search(r'benchmarkSettingsRva=0xe590f8;', header) and re.search(r'benchmarkModeOffset=0x84,benchmarkAttract=2;', header)
assert constants['benchmarkSettingsRva'] + 0x84 == constants['blockFlagRva']
# The flag lives in .data, outside .text and every integrity-hashed code range.
data_rva = next(address for name, _, address, _, _ in sections if name.startswith(b'.data'))
assert data_rva <= constants['blockFlagRva'] < data_rva + 0x1436f0

# The game hashes its own code; the module must never hook or patch it. Its only
# write is the benchmark mode word through ExchangeBenchmarkMode.
source = (root / 'src' / 'flashback_observer.cpp').read_text()
for forbidden in ('MinHook', 'MH_', 'VirtualProtect', 'WriteProcessMemory', 'SwapGuardedPointer', 'PatchVtable', 'memcpy'):
    assert forbidden not in source, forbidden
assert source.count('ExchangeBenchmarkMode(') == 1 and 'base+blockFlagRva),base,expected,value' in source
model = (root / 'src' / 'flashback_observer_model.h').read_text()
assert model.count('InterlockedCompareExchange(') == 1
print('PASS: 28 code guards and 8 relocatable operands match the exact executable; monitor vtable slot 6, '
      'event-15 reset registration, benchmark mode word in .data and a code-write-free module; no execution')
