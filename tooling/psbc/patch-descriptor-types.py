#!/usr/bin/env python3
# PS5 Vulkan - the descriptor types libpsbc's enum cannot express.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch only the compiler work copy; preserve the pinned SDK source tree.

The compiler builds a real RADV descriptor set layout from the caller's
`PsbcDescriptorBinding` list and maps each entry onto a Vulkan descriptor type.
The pinned fork names only NONE, UNIFORM_BUFFER, COMBINED_IMAGE_SAMPLER and
STORAGE_BUFFER, so the types below -- one table row each -- are what this script
adds: the two texel buffers (a 16-byte buffer descriptor) and the storage image
(a 32-byte image descriptor), with the entry stride and the CLI name each one
needs. ps5-opengl-sdk-0.3.0's own fork carries the same storage image type and
the same strides (libpsbc/psbc_compile.c there), and its runtime's
ps5_storage_image_view_descriptor is the descriptor word reference the driver's
writer follows (docs/BLOCKERS.md, the descriptor types).

What this script writes is *canonical*: the enum values and CLI arms are
inserted, and the three statements the types appear in -- the validation's type
list, its expected stride and the layout's type mapping -- are rewritten whole
from the table. That is what makes the script idempotent and independent of what
earlier runs (or a reconstructed tree that already carries the types) left
behind, which an appended arm is not: two scripts that both append to one
expression fail each other's anchors.
"""
import sys
from pathlib import Path

# (PsbcDescriptorType value, the Vulkan type it is lowered as, CLI name, entry
#  stride in bytes)
TYPES = [
    ("PSBC_DESCRIPTOR_UNIFORM_TEXEL_BUFFER", "VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER",
     "uniform_texel_buffer", 16),
    ("PSBC_DESCRIPTOR_STORAGE_TEXEL_BUFFER", "VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER",
     "storage_texel_buffer", 16),
    ("PSBC_DESCRIPTOR_STORAGE_IMAGE", "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE", "storage_image", 32),
]
# The types the fork already names, in the order its validation and mapping list
# them.
FORK_TYPES = [
    ("PSBC_DESCRIPTOR_UNIFORM_BUFFER", "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"),
    ("PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER", "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER"),
    ("PSBC_DESCRIPTOR_STORAGE_BUFFER", "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER"),
]
COMBINED_STRIDE = 48
DEFAULT_STRIDE = 16

ENUM_CLOSE = "} PsbcDescriptorType;"
CLI_NONE = "\treturn PSBC_DESCRIPTOR_NONE;"
VALID_PREFIX = "        const bool valid_type ="
STRIDE_PREFIX = "        const uint32_t expected_stride ="
MAPPING_PREFIX = "            target->type = source->type == PSBC_DESCRIPTOR_UNIFORM_BUFFER"


def all_types():
    """The fork's types then the table's, in one order everything below uses."""
    return FORK_TYPES + [(value, vk) for value, vk, _, _ in TYPES]


def strides():
    """(value, stride) for every type whose stride is not the default 16.

    The fork's combined image sampler is the one of its own types with a stride
    of its own (a 32-byte image descriptor plus a 16-byte sampler), so it leads
    the list the statement below builds; dropping it would make a 48-byte
    combined sampler entry invalid, which tools/check-fragment-inputs.sh catches
    by compiling a sampler2D shader.
    """
    return [("PSBC_DESCRIPTOR_COMBINED_IMAGE_SAMPLER", COMBINED_STRIDE)] + [
        (value, stride) for value, _, _, stride in TYPES if stride != DEFAULT_STRIDE
    ]


def validation_statement():
    types = all_types()
    lines = [VALID_PREFIX]
    for index, (value, _) in enumerate(types):
        lines.append(f"            binding->type == {value}{';' if index == len(types) - 1 else ' ||'}")
    return "\n".join(lines)


def stride_statement():
    lines = [STRIDE_PREFIX]
    for value, stride in strides():
        lines.append(f"            binding->type == {value} ? {stride}u :")
    lines.append(f"            {DEFAULT_STRIDE}u;")
    return "\n".join(lines)


def mapping_statement():
    lines = [MAPPING_PREFIX, "                               ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER"]
    for value, vk in all_types():
        if value == "PSBC_DESCRIPTOR_UNIFORM_BUFFER":
            continue
        lines.append(f"                           : source->type == {value}")
        lines.append(f"                               ? {vk}")
    lines.append("                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;")
    return "\n".join(lines)


def insert_values(path):
    text = path.read_text()
    added = 0
    for value, _, _, _ in TYPES:
        if f"    {value},\n" in text:
            continue
        if text.count(ENUM_CLOSE) != 1:
            raise SystemExit(f"{path}: expected one {ENUM_CLOSE}")
        text = text.replace(ENUM_CLOSE, f"    {value},\n{ENUM_CLOSE}")
        added += 1
    if added:
        path.write_text(text)
    return added


def insert_cli_names(path):
    text = path.read_text()
    added = 0
    for value, _, name, _ in TYPES:
        arm = f'\tif (!strcmp(name, "{name}"))\n\t\treturn {value};'
        if arm in text:
            continue
        if text.count(CLI_NONE) != 1:
            raise SystemExit(f"{path}: expected one {CLI_NONE!r}")
        text = text.replace(CLI_NONE, arm + "\n" + CLI_NONE)
        added += 1
    if added:
        path.write_text(text)
    return added


def replace_statement(text, path, prefix, statement, what):
    """Rewrite the one statement prefix starts with, or leave it when canonical."""
    start = text.find(prefix)
    if start < 0:
        raise SystemExit(f"{path}: expected one {what} anchor ({prefix!r})")
    end = text.find(";", start)
    if end < 0:
        raise SystemExit(f"{path}: no end of the {what} statement")
    current = text[start:end + 1]
    if current == statement:
        return text, 0
    return text[:start] + statement + text[end + 1:], 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch-descriptor-types.py <opengnm-psbc tree>")
    tree = Path(sys.argv[1])
    header = tree / "libpsbc/psbc_compile.h"
    source = tree / "libpsbc/psbc_compile.c"
    cli = tree / "cmd/psbc/main.c"
    values = insert_values(header)
    names = insert_cli_names(cli)
    text = source.read_text()
    text, valid = replace_statement(text, source, VALID_PREFIX, validation_statement(),
                                    "descriptor type validation")
    text, stride = replace_statement(text, source, STRIDE_PREFIX, stride_statement(),
                                     "descriptor entry stride")
    text, mapped = replace_statement(text, source, MAPPING_PREFIX, mapping_statement(),
                                     "descriptor type mapping")
    if valid or stride or mapped:
        source.write_text(text)
    print(
        f"descriptor types: {values} enum values, {names} CLI names, {mapped} type mapping, "
        f"{valid} validation, {stride} entry stride ({len(TYPES)} in the table)"
    )
