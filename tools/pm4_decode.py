#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - AGC command stream decoder.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode the AGC command streams in a probe klog as AMD PM4 packets.

Commands:
  stream    Decode every command stream logged in a klog file, as saved by
            tools/ps5_console.py klog under Klog_Logs/. Each decoded
            register-table load is checked against the agc_gpu_pointer_indirect
            records the title logged for the same stream. For a stream built
            in capture mode, the register tables are read from the captured
            workspace image and the capture is checked for completeness.
  register  Decode one register value named by its AGC table offset.

Packet and register names come from Mesa's AMD headers (MIT) in the
ps5-opengl SDK, src/amd/common/sid.h and amdgfxregs.h, using the gfx103
(RDNA2) register definitions. AGC register tables index context registers
from 0x28000, SH registers from 0xB000 and uconfig registers from 0x30000,
four bytes per index. Set PS5_OPENGL_SDK when the SDK is not next to this
repository.
"""

import argparse
import os
import re
import sys
from pathlib import Path

from ps5vk_log import capture_problems, extract_streams, is_captured, read_runs, workspace_image

ROOT = Path(__file__).resolve().parent.parent
TARGET_GENERATION = "gfx103"
GENERATIONS = ["gfx6", "gfx7", "gfx8", "gfx81", "gfx9", "gfx10", "gfx103", "gfx11", "gfx115",
               "gfx12"]
REGISTER_BASES = {"config": 0x8000, "sh": 0xB000, "context": 0x28000, "uconfig": 0x30000}
KIND_ALIASES = {"cx": "context", "context": "context", "sh": "sh", "uc": "uconfig",
                "uconfig": "uconfig"}
# Indexed register-table loads emitted by sceAgcDcbSet{Cx,Sh,Uc}RegistersIndirect.
TABLE_LOADS = {0x9F: "context", 0x63: "sh", 0x64: "uconfig"}
TABLE_RECORD_BYTES = 8
# Opcodes AGC emits that sid.h does not name.
AGC_OPCODE_NAMES = {0x64: "UCONFIG_TABLE_LOAD"}
# Direct register writes; the *_INDEX forms carry an index in the upper bits of
# their first payload word.
SET_REGISTERS = {0x68: "config", 0x69: "context", 0x76: "sh", 0x79: "uconfig", 0x7A: "uconfig",
                 0x9B: "sh"}
OPCODE_LINE = re.compile(r"#define PKT3_(\w+)\s+0x([0-9A-Fa-f]+)\b")
REGISTER_LINE = re.compile(r"#define R_[0-9A-F]+_(\w+)\s+0x([0-9A-Fa-f]+)\s*(?:/\*(.*?)\*/)?\s*$")
FIELD_LINE = re.compile(r"#define\s+S_[0-9A-F]+_(\w+)\(x\)\s+\(\(\(unsigned\)\(x\) & 0x([0-9A-Fa-f]+)\)"
                        r" << (\d+)\)\s*(?:/\*(.*?)\*/)?\s*$")


def header_directory():
    sdk = Path(os.environ.get("PS5_OPENGL_SDK", ROOT.parent / "ps5-opengl-sdk-0.2.0"))
    directory = sdk / "third_party" / "opengnm-psbc" / "src" / "amd" / "common"
    for name in ("sid.h", "amdgfxregs.h"):
        if not (directory / name).is_file():
            raise SystemExit(f"missing Mesa header {directory / name}; set PS5_OPENGL_SDK")
    return directory


def generation_covers(comment):
    """True when a header comment such as '<= gfx9, gfx10, gfx103' includes the
    target generation. A comment without generation terms covers every one."""
    rank = GENERATIONS.index(TARGET_GENERATION)
    terms = [re.fullmatch(r"(<=|>=)?\s*(gfx\d+)", term.strip()) for term in (comment or "").split(",")]
    terms = [term for term in terms if term and term.group(2) in GENERATIONS]
    if not terms:
        return True
    for term in terms:
        other = GENERATIONS.index(term.group(2))
        relation = term.group(1)
        if (relation == "<=" and rank <= other) or (relation == ">=" and rank >= other) or \
                (relation is None and rank == other):
            return True
    return False


def load_opcode_names(directory):
    names = {}
    for line in (directory / "sid.h").read_text(encoding="utf-8", errors="replace").splitlines():
        match = OPCODE_LINE.match(line)
        if match and int(match.group(2), 16) <= 0xFF:
            names.setdefault(int(match.group(2), 16), match.group(1))
    return {**AGC_OPCODE_NAMES, **names}


def load_registers(directory):
    """Map absolute register address -> (name, [(field, mask, shift)]), gfx103 only."""
    registers = {}
    current = None
    text = (directory / "amdgfxregs.h").read_text(encoding="utf-8", errors="replace")
    for line in text.splitlines():
        match = REGISTER_LINE.match(line)
        if match:
            address = int(match.group(2), 16)
            current = None
            if address not in registers and generation_covers(match.group(3)):
                current = registers[address] = (match.group(1), [])
            continue
        match = FIELD_LINE.match(line)
        if match and current is not None and generation_covers(match.group(4)):
            current[1].append((match.group(1), int(match.group(2), 16), int(match.group(3))))
    return registers


def split_packets(words):
    """PM4 packets as dicts: index, word offset, type, header, opcode, payload
    and whether the logged words end inside the packet."""
    packets = []
    at = 0
    while at < len(words):
        header = words[at]
        kind = header >> 30
        size = 1 if kind == 2 else ((header >> 16) & 0x3FFF) + 2
        packets.append({"index": len(packets), "offset": at, "kind": kind, "header": header,
                        "opcode": (header >> 8) & 0xFF, "payload": words[at + 1:at + size],
                        "truncated": at + size > len(words)})
        if kind == 1:
            break
        at += size
    return packets


def label_address(address, regions):
    for name, (begin, size) in regions:
        if begin is not None and size and begin <= address < begin + size:
            return f"{name}+{address - begin:#x}"
    return f"{address:#x}"


def field_text(value, fields, nonzero_only):
    parts = [f"{name}={(value >> shift) & mask:#x}" for name, mask, shift in fields
             if not nonzero_only or (value >> shift) & mask]
    return ", ".join(parts)


def register_line(address, value, registers, show_fields):
    name, fields = registers.get(address, (None, []))
    text = f"{name or 'unknown register'} ({address:#07x}) = {value:#010x}"
    if show_fields and fields:
        text += f"  [{field_text(value, fields, True)}]"
    return text


def hex_words(payload, limit=12):
    text = " ".join(f"{word:#010x}" for word in payload[:limit])
    return text + (f" ... (+{len(payload) - limit})" if len(payload) > limit else "")


def table_records(address, count, kind, memory, registers, show_fields):
    """One line per record of a register table read from the captured
    workspace image, or no lines when the table is not in the image."""
    if memory is None:
        return []
    begin, image = memory
    at = address - begin
    if at < 0 or at + TABLE_RECORD_BYTES * count > len(image):
        return [f"table outside the captured workspace ({label_address(address, [])})"]
    lines = []
    for index in range(count):
        record = image[at + TABLE_RECORD_BYTES * index:at + TABLE_RECORD_BYTES * (index + 1)]
        offset = int.from_bytes(record[0:2], "little")
        value = int.from_bytes(record[4:8], "little")
        lines.append(f"[{offset:#05x}] "
                     + register_line(REGISTER_BASES[kind] + 4 * offset, value, registers, show_fields))
    return lines


def describe(packet, names, registers, regions, memory, show_fields):
    """A one-line title for the packet, plus one line per register it sets."""
    payload = packet["payload"]
    if packet["kind"] == 2:
        return "type-2 filler", []
    if packet["kind"] == 1:
        return f"invalid type-1 header {packet['header']:#010x}", []
    if packet["kind"] == 0:
        base = (packet["header"] & 0xFFFF) << 2
        lines = [register_line(base + 4 * index, value, registers, show_fields)
                 for index, value in enumerate(payload)]
        return f"type-0 register write, {len(payload)} value(s)", lines
    opcode = packet["opcode"]
    title = f"{names.get(opcode, 'UNKNOWN')} ({opcode:#04x})"
    if opcode in TABLE_LOADS and len(payload) >= 4:
        address = payload[0] | (payload[1] << 32)
        kind = TABLE_LOADS[opcode]
        return (f"{title}: {kind} register table at {label_address(address, regions)}, "
                f"{payload[3]} records",
                table_records(address, payload[3], kind, memory, registers, show_fields))
    if opcode in SET_REGISTERS and payload:
        base = REGISTER_BASES[SET_REGISTERS[opcode]] + 4 * (payload[0] & 0xFFFF)
        lines = [register_line(base + 4 * index, value, registers, show_fields)
                 for index, value in enumerate(payload[1:])]
        return f"{title}: {len(payload) - 1} {SET_REGISTERS[opcode]} register(s)", lines
    if opcode == 0x2D and len(payload) >= 2:
        return f"{title}: {payload[0]} vertices, draw initiator {payload[1]:#x}", []
    if opcode == 0x26 and len(payload) >= 2:
        address = payload[0] | (payload[1] << 32)
        return f"{title}: index buffer at {label_address(address, regions)}", []
    if opcode == 0x13 and payload:
        return f"{title}: {payload[0]} indices", []
    if opcode == 0x27 and len(payload) >= 5:
        address = payload[1] | (payload[2] << 32)
        return (f"{title}: {payload[3]} indices at {label_address(address, regions)}, "
                f"buffer size {payload[0]}, draw initiator {payload[4]:#x}"), []
    if opcode == 0x37 and len(payload) >= 3:
        address = payload[1] | (payload[2] << 32)
        return (f"{title}: control {payload[0]:#x}, address {label_address(address, regions)}, "
                f"data {hex_words(payload[3:])}"), []
    return f"{title}: {hex_words(payload)}", []


def table_loads(packets):
    return sorted((packet["index"], packet["opcode"], packet["payload"][0] | (packet["payload"][1] << 32),
                   packet["payload"][3]) for packet in packets
                  if packet["kind"] == 3 and packet["opcode"] in TABLE_LOADS
                  and len(packet["payload"]) >= 4)


def print_stream(label, stream, names, registers, args):
    """Print one decoded stream; returns the number of failed checks."""
    words = stream["words"]
    packets = split_packets(words)
    expected = stream["word_count"]
    captured = is_captured(stream)
    heading = f"{label} test={stream['test'] or '-'}"
    if stream["buffer"] is not None:
        heading += f" buffer={stream['buffer']}"
    heading += (f": {len(words)} of {expected if expected is not None else '?'} words logged, "
                f"{len(packets)} packets, encoding {stream['encoding'] or 'not logged'}"
                + (", captured" if captured else ""))
    print(heading)
    workspace = stream["workspace"]
    begin = workspace.get("begin")
    regions = [("stage", (begin, (workspace.get("end") or 0) - (begin or 0)))]
    regions += list(stream["regions"].items())
    image = workspace_image(stream)
    memory = (begin, image) if image is not None else None
    if not args.summary:
        calls = {call["begin"]: call for call in stream["calls"]} if args.calls else {}
        for packet in packets:
            call = calls.get(packet["offset"])
            if call is not None:
                arguments = ", ".join(f"{arg:#x}" for arg in call["args"])
                print(f"  {call['name']}({arguments}) -> words {call['begin']}-{call['end']}")
            title, lines = describe(packet, names, registers, regions, memory, args.fields)
            truncated = "  [logged words end inside this packet]" if packet["truncated"] else ""
            print(f"  [{packet['index']:>3}] @{packet['offset']:04d}  {title}{truncated}")
            for line in lines:
                print(f"               {line}")

    failures = 0
    complete = expected is not None and len(words) == expected
    if expected is not None and len(words) < expected:
        print(f"  note: only {len(words)} of {expected} words were logged; checks cover those")
    if complete and packets and packets[-1]["truncated"]:
        print("  CHECK FAILED: packet framing runs past the end of the stream")
        failures += 1
    if stream["tables_checked"]:
        decoded = table_loads(packets)
        logged = sorted(stream["logged_tables"])
        if decoded == logged:
            print(f"  check: {len(decoded)} register-table load(s) match agc_gpu_pointer_indirect")
        elif complete:
            print(f"  CHECK FAILED: decoded table loads {decoded} differ from "
                  f"agc_gpu_pointer_indirect {logged}")
            failures += 1
    if captured:
        problems = capture_problems(stream)
        if problems:
            for problem in problems:
                print(f"  CHECK FAILED: capture: {problem}")
            failures += 1
        else:
            print(f"  check: capture complete: {len(stream['calls'])} helper calls cover the "
                  f"stream, {len(stream['chunks'])} workspace chunks match their FNV-1a 64")
    return failures


def decode_streams(args):
    directory = header_directory()
    names = load_opcode_names(directory)
    registers = load_registers(directory)
    shown = 0
    failures = 0
    number = 0
    for run_number, run in enumerate(read_runs(args.log), 1):
        if args.run is not None and run_number != args.run:
            continue
        for stream in extract_streams(run):
            number += 1
            if (args.test and stream["test"] != args.test) or \
                    (args.stream is not None and number != args.stream):
                continue
            if shown:
                print()
            shown += 1
            failures += print_stream(f"run {run_number} pid={run['pid']} stream {number}", stream,
                                     names, registers, args)
    if not shown:
        print("no logged command stream matches")
        return 2
    print()
    print(f"{shown} stream(s) decoded; "
          + ("every check passed" if not failures else f"{failures} stream(s) failed a check"))
    return 1 if failures else 0


def explain_register(args):
    registers = load_registers(header_directory())
    kind = KIND_ALIASES[args.kind]
    address = REGISTER_BASES[kind] + 4 * args.offset
    name, fields = registers.get(address, (None, []))
    print(f"{kind} offset {args.offset:#x} = {address:#07x} {name or 'unknown register'}"
          f" value {args.value:#010x}")
    for field, mask, shift in fields:
        print(f"  {field:<32} {(args.value >> shift) & mask:#x}")
    if name and not fields:
        print("  no field definitions for gfx103")
    return 0 if name else 1


def main():
    number = lambda text: int(text, 0)  # noqa: E731
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    stream = commands.add_parser("stream", help="decode the command streams in a klog file")
    stream.add_argument("log")
    stream.add_argument("--run", type=int, help="only this run (1-based)")
    stream.add_argument("--test", help="only streams of this runner test, e.g. m4-rtt")
    stream.add_argument("--stream", type=int, help="only this stream (1-based, across runs)")
    stream.add_argument("--fields", action="store_true",
                        help="decode the non-zero fields of every register value")
    stream.add_argument("--calls", action="store_true",
                        help="show the captured AGC helper call before the packets it wrote")
    stream.add_argument("--summary", action="store_true",
                        help="print stream headings and checks without packets")
    stream.set_defaults(handler=decode_streams)

    register = commands.add_parser("register", help="decode one register value")
    register.add_argument("kind", choices=sorted(KIND_ALIASES))
    register.add_argument("offset", type=number, help="AGC table offset, e.g. 0x31c")
    register.add_argument("value", type=number, help="register value, e.g. 0x8828")
    register.set_defaults(handler=explain_register)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
