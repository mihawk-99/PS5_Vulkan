#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - audit the driver's format tables.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Vulkan compatibility probe - audit the driver's format tables.

Phase V0-formats (docs/M5_REFERENCE.md). A Vulkan 1.0 device must support a
set of formats with a set of features, whether or not any extension is enabled
or any feature requested: `formats-v1.4.354.adoc`, "Required Format Support",
the three "Mandatory Format Support" tables. This script reads that file, reads
the table the driver reports through vkGetPhysicalDeviceFormatProperties
(driver/ps5vk_image.c), and prints every required feature that no driver entry
carries.

`{sym1}` means the feature is required on that format. `{sym2}` and `{sym3}`
mean required on at least some of the named formats, or with caveats the table
spells out; they are listed as conditional and do not, on their own, make an
entry missing. The caveat rows (the 4444 formats, whose features are conditional
on an extension this device does not advertise) are reported as conditional too.

A conditional cell is not always unconstrained. A table's footnote rows (the
ones that open with a column span, not a format) can state a requirement over a
*set* of rows -- the depth/stencil table's "`DEPTH_STENCIL_ATTACHMENT_BIT`
feature must: be supported for at least one of `X8_D24_UNORM_PACK32` and
`D32_SFLOAT`, and must: be supported for at least one of `D24_UNORM_S8_UINT` and
`D32_SFLOAT_S8_UINT`" -- and a cell on its own cannot say which clause it belongs
to or whether any of its siblings satisfies it. Such clauses are read here and
evaluated against the driver's whole table: a clause no reported format
satisfies is printed as **unmet**, one row per format the clause names, and
fails `--check` exactly as a missing feature does. A cell that is conditional and
belongs to no clause stays conditional.

Every missing feature is a claim that is not true yet, so the audit is the list
the step closes one row at a time: the driver either reports the bit for a
format it can store and use, or records the format as a deliberate gap with the
layout or descriptor work it needs (docs/V0_FORMATS_AUDIT.md).

Usage: python3 tools/format_audit.py [--check] [spec]
  --check  exit 1 when a required feature is missing, for a gate
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC = ROOT / ".deps/native/vulkan-docs/formats-v1.4.354.adoc"
DRIVER = ROOT / "driver/ps5vk_image.c"

# The columns of the mandatory tables, in the order the tables declare them:
# the image features first, then the buffer ones. The table has one column per
# bit; the last two only exist from 1.3 and 1.1 on, but they are columns of the
# same table and are read the same way.
COLUMNS = [
    "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT",
    "VK_FORMAT_FEATURE_BLIT_SRC_BIT",
    "VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT",
    "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT",
    "VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT",
    "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT",
    "VK_FORMAT_FEATURE_BLIT_DST_BIT",
    "VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT",
    "VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT",
    "VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT",
    "VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT",
    "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT",
    "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT",
]
# Features the maintenance1 note requires wherever SAMPLED_IMAGE is required.
# They are not columns; a format the driver reports as sampled must carry them
# too, which the driver's own comments already say.
TRANSFER = ["VK_FORMAT_FEATURE_TRANSFER_SRC_BIT", "VK_FORMAT_FEATURE_TRANSFER_DST_BIT"]

ROW = re.compile(r"^\|\s*ename:(VK_FORMAT_[A-Z0-9_]+)\s*\|(.*)$")
TABLE = re.compile(r"^\[\[formats-mandatory-features-[a-z0-9-]+\]\]")


def required_formats(spec):
    """The mandatory tables as {format: {feature: marker}}, markers being sym1,
    sym2, sym3 or empty."""
    required = {}
    # Each mandatory table is an anchor, its title and options, then a |====
    # that opens it and a second that closes it: the rows are between the two.
    in_table = False
    started = False
    for line in spec.read_text(encoding="utf-8", errors="replace").splitlines():
        if TABLE.match(line):
            in_table = True
            started = False
            continue
        if in_table and line.startswith("|===="):
            if not started:
                started = True
                continue
            in_table = False
            continue
        if not in_table or not started:
            continue
        match = ROW.match(line)
        if not match:
            continue
        cells = match.group(2).split("|")
        features = {}
        for index, column in enumerate(COLUMNS):
            cell = cells[index] if index < len(cells) else ""
            marker = "sym1" if "sym1" in cell else \
                     "sym2" if "sym2" in cell else \
                     "sym3" if "sym3" in cell else None
            if marker:
                features[column] = marker
        required[match.group(1)] = features
    return required


# A table's footnote rows open with a column span ("15+|") rather than a format
# name, and the unmarked lines after one continue it. A footnote can require a
# feature of a *set* of rows, which no cell's own marker can say: every row the
# clause names carries the same {sym2}, and the clause is satisfied when any one
# of them reports the feature. Reading the rows one at a time cannot see that, so
# the footnotes are read whole and their clauses evaluated against the table.
FOOTNOTE = re.compile(r"^\d+\+\|\s*(.*)$")
CLAUSE = "must: be supported for at least one of"
CLAUSE_FEATURE = re.compile(r"ename:(VK_FORMAT_FEATURE_[A-Z0-9_]+)")
CLAUSE_FORMAT = re.compile(r"ename:(VK_FORMAT_(?!FEATURE_)[A-Z0-9_]+)")


def footnote_paragraphs(spec):
    """The mandatory tables' footnotes as whole paragraphs: a footnote row and
    the lines that continue it, joined with single spaces."""
    paragraph = []
    for line in spec.read_text(encoding="utf-8", errors="replace").splitlines():
        span = FOOTNOTE.match(line)
        if span:
            if paragraph:
                yield " ".join(paragraph)
            paragraph = [span.group(1).strip()]
            continue
        if line.startswith("|") or not line.strip():
            if paragraph:
                yield " ".join(paragraph)
            paragraph = []
            continue
        if paragraph:
            paragraph.append(line.strip())
    if paragraph:
        yield " ".join(paragraph)


def required_clauses(spec):
    """The footnotes' must: clauses as [(feature, [format])]: the feature has to
    be supported for at least one of the named formats. A clause continued with
    "and must:" keeps the feature the clause before it named."""
    clauses = []
    for paragraph in footnote_paragraphs(spec):
        if CLAUSE not in paragraph:
            continue
        # The text before each clause's formats names the feature when the
        # clause opens one, and is the previous clause's tail otherwise.
        parts = paragraph.split(CLAUSE)
        feature = None
        for index in range(1, len(parts)):
            named = CLAUSE_FEATURE.search(parts[index - 1])
            if named:
                feature = named.group(1)
            formats = CLAUSE_FORMAT.findall(parts[index])
            if feature and formats:
                clauses.append((feature, formats))
    return clauses


def reported_formats(source):
    """The driver's table as {format: set(feature)}; the entries are
    {VK_FORMAT_X, <image features>, <buffer features>}."""
    text = source.read_text(encoding="utf-8")
    body = text.split("ps5vk_formats[] = {", 1)[1].split("\n};", 1)[0]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    reported = {}
    for entry in re.finditer(r"\{([^{}]*)\}", body):
        fields = entry.group(1).split(",")
        if len(fields) < 3:
            continue
        name = fields[0].strip()
        if not name.startswith("VK_FORMAT_"):
            continue
        features = set()
        for field in fields[1:]:
            features |= set(re.findall(r"VK_FORMAT_FEATURE_[A-Z0-9_]+", field))
        reported[name] = features
    return reported


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when a required feature is missing")
    parser.add_argument("spec", nargs="?", default=str(SPEC))
    args = parser.parse_args()

    required = required_formats(Path(args.spec))
    reported = reported_formats(DRIVER)
    if not required or not reported:
        print("nothing parsed: check the spec path and driver/ps5vk_image.c")
        return 2

    missing = {}
    conditional = {}
    for name, features in sorted(required.items()):
        for feature, marker in features.items():
            carried = reported.get(name, set())
            if marker == "sym1":
                if feature not in carried:
                    missing.setdefault(name, []).append(feature)
                elif feature == "VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT":
                    for transfer in TRANSFER:
                        if transfer not in carried:
                            missing.setdefault(name, []).append(transfer)
            else:
                conditional.setdefault(name, []).append(f"{feature} ({marker})")

    # A clause is met as soon as one of the formats it names reports the feature,
    # so the rows it names are gaps together or not at all.
    clauses = required_clauses(Path(args.spec))
    unmet = [(feature, formats) for feature, formats in clauses
             if not any(feature in reported.get(name, set()) for name in formats)]

    print(f"{len(required)} formats are required, {len(reported)} are reported")
    print(f"{len(missing)} formats miss a required feature; "
          f"{len(conditional)} have conditional requirements")
    print(f"{len(unmet)} must: clauses are unmet; "
          f"{sum(len(formats) for _, formats in unmet)} formats are named by one")
    print()
    if missing:
        print("missing (one row per format: the features no entry carries):")
        for name, features in sorted(missing.items()):
            print(f"  {name:46s} {' '.join(sorted(features))}")
        print()
    if unmet:
        print("unmet (a must: clause requires the feature of at least one of the formats")
        print("it names and no reported format carries it: one row per format it names):")
        for feature, formats in unmet:
            for name in sorted(formats):
                print(f"  {name:46s} {feature}")
            clause = " ".join(["must: be supported for at least one of"] + sorted(formats))
            print(f"  the clause: {feature} {clause}")
        print()
    if conditional:
        print("conditional (at least some formats, or with caveats):")
        for name, features in sorted(conditional.items()):
            print(f"  {name:46s} {' '.join(sorted(features))}")
        print()
    extra = sorted(set(reported) - set(required))
    if extra:
        print("reported beyond the required set (the probes' own formats):")
        print("  " + " ".join(extra))
    return 1 if args.check and (missing or unmet) else 0


if __name__ == "__main__":
    sys.exit(main())
