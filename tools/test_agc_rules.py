#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - proofs of the hardware rule checks.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prove every rule in tools/agc_rules.py.

Every golden frame must pass every rule, and each rule must fire, alone, on a
golden frame broken the way the console once failed.

Run: python3 tools/test_agc_rules.py
"""

import json
import struct
import unittest
from pathlib import Path

import agc_rules
from pm4_decode import split_packets

GOLDEN = Path(__file__).resolve().parent.parent / "golden"
# M2-M4 frames, the B4 frames that end in a completion marker, the C1 frames
# drawn in one stream and flipped alone in the next, and the C1b frames that
# clear with one pipeline and draw with another in one stream.
GOLDEN_SETS = ("runner", "b4", "c1", "c1-clear")


def load(name, golden_set="runner"):
    document = json.loads((GOLDEN / golden_set / name).read_text(encoding="utf-8"))
    return agc_rules.frame_from_golden(document, name)


def violated(frame):
    return {identifier for identifier, problems in agc_rules.check_frame(frame) if problems}


def find_packet(frame, opcode, predicate=lambda packet: True):
    return next(packet for packet in split_packets(frame.words)
                if packet["kind"] == 3 and packet["opcode"] == opcode and predicate(packet))


def record_at(frame, register):
    """Image offset of the first context-table record of an AGC register."""
    for _, kind, address, count in agc_rules.analyse(frame).tables:
        if kind != "context":
            continue
        at = address - frame.stage
        for index in range(count):
            offset, _ = struct.unpack_from("<HxxI", frame.image, at + 8 * index)
            if offset == register:
                return at + 8 * index
    raise AssertionError(f"{frame.name} has no context record {register:#05x}")


def change_record(frame, register, change):
    at = record_at(frame, register)
    (value,) = struct.unpack_from("<I", frame.image, at + 4)
    struct.pack_into("<I", frame.image, at + 4, change(value))


class GoldenFramesPass(unittest.TestCase):
    def test_every_golden_frame_passes_every_rule(self):
        for golden_set in GOLDEN_SETS:
            paths = sorted((GOLDEN / golden_set).glob("*.json"))
            self.assertTrue(paths, f"no golden files in golden/{golden_set}")
            for path in paths:
                with self.subTest(frame=f"{golden_set}/{path.name}"):
                    self.assertEqual(violated(load(path.name, golden_set)), set())


class EachRuleFiresAlone(unittest.TestCase):
    def assert_only(self, frame, identifier):
        self.assertEqual(violated(frame), {identifier})

    def test_rules_are_all_proven(self):
        proven = {name[len("test_"):].replace("_", "-") for name in dir(self)
                  if name.startswith("test_") and name != "test_rules_are_all_proven"}
        self.assertEqual(proven, {entry["id"] for entry in agc_rules.RULES})

    def test_wait_buffer(self):
        # The wait names buffer 1 while the frame flips buffer 0.
        frame = load("m2-solid-1.json")
        wait = find_packet(frame, agc_rules.WAIT_OPCODE)
        frame.words[wait["offset"] + 2] |= 0x8
        self.assert_only(frame, "wait-buffer")
        # A frame that draws and flips without a wait: M2's wait becomes a NOP
        # of the same length. A flip alone needs none (golden/c1).
        frame = load("m2-solid-1.json")
        wait = find_packet(frame, agc_rules.WAIT_OPCODE)
        frame.words[wait["offset"]] = (frame.words[wait["offset"]] & ~0xFF00) | (0x10 << 8)
        self.assert_only(frame, "wait-buffer")
        # A frame that neither flips nor ends in a completion marker: the B4
        # headless frame with its marker's event cleared.
        frame = load("b4-headless-1.json", "b4")
        marker = find_packet(frame, agc_rules.RELEASE_MEM,
                             lambda packet: packet["payload"][0] & 0xFF == agc_rules.COMPLETION_EVENT)
        frame.words[marker["offset"] + 1] &= ~0xFF
        self.assert_only(frame, "wait-buffer")

    def test_flip_target(self):
        # The frame draws into buffer 1 but flips buffer 0.
        frame = load("m2-solid-1.json")
        change_record(frame, agc_rules.CB_COLOR0_BASE, lambda value: value + (0x2000000 >> 8))
        self.assert_only(frame, "flip-target")

    def test_scanout_swap(self):
        # The M2 orange screen: SWAP_STD on the scanout buffer.
        frame = load("m2-solid-1.json")
        change_record(frame, agc_rules.CB_COLOR0_INFO, lambda value: value & ~0x1800)
        self.assert_only(frame, "scanout-swap")

    def test_blend_export(self):
        # The first M4 blend runs: 32_ABGR pixel exports.
        frame = load("m4-blend-1.json")
        change_record(frame, agc_rules.SPI_SHADER_COL_FORMAT, lambda value: (value & ~0xF) | 9)
        self.assert_only(frame, "blend-export")

    def test_tables_in_workspace(self):
        # The SH table load points below the workspace.
        frame = load("m2-solid-1.json")
        table = find_packet(frame, agc_rules.SH_TABLE_LOAD)
        frame.words[table["offset"] + 1] = 0x00010000
        self.assert_only(frame, "tables-in-workspace")

    def test_index_buffers(self):
        # The square's indices move below every mapped region.
        frame = load("m3-vertex-1.json")
        base = find_packet(frame, agc_rules.INDEX_BASE)
        draw = find_packet(frame, agc_rules.DRAW_INDEX_2)
        frame.words[base["offset"] + 1] = 0x00010000
        frame.words[draw["offset"] + 2] = 0x00010000
        self.assert_only(frame, "index-buffers")

    def test_barrier_before_sampling(self):
        # The render-to-texture barrier becomes a NOP of the same length.
        frame = load("m4-rtt-1.json")
        barrier = find_packet(frame, agc_rules.RELEASE_MEM,
                              lambda packet: packet["payload"][0] & 0xFF
                              == agc_rules.COLOUR_BARRIER_EVENT)
        frame.words[barrier["offset"]] = (frame.words[barrier["offset"]] & ~0xFF00) | (0x10 << 8)
        self.assert_only(frame, "barrier-before-sampling")

    def test_depth_target(self):
        # The depth size register (0x007) is dropped from the target records.
        frame = load("m4-depth-1.json")
        struct.pack_into("<H", frame.image, record_at(frame, 0x007), 0x3FF)
        self.assert_only(frame, "depth-target")


if __name__ == "__main__":
    unittest.main(verbosity=2)
