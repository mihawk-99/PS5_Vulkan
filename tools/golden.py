#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - golden command-stream captures.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn a runner capture klog into golden command-stream files and check PC
models against them.

Commands:
  extract LOG DIR      Check every captured stream of one run and, when all
                       are complete, write one JSON file per stream to DIR,
                       named <test>-<n>.json. Nothing is written if any
                       capture is incomplete.
  check-helpers DIR    Replay every AGC helper call recorded in DIR's golden
                       files through the PC models (build/host/libagc_host.so,
                       built by tools/build-agc-host.sh) and compare the words
                       each model writes with the words the console wrote.
  rebuild DIR          Run the console runner's code on the PC
                       (build/host/runner_host, built by
                       tools/build-host-runner.sh) once per test in capture
                       mode, replaying what the PC cannot compute, and compare
                       every captured frame with its golden file: command
                       words, helper calls, addresses and workspace image.
  replay GOLDEN OUT    Write the host replay of one golden file, the inputs
                       the PC cannot compute, for the Vulkan driver's PC
                       tests, which read it through PS5_HOST_REPLAY.
  compare-submission GOLDEN DUMP
                       Compare the submissions a driver test recorded on the
                       PC (PS5_HOST_SUBMISSION_DUMP) with a golden frame whose
                       draw repeats as often as each submission draws
                       (--draws), packet by packet and register table by
                       register table; table addresses and the completion
                       marker's address and value may differ.

Produce the klog by running the runner with jobs/capture/queue.txt. A golden
file holds what a PC build of the frame encoder needs to reproduce and compare
the frame: the test, target buffer and frame number within the run, how many
flips the process made before it (streams that end in a completion marker do
not flip), the stream words, the AGC helper calls that wrote them with their arguments, the
workspace and allocation addresses, and the non-zero chunks of the stage
workspace image with its FNV-1a 64.
"""

import argparse
import ctypes
import json
import subprocess
import sys
import time
from pathlib import Path

from ps5vk_log import capture_problems, extract_streams, is_captured, read_runs

ROOT = Path(__file__).resolve().parent.parent
GOLDEN_SCHEMA = 1
MODEL_LIBRARY = ROOT / "build" / "host" / "libagc_host.so"
MODEL_BUFFER_WORDS = 4096
RUNNER = ROOT / "build" / "host" / "runner_host"
REBUILD_DIRECTORY = ROOT / "build" / "host" / "rebuild"
# append_target_registers writes these first in a frame's first context table.
TARGET_REGISTER_COUNT = 16
# Helper arguments that point into CPU memory (by argument index).
CPU_POINTER_ARGUMENTS = {"sceAgcCbSetShRegisterRangeDirect": (1,)}
# The span name the runner uses for packets a test wrote by hand instead of
# through an AGC helper. Nothing models them: the recorded words are the
# record, so a check compares them with the golden file and stops there.
RAW_PACKETS = "raw_packets"


def flips_before_of(document):
    """How many flips the console process made before a golden frame. Files
    captured before B4 have no field; every stream in them flipped."""
    return document.get("flips_before", document["frame"] - 1)


def golden_number(value):
    """A golden file's number: the writer writes addresses and sizes as hex
    text, and a hand-written document may hold them as numbers."""
    return int(value, 16) if isinstance(value, str) else value


def golden_document(run, stream, log_name, sequence, frame, flips_before):
    # A test that drove the Vulkan driver logs no command stream and no
    # workspace of its own: what it captures is the pipelines the driver
    # compiled (ps5vk_log.capture_problems). Its regions are the allocations it
    # did name, and the command is empty.
    begin = stream["workspace"].get("begin", 0)
    regions = {}
    if begin:
        regions["stage"] = {"address": f"{begin:#x}", "bytes": stream["workspace"]["end"] - begin}
    for name, (address, size) in sorted(stream["regions"].items()):
        if address is not None:
            regions[name] = {"address": f"{address:#x}", "bytes": size}
    command_bottom = stream["command_bottom"] if stream["command_bottom"] is not None else begin
    return {
        "schema": GOLDEN_SCHEMA,
        "source": {"klog": log_name, "pid": run["pid"]},
        "test": stream["test"],
        "sequence": sequence,
        "frame": frame,
        "flips_before": flips_before,
        "buffer": stream["buffer"],
        "regions": regions,
        "command": {"offset": f"{command_bottom - begin:#x}",
                    "words": [f"{word:#010x}" for word in stream["words"]]},
        "calls": [{"name": call["name"], "words": [call["begin"], call["end"]],
                   "args": [f"{arg:#x}" for arg in call["args"]]} for call in stream["calls"]],
        "workspace": {"fnv1a64": (f"{stream['workspace_fnv1a64']:#018x}"
                                  if stream["workspace_fnv1a64"] is not None else None),
                      "chunk_words": 64,
                      "chunks": {f"{offset:#06x}": "".join(f"{word:08x}" for word in words)
                                 for offset, words in sorted(stream["chunks"].items())}},
        # The driver's pipelines, which a PC rebuild replays: AGC's output for
        # each one lives in its own stage mapping, and the workspace image
        # above covers only the first (docs/M5_PHASE_C.md, the PC model's
        # pipeline capture). A golden file without this key is a capture from
        # before the runner logged them, and stays valid as it is.
        "stages": [{"index": stage["index"],
                    "address": f"{stage['address']:#x}",
                    "bytes": f"{stage['bytes']:#x}",
                    "fnv1a64": f"{stage['fnv1a64']:#018x}",
                    "chunk_words": 64,
                    "chunks": {f"{offset:#06x}": "".join(f"{word:08x}" for word in words)
                               for offset, words in sorted(stage["chunks"].items())}}
                   for stage in stream.get("stages", [])],
    }


def extract(args):
    runs = [run for run in read_runs(args.log)
            if any(is_captured(stream) for stream in extract_streams(run))]
    if not runs:
        print("no run with captured streams in this klog")
        return 2
    if args.run is not None and not 1 <= args.run <= len(runs):
        print(f"--run must be between 1 and {len(runs)}")
        return 2
    run = runs[(args.run or len(runs)) - 1]
    documents = []
    failures = 0
    sequences = {}
    driver_streams = []
    # Streams are numbered in run order. Only streams that flip advance the
    # process's flip count, which each flip packet encodes; in a capture run
    # every stream is captured, so its helper calls show whether it flipped.
    flips = 0
    for frame, stream in enumerate(extract_streams(run), 1):
        flips_before = flips
        flips += any(call["name"] == FLIP_HELPER for call in stream["calls"])
        # --test keeps only the named tests; frames of other tests still count
        # towards frame numbers and flips, so the kept files match the run.
        if not is_captured(stream) or (args.test and stream["test"] not in args.test):
            continue
        # A test that drove the driver is not a frame: the driver submitted, not
        # the runner, so what it captured is the streams it queued and the
        # table images they name. They become a run document plus one document
        # per submission (driver_run_document).
        if stream["submissions"]:
            problems = capture_problems(stream)
            if problems:
                print(f"{stream['test']}: incomplete capture: {'; '.join(problems)}")
                failures += 1
                continue
            driver_streams.append(stream)
            continue
        test = stream["test"] or "stream"
        sequences[test] = sequences.get(test, 0) + 1
        name = f"{test}-{sequences[test]}.json"
        problems = capture_problems(stream)
        if problems:
            print(f"{name}: incomplete capture: {'; '.join(problems)}")
            failures += 1
            continue
        documents.append((name, golden_document(run, stream, Path(args.log).name, sequences[test],
                                                frame, flips_before)))
        print(f"{name}: frame {frame}, {flips_before} flip(s) before, {len(stream['words'])} "
              f"words, {len(stream['calls'])} helper calls, {len(stream['chunks'])} workspace "
              "chunks")
    if driver_streams:
        documents.append(("run-1.json", driver_run_document(run, driver_streams,
                                                            Path(args.log).name)))
        for stream in driver_streams:
            for index, submission in enumerate(stream["submissions"], 1):
                documents.append((f"{stream['test']}-{index}.json",
                                  submission_document(run, stream, submission,
                                                      Path(args.log).name, index)))
            print(f"{stream['test']}: {len(stream['submissions'])} submission(s) and "
                  f"{len(stream['stages'])} pipeline(s) captured")
    if failures:
        print(f"{failures} incomplete capture(s); no golden file written")
        return 1
    directory = Path(args.directory)
    directory.mkdir(parents=True, exist_ok=True)
    for name, document in documents:
        (directory / name).write_text(json.dumps(document, indent=1) + "\n", encoding="utf-8",
                                      newline="\n")
    print(f"wrote {len(documents)} golden file(s) to {directory} from run pid={run['pid']}")
    return 0


class AgcCommandBuffer(ctypes.Structure):
    _fields_ = [("bottom", ctypes.POINTER(ctypes.c_uint32)), ("top", ctypes.POINTER(ctypes.c_uint32)),
                ("up", ctypes.POINTER(ctypes.c_uint32)), ("down", ctypes.POINTER(ctypes.c_uint32)),
                ("callback", ctypes.c_uint64), ("user_data", ctypes.c_void_p),
                ("reserved_dwords", ctypes.c_uint32), ("padding", ctypes.c_uint32)]


WORD_POINTER = ctypes.POINTER(ctypes.c_uint32)
VOID = ctypes.c_void_p
# ctypes signature of every modelled helper after its command buffer.
HELPER_ARGUMENTS = {
    "sceAgcDcbSetCxRegistersIndirect": [VOID, ctypes.c_uint32],
    "sceAgcDcbSetUcRegistersIndirect": [VOID, ctypes.c_uint32],
    "sceAgcDcbSetShRegistersIndirect": [VOID, ctypes.c_uint32],
    "sceAgcCbSetShRegisterRangeDirect": [ctypes.c_uint32, WORD_POINTER, ctypes.c_uint32],
    "sceAgcDcbSetIndexSize": [ctypes.c_uint8, ctypes.c_uint8],
    "sceAgcDcbSetIndexBuffer": [VOID],
    "sceAgcDcbSetIndexCount": [ctypes.c_uint32],
    "sceAgcDcbDrawIndex": [ctypes.c_uint32, VOID, ctypes.c_uint64],
    "sceAgcDcbDrawIndexAuto": [ctypes.c_uint32, ctypes.c_uint64],
    "sceAgcCbReleaseMem": [ctypes.c_uint8, ctypes.c_int16, ctypes.c_uint64, ctypes.c_int8, VOID,
                           ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint16, ctypes.c_uint16,
                           ctypes.c_int8, ctypes.c_int32],
    "sceAgcDcbSetFlip": [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32, ctypes.c_int64],
}
WAIT_HELPER = "sceAgcDriverWaitUntilSafeForRendering"
FLIP_HELPER = "sceAgcDcbSetFlip"


def load_models(path):
    if not path.is_file():
        raise SystemExit(f"missing {path}; build it with bash tools/build-agc-host.sh")
    library = ctypes.CDLL(str(path))
    for name, arguments in HELPER_ARGUMENTS.items():
        function = getattr(library, name)
        function.argtypes = [ctypes.POINTER(AgcCommandBuffer)] + arguments
        function.restype = WORD_POINTER
    wait = getattr(library, WAIT_HELPER)
    wait.argtypes = [ctypes.POINTER(WORD_POINTER), ctypes.c_uint32, ctypes.c_uint32,
                     ctypes.c_uint32, ctypes.c_int]
    wait.restype = ctypes.c_uint32
    library.sceAgcDriverGetWaitRenderingPacketSizeInDwords.restype = ctypes.c_uint32
    library.agc_host_last_error.restype = ctypes.c_char_p
    library.agc_host_reset.argtypes = [ctypes.c_uint32]
    return library


def signed(value, bits):
    return value - (1 << bits) if value >= 1 << (bits - 1) else value


def run_model(library, call, recorded, flips_before):
    """The words the model writes for one recorded call, or an error string."""
    if call["name"] == RAW_PACKETS:
        # Written by hand, so there is no model to ask: the words the run
        # recorded are the record this check compares against.
        return list(recorded)
    storage = (ctypes.c_uint32 * MODEL_BUFFER_WORDS)()
    bottom = ctypes.cast(storage, WORD_POINTER)
    top = ctypes.cast(ctypes.addressof(storage) + 4 * MODEL_BUFFER_WORDS, WORD_POINTER)
    command = AgcCommandBuffer(bottom, top, bottom, top, 0, None, 0, 0)
    args = call["args"]
    name = call["name"]
    # The flip encodes how many flips the process made before it.
    library.agc_host_reset(flips_before)
    if name == WAIT_HELPER:
        up = ctypes.cast(storage, WORD_POINTER)
        size = library.sceAgcDriverGetWaitRenderingPacketSizeInDwords()
        if library.sceAgcDriverWaitUntilSafeForRendering(ctypes.byref(up), size, 0, args[0],
                                                         signed(args[1], 32)) != 0:
            return library.agc_host_last_error().decode()
        written = (ctypes.addressof(up.contents) - ctypes.addressof(storage)) // 4
        return list(storage[:written])
    if name not in HELPER_ARGUMENTS:
        return f"no model for {name}"
    function = getattr(library, name)
    if name == "sceAgcCbSetShRegisterRangeDirect":
        # The values came from CPU memory on the console; the recorded packet
        # is the only copy, so the check covers the framing, not the values.
        values = (ctypes.c_uint32 * args[2])(*recorded[2:2 + args[2]])
        start = function(ctypes.byref(command), args[0], values, args[2])
    elif name == "sceAgcCbReleaseMem":
        start = function(ctypes.byref(command), args[0], signed(args[1], 16), args[2],
                         signed(args[3], 8), args[4] or None, args[5], args[6], args[7], args[8],
                         signed(args[9], 8), signed(args[10], 32))
    elif name == "sceAgcDcbSetFlip":
        start = function(ctypes.byref(command), args[0], signed(args[1], 32), args[2],
                         signed(args[3], 64))
    else:
        start = function(ctypes.byref(command), *args)
    if not start:
        return library.agc_host_last_error().decode()
    if ctypes.addressof(start.contents) != ctypes.addressof(storage):
        return "model did not return the start of its packet"
    written = (ctypes.addressof(command.up.contents) - ctypes.addressof(storage)) // 4
    return list(storage[:written])


def check_helpers(args):
    library = load_models(Path(args.library))
    paths = sorted(Path(args.directory).glob("*.json"))
    if not paths:
        print(f"no golden files in {args.directory}")
        return 2
    totals = {}
    failures = 0
    raw = 0
    for path in paths:
        document = json.loads(path.read_text(encoding="utf-8"))
        words = [int(word, 16) for word in document["command"]["words"]]
        for call in document["calls"]:
            if call["name"] == RAW_PACKETS:
                raw += call["words"][1] - call["words"][0]
        for index, call in enumerate(document["calls"]):
            if call["name"] == RAW_PACKETS:
                continue
            recorded_call = {"name": call["name"], "args": [int(arg, 16) for arg in call["args"]]}
            begin, end = call["words"]
            recorded = words[begin:end]
            produced = run_model(library, recorded_call, recorded, flips_before_of(document))
            passed, count = totals.get(call["name"], (0, 0))
            if produced == recorded:
                totals[call["name"]] = (passed + 1, count + 1)
                continue
            totals[call["name"]] = (passed, count + 1)
            failures += 1
            if isinstance(produced, str):
                detail = produced
            else:
                differing = next((at for at, (a, b) in enumerate(zip(produced, recorded)) if a != b),
                                 min(len(produced), len(recorded)))
                detail = (f"{len(produced)} words written, {len(recorded)} recorded; first "
                          f"difference at word {differing}")
            print(f"MISMATCH {path.name} call {index} {call['name']}({', '.join(call['args'])}): "
                  f"{detail}")
    for name, (passed, count) in sorted(totals.items()):
        print(f"  {passed:>3} of {count:>3}  {name}")
    total = sum(count for _, count in totals.values())
    print(f"{total - failures} of {total} recorded helper calls reproduced exactly "
          f"by {Path(args.library).name}")
    if raw:
        print(f"{raw} packet word(s) recorded as raw spans, compared verbatim")
    return 1 if failures else 0


def golden_image(document):
    """A golden file's stage workspace image, zero outside its chunks."""
    image = bytearray(document["regions"]["stage"]["bytes"])
    for offset, text in document["workspace"]["chunks"].items():
        base = int(offset, 16)
        for index in range(len(text) // 8):
            at = base + 4 * index
            image[at:at + 4] = int(text[8 * index:8 * index + 8], 16).to_bytes(4, "little")
    return bytes(image)


def stage_image(stage):
    """One golden file's captured pipeline stage as bytes, zero outside its
    chunks: what a PC rebuild replays a pipeline's relocated headers and
    linked context and uniforms from."""
    image = bytearray(golden_number(stage["bytes"]))
    for offset, text in stage["chunks"].items():
        base = int(offset, 16)
        for index in range(len(text) // 8):
            at = base + 4 * index
            image[at:at + 4] = int(text[8 * index:8 * index + 8], 16).to_bytes(4, "little")
    return bytes(image)


def register_defaults(document):
    """The AGC register defaults a golden file shows, as (offset, value) pairs,
    or None. The first context table starts with the 16 target registers the
    runner derives from the AGC defaults; its adjustments are idempotent, so the
    adjusted values replay as the defaults. A frame without register tables
    (b5-empty) shows none."""
    table = next((call for call in document.get("calls", [])
                  if call["name"] == "sceAgcDcbSetCxRegistersIndirect"), None)
    if table is None:
        return None
    image = golden_image(document)
    at = int(table["args"][0], 16) - int(document["regions"]["stage"]["address"], 16)
    return [(int.from_bytes(image[at + 8 * index:at + 8 * index + 2], "little"),
             int.from_bytes(image[at + 8 * index + 4:at + 8 * index + 8], "little"))
            for index in range(TARGET_REGISTER_COUNT)]


def run_of(document):
    """The console run a golden file was recorded in."""
    return document["source"]["klog"], document["source"]["pid"]


def stage_document(stage, test=None):
    """One captured image in a golden file: an address, its size, the FNV-1a 64
    of the whole image and its non-zero 64-word chunks."""
    document = {"index": stage["index"], "address": f"{stage['address']:#x}",
                "bytes": f"{stage['bytes']:#x}", "fnv1a64": f"{stage['fnv1a64']:#018x}",
                "chunk_words": 64,
                "chunks": {f"{offset:#06x}": "".join(f"{word:08x}" for word in words)
                           for offset, words in sorted(stage["chunks"].items())}}
    if test is not None:
        document["test"] = test
    return document


def driver_run_document(run, streams, log_name):
    """A run whose tests drove the driver, as the inputs a replay needs: the
    pipelines it compiled, the regions it allocated and the VideoOut handle it
    presented through. One document per run, because those are shared by every
    submission of the process; the submissions themselves are one document
    each (submission_document).

    The table chunks the submissions name are regions too, deduplicated here:
    a replay hands a captured region to the first allocation of its size, and
    the driver's chunk is one allocation, so listing it pins the tables the
    streams' user data points at to the address the console placed them at
    (a frame document carries the same region for its own tables). A run holds
    every test's regions, and two tests of one run allocate their chunks at the
    same addresses: the name carries the test, so one test's chunk does not
    replace the other's and a replay can still pick the test it replays
    (driver_replay_text; the c5-depth bisection's runs held three driver
    tests and lost the middle one's regions to this)."""
    stages = []
    regions = {}
    submissions = []
    video = None
    for stream in streams:
        for stage in stream["stages"]:
            stages.append(stage_document(stage, stream["test"]))
        for name, (address, bytes) in sorted(stream["regions"].items()):
            if address is not None:
                regions[name] = {"address": f"{address:#x}", "bytes": bytes}
        if stream["video_handle"] is not None:
            video = stream["video_handle"]
        for index, submission in enumerate(stream["submissions"], 1):
            for region in submission["regions"]:
                regions.setdefault(f"tables-{stream['test']}-{region['address']:#x}",
                                   {"address": f"{region['address']:#x}",
                                    "bytes": region["bytes"], "test": stream["test"]})
            submissions.append({"test": stream["test"], "label": submission["label"],
                                "file": f"{stream['test']}-{index}.json"})
    return {"schema": GOLDEN_SCHEMA, "kind": "driver-run",
            "source": {"klog": log_name, "pid": run["pid"]},
            "video": video, "stages": stages, "regions": regions, "submissions": submissions}


def region_document(region):
    """One region a submission names. A table chunk carries its captured image;
    a driver's own buffer is an address and a size only, because its contents
    are the application's data and a replay only needs to hand the same address
    to a PC rebuild's own buffer (src/diagnostics.cpp, log_driver_regions)."""
    if region["fnv1a64"] is None and region["chunk_count"] is None:
        return {"index": region["index"], "address": f"{region['address']:#x}",
                "bytes": f"{region['bytes']:#x}"}
    return stage_document(region)


def submission_document(run, stream, submission, log_name, sequence):
    """One command stream a driver queued, with the table images it names.
    A frame rewrites the same chunks, so the images belong here and not to the
    run document."""
    return {"schema": GOLDEN_SCHEMA, "kind": "driver-submission",
            "source": {"klog": log_name, "pid": run["pid"]},
            "test": stream["test"], "sequence": sequence, "label": submission["label"],
            "dwords": submission["dwords"],
            "words": [f"{word:#010x}" for word in submission["words"]],
            "regions": [region_document(region) for region in submission["regions"]]}


def sibling_defaults(document, path):
    """(defaults, name) of another golden file from the same console run, or
    (None, None).

    A test that drove the Vulkan driver logs no context table of its own -- the
    driver builds its tables, not the runner -- while the defaults belong to
    the console process, which every frame of a run shares. A document without
    one therefore takes them from a sibling, exactly as rebuild() takes the
    defaults of another frame of the same run.
    """
    for other_path in sorted(Path(path).parent.glob("*.json")):
        if other_path.name == Path(path).name:
            continue
        other = json.loads(other_path.read_text(encoding="utf-8"))
        if other.get("source") != document.get("source"):
            continue
        defaults = register_defaults(other)
        if defaults is not None:
            return defaults, other_path.name
    return None, None


def replay_text(document, defaults):
    """The inputs the PC runner cannot compute for a test, from its first golden
    file: frame number, flips before it, VideoOut handle, regions, the register
    defaults of its console run (register_defaults) and the workspace image the
    host layer replays shader headers and link outputs from."""
    lines = [f"# PS5 Vulkan host replay from {document['source']['klog']} "
             f"pid {document['source']['pid']}, written by tools/golden.py rebuild",
             f"frame {document['frame']}", f"flips {flips_before_of(document)}"]
    # The wait and the flip name the VideoOut handle; a headless frame has
    # neither, and opens no VideoOut.
    video = next((call for call in document["calls"] if call["name"] in (WAIT_HELPER, FLIP_HELPER)),
                 None)
    if video is not None:
        lines.append(f"video {video['args'][0]}")
    # The pipelines the capture carries come first: the model hands a region to
    # the first allocation of its size, so a driver's stage allocations take
    # their own captured regions rather than the runner's workspace, which the
    # golden lists after them (docs/M5_PHASE_C.md, steps 3 and 4).
    for stage in document.get("stages", []):
        lines.append(f"stage {stage['index']} {stage['address']} "
                     f"{golden_number(stage['bytes']):#x}")
        for offset, text in stage["chunks"].items():
            lines.append(f"stageimage {stage['index']} {offset} {text}")
    for name, region in document["regions"].items():
        lines.append(f"region {name} {region['address']} {region['bytes']:#x}")
    for offset, value in defaults:
        lines.append(f"default {offset:#05x} {value:#010x}")
    for offset, text in document["workspace"]["chunks"].items():
        lines.append(f"workspace {offset} {text}")
    return "\n".join(lines) + "\n"


def driver_replay_text(document, defaults, test=None):
    """A driver run's replay: the process's shared inputs, then the defaults.

    A run document holds the pipelines the driver compiled, the regions it
    allocated and the VideoOut handle it presented through; the submissions
    are separate documents, because a frame rewrites its table chunks
    (driver_run_document). The replay pins the driver's allocations to those
    addresses, so a PC run's submissions are comparable with the console's.
    """
    lines = [f"# PS5 Vulkan host replay from {document['source']['klog']} "
             f"pid {document['source']['pid']}, written by tools/golden.py replay",
             "frame 1", "flips 0"]
    if document.get("video") is not None:
        lines.append(f"video {document['video']:x}")
    # The pipelines come first: the model hands a region to the first
    # allocation of its size, so the driver's stage allocations take their own
    # captured regions before the swapchain allocation takes its.
    # A run holds every test's pipelines; a PC test is one of them, and its
    # first stage allocation has to take the region that test's first pipeline
    # used on the console, so only that test's stages are listed.
    stages = [stage for stage in document.get("stages", [])
              if test is None or stage.get("test") == test]
    for stage in stages:
        lines.append(f"stage {stage['index']} {stage['address']} "
                     f"{golden_number(stage['bytes']):#x}")
        for offset, text in stage["chunks"].items():
            lines.append(f"stageimage {stage['index']} {offset} {text}")
    for name, region in document.get("regions", {}).items():
        # A run holds every test's regions; a PC test is one of them, so a
        # table chunk another test's command buffer allocated is not listed:
        # the model hands a region to the first allocation of its size, and a
        # chunk this test would not have allocated on the console must not
        # take that place.
        if test is not None and region.get("test") not in (None, test):
            continue
        lines.append(f"region {name} {region['address']} "
                     f"{golden_number(region['bytes']):#x}")
    for offset, value in defaults:
        lines.append(f"default {offset:#05x} {value:#010x}")
    return "\n".join(lines) + "\n"


def comparable_calls(calls):
    """Helper calls with arguments that point into CPU memory replaced by "cpu":
    such addresses differ between processes, while the data they point to is
    checked through the command words."""
    comparable = []
    for call in calls:
        args = list(call["args"])
        for index in CPU_POINTER_ARGUMENTS.get(call["name"], ()):
            if index < len(args):
                args[index] = "cpu"
        comparable.append({**call, "args": args})
    return comparable


def compare_documents(golden, rebuilt, limit=4):
    """How a rebuilt frame differs from its golden file; empty when identical."""
    problems = []
    for key in ("buffer", "regions"):
        if golden[key] != rebuilt[key]:
            problems.append(f"{key}: {rebuilt[key]} (console {golden[key]})")
    expected = golden["command"]["words"]
    actual = rebuilt["command"]["words"]
    if golden["command"]["offset"] != rebuilt["command"]["offset"]:
        problems.append(f"command offset {rebuilt['command']['offset']} "
                        f"(console {golden['command']['offset']})")
    differing = [index for index in range(max(len(expected), len(actual)))
                 if index >= len(expected) or index >= len(actual) or expected[index] != actual[index]]
    if differing:
        shown = ", ".join(
            f"word {index} {actual[index] if index < len(actual) else '-'} "
            f"(console {expected[index] if index < len(expected) else '-'})"
            for index in differing[:limit])
        problems.append(f"command: {len(differing)} words differ ({len(actual)} built, "
                        f"{len(expected)} recorded): {shown}")
    golden_calls = comparable_calls(golden["calls"])
    rebuilt_calls = comparable_calls(rebuilt["calls"])
    if golden_calls != rebuilt_calls:
        pairs = list(zip(golden_calls, rebuilt_calls))
        index = next((at for at, (a, b) in enumerate(pairs) if a != b), len(pairs))
        built = rebuilt_calls[index] if index < len(rebuilt_calls) else "-"
        recorded = golden_calls[index] if index < len(golden_calls) else "-"
        problems.append(f"helper calls differ from call {index}: {built} (console {recorded})")
    if golden["workspace"] != rebuilt["workspace"]:
        console = golden_image(golden)
        host = golden_image(rebuilt)
        offsets = [at for at in range(0, min(len(console), len(host)), 4)
                   if console[at:at + 4] != host[at:at + 4]]
        shown = ", ".join(
            f"stage+{at:#06x} {int.from_bytes(host[at:at + 4], 'little'):#010x} "
            f"(console {int.from_bytes(console[at:at + 4], 'little'):#010x})"
            for at in offsets[:limit])
        problems.append(f"workspace: {len(offsets)} words differ: {shown}")
    return problems


def rebuild(args):
    runner = Path(args.runner)
    if not runner.is_file():
        raise SystemExit(f"missing {runner}; build it with bash tools/build-host-runner.sh")
    goldens = sorted(((json.loads(path.read_text(encoding="utf-8")), path.name)
                      for path in Path(args.directory).glob("*.json")),
                     key=lambda entry: entry[0]["frame"])
    # One PC run per test run on the console: consecutive frames of one test.
    # A test queued twice (m2-solid first and last) is rebuilt twice, each
    # from its own frame number and VideoOut handle.
    units = []
    for document, name in goldens:
        if args.test and document["test"] != args.test:
            continue
        last = units[-1][-1][0] if units else None
        if last is not None and last["test"] == document["test"] and \
                last["frame"] + 1 == document["frame"]:
            units[-1].append((document, name))
        else:
            units.append([(document, name)])
    if not units:
        print(f"no golden files to rebuild in {args.directory}")
        return 2
    # The register defaults belong to the console process, so a frame without
    # register tables replays those another frame of its run shows.
    run_defaults = {}
    for document, _ in goldens:
        defaults = register_defaults(document)
        if defaults is not None:
            run_defaults.setdefault(run_of(document), defaults)
    work = Path(args.work)
    download0 = work / "download0"
    download0.mkdir(parents=True, exist_ok=True)
    frames = 0
    failures = 0
    for entries in units:
        test = entries[0][0]["test"]
        label = f"{test}-frame{entries[0][0]['frame']}"
        replay = work / f"{label}.replay"
        queue = work / f"{label}.queue"
        log = work / f"{label}.log"
        defaults = register_defaults(entries[0][0]) or run_defaults.get(run_of(entries[0][0]))
        frames += len(entries)
        if defaults is None:
            klog, pid = run_of(entries[0][0])
            print(f"{label}: no register defaults: no golden file of {klog} pid {pid} "
                  "has a context register table")
            failures += len(entries)
            continue
        replay.write_text(replay_text(entries[0][0], defaults), encoding="utf-8", newline="\n")
        # With --compile the runner compiles the test's shaders from SPIR-V
        # first, so the frames must come out identical from compiled packages.
        mode = "compile\n" if args.compile else ""
        queue.write_text(f"capture\n{mode}{test}\n", encoding="utf-8", newline="\n")
        started = time.monotonic()
        with log.open("w", encoding="utf-8", newline="\n") as output:
            result = subprocess.run(
                [str(runner), "--replay", str(replay), "--app0", str(ROOT),
                 "--download0", str(download0), "--queue", str(queue)],
                stdout=output, stderr=subprocess.STDOUT, timeout=args.timeout, check=False)
        elapsed = time.monotonic() - started
        runs = read_runs(log)
        run = runs[-1] if runs else None
        streams = [stream for stream in extract_streams(run) if is_captured(stream)] if run else []
        if result.returncode != 0 or run is None or not run["ended"] or len(streams) != len(entries):
            print(f"{label}: runner exit {result.returncode}, {len(streams)} of {len(entries)} "
                  f"frames captured; see {log}")
            failures += len(entries)
            continue
        print(f"{label}: {len(streams)} frame(s) rebuilt in {elapsed:.1f} s")
        for (golden, name), stream in zip(entries, streams):
            problems = capture_problems(stream)
            if not problems:
                rebuilt = golden_document(run, stream, log.name, golden["sequence"], golden["frame"],
                                          flips_before_of(golden))
                problems = compare_documents(golden, rebuilt)
            if problems:
                failures += 1
                print(f"  {name}: DIFFERENT")
                for problem in problems:
                    print(f"    {problem}")
            else:
                print(f"  {name}: identical ({len(golden['command']['words'])} words, "
                      f"{len(golden['calls'])} helper calls, "
                      f"{len(golden['workspace']['chunks'])} workspace chunks)")
    print(f"{frames - failures} of {frames} golden frames rebuilt identically on the PC")
    return 1 if failures else 0


def write_replay(args):
    document = json.loads(Path(args.golden).read_text(encoding="utf-8"))
    defaults = register_defaults(document)
    if defaults is None:
        # A capture with no context table of its own: a test that drove the
        # driver, which builds its tables rather than the runner.
        defaults, sibling = sibling_defaults(document, args.golden)
        if defaults is not None:
            print(f"{args.golden}: register defaults from {sibling}, the same run")
    if defaults is None:
        print(f"{args.golden}: no context register table to take register defaults from")
        return 1
    text = (driver_replay_text(document, defaults, args.test)
            if document.get("kind") == "driver-run"
            else replay_text(document, defaults))
    Path(args.output).write_text(text, encoding="utf-8", newline="\n")
    return 0


# Register-table loads, and the completion marker's RELEASE_MEM.
TABLE_LOADS = {0x9F: "context", 0x64: "uconfig", 0x63: "SH"}
RELEASE_MEM = 0x49
# SET_SH_REG: a direct write of one SH register range, which is how
# sceAgcCbSetShRegisterRangeDirect encodes the user data a draw programs.
SET_SH_REG = 0x76
COMPLETION_MARKER = 0x0030C528


def stream_packets(words):
    """(start, words) of each packet of a stream the AGC helpers wrote, all type 3."""
    packets = []
    at = 0
    while at < len(words):
        header = words[at]
        length = ((header >> 16) & 0x3FFF) + 2
        if header >> 30 != 3 or at + length > len(words):
            raise ValueError(f"word {at} ({header:#010x}) does not start a complete type-3 packet")
        packets.append((at, words[at:at + length]))
        at += length
    return packets


def records_of(words):
    """(offset, value) records of a register table given as 32-bit words."""
    return [(words[index] & 0xFFFF, words[index + 1]) for index in range(0, len(words) - 1, 2)]


def golden_table(document, address, count):
    if isinstance(document.get("regions"), list):
        # A driver submission: the tables live in the regions the submission
        # named, and each region's image belongs to that submission. A table
        # can also sit in one of the run's pipeline stages -- the linked
        # context and uniforms live there -- so those are searched too
        # (compare_run hands them in).
        for region in list(document["regions"]) + list(document.get("stages", [])):
            base = int(region["address"], 16)
            if not (base <= address and address + 8 * count <= base + golden_number(region["bytes"])):
                continue
            image = stage_image(region)
            at = address - base
            return records_of([int.from_bytes(image[at + 4 * index:at + 4 * index + 4], "little")
                               for index in range(2 * count)])
        raise ValueError(f"table {address:#x} lies outside the submission's captured regions")
    image = golden_image(document)
    at = address - int(document["regions"]["stage"]["address"], 16)
    if at < 0 or at + 8 * count > len(image):
        raise ValueError(f"table {address:#x} lies outside the golden stage workspace")
    return records_of([int.from_bytes(image[at + 4 * index:at + 4 * index + 4], "little")
                       for index in range(2 * count)])


def dumped_table(submission, address, count):
    text = submission["tables"].get(f"{address:#x}", "")
    if len(text) != 16 * count:
        raise ValueError(f"the dump holds no {count}-record table at {address:#x}")
    return records_of([int(text[8 * index:8 * index + 8], 16) for index in range(2 * count)])


def hex_words(words):
    return " ".join(f"{word:#010x}" for word in words)


def declared_user_data(packets, registers, documented):
    """The packets of a driver stream a console frame has no counterpart for:
    the SET_SH_REG writes of the declared user-data registers, which the
    driver puts before every draw (docs/M5_PHASE_C.md, C1b). Registers is a
    set of AGC byte offsets like 0x8c, which is the first word the packet
    writes."""
    if not registers:
        return packets
    kept = []
    for at, packet in packets:
        if ((packet[0] >> 8) & 0xFF) == SET_SH_REG and len(packet) > 1 and \
                packet[1] in registers:
            documented.append(f"user data at SH {packet[1]:#04x}, written by the driver")
            continue
        kept.append((at, packet))
    return kept


def compare_packets(golden, submission, draws, expected, extra_sh_registers=()):
    """How one recorded submission differs from a golden frame whose draw
    section, the packets before its first RELEASE_MEM, repeats draws times:
    (problems, expected differences found, their register offsets, packets,
    register tables)."""
    try:
        console = stream_packets([int(word, 16) for word in golden["command"]["words"]])
    except ValueError as error:
        return [str(error)], [], set(), 0, 0
    end = next((index for index, (_, packet) in enumerate(console)
                if (packet[0] >> 8) & 0xFF == RELEASE_MEM), len(console))
    console = [(golden, packet) for _, packet in console[:end] * draws + console[end:]]
    return compare_stream(console, submission, expected, extra_sh_registers)


def compare_stream(console, submission, expected, extra_sh_registers=()):
    """How one recorded submission differs from console packets, each given
    with the golden document holding its register tables: (problems, expected
    differences found, their register offsets, packets, register tables).
    expected maps register offsets to the values the driver programs instead
    of the console's, and extra_sh_registers the user-data registers whose
    writes the driver adds and a console frame may not have."""
    problems = []
    documented = []
    seen = set()
    tables = 0
    driver = []
    try:
        driver = stream_packets([int(word, 16) for word in submission["words"]])
        driver = declared_user_data(driver, extra_sh_registers, documented)
        if len(console) != len(driver):
            problems.append(f"{len(driver)} packets (console {len(console)})")
        for (golden, recorded), (at, built) in zip(console, driver):
            where = f"packet at word {at}"
            opcode = (recorded[0] >> 8) & 0xFF
            if built[0] != recorded[0]:
                problems.append(f"{where}: header {built[0]:#010x} (console {recorded[0]:#010x})")
            elif opcode in TABLE_LOADS:
                # Address low, address high, 0x80000000 and the record count:
                # the table may lie elsewhere, in the same address window.
                if built[2:] != recorded[2:]:
                    problems.append(f"{where}: {TABLE_LOADS[opcode]} table load {hex_words(built)} "
                                    f"(console {hex_words(recorded)})")
                    continue
                count = recorded[4]
                pairs = zip(golden_table(golden, recorded[1] | recorded[2] << 32, count),
                            dumped_table(submission, built[1] | built[2] << 32, count))
                for index, (console_record, driver_record) in enumerate(pairs):
                    if driver_record == console_record:
                        continue
                    offset, value = driver_record
                    if offset == console_record[0] and expected.get(offset) == value:
                        documented.append(f"register {offset:#05x} = {value:#010x} "
                                          f"(console {console_record[1]:#010x})")
                        seen.add(offset)
                    else:
                        problems.append(
                            f"{where}: {TABLE_LOADS[opcode]} record {index}: register "
                            f"{offset:#05x} = {value:#010x} (console {console_record[0]:#05x} = "
                            f"{console_record[1]:#010x})")
                tables += 1
            elif opcode == RELEASE_MEM and len(recorded) > 1 and recorded[1] == COMPLETION_MARKER:
                # The marker's address low word and value belong to the submission.
                if len(built) != len(recorded) or \
                        [word for index, word in enumerate(built) if index not in (3, 5)] != \
                        [word for index, word in enumerate(recorded) if index not in (3, 5)]:
                    problems.append(f"{where}: completion marker {hex_words(built)} "
                                    f"(console {hex_words(recorded)})")
            elif built != recorded:
                problems.append(f"{where}: {hex_words(built)} (console {hex_words(recorded)})")
    except ValueError as error:
        problems.append(str(error))
    return problems, documented, seen, len(driver), tables


def compare_run(args):
    """Each command stream the console's driver queued for a test, against the
    PC's recording of the same test.

    Both sides are the same code path, so the streams must be identical word
    for word; only the completion marker's address and value differ, and each
    belongs to its own process. This is the comparison a driver-path test gets
    instead of a runner-built proxy: the proxy cannot pin two pipelines
    (docs/M5_PHASE_C.md, the region-size finding).
    """
    run = json.loads(Path(args.run).read_text(encoding="utf-8"))
    directory = Path(args.run).parent
    lines = [line for line in Path(args.dump).read_text(encoding="utf-8").splitlines()
             if line.strip()]
    wanted = [entry for entry in run["submissions"]
              if args.test is None or entry["test"] == args.test]
    if not wanted:
        print(f"{args.run}: no submission of {args.test or 'any test'} in this run")
        return 2
    if len(lines) != len(wanted):
        print(f"{args.dump}: {len(lines)} submissions recorded, expected {len(wanted)}")
        return 1
    failures = 0
    for entry, line in zip(wanted, lines):
        document = json.loads((directory / entry["file"]).read_text(encoding="utf-8"))
        # The run's pipelines hold the linked context and uniforms a
        # submission's tables load, so the comparison reads them from there.
        document = dict(document, stages=run.get("stages", []))
        words = [int(word, 16) for word in document["words"]]
        console = [(document, packet) for _, packet in stream_packets(words)]
        problems, _, _, packets, tables = compare_stream(console, json.loads(line), {})
        label = f"{Path(args.dump).name} {entry['test']} {entry['label']}"
        if problems:
            failures += 1
            print(f"{label}: DIFFERENT from {entry['file']}")
            for problem in problems:
                print(f"  {problem}")
        else:
            print(f"{label}: identical to {entry['file']}: {packets} packets, "
                  f"{tables} register tables")
    return 1 if failures else 0


def compare_submission(args):
    golden = json.loads(Path(args.golden).read_text(encoding="utf-8"))
    lines = [line for line in Path(args.dump).read_text(encoding="utf-8").splitlines()
             if line.strip()]
    draws = [int(count) for count in args.draws.split(",")]
    if len(lines) != len(draws):
        print(f"{args.dump}: {len(lines)} submissions recorded, expected {len(draws)}")
        return 1
    expected = {}
    for text in args.expect_record or []:
        offset, _, value = text.partition("=")
        expected[int(offset, 16)] = int(value, 16)
    extra_sh_registers = {int(offset, 16) for offset in args.extra_sh_register or []}

    name = Path(args.golden).name
    failures = 0
    seen = set()
    for index, (line, count) in enumerate(zip(lines, draws)):
        problems, documented, found, packets, tables = compare_packets(
            golden, json.loads(line), count, expected, extra_sh_registers)
        seen |= found
        label = f"{Path(args.dump).name} submission {index + 1} ({count} draw{'s' if count != 1 else ''})"
        if problems:
            failures += 1
            print(f"{label}: DIFFERENT from {name}")
            for problem in problems:
                print(f"  {problem}")
        else:
            differences = sorted(set(documented))
            print(f"{label}: identical to {name}: {packets} packets, {tables} register tables" +
                  (f"; expected differences: {'; '.join(differences)}" if differences else ""))
    for offset in sorted(set(expected) - seen):
        failures += 1
        print(f"expected difference not found: register {offset:#05x} = {expected[offset]:#010x}")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("extract", help="write golden files from a capture klog")
    command.add_argument("log")
    command.add_argument("directory")
    command.add_argument("--run", type=int,
                         help="which captured run of the klog (1-based; default the last)")
    command.add_argument("--test", action="append",
                         help="extract only this runner test's frames (repeatable), e.g. to "
                              "leave out a test whose capture is incomplete by design")
    command.set_defaults(handler=extract)
    command = commands.add_parser("check-helpers",
                                  help="replay recorded helper calls through the PC models")
    command.add_argument("directory")
    command.add_argument("--library", default=str(MODEL_LIBRARY),
                         help="model library (default build/host/libagc_host.so)")
    command.set_defaults(handler=check_helpers)
    command = commands.add_parser("replay", help="write the host replay of one golden file")
    command.add_argument("golden")
    command.add_argument("output")
    command.add_argument("--test", help="for a driver run: the test whose pipelines to place, "
                                        "e.g. c1-triangle")
    command.set_defaults(handler=write_replay)
    command = commands.add_parser("compare-run",
                                  help="compare a driver test's submissions with the run the "
                                       "console's driver recorded")
    command.add_argument("run", help="the run document (run-1.json) of a driver capture")
    command.add_argument("dump")
    command.add_argument("--test", help="the runner test whose submissions to compare, e.g. "
                                        "c1-triangle")
    command.set_defaults(handler=compare_run)
    command = commands.add_parser("compare-submission",
                                  help="compare a driver test's submission with a golden frame")
    command.add_argument("golden")
    command.add_argument("dump")
    command.add_argument("--draws", default="1", metavar="COUNTS",
                         help="comma-separated draws of each recorded submission, whose draw "
                              "section is the golden frame's repeated (default 1)")
    command.add_argument("--expect-record", action="append", metavar="OFFSET=VALUE",
                         help="a register-table record the driver writes differently by design "
                              "(repeatable)")
    command.add_argument("--extra-sh-register", action="append", metavar="OFFSET",
                         help="an SH register's user-data write the driver makes before every "
                              "draw and the golden frame has none of (repeatable)")
    command.set_defaults(handler=compare_submission)
    command = commands.add_parser("rebuild",
                                  help="rebuild the golden frames with the PC runner and compare")
    command.add_argument("directory")
    command.add_argument("--test", help="only this runner test, e.g. m4-rtt")
    command.add_argument("--compile", action="store_true",
                         help="compile each test's shaders from SPIR-V in the runner first "
                              "(queue keyword compile)")
    command.add_argument("--runner", default=str(RUNNER),
                         help="PC runner (default build/host/runner_host)")
    command.add_argument("--work", default=str(REBUILD_DIRECTORY),
                         help="replays, queues and logs (default build/host/rebuild)")
    command.add_argument("--timeout", type=float, default=600.0,
                         help="seconds per test (default 600)")
    command.set_defaults(handler=rebuild)
    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
