"""Read and write normal GRID 2 AVX profile files using the verified local game codec."""
import hashlib
import struct
import zlib

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

CODE_SHA256 = "c400fe00e7e177d6f2fe8f7ae6da52c5d98110fc634afa5362634e076ce92cc2"
STANDALONE_AVX = "825f30a49a668870c7bfab0309c3afb01a396fc1cec0be8df9764e32bf2fb53f"
STANDALONE_CODE_SHA256 = "445327aa5d830fd0a3a63f676fb1eb647e7d1610d3ae8a6d28263fededafbcdf"
PROFILE_CAPACITY = 262144
NAMES = ("savegame@profile0#nxdtvpki.lkr", "savegame@profile0#qkigmyyi.lkr")
DIRECTORY = "savegame@profile0#qkrhmyxe"
FILE_IDS = (1838330113, 2210129629)


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


class StockWriter:
    """All execution and output stay in guest memory. The installed game is read only."""

    owner = 0x20000000
    stack = 0x21017000
    stop = 0x22000000
    source = 0x24000000

    def __init__(self, executable, verified_hash):
        image = executable.read_bytes()
        # The launcher checks the signed Steam build or the exact standalone pair.
        # Both supported AVX images use the same save routines and section layout.
        expected_code = STANDALONE_CODE_SHA256 if verified_hash == STANDALONE_AVX else CODE_SHA256
        if (hashlib.sha256(image).hexdigest() != verified_hash or
                hashlib.sha256(image[1024:1024+11619840]).hexdigest() != expected_code):
            raise ValueError('Unsupported GRID 2 executable for completed careers')
        self.vm = Uc(UC_ARCH_X86, UC_MODE_32)
        pe = struct.unpack_from("<I", image, 60)[0]
        first = pe + 24 + struct.unpack_from("<H", image, pe + 20)[0]
        self.vm.mem_map(0x400000, 0x100e000)
        for i in range(struct.unpack_from("<H", image, pe + 6)[0]):
            rva, size, offset = struct.unpack_from("<III", image, first + 40 * i + 12)
            if size:
                self.vm.mem_write(0x400000 + rva, image[offset:offset + size])

        self.vm.mem_map(self.owner, 0x20000)
        self.vm.mem_map(0x21000000, 0x20000)
        self.vm.mem_map(self.stop, 0x1000)
        self.heap = 0x23000000
        self.vm.mem_map(self.heap, 0x80000)
        self.vm.mem_map(self.source, 0x40000)
        self.callbacks = []
        self.output = bytearray()
        # Stand-ins provide ordinary host services only. Retail code performs
        # the archive digest comparison, creates its opaque context and writes both files.
        self.bind(0xf16014, 5, self.acquire_hash_provider)
        self.bind(0xf1601c, 5, self.create_hash)
        self.bind(0xf16018, 4, self.hash_data)
        self.bind(0xf16030, 5, self.hash_result)
        self.bind(0xf16000, 1, lambda handle: int(handle == 2))
        self.bind(0xf16010, 2, lambda handle, flags: int(handle == 1 and flags == 0))
        self.bind(0xf161f8, 5, self.write_file)
        self.bind(0xf16178, 1, self.clock)
        self.bind(None, 3, self.copy_bytes, address=0xec636c, cdecl=True)

        allocator = self.owner + 0x1000
        self.put(allocator, allocator + 0x100)
        self.put(allocator + 0x108, self.bind(None, 2, self.allocate))
        self.put(self.owner + 4, allocator)
        self.put(self.owner + 0x5a0, 65536)
        descriptor = self.owner + 0xa0
        if self.call(0x991cc0, descriptor, allocator, 0) != descriptor:
            raise ValueError("Stock archive descriptor construction failed")
        if self.call(0x6cee40, descriptor, 0x10875a0, self.word(0x108caa0),
                     0x11902a0, self.word(0x108caa4)):
            raise ValueError("Stock archive descriptor verification failed")
        # Stop when the factory succeeds, before unrelated archive indexing starts.
        self.call(0x4fc180, self.owner, until=0x4fc21f)
        context = self.word(self.owner + 0x620)
        if not context:
            raise ValueError("Stock archive provider unavailable")
        self.service = self.owner + 0x2000
        self.put(self.service + 4, allocator)
        self.put(self.service + 0x10, context)
        if self.call(0x745330, self.service):
            raise ValueError("Stock save writer initialization failed")

    def word(self, address):
        return struct.unpack("<I", self.vm.mem_read(address, 4))[0]

    def put(self, address, value):
        self.vm.mem_write(address, words(value))

    def bind(self, slot, count, handler, *, address=None, cdecl=False):
        address = address or self.stop + 16 * (len(self.callbacks) + 1)
        if slot:
            self.put(slot, address)

        def forward(vm, _ip, _size, _data):
            sp = vm.reg_read(UC_X86_REG_ESP)
            ret, *args = struct.unpack(f"<{count + 1}I", vm.mem_read(sp, (count + 1) * 4))
            vm.reg_write(UC_X86_REG_EAX, handler(*args))
            vm.reg_write(UC_X86_REG_ESP, sp + 4 + (0 if cdecl else 4 * count))
            vm.reg_write(UC_X86_REG_EIP, ret)

        self.callbacks.append(self.vm.hook_add(UC_HOOK_CODE, forward, begin=address, end=address))
        return address

    def call(self, method, owner, *args, until=None):
        end = self.stop if until is None else until
        self.vm.mem_write(self.stack, words(self.stop, *args))
        self.vm.reg_write(UC_X86_REG_ESP, self.stack)
        self.vm.reg_write(UC_X86_REG_ECX, owner)
        self.vm.emu_start(method, end, count=120_000_000)
        if self.vm.reg_read(UC_X86_REG_EIP) != end:
            raise ValueError("The stock method did not finish")
        return self.vm.reg_read(UC_X86_REG_EAX)

    def allocate(self, length, alignment):
        start = (self.heap + 15) & ~15
        if alignment not in (1, 4, 16) or not 0 < length <= 65536 or start + length > 0x23080000:
            raise ValueError("Unexpected stock allocation")
        self.heap = start + length
        return start

    def copy_bytes(self, dest, source, length):
        if length > 0x40000:
            raise ValueError("Unexpected copy length")
        if length:
            self.vm.mem_write(dest, bytes(self.vm.mem_read(source, length)))
        return dest

    def acquire_hash_provider(self, dest, container, provider, kind, flags):
        if container or kind != 24 or flags != 0xf0000000:
            raise ValueError("Expected the stock unkeyed hash provider")
        self.put(dest, 1)
        return 1

    def create_hash(self, handle, algorithm, key, flags, dest):
        if (handle, algorithm, key, flags) != (1, 0x800c, 0, 0):
            raise ValueError("Expected SHA-256")
        self.hash = hashlib.sha256()
        self.put(dest, 2)
        return 1

    def hash_data(self, handle, data, length, flags):
        if handle != 2 or flags or length > 0x10000:
            raise ValueError("Unexpected stock hash input")
        self.hash.update(self.vm.mem_read(data, length))
        return 1

    def hash_result(self, handle, param, dest, length, flags):
        if (handle, param, flags) != (2, 2, 0) or self.word(length) < 32:
            raise ValueError("Unexpected stock hash output")
        self.vm.mem_write(dest, self.hash.digest())
        self.put(length, 32)
        return 1

    def clock(self, out):
        # The final short block is padded from the clock. Keep unused tail bytes deterministic.
        self.vm.mem_write(out, struct.pack("<Q", 123456789))
        return 1

    def write_file(self, handle, data, length, used, overlapped):
        if (handle != 1 or length > 65536 or len(self.output) + length > self.limit
                or self.word(overlapped + 8) != len(self.output)):
            raise ValueError("Unexpected stock file write")
        self.output.extend(self.vm.mem_read(data, length))
        self.put(used, length)
        return 1

    def encode(self, source):
        if not 32 < len(source) < PROFILE_CAPACITY:
            raise ValueError("Invalid logical save size")
        self.vm.mem_write(self.source, source)
        self.output.clear()
        self.limit = (len(source) + 15) & ~15
        entry = self.owner + 0x3000
        previous, used, record = entry + 0x100, entry + 0x104, entry + 0x200
        self.vm.mem_write(entry, bytes(0x210))
        self.put(entry + 0x2c, len(source))
        self.put(record + 8, self.call(0xaa5230, 0))
        status = self.call(0xa7dfe0, self.service, 0, entry, record, previous, 0, 1,
                           len(source), self.source, 0, used)
        if status or self.word(used) != len(source) or len(self.output) != self.limit:
            raise ValueError("Incomplete stock file write")
        self.last_checksum = self.word(record + 8)
        return bytes(self.output)


def read_chunks(reader, physical):
    decoded = bytearray()
    used = reader.owner + 0x1800
    dest = reader.source + 65536
    for offset in range(0, len(physical), 65536):
        part = physical[offset:offset + 65536]
        reader.vm.mem_write(reader.source, part)
        if (reader.call(0x558420, reader.service, reader.source, len(part), dest, 65536, used)
                or reader.word(used) != len(part)):
            raise ValueError("Incomplete stock file read")
        decoded.extend(reader.vm.mem_read(dest, len(part)))
    return bytes(decoded)


def checksum(reader, *parts):
    value = reader.call(0xaa5230, 0)
    for part in parts:
        reader.vm.mem_write(reader.source, part)
        value = reader.call(0x6579d0, 0, value, reader.source, len(part))
    return value


def directory_fields(reader, physical):
    if len(physical) != 416:
        raise ValueError("Expected a normal two-file profile directory")
    raw = read_chunks(reader, physical)
    stored, count = struct.unpack_from("<II", raw, 0x180)
    if count != 2 or checksum(reader, raw[:0x180], raw[0x184:]) != stored:
        raise ValueError("Invalid profile directory checksum or count")
    if raw[0x100:0x104] != bytes(4):
        raise ValueError("Expected the original profile directory without thumbnail")
    end = next((i+2 for i in range(0, 256, 2) if raw[i:i+2] == bytes(2)), None)
    if end is None:
        raise ValueError("Original directory has no terminated label")
    return raw[:end], list(struct.iter_unpack("<III", raw[0x188:]))


def write_directory(writer, label, rows):
    # Supply the new display label and the absence of optional thumbnail
    # to the stock metadata writer, just as its normal operation callbacks do.
    op = writer.owner + 0x5000
    request, profile, vtable, vector, records = (op + i * 0x100 for i in range(1, 6))
    writer.put(op, request)
    writer.put(request + 0x70, profile)
    writer.put(profile, vtable)

    def copy_label(out):
        if writer.word(out + 4) < len(label) // 2:
            raise ValueError("Stock directory label buffer too small")
        writer.vm.mem_write(writer.word(out + 8), label)
        return 0

    writer.put(vtable + 0x34, writer.bind(None, 1, copy_label))
    writer.put(vtable + 0x38, writer.bind(None, 0, lambda: 1))
    entries = b"".join(struct.pack("<III", *entry) for entry in rows)
    writer.put(vector + 4, records)
    writer.put(vector + 8, records + len(entries))
    writer.vm.mem_write(records, entries)
    # The encoded file is complete before the game builds an OS path to open it.
    writer.call(0x5a96c0, writer.service, op, vector, until=0x5a99bb)
    size = writer.word(writer.vm.reg_read(UC_X86_REG_EBP) - 8)
    if size != 416:
        raise ValueError("Unexpected stock directory output size")
    return bytes(writer.vm.mem_read(writer.word(writer.service + 0x33c) + 0x8000, size))


def progress_file(member):
    checksum = hashlib.sha256(member).digest()
    if not member or len(member) + len(checksum) > PROFILE_CAPACITY:
        raise ValueError("Progress file does not fit the retail profile buffer")
    return member + checksum


def decode_logical_member(data, *, padded):
    reader = zlib.decompressobj(-15)
    try:
        group = reader.decompress(data, 2 * 1024 * 1024)
    except zlib.error as error:
        raise ValueError("Invalid compressed profile member") from error
    if not reader.eof:
        raise ValueError("Expected one complete, bounded profile member")
    member_size = len(data) - len(reader.unused_data)
    checked = progress_file(data[:member_size])
    tail = data[len(checked):]
    if not data.startswith(checked) or (tail.strip(b"\xbe") if padded else tail):
        raise ValueError("Profile checksum or padding differs from the stock format")
    return group


def encode_settings_file(group, progress_size):
    checked = progress_file(zlib.compress(group, wbits=-15))
    size = PROFILE_CAPACITY - progress_size
    if progress_size <= 0 or len(checked) > size:
        raise ValueError("Insufficient settings padding for the new progress file")
    return checked.ljust(size, b"\xbe")
