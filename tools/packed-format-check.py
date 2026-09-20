#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - check the packed formats' expected colours.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Vulkan compatibility probe - decode the packed probe texels on the PC.

Phase V0-formats (docs/M5_REFERENCE.md). run_vulkan_format_sample_frames
uploads one texel per format and requires the frame to hold the colour that
texel decodes to; the table in src/diagnostics.cpp carries both the texel's
bytes and the colour, and the console compares the hardware against it. That
makes the table the probe's ground truth, so a wrong byte pattern or a wrong
expectation would be read as a hardware result -- which is what happened once,
when B4G4R4A4's texel was written 0xF8F4 instead of 0x48FF and the console
faithfully reported the colour that word means.

This decodes every packed entry from the source, per the Vulkan specification's
rules for its format (UNORM expansion is round(v * 255 / (2**n - 1)), the
shared-exponent forms are mantissa * 2**(exponent - bias)), and requires the
documented colour to be what the texel means within one 8-bit level. It is the
PC half of the packed families' probe: the console proves the hardware decodes
the word this way, this proves the word and the expectation agree.

Run through make test (tests/test_tools.py).
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "src/diagnostics.cpp"

# The packed families, with the bits of each channel in the little-endian word
# and how the channel is decoded: (shift, bits, kind).
PACKED = {
    "VK_FORMAT_R5G6B5_UNORM_PACK16": {
        "red": (11, 5, "unorm"),
        "green": (5, 6, "unorm"),
        "blue": (0, 5, "unorm"),
    },
    "VK_FORMAT_A1R5G5B5_UNORM_PACK16": {
        "red": (10, 5, "unorm"),
        "green": (5, 5, "unorm"),
        "blue": (0, 5, "unorm"),
    },
    "VK_FORMAT_B4G4R4A4_UNORM_PACK16": {
        "red": (4, 4, "unorm"),
        "green": (8, 4, "unorm"),
        "blue": (12, 4, "unorm"),
    },
    "VK_FORMAT_E5B9G9R9_UFLOAT_PACK32": {
        "red": (0, 9, "shared9"),
        "green": (9, 9, "shared9"),
        "blue": (18, 9, "shared9"),
        "exponent": (27, 5, "exponent"),
    },
    "VK_FORMAT_B10G11R11_UFLOAT_PACK32": {
        # An 11-bit channel is a 5-bit exponent over a 6-bit mantissa, a 10-bit
        # one a 5-bit exponent over a 5-bit mantissa.
        "red": (0, 6, "float"),
        "green": (11, 6, "float"),
        "blue": (22, 5, "float"),
    },
    "VK_FORMAT_A2B10G10R10_UNORM_PACK32": {
        "red": (0, 10, "unorm"),
        "green": (10, 10, "unorm"),
        "blue": (20, 10, "unorm"),
    },
}

ENTRY = re.compile(
    r"\{VK_FORMAT_([A-Z0-9_]+), (\d+), \{([^}]*)\}, \{([^}]*)\}\}"
)


def decode(format_name, word):
    """The colour a little-endian texel word means, as 8-bit levels."""
    layout = PACKED[format_name]
    channels = {}
    for name, (shift, bits, kind) in layout.items():
        # A float channel's field is its mantissa plus the five-bit exponent;
        # every other field is exactly as wide as its bits.
        width = bits + 5 if kind == "float" else bits
        channels[name] = (word >> shift) & ((1 << width) - 1)
    if "exponent" in channels:
        # The shared-exponent form: a 9-bit mantissa a channel, all scaled by
        # 2**(exponent - 15), and the value is the mantissa over 2**9.
        exponent = channels["exponent"] - 15
        value = {
            name: channels[name] / 512.0 * 2.0**exponent
            for name in ("red", "green", "blue")
        }
    elif layout["red"][2] == "float":
        # An 11-bit float a channel: a 5-bit exponent (bias 15) over a mantissa
        # with an implicit one, and the ten-bit channel is the same with one
        # mantissa bit fewer.
        value = {}
        for name in ("red", "green", "blue"):
            mantissa_bits = layout[name][1]
            raw = channels[name]
            exponent = raw >> mantissa_bits
            mantissa = raw & ((1 << mantissa_bits) - 1)
            value[name] = (
                (1.0 + mantissa / (1 << mantissa_bits)) * 2.0 ** (exponent - 15)
                if exponent
                else 0.0
            )
    else:
        value = {
            name: channels[name] / ((1 << layout[name][1]) - 1)
            for name in ("red", "green", "blue")
        }
    return [max(0, min(255, round(value[name] * 255.0))) for name in ("red", "green", "blue")]


def main():
    source = SOURCE.read_text(encoding="utf-8")
    entries = []
    for match in ENTRY.finditer(source):
        name = "VK_FORMAT_" + match.group(1)
        if name not in PACKED:
            continue
        texel = [int(byte, 16) for byte in re.findall(r"0x([0-9a-f]{2})", match.group(3))]
        expected = [int(byte, 16) for byte in re.findall(r"0x([0-9a-f]{2})", match.group(4))]
        entries.append((name, texel, expected))
    if len(entries) != len(PACKED):
        print(
            f"src/diagnostics.cpp holds {len(entries)} packed sample entries, "
            f"not {len(PACKED)}",
            file=sys.stderr,
        )
        return 1
    failed = False
    for name, texel, expected in entries:
        word = sum(byte << (8 * index) for index, byte in enumerate(texel))
        decoded = decode(name, word)
        close = all(abs(got - want) <= 1 for got, want in zip(decoded, expected))
        if not close:
            print(
                f"  {name}: texel 0x{word:0{len(texel) * 2}x} decodes to "
                f"{decoded}, the table expects {expected}",
                file=sys.stderr,
            )
            failed = True
        else:
            print(f"  {name}: 0x{word:0{len(texel) * 2}x} decodes to {decoded}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
