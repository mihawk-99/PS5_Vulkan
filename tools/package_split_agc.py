#!/usr/bin/env python3
# ps5-native-app-boilerplate - Split-AGC canary packaging.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wrap known-good split AGC header/text blobs as canary ELF packages."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path

MAGIC = 0x34333231
RESOURCES_MAGIC = 0x3152474E
RESOURCES_LIMIT = 0x1000
STAGES = {"vertex": (2, 0x080), "pixel": (1, 0x006)}


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def inspect_split(label: str, header: bytes, text: bytes) -> int:
    if len(header) < 96 or struct.unpack_from("<I", header)[0] != MAGIC:
        raise ValueError(f"{label}: invalid AGC header")
    if struct.unpack_from("<I", header, 64)[0] != len(header):
        raise ValueError(f"{label}: header size field does not match")
    if struct.unpack_from("<I", header, 68)[0] != len(text):
        raise ValueError(f"{label}: text size field does not match")
    stage, checksum_offset = STAGES[label]
    if header[0x5A] != stage:
        raise ValueError(f"{label}: expected stage {stage}, got {header[0x5A]}")
    table = 32 + struct.unpack_from("<Q", header, 32)[0]
    count = header[92]
    if table + count * 8 > len(header):
        raise ValueError(f"{label}: register table is outside header")
    values = [
        (struct.unpack_from("<H", header, table + index * 8)[0],
         struct.unpack_from("<I", header, table + index * 8 + 4)[0])
        for index in range(count)
    ]
    matches = [value for offset, value in values if offset == checksum_offset]
    if not matches or matches[-1] == 0:
        raise ValueError(f"{label}: unresolved program-checksum register 0x{checksum_offset:03x}")
    return matches[-1]


def wrap(header: bytes, text: bytes) -> bytes:
    names = b"\0.shader_text\0.shader_header\0.shstrtab\0"
    text_offset = 0x100
    header_offset = align(text_offset + len(text), 8)
    names_offset = align(header_offset + len(header), 8)
    shoff = align(names_offset + len(names), 8)
    total = shoff + 4 * 64
    output = bytearray(total)
    output[text_offset:text_offset + len(text)] = text
    output[header_offset:header_offset + len(header)] = header
    output[names_offset:names_offset + len(names)] = names
    elf_header = (b"\x7fELF\x02\x01\x01" + b"\0" * 9,
                  3, 0x3E, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 4, 3)
    struct.pack_into("<16sHHIQQQIHHHHHH", output, 0, *elf_header)
    records = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 0x6, 0, 0, text_offset, len(text), 0, 0, 0x100, 0),
        (14, 0x3, 0, 0, header_offset, len(header), 0, 0, 8, 0),
        (29, 0x3, 0, 0, names_offset, len(names), 0, 0, 1, 0),
    ]
    for index, record in enumerate(records):
        struct.pack_into("<IIQQQQIIQQ", output, shoff + index * 64, *record)
    return bytes(output)


def inspect_resources(data: bytes) -> None:
    # The live-frame canary copies this into a 4 KiB workspace slot and
    # relocates its descriptor tables in place.
    if len(data) < 0x800 or len(data) >= RESOURCES_LIMIT:
        raise ValueError(f"resources: size {len(data)} outside [0x800, 0x1000)")
    if struct.unpack_from("<I", data)[0] != RESOURCES_MAGIC:
        raise ValueError("resources: invalid descriptor-table magic")


def digest(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vertex-header", type=Path, required=True)
    parser.add_argument("--vertex-text", type=Path, required=True)
    parser.add_argument("--pixel-header", type=Path, required=True)
    parser.add_argument("--pixel-text", type=Path, required=True)
    parser.add_argument("--resources", type=Path,
                        help="descriptor-table blob for the PPSA99996 live frame")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args()
    sources = {
        "vertex": (args.vertex_header.read_bytes(), args.vertex_text.read_bytes()),
        "pixel": (args.pixel_header.read_bytes(), args.pixel_text.read_bytes()),
    }
    packages = {}
    registers = {}
    for label, (header, text) in sources.items():
        registers[label] = inspect_split(label, header, text)
        packages[label] = wrap(header, text)
    files = {f"{label}.bin": package for label, package in packages.items()}
    manifest = {label: packages[label] for label in ("vertex", "pixel")}
    if args.resources:
        resources = args.resources.read_bytes()
        inspect_resources(resources)
        files["resources.bin"] = resources
        manifest["resources"] = resources
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (args.output / name).write_bytes(data)
    # Write bytes so receipts keep LF endings; sha256sum --check rejects CRLF
    # file names.
    (args.output / "checksums.txt").write_bytes(
        "".join(f"{label} {digest(data)}\n" for label, data in manifest.items()).encode("ascii"))
    (args.output / "SHA256SUMS").write_bytes(
        "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                for name, data in files.items()).encode("ascii"))
    provenance = (
        "ProsperoLight split AGC assets; headers and text copied byte-for-byte.\n"
        f"vertex text: {args.vertex_text.name}\n"
        f"pixel text: {args.pixel_text.name}\n"
        f"vertex checksum register 0x080 = 0x{registers['vertex']:08x}\n"
        f"pixel checksum register 0x006 = 0x{registers['pixel']:08x}\n")
    if args.resources:
        provenance += f"resources: {args.resources.name} copied byte-for-byte\n"
    (args.output / "PROVENANCE.txt").write_bytes(provenance.encode("ascii"))
    print(f"prepared ProsperoLight canary packages in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
