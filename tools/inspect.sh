#!/usr/bin/env bash
# ps5-native-app-boilerplate - Static ELF/FSELF validator.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks the loader-visible structure of a generated native application
# without changing it: OS/ABI, ELF type, program headers, the flags-zero
# linking LOAD, the process-parameter record and, for a development FSELF
# container, its segment table and extension records.
#
# Usage: tools/inspect.sh FILE
# Exits 2 on a usage error and 1 when a structural check fails.

set -euo pipefail

usage() {
    cat <<'EOF'
usage: tools/inspect.sh FILE

  FILE   Raw ELF or development FSELF container, for example
         dist/PPSA99999/eboot.bin or runtime/libc.prx

Reports the loader-visible structure and exits non-zero on any structural
error. Read-only: the input file is never modified.
EOF
}

if [[ ${1:-} == -h || ${1:-} == --help ]]; then
    usage
    exit 0
fi
if (( $# != 1 )); then
    usage >&2
    exit 2
fi
[[ -f $1 ]] || {
    echo "inspect: file not found: $1" >&2
    exit 2
}
command -v python3 >/dev/null || {
    echo "inspect: python3 is required" >&2
    exit 2
}

exec python3 - "$1" <<'PY'
from pathlib import Path
import struct
import sys

path = Path(sys.argv[1])
data = path.read_bytes()


def need(offset, length):
    if offset < 0 or offset + length > len(data):
        raise SystemExit(f"inspect: unexpected end of file at {offset:#x} in {path}")


def u16(offset):
    need(offset, 2)
    return struct.unpack_from("<H", data, offset)[0]


def u32(offset):
    need(offset, 4)
    return struct.unpack_from("<I", data, offset)[0]


def u64(offset):
    need(offset, 8)
    return struct.unpack_from("<Q", data, offset)[0]


def hexv(value):
    return f"0x{value:x}"


def show(fields):
    width = max(len(name) for name, _ in fields)
    for name, value in fields:
        print(f"{name:<{width}} : {value}")


container = False
elf = 0
ext_marker = 0
fself_compatible = None
auth_info_embedded = None

# A development FSELF wraps the real ELF after a segment table whose entry
# count lives at 0x18.
if len(data) >= 0x20 and u32(0) in (0x1D3D154F, 4009038932):
    container = True
    entries = u16(0x18)
    header_size = u16(0x0C)
    meta_size = u16(0x0E)
    elf = 0x20 + 0x20 * entries
    if elf + 0x40 > len(data) or u32(elf) != 0x464C457F:
        raise SystemExit(
            f"inspect: {path} is an FSELF without an ELF header at the "
            "segment-table boundary."
        )

    phoff = u64(elf + 0x20)
    phnum = u16(elf + 0x38)
    ext = (elf + phoff + 0x38 * phnum + 15) & ~15
    if ext + 16 <= len(data):
        ext_marker = u64(ext + 8)
    fself_compatible = ext_marker == 1

    # Where current kstuff-lite looks for an optional 0x88-byte auth record.
    auth_offset = ext + 0x40 + 0x30 + 0x50 * entries + 0x50
    auth_info_embedded = auth_offset + 8 <= len(data) and u64(auth_offset) == 0x88

    show([
        ("Layer", "FSELF"),
        ("SegmentEntries", entries),
        ("HeaderSize", hexv(header_size)),
        ("MetaSize", hexv(meta_size)),
        ("ElfOffset", hexv(elf)),
        ("ExtendedInfoOffset", hexv(ext)),
        ("ExtendedInfoMarker", hexv(ext_marker)),
        ("KstuffRecognized", fself_compatible),
        ("EmbeddedAuthInfo", auth_info_embedded),
    ])

if elf + 0x40 > len(data) or u32(elf) != 0x464C457F:
    raise SystemExit(f"inspect: {path} is neither a raw ELF nor a supported FSELF.")

os_abi = data[elf + 7]
abi_version = data[elf + 8]
elf_type = u16(elf + 0x10)
entry = u64(elf + 0x18)
phoff = u64(elf + 0x20)
phentsize = u16(elf + 0x36)
phnum = u16(elf + 0x38)

program_headers = []
mapped_loads = []
proc_param = None
linking_load = None

for index in range(phnum):
    p = elf + phoff + index * phentsize
    header = {
        "index": index,
        "type": u32(p),
        "flags": u32(p + 4),
        "offset": u64(p + 8),
        "address": u64(p + 16),
        "file_size": u64(p + 32),
        "memory_size": u64(p + 40),
        "align": u64(p + 48),
    }
    program_headers.append(header)
    if header["type"] == 1:
        if header["flags"] == 0:
            linking_load = index
        else:
            mapped_loads.append(header)
    if header["type"] == 0x61000001:
        proc_param = header["offset"]

errors = []
if os_abi != 9:
    errors.append("OS/ABI is not FreeBSD (9).")
if abi_version != 2:
    errors.append("ABI version is not 2.")
if elf_type != 0xFE10:
    errors.append("ELF type is not ET_SCE_EXEC_ASLR (0xfe10).")
if phentsize != 0x38:
    errors.append("Program-header size is not 0x38.")
if proc_param is None:
    errors.append("PT_SCE_PROCPARAM is missing.")
if linking_load is None:
    errors.append("The flags-zero linking LOAD is missing.")
for load in mapped_loads:
    if load["align"] != 0x4000:
        errors.append(f"Mapped LOAD {load['index']} is not 0x4000 aligned.")
    if load["memory_size"] < load["file_size"]:
        errors.append(
            f"LOAD {load['index']} has memsz smaller than filesz."
        )
    if load["flags"] not in (1, 4, 6):
        errors.append(
            f"LOAD {load['index']} has unsupported flags {load['flags']}."
        )
for header in program_headers:
    if header["type"] == 0x6FFFFF00:
        if header["memory_size"] != 0:
            errors.append(
                f"Comment header {header['index']} must have zero memsz."
            )
        if header["align"] != 0x10:
            errors.append(
                f"Comment header {header['index']} must use 0x10 alignment."
            )
    if (header["type"] == 4 and header["address"] == 0
            and header["memory_size"] != 0):
        errors.append(
            f"Unmapped NOTE header {header['index']} must have zero memsz."
        )

proc_magic = None
companion = None
sdk = None
if (not container and proc_param is not None
        and elf + proc_param + 0x60 <= len(data)):
    proc_magic = data[elf + proc_param + 8:elf + proc_param + 12].decode(
        "ascii", "replace"
    )
    companion = u32(elf + proc_param + 0x10)
    sdk = u32(elf + proc_param + 0x14)
    if proc_magic != "ORBI":
        errors.append("Process-parameter magic is not ORBI.")

show([
    ("Layer", "ELF"),
    ("Container", container),
    ("OsAbi", os_abi),
    ("AbiVersion", abi_version),
    ("Type", hexv(elf_type)),
    ("Entry", hexv(entry)),
    ("ProgramHeaders", phnum),
    ("MappedLoads", len(mapped_loads)),
    ("LinkingLoadIndex", linking_load),
    ("ProcessParamMagic", proc_magic),
    ("CompanionVersion", hexv(companion) if companion is not None else None),
    ("SdkVersion", hexv(sdk) if sdk is not None else None),
    ("StaticErrors", len(errors)),
])

if errors:
    for message in errors:
        print(f"inspect: {message}", file=sys.stderr)
    raise SystemExit(1)
PY
