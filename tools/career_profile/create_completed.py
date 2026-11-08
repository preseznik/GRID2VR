"""Create a completed offline career inside the launcher's unpublished staging directory."""
import os
from pathlib import Path
import re
import sys
import zlib

from career_model import complete, display_label, load_catalog
from stock_save import (DIRECTORY, FILE_IDS, NAMES, PROFILE_CAPACITY, StockWriter, checksum,
                        decode_logical_member, directory_fields, encode_settings_file, progress_file,
                        read_chunks, write_directory)


def prepare(folder, name, game, verified_hash):
    files = [folder / filename for filename in (*NAMES, DIRECTORY)]
    if (set(folder.iterdir()) != set(files) or any(not p.is_file() or p.is_symlink() for p in files)
            or files[2].stat().st_size != 416):
        raise ValueError("Expected the three normal GRID 2 profile files")
    reader, writer = StockWriter(game, verified_hash), StockWriter(game, verified_hash)
    label, rows = directory_fields(reader, files[2].read_bytes())
    if tuple(row[0] for row in rows) != FILE_IDS or sum(row[1] for row in rows) != PROFILE_CAPACITY:
        raise ValueError("Unsupported normal profile directory")
    catalog = load_catalog()
    frames, physical, entries = [], [], []
    counts = None
    for i, (file_id, size, stored) in enumerate(rows):
        if not 32 < size < PROFILE_CAPACITY or files[i].stat().st_size != ((size + 15) & ~15):
            raise ValueError("Invalid physical save length")
        logical = read_chunks(reader, files[i].read_bytes())[:size]
        if checksum(reader, logical) != stored:
            raise ValueError("Source file checksum disagrees with its directory")
        group, original, represented = complete(decode_logical_member(logical, padded=i == 1), name, catalog)
        if counts is not None and counts != (original, represented):
            raise ValueError("The two normal profile files disagree about the career")
        counts = original, represented
        frame = (progress_file(zlib.compress(group, wbits=-15)) if i == 0
                 else encode_settings_file(group, len(frames[0])))
        if decode_logical_member(frame, padded=i == 1) != group:
            raise ValueError("Completed career did not round-trip")
        frames.append(frame)
        physical.append(writer.encode(frame))
        entries.append((file_id, len(frame), writer.last_checksum))
    label = display_label(label, name, *counts)
    directory = write_directory(writer, label, entries)
    decoded_label, decoded_entries = directory_fields(reader, directory)
    if decoded_label != label or decoded_entries != entries:
        raise ValueError("Completed load-menu directory did not round-trip")
    for payload, frame, (_, size, digest) in zip(physical, frames, decoded_entries):
        if read_chunks(reader, payload)[:size] != frame or checksum(reader, frame) != digest:
            raise ValueError("Completed profile failed stock file readback")
    return dict(zip(files, [*physical, directory]))


def main(remote, name, game, verified_hash):
    absolute = str(remote.resolve(strict=True))
    if os.name == "nt" and not absolute.startswith("\\\\?\\"):
        absolute = ("\\\\?\\UNC\\" + absolute[2:] if absolute.startswith("\\\\") else "\\\\?\\" + absolute)
    remote = Path(absolute)
    if (remote.name != "remote" or not re.fullmatch(r"\.creating-[0-9a-f]{32}", remote.parent.name)
            or remote.parent.parent.name != "profiles"):
        raise ValueError("Expected a new launcher profile's private staging folder")
    files = prepare(remote, name, game, verified_hash)
    for path, data in files.items():
        path.write_bytes(data)
    if any(path.read_bytes() != data for path, data in files.items()):
        raise OSError("Personalized profile could not be verified on disk")


if __name__ == "__main__":
    main(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]), sys.argv[4])
