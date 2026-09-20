#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - hardware rule checks on AGC command streams.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check AGC command streams against the rules the console taught us.

Each rule encodes a hardware finding recorded in docs/HARDWARE_FINDINGS.md: a
frame that broke it rendered wrongly, deadlocked or faulted on the PS5. The
checks run on golden files, captured console klogs and klogs of the PC runner
(build/host/rebuild/*.log). A change that deliberately alters frames, so its
golden files no longer match, is still held to the hardware rules.

Usage:
  agc_rules.py PATH...   golden files, directories of golden files, or klogs
  agc_rules.py --list    describe every rule

The checks replay each frame's register state packet by packet: table loads
are read from the stage workspace image, direct SH writes from the stream.
Rules that need register tables or descriptors are skipped for streams
without a captured workspace. tools/test_agc_rules.py proves every rule on a
deliberately broken golden frame.
"""

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from golden import golden_image
from pm4_decode import TABLE_LOADS, split_packets
from ps5vk_log import extract_streams, read_runs, workspace_image

WAIT_OPCODE = 0x93
RELEASE_MEM = 0x49
SET_SH_REG = 0x76
INDEX_BASE = 0x26
DRAW_INDEX_2 = 0x27
DRAW_INDEX_AUTO = 0x2D
CONTEXT_TABLE_LOAD = 0x9F
SH_TABLE_LOAD = 0x63
# RELEASE_MEM event 45 flushes and invalidates colour-buffer data.
COLOUR_BARRIER_EVENT = 45
# RELEASE_MEM event 40 has the GPU write a completion marker (Phase B4).
COMPLETION_EVENT = 40
# The wait and the flip both carry 0x800040a0 | target buffer << 3.
BUFFER_WORD = 0x800040A0
TABLE_RECORD_BYTES = 8
IMAGE_DESCRIPTOR_WORDS = 12
PIXEL_USER_DATA = 0x0C
PIXEL_USER_DATA_WORDS = 16
# AGC context register offsets.
CB_COLOR0_BASE = 0x318
CB_COLOR0_INFO = 0x31C
CB_COLOR0_BASE_HIGH = 0x390
SPI_SHADER_COL_FORMAT = 0x1C5
CB_BLEND0_CONTROL = 0x1E0
DB_Z_READ_BASE = 0x012
DEPTH_TARGET_REGISTERS = (0x010, 0x011, 0x012, 0x013, 0x014, 0x015, 0x01A, 0x01B, 0x01C, 0x01D,
                          0x01E, 0x002, 0x005, 0x007, 0x00B, 0x00A)
COLOR_8_8_8_8 = 0xA
NUMBER_TYPE_UNORM = 0
EXPORT_FP16_ABGR = 4
SWAP_ALT = 1


@dataclass
class Frame:
    """One command stream and the memory its addresses point into."""
    name: str
    words: list
    stage: int
    image: object  # bytearray of the stage workspace, or None without a capture
    regions: dict  # region name -> (address, bytes)


@dataclass
class Draw:
    packet: int
    context: dict  # AGC context offset -> value at the draw
    sh: dict       # AGC SH offset -> value at the draw


@dataclass
class Analysis:
    tables: list          # (packet index, kind, address, record count)
    draws: list
    barriers: list        # packet indices of colour barriers
    completions: list     # packet indices of completion markers
    wait_buffer: object
    flip_buffer: object
    index_problems: list


def frame_from_golden(document, name):
    regions = {key: (int(value["address"], 16), value["bytes"])
               for key, value in document["regions"].items()}
    return Frame(name, [int(word, 16) for word in document["command"]["words"]],
                 regions["stage"][0], bytearray(golden_image(document)), regions)


def frames_from_klog(path):
    frames = []
    for run_number, run in enumerate(read_runs(path), 1):
        for number, stream in enumerate(extract_streams(run), 1):
            begin = stream["workspace"].get("begin")
            end = stream["workspace"].get("end")
            if begin is None or end is None or not stream["words"]:
                continue
            regions = {"stage": (begin, end - begin)}
            regions.update({name: (address, size) for name, (address, size)
                            in stream["regions"].items() if address is not None and size})
            image = workspace_image(stream)
            label = f"{Path(path).name} run {run_number} stream {number} {stream['test'] or ''}"
            frames.append(Frame(label.strip(), stream["words"], begin,
                                bytearray(image) if image is not None else None, regions))
    return frames


def region_of(frame, address, size=1):
    for name, (begin, length) in frame.regions.items():
        if begin <= address and address + size <= begin + length:
            return name
    return None


def read_records(frame, address, count):
    """(offset, value) records of a register table in the workspace image, or
    None when the image is missing or the table is outside it."""
    if frame.image is None:
        return None
    at = address - frame.stage
    if at < 0 or at + TABLE_RECORD_BYTES * count > len(frame.image):
        return None
    return [struct.unpack_from("<HxxI", frame.image, at + TABLE_RECORD_BYTES * index)
            for index in range(count)]


def buffer_of(word):
    return (word >> 3) & 1 if word & ~0x8 == BUFFER_WORD else None


def analyse(frame):
    analysis = Analysis(tables=[], draws=[], barriers=[], completions=[], wait_buffer=None,
                        flip_buffer=None, index_problems=[])
    context = {}
    sh = {}
    index_address = None
    for packet in split_packets(frame.words):
        if packet["kind"] != 3 or packet["truncated"]:
            continue
        opcode = packet["opcode"]
        payload = packet["payload"]
        if opcode in TABLE_LOADS and len(payload) >= 4:
            address = payload[0] | (payload[1] << 32)
            analysis.tables.append((packet["index"], TABLE_LOADS[opcode], address, payload[3]))
            records = read_records(frame, address, payload[3])
            if records is not None and opcode == CONTEXT_TABLE_LOAD:
                context.update(dict(records))
            elif records is not None and opcode == SH_TABLE_LOAD:
                sh.update(dict(records))
        elif opcode == SET_SH_REG and payload:
            base = payload[0] & 0xFFFF
            for index, value in enumerate(payload[1:]):
                sh[base + index] = value
        elif opcode == WAIT_OPCODE and len(payload) >= 2 and analysis.wait_buffer is None:
            analysis.wait_buffer = buffer_of(payload[1])
        elif opcode == RELEASE_MEM and payload:
            if payload[0] & 0xFF == COLOUR_BARRIER_EVENT:
                analysis.barriers.append(packet["index"])
            elif payload[0] & 0xFF == COMPLETION_EVENT:
                analysis.completions.append(packet["index"])
            elif len(payload) >= 3 and buffer_of(payload[2]) is not None:
                analysis.flip_buffer = buffer_of(payload[2])
        elif opcode == INDEX_BASE and len(payload) >= 2:
            index_address = payload[0] | (payload[1] << 32)
        elif opcode in (DRAW_INDEX_2, DRAW_INDEX_AUTO):
            if opcode == DRAW_INDEX_2 and len(payload) >= 5:
                address = payload[1] | (payload[2] << 32)
                count = payload[3]
                if address != index_address:
                    analysis.index_problems.append(
                        f"packet {packet['index']}: DRAW_INDEX_2 reads {address:#x}, "
                        f"INDEX_BASE set {index_address:#x}" if index_address is not None else
                        f"packet {packet['index']}: DRAW_INDEX_2 without INDEX_BASE")
                elif region_of(frame, address, 2 * count) is None:
                    analysis.index_problems.append(
                        f"packet {packet['index']}: {count} 16-bit indices at {address:#x} "
                        "lie outside every mapped region")
            analysis.draws.append(Draw(packet["index"], dict(context), dict(sh)))
    return analysis


def colour_target(context):
    base = context.get(CB_COLOR0_BASE)
    high = context.get(CB_COLOR0_BASE_HIGH)
    return None if base is None or high is None else (base << 8) | ((high & 0xFF) << 40)


def sampled_images(frame, sh):
    """Addresses of the images a draw's pixel descriptor sets sample. Pixel
    user-data words are 32-bit pointers combined with the workspace's high
    word; a 48-byte combined image-sampler descriptor has word 3 of the form
    0x9xxx_xfac and encodes its image address as word 0 << 8 plus the low byte
    of word 1 << 40."""
    if frame.image is None:
        return []
    high = frame.stage >> 32
    images = []
    for dword in range(PIXEL_USER_DATA_WORDS):
        value = sh.get(PIXEL_USER_DATA + dword)
        if not value:
            continue
        at = ((high << 32) | value) - frame.stage
        if 0 <= at and at + 4 * IMAGE_DESCRIPTOR_WORDS <= len(frame.image):
            words = struct.unpack_from(f"<{IMAGE_DESCRIPTOR_WORDS}I", frame.image, at)
            if words[3] >> 28 == 9 and words[3] & 0xFFF == 0xFAC:
                images.append((words[0] << 8) | ((words[1] & 0xFF) << 40))
    return images


RULES = []


def rule(identifier, summary, finding, needs_image):
    def register(check):
        RULES.append({"id": identifier, "summary": summary, "finding": finding,
                      "needs_image": needs_image, "check": check})
        return check
    return register


@rule("wait-buffer",
      "a frame that draws and flips names the flipped buffer in its wait-until-safe packet; a "
      "frame that does not flip ends in a completion marker; a flip alone needs no wait",
      "e27f6b8: a wait naming the on-screen buffer deadlocked frame 1; B4 (golden/b4): "
      "frames confirmed by a completion marker need no flip, and headless ones no wait; C1 "
      "(golden/c1): a stream holding only the flip of a buffer drawn in an earlier, completed "
      "stream flips it", False)
def check_wait_buffer(frame, analysis):
    if analysis.flip_buffer is None:
        return [] if analysis.completions else ["neither a flip nor a completion marker"]
    if not analysis.draws:
        return []
    if analysis.wait_buffer is None:
        return ["no wait-until-safe packet"]
    if analysis.wait_buffer != analysis.flip_buffer:
        return [f"the wait names buffer {analysis.wait_buffer}, the flip shows buffer "
                f"{analysis.flip_buffer}"]
    return []


@rule("flip-target", "the frame's last draw renders into the buffer it flips",
      "e27f6b8: frames alternate VideoOut buffers 0 and 1", True)
def check_flip_target(frame, analysis):
    framebuffer = frame.regions.get("framebuffer")
    if not analysis.draws or analysis.flip_buffer is None or framebuffer is None:
        return []
    target = colour_target(analysis.draws[-1].context)
    expected = framebuffer[0] + analysis.flip_buffer * (framebuffer[1] // 2)
    if target != expected:
        shown = f"{target:#x}" if target is not None else "no colour target"
        return [f"last draw (packet {analysis.draws[-1].packet}) renders into {shown}; "
                f"buffer {analysis.flip_buffer} is {expected:#x}"]
    return []


@rule("scanout-swap", "draws into VideoOut buffers use COMP_SWAP = SWAP_ALT (B8G8R8A8)",
      "5fa3b84: SWAP_STD scanout showed orange for light blue", True)
def check_scanout_swap(frame, analysis):
    problems = []
    for draw in analysis.draws:
        target = colour_target(draw.context)
        if target is None or region_of(frame, target) != "framebuffer":
            continue
        swap = (draw.context.get(CB_COLOR0_INFO, 0) >> 11) & 3
        if swap != SWAP_ALT:
            problems.append(f"draw at packet {draw.packet} scans out with COMP_SWAP {swap}")
    return problems


@rule("blend-export", "blended draws into 8-bit UNORM targets export FP16_ABGR colour",
      "d050664: 32_ABGR exports corrupted source-alpha blending", True)
def check_blend_export(frame, analysis):
    problems = []
    for draw in analysis.draws:
        if not (draw.context.get(CB_BLEND0_CONTROL, 0) >> 30) & 1:
            continue
        info = draw.context.get(CB_COLOR0_INFO, 0)
        if (info >> 2) & 0x1F != COLOR_8_8_8_8 or (info >> 8) & 7 != NUMBER_TYPE_UNORM:
            continue
        export = draw.context.get(SPI_SHADER_COL_FORMAT)
        if export is None or export & 0xF != EXPORT_FP16_ABGR:
            shown = f"{export & 0xF}" if export is not None else "unset"
            problems.append(f"blended draw at packet {draw.packet} exports colour format {shown}")
    return problems


@rule("tables-in-workspace", "indirect register tables lie wholly inside the stage workspace",
      "9bfe637: a table on the stack faulted the GPU asynchronously", False)
def check_tables(frame, analysis):
    return [f"packet {index}: {kind} table of {count} records at {address:#x} is outside the "
            "workspace" for index, kind, address, count in analysis.tables
            if region_of(frame, address, TABLE_RECORD_BYTES * count) != "stage"]


@rule("index-buffers", "each indexed draw reads the index buffer it set, inside mapped memory",
      "e5fb5ba: the index buffer is read from the GPU-visible workspace", False)
def check_index_buffers(frame, analysis):
    return list(analysis.index_problems)


@rule("barrier-before-sampling",
      "a colour barrier precedes sampling an image drawn earlier in the frame",
      "388d18e: ps5-opengl's RELEASE_MEM barrier before sampling the offscreen target", True)
def check_barriers(frame, analysis):
    problems = []
    last_drawn = {}
    for draw in analysis.draws:
        for image in sampled_images(frame, draw.sh):
            region = region_of(frame, image)
            if region in last_drawn and not any(last_drawn[region] < barrier < draw.packet
                                                for barrier in analysis.barriers):
                problems.append(f"draw at packet {draw.packet} samples {region} drawn at packet "
                                f"{last_drawn[region]} without a barrier in between")
        target = colour_target(draw.context)
        region = region_of(frame, target) if target is not None else None
        if region is not None:
            last_drawn[region] = draw.packet
    return problems


@rule("depth-target", "a bound depth buffer carries all 16 depth-target registers",
      "9faf8b2: ps5-opengl's 16 depth-target records", True)
def check_depth_target(frame, analysis):
    problems = []
    for draw in analysis.draws:
        if draw.context.get(DB_Z_READ_BASE):
            missing = [f"{offset:#05x}" for offset in DEPTH_TARGET_REGISTERS
                       if offset not in draw.context]
            if missing:
                problems.append(f"draw at packet {draw.packet} binds depth without "
                                f"{', '.join(missing)}")
    return problems


def check_frame(frame):
    """(rule id, problems) per rule; problems is None when the rule was skipped."""
    analysis = analyse(frame)
    return [(entry["id"], None if entry["needs_image"] and frame.image is None
             else entry["check"](frame, analysis)) for entry in RULES]


def load_frames(path):
    path = Path(path)
    if path.is_dir():
        return [frame_from_golden(json.loads(child.read_text(encoding="utf-8")), child.name)
                for child in sorted(path.glob("*.json"))]
    if path.suffix == ".json":
        return [frame_from_golden(json.loads(path.read_text(encoding="utf-8")), path.name)]
    return frames_from_klog(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="golden files, directories or klogs")
    parser.add_argument("--list", action="store_true", help="describe every rule")
    args = parser.parse_args()
    if args.list:
        for entry in RULES:
            image = " (needs a captured workspace)" if entry["needs_image"] else ""
            print(f"{entry['id']:<26} {entry['summary']}{image}\n{'':<26} {entry['finding']}")
        return 0
    if not args.paths:
        parser.error("give golden files, directories or klogs, or --list")
    frames = [frame for path in args.paths for frame in load_frames(path)]
    if not frames:
        print("no command streams found")
        return 2
    violations = 0
    for frame in frames:
        results = check_frame(frame)
        broken = [(identifier, problems) for identifier, problems in results if problems]
        skipped = [identifier for identifier, problems in results if problems is None]
        checked = len(results) - len(skipped)
        note = f"; skipped without a workspace image: {', '.join(skipped)}" if skipped else ""
        if not broken:
            print(f"{frame.name}: {checked} of {len(results)} rules pass{note}")
            continue
        print(f"{frame.name}: {len(broken)} rule(s) broken{note}")
        for identifier, problems in broken:
            violations += len(problems)
            for problem in problems:
                print(f"  RULE {identifier}: {problem}")
    print(f"{len(frames)} stream(s) checked against {len(RULES)} hardware rules; "
          f"{violations} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
