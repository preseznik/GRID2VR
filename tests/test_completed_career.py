import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/career_profile"))
from career_model import RESULT, U32, complete_events, display_label, load_catalog, rename, text_setting


class CompletedCareerTests(unittest.TestCase):
    def setUp(self):
        self.events = {row[0]: row for row in load_catalog()["events"]}

    def table(self, changes=None, ids=None):
        ids = list(self.events) if ids is None else ids
        data = bytearray()
        for event in ids:
            row = [0] * 22
            row[0:4] = [event, -1, -1, -1]
            for index, value in (changes or {}).get(event, {}).items():
                row[index] = value
            data.extend(U32.pack(1) + RESULT.pack(*row))
        data.extend(U32.pack(0) * (150 - len(ids)))
        return data

    def test_counts_rounds_repeats_and_an_unwon_challenge(self):
        self.assertEqual(len(self.events), 130)
        challenge = next(event for event, row in self.events.items() if row[7])
        multi = next(event for event, row in self.events.items() if row[4] > 1)
        data = self.table({challenge: {1: 2, 12: 2, 19: 3, 21: 1},
                           multi: {1: 1, 12: 4, 19: 5, 21: 2}})
        expected = sum(row[4] for row in self.events.values())
        expected += 2 * self.events[challenge][4] + 3 * self.events[multi][4]
        self.assertEqual(complete_events(data, self.events, 0, len(data)), expected)
        offset = list(self.events).index(challenge) * (RESULT.size + 4) + 4
        row = RESULT.unpack_from(data, offset)
        self.assertEqual((row[1], row[12], row[13], row[19], row[21]), (0, 3, 1, 4, 2))
        saved = bytes(data)
        self.assertEqual(complete_events(data, self.events, 0, len(data)), expected)
        self.assertEqual(data, saved)

    def test_incomplete_ambiguous_and_damaged_results(self):
        ids = list(self.events)
        for data in (self.table(ids=ids[:-1]), self.table(ids=ids[:-1] + [ids[1]]),
                     self.table()[:-1], self.table({ids[3]: {12: 3, 19: 1}})):
            with self.assertRaises(ValueError):
                complete_events(data, self.events, 0, len(data))

    def test_menu_keeps_online_count_and_language(self):
        for wording in ("Career Races: 88, Online Races: 1", "Carreras: 88, En línea: 1"):
            raw = ("Old Name - GRID 2 " + wording + "\0").encode("utf-16-le")
            expected = "Driver1 - GRID 2 " + wording.replace("88", "193") + "\0"
            self.assertEqual(display_label(raw, "Driver1", 88, 193).decode("utf-16-le"), expected)
            with self.assertRaises(ValueError):
                display_label(raw, "Driver1", 40, 193)
        blank = " - GRID 2 Career Races:  182, Online Races: 0\0".encode("utf-16-le")
        self.assertEqual(display_label(blank, "Driver1", 182, 182).decode("utf-16-le"),
                         "Driver1 - GRID 2 Career Races: 182, Online Races: 0\0")

    def test_variable_driver_names_preserve_following_setting(self):
        first, last = (text_setting(key) for key in (b"Player_FirstName", b"Player_LastName"))
        tail = b"\x01\x0a\0\0\0profile_id" + bytes(range(32))
        for name in ("A", "Z" * 24):
            data = bytearray(first + U32.pack(2) + b"Jo" + last + U32.pack(4) + b"Park" + tail)
            rename(data, name)
            self.assertEqual(data, first + U32.pack(0) + last + U32.pack(len(name)) + name.encode() + tail)


if __name__ == "__main__":
    unittest.main()
