#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - audit the driver's reported limits.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Vulkan compatibility probe - audit the driver's reported limits.

Phase B2 (docs/M5_REFERENCE.md). A Vulkan 1.0 device reports a
VkPhysicalDeviceLimits table, and the specification's Required Limits table says
what each member must be at least (``min``) or at most (``max``) for a 1.0
device: `limits-v1.4.354.adoc`, "Required Limits". This script reads that file,
reads the table the driver reports (driver/ps5vk_physical_device.c,
ps5vk_get_properties), and prints every member whose reported value does not
meet the table.

The driver's own table is the specification's minimums by design
(driver/ps5vk_physical_device.c: "Limits: the Vulkan specification's required
values"), so this audit is what holds the transcription to the source: a member
whose value is below the required minimum, or above a required maximum, is a
claim the device does not meet, and every one of them is B2's remaining work
until a probe raises it or the table is corrected.

Limits of features the device does not support take the table's "Unsupported
Limit" column, and the driver reports those (sparseAddressSpaceSize 0, the
tessellation and geometry members 0): the audit accepts either column.

Usage: python3 tools/limits_audit.py [--check] [spec]
  --check  exit 1 while a reported limit does not meet the table, for a gate
"""

import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / ".deps/native/vulkan-docs/limits-v1.4.354.adoc"
DRIVER = ROOT / "driver/ps5vk_physical_device.c"
MACROS = [ROOT / "driver/ps5vk_private.h", DRIVER]
# The members of VkPhysicalDeviceLimits, which is the structure the table below
# is about: the Required Limits table also lists limits of extension structures
# (descriptor sizes, subgroup size, sparse residency), and those are not part of
# a 1.0 device's table. The header is the SDK's, whose copy is what the driver
# compiles against.
HEADERS = [
    Path(os.environ.get("PS5_OPENGL_SDK", ROOT.parent / "ps5-opengl-sdk-0.2.0"))
    / "third_party/Vulkan-Headers/include/vulkan/vulkan_core.h",
    ROOT / ".deps/native/mesa/mesa-26.2.0/include/vulkan/vulkan_core.h",
]
# Footnote 8: "The minimum maxDescriptorSet* limit is n times the corresponding
# specification minimum maxPerStageDescriptor* limit, where n is the number of
# shader stages supported by the VkPhysicalDevice" -- three here (vertex,
# fragment and compute), which is what PS5VK_SHADER_STAGES is and what the
# driver's per-set values are written with.
FOOTNOTE_8 = {
    "maxDescriptorSetSamplers": "maxPerStageDescriptorSamplers",
    "maxDescriptorSetUniformBuffers": "maxPerStageDescriptorUniformBuffers",
    "maxDescriptorSetStorageBuffers": "maxPerStageDescriptorStorageBuffers",
    "maxDescriptorSetSampledImages": "maxPerStageDescriptorSampledImages",
    "maxDescriptorSetStorageImages": "maxPerStageDescriptorStorageImages",
}
# Footnote 2: "maxPerStageResources must be at least the smallest of the sum of
# the maxPerStageDescriptor* limits and maxColorAttachments, or 128" -- the
# driver reports the sum, 44, which is what the smaller-of rule asks for.
FOOTNOTE_2_SUM = [
    "maxPerStageDescriptorUniformBuffers",
    "maxPerStageDescriptorStorageBuffers",
    "maxPerStageDescriptorSampledImages",
    "maxPerStageDescriptorStorageImages",
    "maxPerStageDescriptorInputAttachments",
    "maxColorAttachments",
]

# The enumerated values a core cell may name: the sample counts and the two
# booleans. A cell like "(A | B)" is a mask the device must cover.
ENUMS = {
    "VK_TRUE": 1,
    "VK_FALSE": 0,
    "VK_SAMPLE_COUNT_1_BIT": 0x1,
    "VK_SAMPLE_COUNT_2_BIT": 0x2,
    "VK_SAMPLE_COUNT_4_BIT": 0x4,
    "VK_SAMPLE_COUNT_8_BIT": 0x8,
    "VK_SAMPLE_COUNT_16_BIT": 0x10,
    "VK_SAMPLE_COUNT_32_BIT": 0x20,
    "VK_SAMPLE_COUNT_64_BIT": 0x40,
}

LIMITS_STRUCT = re.compile(r"typedef struct VkPhysicalDeviceLimits \{(.*?)\} VkPhysicalDeviceLimits;", re.S)
# A member declaration is a type and a name, with an array extent for the
# few that are arrays (maxViewportDimensions, pointSizeRange).
STRUCT_FIELD = re.compile(
    r"^\s*(?:const\s+)?[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*;", re.M
)

TABLE_HEAD = "| Limit | Unsupported Limit | Supported Limit"
# A row may carry an adoc footnote on the limit's name ("maxViewportDimensions
# ^3^"), which is not part of the name.
ROW = re.compile(r"^\|\s*pname:([A-Za-z0-9_]+)(?:\s*\^[0-9]+\^)?\s*\|(.*)$")
NUMBER = re.compile(r"(\d+)\s*(?:\{core\})?")
DEFINE = re.compile(r"^#define\s+([A-Z][A-Z0-9_]*)\s+(\d+)\s*$")
FIELD = re.compile(r"^\.([A-Za-z][A-Za-z0-9]*)\s*=\s*(.+?),?\s*$")


def keep_row(rows, name, columns):
    """One row per name. The Required Limits table repeats some limits in its
    extension sections, and the core row is the one a 1.0 device answers to, so
    a comparable row replaces a skipped one and never the other way round."""
    parsed = parse_row(columns)
    if name not in rows or (len(rows[name]) == 1 and len(parsed) == 4):
        rows[name] = parsed


def requirements(spec):
    """The Required Limits table: name -> (unsupported, supported, kind)."""
    lines = spec.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        start = next(index for index, line in enumerate(lines) if TABLE_HEAD in line)
    except StopIteration:
        return {}
    rows = {}
    name = None
    columns = ""
    for line in lines[start + 1 :]:
        if line.startswith("|==="):
            if name:
                keep_row(rows, name, columns)
            break
        match = ROW.match(line)
        if match:
            if name:
                keep_row(rows, name, columns)
            name, columns = match.group(1), match.group(2)
            continue
        if name and not line.startswith("|") and not line.strip().startswith(
            ("ifdef::", "ifndef::", "ifeval::", "endif::")
        ):
            columns += " " + line.strip()
    return rows


def core_text(supported):
    """The ``{core}`` value of a supported column, or the column's first value
    when it carries no tag (the whole column is then the core requirement). A
    column continues with other feature levels after a ``+``, and a value may
    carry an adoc footnote (``^6^``)."""
    footnote = r"\s*\^[0-9]+\^\s*$"
    first = re.split(r"\s\+\s", supported.strip(), maxsplit=1)[0].strip()
    first = re.sub(footnote, "", first)
    tagged = re.fullmatch(r"(.*?)\s*\(\{core\}\)", first)
    if tagged is not None:
        # The marker may sit between the value and its tag ("128 ^2^ ({core})").
        first = re.sub(footnote, "", tagged.group(1).strip())
        if first == "-":
            return "-"
    elif not re.fullmatch(
        r"\([^()]*\)|-?[0-9]+|2\^[0-9]+\^(-1)?|(?:ename:)?[A-Z][A-Z0-9_]*", first
    ):
        return None
    return first


def numbers(text):
    """A scalar or a ``(x,y)`` tuple as numbers, or None when any part is not
    one (a formula like ``64.0 - ULP``, or a boolean)."""
    parts = [part.strip() for part in text[1:-1].split(",")] if text.startswith("(") else [text]
    values = []
    for part in parts:
        part = part.strip()
        if re.fullmatch(r"2\^[0-9]+\^", part):
            values.append(2 ** int(part[2:-1]))
        elif re.fullmatch(r"2\^[0-9]+\^-1", part):
            values.append(2 ** int(part[2:-3]) - 1)
        elif re.fullmatch(r"-?[0-9]+", part):
            values.append(int(part))
        elif re.fullmatch(r"-?[0-9]+\.[0-9]+", part):
            values.append(float(part))
        elif re.sub(r"^(?:e|p|s|t)name:", "", part) in ENUMS:
            values.append(ENUMS[re.sub(r"^(?:e|p|s|t)name:", "", part)])
        elif "|" in part:
            # A mask of enums: every one of them is required.
            mask = 0
            for name in [piece.strip() for piece in part.split("|")]:
                bare = re.sub(r"^(?:e|p|s|t)name:", "", name)
                if bare not in ENUMS:
                    return None
                mask |= ENUMS[bare]
            values.append(mask)
        else:
            return None
    return values


def parse_row(columns):
    """One row: (unsupported, values, kinds), or a reason it is not a core
    requirement this audit can compare."""
    if "ifdef::" in columns or "endif::" in columns:
        return ("not core: the row is gated on an extension",)
    parts = [column.replace("\\|", "|").strip() for column in re.split(r"(?<!\\)\|", columns)]
    if len(parts) < 3:
        return ("not core: the row has no limit type",)
    unsupported, supported, kinds = parts[0], parts[1], parts[2]
    kind_list = []
    for kind in re.findall(r"\b(min|max)\b", kinds):
        kind_list.append(kind)
    if not kind_list and "Boolean" in kinds:
        # A Boolean row's supported column is a value the member has to equal,
        # not a bound it has to meet (standardSampleLocations is VK_TRUE).
        kind_list = ["equal"]
    if not kind_list:
        return (f"not compared: the row's type is {kinds!r}",)
    text = core_text(supported)
    if text is None:
        return ("not compared: the supported column names no core value",)
    if text == "-":
        return ("not compared: the core column requires nothing",)
    is_mask = "|" in text
    values = numbers(text)
    if values is None:
        if kind_list == ["equal"]:
            return ("not compared: the supported column is a formula or a boolean",)
        # A row whose supported column is a formula, and whose unsupported
        # column is a value: the device has to report that value while the
        # feature the row is conditional on stays off (lineWidthRange and
        # pointSizeRange are (1.0,1.0) without wideLines and largePoints, and
        # the caller checks the feature).
        if unsupported not in ("-", ""):
            numbers_ = numbers(unsupported)
            if numbers_ is not None:
                return (None, numbers_, ["equal"] * len(numbers_), False)
        return ("not compared: the supported column is a formula or a boolean",)
    if len(kind_list) == 1:
        kind_list *= len(values)
    if len(kind_list) != len(values):
        return (f"not compared: {len(values)} values but the types are {kinds!r}",)
    fallback = None
    if unsupported not in ("-", ""):
        numbers_ = numbers(unsupported)
        fallback = numbers_[0] if numbers_ else None
    return (fallback, values, kind_list, is_mask)


def macros():
    """The driver's own macros, so a value written with one can be read."""
    found = {}
    for path in MACROS:
        for line in path.read_text(encoding="utf-8").splitlines():
            match = DEFINE.match(line)
            if match:
                found[match.group(1)] = int(match.group(2))
    return found


def evaluate(expression, values):
    """A driver initializer as a number, or None when it is not an integer."""
    text = re.sub(r"UINT(?:32|64)_C\(([0-9]+)\)", r"\1", expression)
    known = dict(values, **ENUMS)
    text = re.sub(
        r"\b([A-Z][A-Z0-9_]*)\b",
        lambda match: str(known[match.group(1)]) if match.group(1) in known else match.group(0),
        text,
    )
    text = re.sub(r"([0-9])[fFuU]\b", r"\1", text)
    if not re.fullmatch(r"[0-9.+\-*/()<>|& ]+", text):
        return None
    try:
        return eval(text, {"__builtins__": {}}, {})  # noqa: S307 - digits only
    except Exception:
        return None


def core_members():
    """The member names of VkPhysicalDeviceLimits, or None when no header is
    there to read them from."""
    for header in HEADERS:
        if not header.is_file():
            continue
        match = LIMITS_STRUCT.search(header.read_text(encoding="utf-8", errors="replace"))
        if match:
            return set(STRUCT_FIELD.findall(match.group(1)))
    return None


def reported(source, values):
    """The members ps5vk_get_properties reports: name -> number, None when the
    initializer is not an integer (an array or a float)."""
    body = source.read_text(encoding="utf-8")
    # The values carry trailing comments ("/* no multiViewport */"), which are
    # not part of the initializer.
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    try:
        body = body[body.index("ps5vk_get_properties") :]
        body = body[body.index("(struct vk_properties){") :]
        body = body[: body.index("\n}")]
    except ValueError:
        return {}
    fields = {}
    for line in body.splitlines():
        match = FIELD.match(line.strip())
        if not match:
            continue
        expression = match.group(2).strip()
        if expression.startswith("{") and expression.endswith("}"):
            elements = [evaluate(element, values) for element in expression[1:-1].split(",")]
            fields[match.group(1)] = None if any(e is None for e in elements) else elements
        else:
            value = evaluate(expression, values)
            fields[match.group(1)] = None if value is None else [value]
    return fields


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="exit 1 on a limit that misses")
    parser.add_argument(
        "--verbose", action="store_true", help="print every required limit that is not compared"
    )
    parser.add_argument("spec", nargs="?", default=str(SPEC))
    args = parser.parse_args()
    spec = Path(args.spec)
    if not spec.is_file():
        print(f"missing {spec}; run tools/setup-native-dependencies.sh", file=sys.stderr)
        return 2
    table = requirements(spec)
    if not table:
        print(f"nothing parsed from {spec}: check the table's heading", file=sys.stderr)
        return 2
    members = core_members()
    if members is None:
        print(
            "no VkPhysicalDeviceLimits header found; set PS5_OPENGL_SDK to read the "
            "members from",
            file=sys.stderr,
        )
        return 2
    core = {name: row for name, row in table.items() if name in members}
    values = macros()
    fields = reported(DRIVER, values)
    if not fields:
        print(f"nothing parsed from {DRIVER}: check ps5vk_get_properties", file=sys.stderr)
        return 2

    wrong, skipped = [], []
    compared = 0
    for name, row in sorted(core.items()):
        if len(row) == 1:
            skipped.append((name, row[0]))
            continue
        fallback, required, kinds, is_mask = row
        # A member the initializer does not name is zero: the structure is
        # zero-initialised and only the named members are set.
        reported_values = fields.get(name, [0])
        if reported_values is None:
            skipped.append((name, "not a number or a list of numbers in the driver's table"))
            continue
        if len(reported_values) != len(required):
            skipped.append(
                (name, f"the table gives {len(required)} values and the driver {len(reported_values)}")
            )
            continue
        compared += 1
        if name in FOOTNOTE_8:
            stages = values.get("PS5VK_SHADER_STAGES")
            per_stage = core.get(FOOTNOTE_8[name])
            if stages is None or per_stage is None or len(per_stage) == 1:
                skipped.append((name, "footnote 8 needs the stage count and a per-stage row"))
                compared -= 1
                continue
            required, kinds = [stages * per_stage[1][0]], ["min"]
        elif name == "maxPerStageResources":
            total = 0
            for member in FOOTNOTE_2_SUM:
                values_ = fields.get(member)
                if values_ is None:
                    total = None
                    break
                total += values_[0]
            if total is not None:
                required, kinds = [min(required[0], total)], ["min"]
        for index, (kind, want, has) in enumerate(zip(kinds, required, reported_values)):
            if is_mask:
                # A mask requirement is a subset of what the device reports.
                meets = (has & want) == want
            elif kind == "equal":
                meets = has == want
            else:
                meets = has >= want if kind == "min" else has <= want
            if not meets and fallback is not None and has == fallback and index == 0:
                meets = True
            if not meets:
                member = name if len(required) == 1 else f"{name}[{index}]"
                wrong.append((member, kind, want, has, name not in fields))

    for member, kind, want, has, absent in wrong:
        where = " (the initializer does not name it)" if absent else ""
        if kind == "equal":
            print(f"{member}: the driver reports {has}, the table requires exactly {want}{where}")
        else:
            print(f"{member}: the driver reports {has}, the table requires {kind} {want}{where}")
    if args.verbose:
        for name, reason in skipped:
            print(f"{name}: {reason}")
    print(
        f"{len(core)} required VkPhysicalDeviceLimits members, {compared} compared with the "
        f"table (footnotes 2 and 8 applied), {len(wrong)} missing its minimum or maximum, "
        f"{len(skipped)} not compared (--verbose lists them)"
    )
    # What this audit cannot see: the limits are numbers, and sampler state is
    # not one -- a device can report every maximum correctly and still accept
    # exactly one sampler configuration (PS5_VULKAN_REQUESTS.md R2, the request
    # that found R1, R2 and R3 and asked the audits to say what they cannot see,
    # R4). The runner case beside it is that state's coverage.
    print("\nnot visible here, and the case that covers it:")
    print("  sampler state: the address modes v0-sampler-address exercises (repeat, "
          "mirrored repeat, clamp to edge); the filters, LOD bias, anisotropy, compare "
          "and border colours are refusals it does not cover: "
          "v0-sampler-address (jobs/v0-sampler-address/queue.txt)")
    if args.check and wrong:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
