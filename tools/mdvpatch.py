#!/usr/bin/env python3
"""Retarget the drive names hardcoded inside a Sinclair QL .MDV cartridge image.

Most QL software is written to run from one fixed drive: the boot program says
EXEC_W mdv1_game, and some executables carry "mdv1_datafile" strings of their
own. Mount such a cartridge in drive 2 and it goes looking for its files on
drive 1. This tool rewrites those names inside the image so the cartridge runs
from another drive. Works on Windows, Linux and macOS; no dependencies.

Requires: Python 3.8+.

Commands:
  list  IMAGE                  list the files on the cartridge
  scan  IMAGE                  show every drive name found, with its context
  retarget IMAGE --to mdv2     write a copy that runs from mdv2_
      [--from mdv1]            the name to replace (default: mdv1)
      [-o OUT]                 output file (default: IMAGE with _<to> added,
                               replacing a _<from> ending if there is one)
      [--files a,b]            only patch these files (default: all files)
      [--dry-run]              show what would change, write nothing

A drive name is three letters, a digit 1-8 and an underscore (mdv1_, flp2_).
The replacement is the same length, so nothing in the image moves. Each
changed sector gets its data checksum recomputed (sum of the 512 data bytes
plus 0x0F0F, little-endian), so the result is valid for any emulator or tool.
The original image is never modified.

A match is a plain text search. It cannot find a name the program builds at
run time, and in principle it could hit five bytes of unrelated binary data
that happen to spell a drive name; 'scan' shows the context of every match so
that can be judged before patching.

.MDV layout (per 686-byte sector, 255 sectors, 174930 bytes in all):
  12 preamble | 14 header + 2 checksum | 12 preamble |
  file no, block no + 2 checksum | 8 preamble | 512 data + 2 checksum | ...
Block 0 of a file starts with its 64-byte QDOS header (length, type, name).
"""
import argparse
import os
import re
import struct
import sys

IMAGE_SIZE = 174930
SECTOR_SIZE = 686
SECTOR_COUNT = 255
OFF_FILE_NO = 40          # record header: file number, block number
OFF_DATA = 52
OFF_DATA_CKS = 564
BLOCK_SIZE = 512
FILE_HEADER_SIZE = 64

# File numbers that are not files: the directory, the sector map, free and bad
# sectors.
NOT_A_FILE = (0x00, 0xF8, 0xFD, 0xFF)

DEVICE_RE = re.compile(rb"[A-Za-z]{3}[1-8]_")
CONTEXT_BYTES = 16


def checksum(data):
    return (sum(data) + 0x0F0F) & 0xFFFF


def load_image(path):
    with open(path, "rb") as f:
        image = bytearray(f.read())
    if len(image) != IMAGE_SIZE:
        sys.exit(f"{path}: {len(image)} bytes, expected {IMAGE_SIZE} "
                 f"- not a standard .MDV image")
    return image


def find_files(image):
    """Return {name: (length, type, [sector offsets in block order])}."""
    blocks_by_file = {}
    for sector in range(SECTOR_COUNT):
        offset = sector * SECTOR_SIZE
        file_no, block_no = image[offset + OFF_FILE_NO], image[offset + OFF_FILE_NO + 1]
        if file_no in NOT_A_FILE:
            continue
        blocks_by_file.setdefault(file_no, {})[block_no] = offset

    files = {}
    for blocks in blocks_by_file.values():
        # Sectors left over from deleted files still carry a file number; a
        # real file has a sane header and an unbroken run of blocks from 0.
        if sorted(blocks) != list(range(len(blocks))):
            continue
        header = image[blocks[0] + OFF_DATA: blocks[0] + OFF_DATA + FILE_HEADER_SIZE]
        length = struct.unpack(">I", header[0:4])[0]
        name_length = struct.unpack(">H", header[14:16])[0]
        if not 1 <= name_length <= 36:
            continue
        if not FILE_HEADER_SIZE <= length <= len(blocks) * BLOCK_SIZE:
            continue
        name = header[16:16 + name_length].decode("latin-1")
        files[name] = (length, header[5], [blocks[b] for b in sorted(blocks)])
    return files


def read_file(image, sector_offsets):
    return bytearray(b"".join(image[o + OFF_DATA: o + OFF_DATA + BLOCK_SIZE]
                              for o in sector_offsets))


def write_file(image, sector_offsets, data):
    """Put a file's blocks back and fix the checksum of every changed sector."""
    for index, offset in enumerate(sector_offsets):
        block = data[index * BLOCK_SIZE:(index + 1) * BLOCK_SIZE]
        if image[offset + OFF_DATA: offset + OFF_DATA + BLOCK_SIZE] == block:
            continue
        image[offset + OFF_DATA: offset + OFF_DATA + BLOCK_SIZE] = block
        struct.pack_into("<H", image, offset + OFF_DATA_CKS, checksum(block))


def bad_checksums(image):
    return sum(
        struct.unpack_from("<H", image, s * SECTOR_SIZE + OFF_DATA_CKS)[0]
        != checksum(image[s * SECTOR_SIZE + OFF_DATA: s * SECTOR_SIZE + OFF_DATA + BLOCK_SIZE])
        for s in range(SECTOR_COUNT))


def device_matches(data, length):
    """Drive names in a file's contents (the 64-byte header is skipped)."""
    return [m for m in DEVICE_RE.finditer(bytes(data[:length]))
            if m.start() >= FILE_HEADER_SIZE]


def context(data, length, start):
    lo = max(FILE_HEADER_SIZE, start - CONTEXT_BYTES)
    hi = min(length, start + 5 + CONTEXT_BYTES)
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data[lo:hi])


def parse_device(text):
    name = text.rstrip("_")
    if not re.fullmatch(r"[A-Za-z]{3}[1-8]", name):
        sys.exit(f"'{text}' is not a drive name; expected something like mdv2")
    return name.lower()


def cmd_list(args):
    image = load_image(args.image)
    for name, (length, file_type, _) in sorted(find_files(image).items()):
        kind = "executable" if file_type == 1 else "data"
        print(f"  {name:<36} {length - FILE_HEADER_SIZE:>7} bytes  {kind}")


def cmd_scan(args):
    image = load_image(args.image)
    found = False
    for name, (length, _, sectors) in sorted(find_files(image).items()):
        data = read_file(image, sectors)
        for match in device_matches(data, length):
            found = True
            print(f"  {name:<20} {match.group(0).decode():<6} "
                  f"...{context(data, length, match.start())}...")
    if not found:
        print("  no drive names found")


def cmd_retarget(args):
    source, target = parse_device(args.source), parse_device(args.to)
    if source == target:
        sys.exit("--from and --to are the same drive")
    image = load_image(args.image)
    files = find_files(image)
    bad_before = bad_checksums(image)

    selected = sorted(files)
    if args.files:
        wanted = [f.strip() for f in args.files.split(",") if f.strip()]
        missing = [f for f in wanted if f not in files]
        if missing:
            sys.exit(f"not on the cartridge: {', '.join(missing)}")
        selected = wanted

    changes = 0
    for name in selected:
        length, _, sectors = files[name]
        data = read_file(image, sectors)
        for match in device_matches(data, length):
            found = match.group(0)[:4]
            if found.lower() != source.encode():
                continue
            # Keep the capitalisation the program used (MDV1_ stays upper).
            replacement = target.upper() if found.isupper() else target
            print(f"  {name:<20} {context(data, length, match.start())}")
            data[match.start():match.start() + 4] = replacement.encode()
            changes += 1
        write_file(image, sectors, data)

    if changes == 0:
        sys.exit(f"no '{source}_' found - nothing to do")
    print(f"{changes} name(s) changed from {source}_ to {target}_")
    if bad_checksums(image) != bad_before:
        sys.exit("internal error: checksums did not come out right; nothing written")
    if args.dry_run:
        print("dry run - nothing written")
        return

    out = args.output
    if not out:
        base, ext = os.path.splitext(args.image)
        # Game_mdv2.mdv patched back to mdv1 becomes Game_mdv1.mdv, not
        # Game_mdv2_mdv1.mdv.
        if base.lower().endswith(f"_{source}"):
            base = base[:-len(source) - 1]
        out = f"{base}_{target}{ext}"
    if os.path.abspath(out) == os.path.abspath(args.image):
        sys.exit("refusing to overwrite the original image; choose another -o")
    with open(out, "wb") as f:
        f.write(image)
    print(f"written: {out}")


def main():
    parser = argparse.ArgumentParser(
        description="Retarget the drive names hardcoded in a QL .MDV image.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("list", help="list the files on the cartridge")
    p.add_argument("image")
    p.set_defaults(run=cmd_list)

    p = sub.add_parser("scan", help="show every drive name found")
    p.add_argument("image")
    p.set_defaults(run=cmd_scan)

    p = sub.add_parser("retarget", help="write a copy that runs from another drive")
    p.add_argument("image")
    p.add_argument("--to", required=True, help="new drive name, e.g. mdv2")
    p.add_argument("--from", dest="source", default="mdv1",
                   help="drive name to replace (default: mdv1)")
    p.add_argument("-o", "--output", help="output file")
    p.add_argument("--files", help="comma-separated files to patch (default: all)")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(run=cmd_retarget)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
