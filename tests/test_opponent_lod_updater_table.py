"""Bind complete updater interface/dispatch proof to the exact shipped PE."""
import hashlib
from pathlib import Path
import struct

game=Path(r'E:\SteamLibrary\steamapps\common\grid 2\grid2_avx.exe')
data=game.read_bytes()
assert hashlib.sha256(data).hexdigest()=='519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502'
pe=struct.unpack_from('<I',data,0x3c)[0];opt=pe+24
base=struct.unpack_from('<I',data,opt+28)[0]
sections=[struct.unpack_from('<8s4I',data,opt+struct.unpack_from('<H',data,pe+20)[0]+i*40)
          for i in range(struct.unpack_from('<H',data,pe+6)[0])]
def offset(rva):
    for _,_,va,size,raw in sections:
        if va<=rva<va+size:return raw+rva-va
    raise ValueError(hex(rva))
def block(rva,count):return struct.unpack_from('<'+'I'*count,data,offset(rva))
expected=(0x3da040,0x76e580,0x4a920,0x4d950,0x4d950,0x99bc30,
          0x58ddf0,0x15bd40,0x623c20,0x1090f0,0x693370)
assert block(0xcdcb68,11)==tuple(base+rva for rva in expected)
assert block(0xcdcb64,1)==(base+0x693370,)
# Actual constructors publish these two adjacent, complete table starts.
assert block(0x282f04,1)==(base+0xcdcb68,)
assert block(0x2aca2d,1)==(base+0xcdcb94,)
assert 0xcdcb94-0xcdcb68==11*4
assert block(0xcb1160,11)[2:]==block(0xcdcb68,11)[2:]
assert block(0xcdcb94,11)[2:]==block(0xcdcb68,11)[2:]
# The recorded return at 52156f follows +14 slot 5; +28 slot 10 returns x87 double.
assert data[offset(0x521563):offset(0x52156f)]==bytes.fromhex('8b 4e 10 8b 01 8b 50 14 6a 00 ff d2')
assert data[offset(0x5215be):offset(0x5215cb)]==bytes.fromhex('8b 4e 10 8b 01 8b 50 28 ff d2 dd 5d e0')
print('PASS: exact PE, eleven-method updater/base/neighbor layout, constructor boundaries and loading +14/+28 ABI call sites; no execution')
