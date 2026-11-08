"""Apply the stock base-game single-player career catalog to a copy of a normal save."""
import json
from pathlib import Path
import re
from struct import Struct


U32 = Struct("<I")
WORD = Struct("<i")
ROW = Struct("<iii")
RESULT = Struct("<22i")
SPONSOR = Struct("<iBiii")
BASE_BYTES = 10112
BASE_HEADER = bytes.fromhex("a4be953a 00 0f000000 80270000 01000000")
COUNT = U32
ENTRY = Struct("<ii")


def component_tag(name):
    value = 5381
    for byte in name.lower().encode("ascii"):
        value = (33 * value + byte) & 0xffffffff
    return U32.pack(value) + bytes(1)


def unique(data, marker):
    if data.count(marker) != 1:
        raise ValueError("Unrecognized GRID 2 save layout")
    return data.index(marker)


def base_unlock_tables(group):
    if group.count(BASE_HEADER) != 1:
        raise ValueError("Expected one measured base-game Unlocks package")
    start = group.index(BASE_HEADER) + len(BASE_HEADER) - WORD.size
    end = start + BASE_BYTES
    if end > len(group):
        raise ValueError("Truncated base-game Unlocks package")

    # The native serializer traverses registered tables in database-index order.
    tables = []
    pos = start + WORD.size
    for _ in range(16):
        rows = {}
        while pos + WORD.size <= end and WORD.unpack_from(group, pos)[0] != -1:
            if pos + ROW.size > end:
                raise ValueError("Truncated Unlocks row")
            key, _, state = ROW.unpack_from(group, pos)
            if key in rows:
                raise ValueError("Duplicate Unlocks row")
            rows[key] = (pos, state)
            pos += ROW.size
        if pos + WORD.size > end:
            raise ValueError("Missing Unlocks table terminator")
        pos += WORD.size
        tables.append(rows)
    if pos != end:
        raise ValueError("Unexpected trailing base-game Unlocks table")
    return tables


def event_rewards(group):
    start = component_tag("rewards_save")
    end = component_tag("VehicleSettingsManager")
    if group.count(start) != 1 or group.count(end) != 1:
        raise ValueError("Ambiguous rewards component")
    pos = group.index(start) + len(start)
    limit = group.index(end)

    def count():
        nonlocal pos
        if pos + COUNT.size > limit:
            raise ValueError("Truncated rewards component")
        value, = COUNT.unpack_from(group, pos)
        pos += COUNT.size
        return value

    # The base serializer frames each package independently. The three ordered
    # maps follow the packages and the five words describing the last award.
    if count() != 15:
        raise ValueError("Unexpected number of reward packages")
    for _ in range(15):
        size = count()
        if size > limit - pos:
            raise ValueError("Truncated reward package")
        pos += size
    pos += 20
    maps = []
    for _ in range(3):
        begin = pos
        length = count() * ENTRY.size
        if length > limit - pos:
            raise ValueError("Truncated reward map")
        pairs = list(ENTRY.iter_unpack(group[pos:pos+length]))
        keys = [key for key, _ in pairs]
        if keys != sorted(set(keys)):
            raise ValueError("Repeated or unordered reward key")
        pos += length
        maps.append((begin, pos, pairs))
    if limit - pos != 8:
        raise ValueError("Unexpected reward component ending")
    return maps[1]


def text_setting(key):
    return b"\x04" + U32.pack(len(key)) + key


def name_span(data, key):
    start = unique(data, text_setting(key)) + len(text_setting(key))
    size, = U32.unpack_from(data, start)
    if size > 256 or start + 4 + size > len(data):
        raise ValueError("Invalid driver name in the source career")
    return start, start + 4 + size


def rename(data, name):
    first, end_first = name_span(data, b"Player_FirstName")
    last, end_last = name_span(data, b"Player_LastName")
    if (end_first + len(text_setting(b"Player_LastName")) != last
            or not data.startswith(b"\x01\x0a\x00\x00\x00profile_id", end_last)):
        raise ValueError("Unexpected driver settings")
    # The stock save-menu formatter emits a surname alone. With only a first
    # name it emits nothing the next time the game rewrites the directory.
    value = name.encode("ascii")
    data[last:end_last] = U32.pack(len(value)) + value
    data[first:end_first] = U32.pack(0)


def load_catalog():
    return json.loads(Path(__file__).with_name("catalog.json").read_bytes())


def display_label(raw, name, old_count, new_count):
    # Keep the game's localized wording and its separate online-race count.
    text = raw.decode("utf-16-le").removesuffix("\0")
    match = re.fullmatch(r".*?( - GRID 2 .+?:)\s*(\d+)(\s*,\s*.+?:\s*\d+\s*)", text)
    if match is None or int(match[2]) != old_count:
        raise ValueError("The career count and load-menu label disagree in the source save")
    value = f"{name}{match[1]} {new_count}{match[3]}\0".encode("utf-16-le")
    if len(value) > 256:
        raise ValueError("The localized load-menu label is too long")
    return value


def complete_events(data, events, start, limit):
    # The stock base-game category begins with California event 483. Each file
    # has 150 presence flags; only occupied slots contain an 88-byte result.
    first = U32.pack(1) + U32.pack(483)
    pos = start + unique(data[start:limit], first)
    seen = set()
    count = 0
    for _ in range(150):
        if pos + 4 > limit:
            raise ValueError("Truncated career event table")
        flag, = U32.unpack_from(data, pos)
        pos += 4
        if flag == 0:
            continue
        if flag != 1 or pos + RESULT.size > limit:
            raise ValueError("Unrecognized career event record")
        result = RESULT.unpack_from(data, pos)
        event, rank, finishes, wins, starts, podiums = (result[i] for i in (0, 1, 12, 13, 19, 21))
        if event in events:
            if event in seen:
                raise ValueError("Duplicate career result")
            seen.add(event)
            _, _, _, _, rounds, points, fans, vehicle = events[event]
            if not (0 <= wins <= podiums <= finishes <= starts < 0x7fffffff):
                raise ValueError("Invalid race counts in the source career")
            placed = rank >= 0 and finishes > 0
            if not placed or (vehicle and (rank != 0 or wins == 0)):
                # Record a win, preserving the player's prior attempts and finishes.
                finishes += 1
                RESULT.pack_into(data, pos, event, 0, 0, rank if placed else -1, 2147483647, 0,
                                 1000, 1000, 0, 1000, 1000, 1000, finishes, wins + 1, 1,
                                 rounds * points, fans, 0, 0, starts + 1, 0, podiums + 1)
            count += rounds * finishes
        pos += RESULT.size
    if seen != events.keys() or not 0 < count <= 0x7fffffff:
        raise ValueError("The normal save is missing stock career events. Open Career and save in GRID 2 first.")
    return count


def complete(data, name, catalog):
    data = bytearray(data)
    if not re.fullmatch(r"[A-Za-z0-9]{1,24}", name):
        raise ValueError("Use 1–24 letters or numbers for the driver name")
    events = {e[0]: e for e in catalog["events"]}
    if len(events) != 130:
        raise ValueError("Invalid stock career catalog")
    history = unique(data, component_tag("RaceHistory") + Struct("<III").pack(1337, 152, 41))
    following = unique(data, component_tag("career manager sdu name"))
    count = complete_events(data, events, history, following)
    counter = history + 5 + 124
    old_count, = U32.unpack_from(data, counter)
    U32.pack_into(data, counter, count)
    U32.pack_into(data, history + 5 + 12 + 16 * 20, 35_003_500)
    # This fixed suffix ends the RaceHistory component. Validate each saved
    # objective's layout before replacing it with the completed sponsor choices.
    pos = following - 9 - 456 - 6 * SPONSOR.size
    for selection in catalog["sponsors"]:
        phase, enabled, *_ = SPONSOR.unpack_from(data, pos)
        if not 0 <= phase <= 8 or enabled not in (0, 1):
            raise ValueError("Unexpected sponsor selection")
        SPONSOR.pack_into(data, pos, *selection)
        pos += SPONSOR.size
    for _, *values in catalog["objectives"]:
        layout = Struct("<iBi" if len(values) == 3 else "<iB")
        state, flag, *_ = layout.unpack_from(data, pos)
        if state not in (0, 1, 2, 3) or flag not in (0, 1):
            raise ValueError("Unexpected objective state")
        layout.pack_into(data, pos, *values)
        pos += layout.size
    if pos != following - 9:
        raise ValueError("Invalid career objective layout")
    Struct("<Bii").pack_into(data, pos, 1, 8, 4)
    Struct("<ii").pack_into(data, unique(data, component_tag("TimelineParams")) + 5, 8, 0)
    Struct("<5i").pack_into(data, unique(data, component_tag("CareerParams")) + 5, 5, 8, 34, 565, 1)
    tables = base_unlock_tables(data)
    targets = {2: set(events), 3: {e[1] for e in events.values()}, 5: {e[2] for e in events.values()},
               6: {e[3] for e in events.values()}, 8: set(catalog["goldTrophies"]),
               9: {e[7] for e in events.values() if e[7]}}
    for index, keys in targets.items():
        if not keys <= tables[index].keys():
            raise ValueError("Incompatible base-game unlock table")
        for key in keys:
            offset, _ = tables[index][key]
            WORD.pack_into(data, offset + 8, 2)
    # Preserve other earned rewards and all DLC package data.
    start, end, pairs = event_rewards(data)
    rewards = dict(pairs)
    for event in catalog["rewardEvents"]:
        rewards.setdefault(event, 0)
    data[start:end] = U32.pack(len(rewards)) + b"".join(ENTRY.pack(*entry) for entry in sorted(rewards.items()))
    WORD.pack_into(data, unique(data, component_tag("VehicleSettingsManager")) - 4, 8)
    rename(data, name)
    return bytes(data), old_count, count
