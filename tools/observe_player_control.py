"""Read-only research probe for the exact GRID 2 controller candidate.

Requires an active, owned diagnostic run. No process writes, remote calls,
hooks, input or window APIs. Raw heap blocks are searched in memory and discarded.
The bounded receipt describes candidates, never a proven human-control handoff.
"""
from game_executable import validate_avx
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import struct
import time

from session_config import SUPPORTED_AVX_SHA256, plain_path

MANAGER_TABLE = 0xD0BC48
PARENT_TABLE = 0xD1266C
UPDATE = 0x520600
MODE = 0xE5917C
UPDATE_BYTES = bytes.fromhex(
    '55 8b ec 83 ec 64 80 79 24 01 89 4d f0 0f 85 ee 0a 00 00 '
    '83 3d 7c 91 25 01 01 8b 41 14 89 45 d0 '
    '75 06 c6 41 34 01 eb 04 c6 41 34 00')


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def manager_header(data, parent, base):
    """Reject incidental vtable words before following any candidate pointers."""
    if len(data) != 0x64 or len(parent) != 4:
        return None
    if u32(data) != base + MANAGER_TABLE or u32(parent) != base + PARENT_TABLE:
        return None
    head, count = u32(data, 0x14), u32(data, 0x18)
    if not 0x10000 <= head < 0xFFFFFFE8 or head % 4 or not 1 <= count <= 16:
        return None
    if data[0x24] != 1 or any(data[i] > 1 for i in range(0x34, 0x38)):
        return None
    # Enum semantics are still being researched; these values are recorded only,
    # never used as pointers or permission to patch. Live attract mode uses 4.
    return dict(head=head, count=count, enabled=data[0x24],
                mode2c=u32(data, 0x2C), mode30=u32(data, 0x30), flags34_37=list(data[0x34:0x38]))


def guarded_update(base):
    expected = bytearray(UPDATE_BYTES)
    struct.pack_into('<I', expected, 21, base + MODE)  # ASLR relocation, not a patch.
    return bytes(expected)


class Process:
    def __init__(self, pid):
        self.k = C.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
        self.k.OpenProcess.restype = W.HANDLE
        self.k.CloseHandle.argtypes = [W.HANDLE]
        self.k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        self.k.VirtualQueryEx.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t]
        self.k.VirtualQueryEx.restype = C.c_size_t
        self.k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
        self.k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
        self.handle = self.k.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ only
        if not self.handle:
            raise C.WinError(C.get_last_error())

    def close(self):
        self.k.CloseHandle(self.handle)

    def identity(self):
        name, size = C.create_unicode_buffer(32768), W.DWORD(32768)
        if not self.k.QueryFullProcessImageNameW(self.handle, 0, name, C.byref(size)):
            raise C.WinError(C.get_last_error())
        times = [W.FILETIME() for _ in range(4)]
        if not self.k.GetProcessTimes(self.handle, *(C.byref(t) for t in times)):
            raise C.WinError(C.get_last_error())
        created = ((times[0].dwHighDateTime << 32) | times[0].dwLowDateTime) / 1e7 - 11644473600
        return plain_path(name.value), created

    def base(self):
        psapi = C.WinDLL('psapi', use_last_error=True)
        psapi.EnumProcessModulesEx.argtypes = [W.HANDLE, C.POINTER(C.c_void_p), W.DWORD, C.POINTER(W.DWORD), W.DWORD]
        modules, needed = (C.c_void_p * 1024)(), W.DWORD()
        if not psapi.EnumProcessModulesEx(self.handle, modules, C.sizeof(modules), C.byref(needed), 3):
            raise C.WinError(C.get_last_error())
        return modules[0]

    def read(self, address, size):
        if not 0x10000 <= address < 0x100000000 or not 0 < size <= 65536 or address + size > 0x100000000:
            raise ValueError('Out-of-bound x86 memory read')
        result, actual = C.create_string_buffer(size), C.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, result, size, C.byref(actual)) or actual.value != size:
            raise OSError(f'Unreadable candidate at {address:#x}')
        return result.raw

    def discover(self, base):
        class Region(C.Structure):
            _fields_ = [('BaseAddress', C.c_void_p), ('AllocationBase', C.c_void_p),
                        ('AllocationProtect', W.DWORD), ('PartitionId', W.WORD),
                        ('RegionSize', C.c_size_t), ('State', W.DWORD), ('Protect', W.DWORD), ('Type', W.DWORD)]
        address, scanned, started, matches, rejected = 0x10000, 0, time.monotonic(), [], []
        needle = struct.pack('<I', base + MANAGER_TABLE)
        region = Region()
        while address < 0x100000000 and self.k.VirtualQueryEx(self.handle, address, C.byref(region), C.sizeof(region)):
            start, end = region.BaseAddress or 0, (region.BaseAddress or 0) + region.RegionSize
            if end <= address:
                raise ValueError('Invalid memory region')
            if region.State == 0x1000 and region.Type == 0x20000 and region.Protect == 4:
                for pos in range(start, min(end, 0x100000000), 65536):
                    if time.monotonic() - started > 8 or scanned >= 2 * 1024**3:
                        raise ValueError('Discovery bound reached; partial candidates refused')
                    size = min(65536, end - pos, 0x100000000 - pos)
                    scanned += size
                    try:
                        data = self.read(pos, size)
                    except OSError:
                        continue
                    offset = data.find(needle)
                    while offset >= 0:
                        obj = pos + offset
                        if obj % 4 == 0 and obj >= 0x10190:
                            try:
                                data_header, parent = self.read(obj, 0x64), self.read(obj - 0x190, 4)
                                header = manager_header(data_header, parent, base)
                                if header:
                                    matches.append(obj)
                                elif len(rejected) < 16:
                                    rejected.append(dict(address=hex(obj), parentTableRva=hex(u32(parent)-base),
                                                         count=u32(data_header, 0x18), enabled=data_header[0x24],
                                                         mode2c=u32(data_header, 0x2C), mode30=u32(data_header, 0x30),
                                                         flags34_37=list(data_header[0x34:0x38])))
                            except OSError:
                                pass
                        if len(matches) > 16:
                            raise ValueError('Too many manager candidates')
                        offset = data.find(needle, offset + 1)
            address = end
        return matches, scanned, rejected


def sample(process, base, obj):
    header = manager_header(process.read(obj, 0x64), process.read(obj - 0x190, 4), base)
    if not header:
        raise ValueError('Manager identity changed')
    # Observe the first native tree entry only; never execute its virtual methods.
    node = u32(process.read(header['head'], 24))
    entry = process.read(node, 24)
    vehicle, controller = u32(entry, 12), u32(entry, 16)
    group = process.read(controller, 64)
    table = u32(group)
    if not base <= table < base + 0xE00000:
        raise ValueError('Controller table outside supported image')
    start, end = u32(group, 8), u32(group, 12)
    if start % 4 or end < start or (end - start) % 4 or end - start > 16 * 4:
        raise ValueError('Controller vector bound')
    children = []
    for offset in range(0, end - start, 4):
        child = u32(process.read(start + offset, 4))
        child_data = process.read(child, 64)
        child_table = u32(child_data)
        if not base <= child_table < base + 0xE00000:
            raise ValueError('Child controller table outside supported image')
        methods = {}
        for slot in (2, 11, 12):
            target = u32(process.read(child_table + slot * 4, 4))
            if not base + 0x1000 <= target < base + 0xC00000:
                raise ValueError('Child method outside supported code')
            methods[str(slot)] = dict(rva=hex(target-base), code=process.read(target, 16).hex())
        children.append(dict(address=hex(child), tableRva=hex(child_table-base), prefix=child_data.hex(), methods=methods))
    return dict(address=hex(obj), globalMode=u32(process.read(base + MODE, 4)),
                recordedPlayback=hex(u32(process.read(base + 0xE61010, 4))),
                parentMode1bc=u32(process.read(obj-0x190+0x1BC, 4)), **header,
                node=hex(node), vehicle=hex(vehicle), vehicleFlags470=u32(process.read(vehicle+0x470, 4)),
                group=hex(controller), groupTableRva=hex(table-base), groupPrefix=group.hex(), children=children)


def observe(pid, run):
    root = Path(__file__).resolve().parents[1]
    run = plain_path(run)
    if run.parent != root / 'artifacts' or not run.name.startswith('diagnostic-'):
        raise ValueError('Expected an owned diagnostic directory')
    journal = json.loads((root/'artifacts/diagnostic-pending.json').read_text(encoding='utf-8-sig'))
    game = plain_path(journal['gameRoot'])
    marker = plain_path(game/'GRID2VR.diagnostic-session')
    if plain_path(journal['run']) != run or marker.read_bytes() != (run/marker.name).read_bytes():
        raise ValueError('Active diagnostic ownership mismatch')
    process = Process(pid)
    try:
        exe, created = process.identity()
        if exe != game/'grid2_avx.exe' or created < marker.stat().st_ctime - 1:
            raise ValueError('Process does not belong to this diagnostic')
        executable_hash = validate_avx(exe, SUPPORTED_AVX_SHA256)
        base = process.base()
        if process.read(base+UPDATE, len(UPDATE_BYTES)) != guarded_update(base):
            raise ValueError('Controller update instruction mismatch')
        if u32(process.read(base+MANAGER_TABLE+4, 4)) != base+UPDATE:
            raise ValueError('Controller update table mismatch')
        objects, scanned, rejected = process.discover(base)
        samples = []
        for index in range(3):
            rows = []
            for obj in objects:
                try:
                    rows.append(sample(process, base, obj))
                except (OSError, ValueError) as error:
                    rows.append(dict(address=hex(obj), error=str(error)))
            samples.append(dict(index=index, candidates=rows))
            if index < 2:
                time.sleep(.25)
        return dict(schema=1, pid=pid, base=hex(base), executableSha256=executable_hash,
                    scannedBytes=scanned, candidates=len(objects), rejected=rejected, samples=samples,
                    processWrites=False, inputInjected=False, playerControlValidated=False,
                    limits='Read-only candidate observations. Sampling is not atomic; object semantics and human control remain unproven.')
    finally:
        process.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--phase', choices=('startup', 'race', 'late'), default='race')
    args = parser.parse_args()
    output = plain_path(args.run/f'player-control-observation-{args.phase}.json')
    if output.exists():
        raise ValueError('Existing observation retained')
    result = observe(args.pid, args.run)
    encoded = json.dumps(result, indent=2)
    if len(encoded) > 128 * 1024:
        raise ValueError('Receipt exceeded storage bound')
    with output.open('x', encoding='utf-8') as target:
        target.write(encoded)
    print(json.dumps(dict(output=str(output), candidates=result['candidates'], processWrites=False)))
