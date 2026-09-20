#!/usr/bin/env python3
# ps5-native-app-boilerplate - Strict canary shader input preparation.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare strict AGC shader inputs for the PPSA99998 canary."""

from __future__ import annotations

import argparse
import hashlib
import struct
from pathlib import Path


AGC_MAGIC = 0x34333231
STAGES = {"vertex": (2, 0x080), "pixel": (1, 0x006)}


class PackageError(ValueError):
    pass


def section(data: bytes, wanted: str) -> bytes:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise PackageError("not an ELF64 package")
    shoff = struct.unpack_from("<Q", data, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 58)
    if shentsize < 64 or not shnum or shstrndx >= shnum:
        raise PackageError("invalid section table")
    records = []
    for index in range(shnum):
        at = shoff + index * shentsize
        if at + 64 > len(data):
            raise PackageError("truncated section table")
        records.append(struct.unpack_from("<IIQQQQIIQQ", data, at))
    names = records[shstrndx]
    names_data = data[names[4]:names[4] + names[5]]
    for record in records:
        name_at = record[0]
        end = names_data.find(b"\0", name_at)
        name = names_data[name_at:end].decode("ascii") if end >= 0 else ""
        start, size = record[4], record[5]
        if name == wanted:
            if start + size > len(data):
                raise PackageError(f"{wanted} is outside the package")
            return data[start:start + size]
    raise PackageError(f"missing {wanted}")


def inspect(path: Path, label: str) -> tuple[bytes, str, str]:
    data = path.read_bytes()
    header = section(data, ".shader_header")
    code = section(data, ".shader_text")
    if len(header) < 96 or struct.unpack_from("<I", header, 0)[0] != AGC_MAGIC:
        raise PackageError("invalid AGC shader header")
    header_size, code_size = struct.unpack_from("<II", header, 64)
    if header_size != len(header) or code_size != len(code):
        raise PackageError("declared header/code sizes do not match sections")
    stage, checksum_offset = STAGES[label]
    if header[90] != stage:
        raise PackageError(f"expected {label} stage {stage}, found {header[90]}")
    table = 32 + struct.unpack_from("<Q", header, 32)[0]
    count = header[92]
    if table + count * 8 > len(header):
        raise PackageError("shader-register table is outside the header")
    values = {
        struct.unpack_from("<H", header, table + index * 8)[0]:
        struct.unpack_from("<I", header, table + index * 8 + 4)[0]
        for index in range(count)
    }
    if checksum_offset not in values:
        raise PackageError(
            f"{label} package lacks program-checksum register 0x{checksum_offset:03x}")
    if values[checksum_offset] == 0:
        raise PackageError(
            f"{label} program-checksum register is zero/unresolved")
    fnv = 0xCBF29CE484222325
    for byte in data:
        fnv = ((fnv ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return data, f"{fnv:016x}", hashlib.sha256(data).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vertex", type=Path)
    parser.add_argument("pixel", type=Path)
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        vertex, vertex_fnv, vertex_sha = inspect(args.vertex, "vertex")
        pixel, pixel_fnv, pixel_sha = inspect(args.pixel, "pixel")
    except (OSError, struct.error, UnicodeError, PackageError) as exc:
        parser.error(str(exc))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "vertex.bin").write_bytes(vertex)
    (args.output / "pixel.bin").write_bytes(pixel)
    (args.output / "checksums.txt").write_text(
        f"vertex {vertex_fnv}\npixel {pixel_fnv}\n", encoding="ascii")
    (args.output / "SHA256SUMS").write_text(
        f"{vertex_sha}  vertex.bin\n{pixel_sha}  pixel.bin\n", encoding="ascii")
    print(f"prepared strict canary packages in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
