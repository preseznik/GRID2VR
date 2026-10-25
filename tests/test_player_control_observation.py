"""Candidate parsing must not follow stale or incidental heap pointers."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from observe_player_control import MANAGER_TABLE, PARENT_TABLE, MODE, UPDATE_BYTES, guarded_update, manager_header


class ObservationGuards(unittest.TestCase):
    def setUp(self):
        self.base = 0x5E0000
        self.data = bytearray(0x64)
        for offset, value in ((0, self.base+MANAGER_TABLE), (0x14, 0x10004000), (0x18, 1), (0x2C, 2), (0x30, 2)):
            struct.pack_into('<I', self.data, offset, value)
        self.data[0x24] = 1
        self.parent = struct.pack('<I', self.base+PARENT_TABLE)

    def test_supported_shape_is_only_a_candidate(self):
        value = manager_header(self.data, self.parent, self.base)
        self.assertEqual(value['count'], 1)
        self.assertEqual(value['mode30'], 2)
        self.assertNotIn('playerControlValidated', value)

    def test_rejects_incidental_and_stale_objects(self):
        for offset, value in ((0, 0), (0x14, 0), (0x14, 0x10004001), (0x18, 0), (0x18, 17)):
            data = self.data.copy()
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset, value=value):
                self.assertIsNone(manager_header(data, self.parent, self.base))
        self.assertIsNone(manager_header(self.data, bytes(4), self.base))
        self.assertIsNone(manager_header(self.data[:-1], self.parent, self.base))
        for offset in (0x24, 0x34, 0x35, 0x36, 0x37):
            data = self.data.copy()
            data[offset] = 2
            self.assertIsNone(manager_header(data, self.parent, self.base))

    def test_aslr_guard_changes_only_relocated_global_operand(self):
        self.assertEqual(guarded_update(0x400000), UPDATE_BYTES)
        actual = guarded_update(self.base)
        self.assertEqual(struct.unpack_from('<I', actual, 21)[0], self.base+MODE)
        self.assertEqual(actual[:21]+actual[25:], UPDATE_BYTES[:21]+UPDATE_BYTES[25:])

    def test_unknown_modes_are_observed_without_claiming_control(self):
        for mode in (4, 0xFFFFFFFF):
            struct.pack_into('<I', self.data, 0x2C, mode)
            self.assertEqual(manager_header(self.data, self.parent, self.base)['mode2c'], mode)


if __name__ == '__main__':
    unittest.main()
