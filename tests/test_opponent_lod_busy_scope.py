"""Check the exact-image scope proof without a disassembler dependency.

The complete bounded decode was audited separately. This fixture binds that
proof to the frozen full-file identity, all 33 exact body extents/hashes, the ten
shared-lock calls and both external tail transfers. A new executable identity
requires a new audit, not merely updating source guard hashes.
"""
import hashlib
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
GAME = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r'E:\SteamLibrary\steamapps\common\grid 2\grid2_avx.exe')
EXPECTED = {
    0xf0b60: 0x14cc, 0x17d040: 0x516, 0x7aa690: 0x1ce, 0x4eed0: 0xb2,
    0x282ed0: 0x7f, 0x3da040: 0x22, 0x6a260: 0x81, 0x215740: 0x16,
    0x71a280: 0x28, 0x17e710: 0x12, 0x6032a0: 0x38, 0x759750: 0x3f,
    0x597bb0: 0x6a, 0x7d4170: 0x70, 0x1b3770: 0x92, 0x489160: 0x7c,
    0x3bc10: 0x66, 0x2159f0: 0xbcc, 0x76fe90: 0x29, 0x25a3d0: 0x17,
    0x56fe00: 0x61, 0x392680: 0xa, 0x648910: 0xa, 0x458590: 0x782,
    0x3c0340: 0xb15, 0x5e6ea0: 0x234, 0x6d3490: 0x18e6, 0x4a920: 1,
    0x2e3930: 0x367, 0x3572f0: 0x784, 0x4eb620: 0x1a, 0x169050: 0x972,
    0x76e580: 0x968,
}
SHARED = {0x392680, 0x648910}
CALLS = ((0x1b378f, 0x392680, 0x1b3770), (0x1b37f7, 0x648910, 0x1b3770),
         (0x48917f, 0x392680, 0x489160), (0x4891d1, 0x648910, 0x489160),
         (0x3bc1b, 0x392680, 0x3bc10), (0x3bc6b, 0x648910, 0x3bc10),
         (0x215f35, 0x392680, 0x2159f0), (0x215f52, 0x648910, 0x2159f0),
         (0x215f78, 0x392680, 0x2159f0), (0x21632e, 0x648910, 0x2159f0))
TAILS = ((0x215751, 0x6032a0), (0x25a3de, 0x489160))

data = GAME.read_bytes()
assert hashlib.sha256(data).hexdigest() == '519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
pe = struct.unpack_from('<I', data, 0x3c)[0]
opt = pe + 24
optsize = struct.unpack_from('<H', data, pe + 20)[0]
sections = [struct.unpack_from('<8s4I', data, opt + optsize + i * 40)
            for i in range(struct.unpack_from('<H', data, pe + 6)[0])]


def offset(rva):
    for _, _, base, size, raw in sections:
        if base <= rva < base + size:
            return raw + rva - base
    raise ValueError(hex(rva))


runtime = (ROOT / 'src/opponent_lod_runtime.cpp').read_text()
block = runtime.split('const Guard auxiliary[]{', 1)[1].split('\n};', 1)[0]
number = r'(0x[0-9a-f]+|[0-9]+)'
pattern = r'\{' + number + ',' + number + r',"([0-9a-f]{64})"(?:,\{([^}]*)\})?(?:,(BusyScope::\w+))?\}'
guards = {int(rva, 0): (int(size, 0), digest, scope != 'BusyScope::SharedLockDependency')
          for rva, size, digest, _, scope in re.findall(pattern, block)}
assert len(guards) == 28
native = (ROOT / 'src/opponent_lod_native.cpp').read_text()
pattern = r'\{"(callback|builder|wrapper|selector|updater)",' + number + ',' + number + r',"([0-9a-f]{64})",'
for _, rva, size, digest in re.findall(pattern, native):
    assert int(rva, 0) not in guards
    guards[int(rva, 0)] = (int(size, 0), digest, True)
assert len(guards) == 33 and {rva: size for rva, (size, _, _) in guards.items()} == EXPECTED
assert {rva for rva, (_, _, owned) in guards.items() if not owned} == SHARED
for rva, (size, digest, _) in guards.items():
    assert hashlib.sha256(data[offset(rva):offset(rva) + size]).hexdigest() == digest, hex(rva)
for call, target, owner in CALLS:
    at = offset(call)
    assert data[at] == 0xe8 and call + 5 + struct.unpack_from('<i', data, at + 1)[0] == target
    size, _, owned = guards[owner]
    assert owned and owner <= call + 5 < owner + size
for site, target in TAILS:
    at = offset(site)
    assert data[at] == 0xe9 and site + 5 + struct.unpack_from('<i', data, at + 1)[0] == target
    assert target in guards and guards[target][2]
print('PASS: frozen exact image, all 33 complete integrity bodies; only two shared dependencies are non-owning; ten protected helper returns and both protected tail targets; no target execution')
