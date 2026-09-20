# PS5 Vulkan compatibility probe - [PS5VK] klog record parsing.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Parse the [PS5VK] JSON lines the probe titles write to klog into runs and
command streams.

Shared by tools/ps5_console.py, tools/pm4_decode.py and tools/golden.py.
"""

import json
import re

PREFIX = "[PS5VK] "
# Statuses that need no attention; every other probe status is notable.
QUIET_STATUSES = {"PASS", "INFO", "ARMED", "NOT_REQUIRED"}
STREAM_PROBES = {"agc_live_command_encoding", "agc_linked_command_encoding"}
ALLOCATION_PROBES = {"agc_live_framebuffer": "framebuffer", "agc_live_offscreen": "offscreen",
                     "agc_live_depth": "depth", "agc_live_source": "source",
                     # V0-query: the driver's query counters, whose addresses the
                     # submission's ZPASS_DONE samples name.
                     "agc_query_pool": "query_pool"}
LOGGED_TABLE = re.compile(r"packet=(\d+) opcode=0x([0-9a-f]+) address=0x([0-9a-f]+) count=(\d+)")
CAPTURE_CALL = re.compile(r"^(\w+) words=(\d+)-(\d+) args=((?:0x[0-9a-f]+,?)*)$")
FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3


def parse_record(line):
    """The JSON record carried by one klog line, or None for any other line."""
    at = line.find(PREFIX)
    if at < 0:
        return None
    try:
        record = json.loads(line[at + len(PREFIX):])
    except json.JSONDecodeError:
        return None
    return record if isinstance(record, dict) else None


class RunTracker:
    """Collects records into runs delimited by run_start and run_end.

    A run is a dict: pid, ended, every record in order, the count of each
    probe status, the runner_test records, the notable probe records and the
    last record seen. echo, when given, receives a line for each test start,
    test result and notable record as they arrive.
    """

    def __init__(self, echo=None):
        self.echo = echo
        self.run = None
        self.completed = []

    def _say(self, text):
        if self.echo:
            self.echo(text)

    def runs(self):
        """Completed runs, then the run still open, if any."""
        return self.completed + ([self.run] if self.run is not None else [])

    def feed(self, line):
        """Consume one klog line; returns the record's event name, if any."""
        record = parse_record(line)
        if record is None:
            return None
        event = record.get("event")
        if event == "run_start":
            if self.run is not None:
                self.completed.append(self.run)
            self.run = {"pid": record.get("pid"), "ended": False, "records": [],
                        "statuses": {}, "notable": [], "tests": [], "last": None}
            self._say(f"run_start pid={record.get('pid')}")
            return event
        if self.run is None:
            return None
        self.run["records"].append(record)
        self.run["last"] = record
        if event == "run_end":
            self.run["ended"] = True
            self.completed.append(self.run)
            self.run = None
            return event
        if event != "probe":
            return event
        status = record.get("status", "?")
        probe = record.get("probe", "?")
        self.run["statuses"][status] = self.run["statuses"].get(status, 0) + 1
        if probe == "runner_test_start":
            self._say(f"  running {record.get('detail', '')}")
        elif probe == "runner_test":
            self.run["tests"].append(record)
            self._say(f"  test {status:<12} {record.get('detail', '')}")
        elif status not in QUIET_STATUSES:
            self.run["notable"].append(record)
            self._say(f"  {status:<12} {probe} ({record.get('result')}) {record.get('detail', '')}")
        return event


def read_runs(path):
    """Every run recorded in a saved klog file."""
    tracker = RunTracker()
    with open(path, encoding="utf-8", errors="replace") as source:
        for line in source:
            tracker.feed(line.rstrip("\r\n"))
    return tracker.runs()


def parse_number(value):
    return int(value, 0) if isinstance(value, str) else value


def _new_stream(test, buffer, allocations):
    return {"test": test, "buffer": buffer, "words": [], "word_count": None, "encoding": None,
            "regions": {name: region_bounds(entry) for name, entry in allocations.items()},
            "workspace": {}, "command_bottom": None, "logged_tables": [],
            "tables_checked": False, "calls": [], "call_count": None, "chunks": {},
            "chunk_count": None, "workspace_fnv1a64": None, "stages": [], "stage_count": None,
            "submissions": [], "video_handle": None}


def region_bounds(entry):
    """A region's (address, bytes). Most allocation records give them directly;
    the driver's swapchain images are two halves of one allocation, so their
    records accumulate a lowest start, a highest start and a length, and the
    region they spell is the allocation both halves lie in (ps5vk_wsi.c)."""
    if len(entry) == 3 and all(value is not None for value in entry):
        low, high, bytes_at = entry
        return (low, high + bytes_at - low)
    return tuple(entry)


def _refresh_regions(stream, allocations):
    """A stream records the allocations it knows when it starts, but a test can
    name one after its first submission -- a presenting test names the image it
    acquired, and its first submission is the draw into it -- so an allocation
    recorded later joins the stream that is open."""
    if stream is not None:
        stream["regions"] = {name: region_bounds(entry)
                             for name, entry in allocations.items()}


def _new_stage(index):
    """One captured pipeline stage: the driver's stage mapping that AGC
    relocated a pipeline's headers into and linked its context and uniforms
    in, as the runner logs it (src/diagnostics.cpp, log_driver_stages)."""
    return {"index": index, "address": None, "bytes": None, "chunks": {},
            "chunk_count": None, "fnv1a64": None}


def _new_submission(label):
    """One command stream a driver queued, as the runner logs it: the words it
    submitted and, for each table address those words name, the image of the
    region that holds it (src/diagnostics.cpp, log_driver_submission)."""
    return {"label": label, "dwords": None, "words": [], "regions": []}


def _new_submission_region(index):
    """One table region as of a submission: a frame rewrites the same chunks,
    so the image belongs to the submission, not the run."""
    return {"index": index, "address": None, "bytes": None, "chunks": {},
            "chunk_count": None, "fnv1a64": None}


def extract_streams(run):
    """The command streams of one run, in order.

    Each stream records its runner test, target buffer, logged words,
    allocations, workspace bounds and logged indirect tables. A stream built
    in capture mode also holds its AGC helper calls and the non-zero chunks of
    its stage workspace image, keyed by byte offset.
    """
    streams = []
    stream = None
    test = None
    allocations = {}
    for record in run["records"]:
        event = record.get("event")
        probe = record.get("probe")
        field = record.get("field")
        if probe == "runner_test_start" and event == "probe":
            test = record.get("detail")
            allocations = {}
            stream = None
        elif probe in ALLOCATION_PROBES and field in ("address", "bytes"):
            entry = allocations.setdefault(ALLOCATION_PROBES[probe], [None, None])
            entry[0 if field == "address" else 1] = parse_number(record.get("value"))
            _refresh_regions(stream, allocations)
        elif probe == "agc_gpu_pointer_swapchain_image" and field in ("begin", "bytes"):
            # A test that presents through the driver logs the storage of each
            # image it acquires; the driver maps both images of a swapchain in
            # one allocation and names each half of it, so the region the
            # replay pins is that allocation (ps5vk_wsi.c, region_bounds).
            entry = allocations.setdefault("framebuffer", [None, None, None])
            if field == "bytes":
                entry[2] = parse_number(record.get("value"))
            else:
                starts_at = parse_number(record.get("value"))
                entry[0] = starts_at if entry[0] is None else min(entry[0], starts_at)
                entry[1] = starts_at if entry[1] is None else max(entry[1], starts_at)
            _refresh_regions(stream, allocations)
        elif probe == "agc_live_frame" and field == "buffer_index":
            stream = _new_stream(test, record.get("value"), allocations)
            streams.append(stream)
        elif probe in STREAM_PROBES:
            if stream is None or (stream["encoding"] is not None and event != "probe"):
                stream = _new_stream(test, None, allocations)
                streams.append(stream)
            if event == "probe":
                stream["encoding"] = record.get("status")
            elif field == "word":
                stream["words"].append(parse_number(record.get("value")))
            elif field == "word_count":
                stream["word_count"] = record.get("value")
        elif stream is None and probe not in ("agc_capture_stage", "agc_capture_submission",
                                              "agc_capture_region", "agc_capture_video"):
            continue
        elif probe == "agc_gpu_pointer_workspace" and field in ("begin", "end"):
            stream["workspace"][field] = parse_number(record.get("value"))
        elif probe == "agc_gpu_pointer_command" and field == "bottom":
            stream["command_bottom"] = parse_number(record.get("value"))
        elif probe == "agc_gpu_pointer_indirect":
            if event == "probe":
                match = LOGGED_TABLE.search(record.get("detail", ""))
                if match:
                    stream["logged_tables"].append((int(match.group(1)), int(match.group(2), 16),
                                                    int(match.group(3), 16), int(match.group(4))))
            elif field == "table_count":
                stream["tables_checked"] = True
        elif probe == "agc_capture_call":
            if event == "probe":
                match = CAPTURE_CALL.match(record.get("detail", ""))
                if match:
                    stream["calls"].append({
                        "name": match.group(1), "begin": int(match.group(2)),
                        "end": int(match.group(3)),
                        "args": [int(arg, 16) for arg in match.group(4).split(",") if arg]})
            elif field == "calls":
                stream["call_count"] = record.get("value")
        elif probe == "agc_capture_workspace":
            if event == "words":
                text = record.get("value", "")
                stream["chunks"][record.get("offset")] = [int(text[at:at + 8], 16)
                                                          for at in range(0, len(text), 8)]
            elif field == "chunks":
                stream["chunk_count"] = record.get("value")
            elif field == "fnv1a64":
                stream["workspace_fnv1a64"] = parse_number(record.get("value"))
        elif probe == "agc_capture_stage":
            # A test that drives the Vulkan driver logs no command stream of
            # its own -- the driver submits, not the runner -- so the pipelines
            # it captured are a stream of their own.
            if stream is None:
                stream = _new_stream(test, None, allocations)
                streams.append(stream)
            if field == "count":
                # A capture may log its stages in more than one call: a probe
                # that draws through one device per frame logs each device's
                # pipelines as it goes (the format probe's seventeen), and the
                # count that matters is the total (capture_problems).
                stream["stage_count"] = (stream["stage_count"] or 0) + record.get("value")
            elif field == "index":
                stream["stages"].append(_new_stage(record.get("value")))
            elif stream["stages"]:
                stage = stream["stages"][-1]
                if event == "words":
                    text = record.get("value", "")
                    stage["chunks"][record.get("offset")] = [int(text[at:at + 8], 16)
                                                             for at in range(0, len(text), 8)]
                elif field == "address" or field == "bytes":
                    stage[field] = parse_number(record.get("value"))
                elif field == "chunks":
                    stage["chunk_count"] = record.get("value")
                elif field == "fnv1a64":
                    stage["fnv1a64"] = parse_number(record.get("value"))
        elif probe == "agc_capture_video":
            if stream is None:
                stream = _new_stream(test, None, allocations)
                streams.append(stream)
            if field == "handle":
                stream["video_handle"] = parse_number(record.get("value"))
        elif probe == "agc_capture_submission":
            if stream is None:
                stream = _new_stream(test, None, allocations)
                streams.append(stream)
            if event == "probe":
                stream["submissions"].append(_new_submission(record.get("detail")))
            elif stream["submissions"] and field == "dwords":
                stream["submissions"][-1]["dwords"] = record.get("value")
            elif stream["submissions"] and field == "word":
                stream["submissions"][-1]["words"].append(parse_number(record.get("value")))
        elif probe == "agc_capture_region":
            # A submission's table regions, read in order after it: a frame
            # rewrites the same chunks, so each submission carries its own.
            if stream is None or not stream["submissions"]:
                continue
            submission = stream["submissions"][-1]
            if field == "index":
                submission["regions"].append(_new_submission_region(record.get("value")))
            elif submission["regions"]:
                region = submission["regions"][-1]
                if event == "words":
                    text = record.get("value", "")
                    region["chunks"][record.get("offset")] = [int(text[at:at + 8], 16)
                                                              for at in range(0, len(text), 8)]
                elif field == "address" or field == "bytes":
                    region[field] = parse_number(record.get("value"))
                elif field == "chunks":
                    region["chunk_count"] = record.get("value")
                elif field == "fnv1a64":
                    region["fnv1a64"] = parse_number(record.get("value"))
    return streams


def fnv1a64(data):
    value = FNV64_OFFSET
    for byte in data:
        value = ((value ^ byte) * FNV64_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def workspace_image(stream):
    """The captured stage workspace as bytes, zero wherever no chunk was
    logged, or None when the stream has no capture."""
    begin = stream["workspace"].get("begin")
    end = stream["workspace"].get("end")
    if begin is None or end is None or not stream["chunks"]:
        return None
    image = bytearray(end - begin)
    for offset, words in stream["chunks"].items():
        for index, word in enumerate(words):
            at = offset + 4 * index
            if 0 <= at and at + 4 <= len(image):
                image[at:at + 4] = word.to_bytes(4, "little")
    return bytes(image)


def stage_image(stage):
    """One captured pipeline stage as bytes, zero wherever no chunk was
    logged, or None when the stage carries no image."""
    if not stage["chunks"] or not stage["bytes"]:
        return None
    image = bytearray(stage["bytes"])
    for offset, words in stage["chunks"].items():
        for index, word in enumerate(words):
            at = offset + 4 * index
            if 0 <= at and at + 4 <= len(image):
                image[at:at + 4] = word.to_bytes(4, "little")
    return bytes(image)


def is_captured(stream):
    return bool(stream["calls"] or stream["chunks"] or stream["workspace_fnv1a64"] is not None or
                stream.get("stages"))


def capture_problems(stream):
    """Why a captured stream cannot serve as a golden file; empty when it can.

    The workspace image must match its logged FNV-1a 64 and the logged stream
    words, every chunk and helper call must be present, and the helper calls
    must cover the stream's words contiguously from word 0 to its end. Every
    captured pipeline stage must be present and match its own FNV-1a 64: a
    stage the capture lost is a pipeline a PC rebuild cannot replay. A stream
    with no command words, no workspace and no helper calls is a test that
    drove the Vulkan driver -- the driver submits, not the runner -- and its
    pipelines are the whole capture, so only they are checked.
    """
    problems = []
    stages = stream.get("stages", [])
    if stream.get("stage_count") is not None and stream["stage_count"] != len(stages):
        problems.append(f"{len(stages)} of {stream['stage_count']} pipeline stages logged")
    for stage in stages:
        index = stage.get("index")
        if stage["address"] is None or stage["bytes"] is None:
            problems.append(f"pipeline stage {index}: no address or size logged")
            continue
        if stage["chunk_count"] != len(stage["chunks"]):
            problems.append(f"pipeline stage {index}: {len(stage['chunks'])} of "
                            f"{stage['chunk_count']} chunks logged")
        image = stage_image(stage)
        if image is None or fnv1a64(image) != stage["fnv1a64"]:
            problems.append(f"pipeline stage {index}: image does not match its logged FNV-1a 64")
    for submission in stream.get("submissions", []):
        label = submission.get("label")
        if submission["dwords"] is None or submission["dwords"] != len(submission["words"]):
            problems.append(f"submission {label}: {len(submission['words'])} of "
                            f"{submission['dwords']} words logged")
        for region in submission["regions"]:
            # A region with no logged image is an address and a size only: a
            # driver's own buffer, which a capture pins so a replay can hand the
            # same address to a PC rebuild's buffer, but whose contents are the
            # application's data and are not logged (src/diagnostics.cpp,
            # log_driver_regions).
            if region["chunk_count"] is None and region["fnv1a64"] is None:
                continue
            if region["chunk_count"] != len(region["chunks"]):
                problems.append(f"submission {label}: region {region['index']} holds "
                                f"{len(region['chunks'])} of {region['chunk_count']} chunks")
            image = stage_image(region)
            if image is None or fnv1a64(image) != region["fnv1a64"]:
                problems.append(f"submission {label}: region {region['index']} does not match its "
                                "logged FNV-1a 64")
    if (stream["word_count"] is None and not stream["chunks"] and
            stream["workspace_fnv1a64"] is None and not stream["calls"]):
        return problems

    count = stream["word_count"]
    if count is None or len(stream["words"]) != count:
        problems.append(f"{len(stream['words'])} of {count} stream words logged")
    image = workspace_image(stream)
    if image is None:
        problems.append("no workspace image")
    else:
        if stream["chunk_count"] != len(stream["chunks"]):
            problems.append(f"{len(stream['chunks'])} of {stream['chunk_count']} workspace chunks logged")
        if fnv1a64(image) != stream["workspace_fnv1a64"]:
            problems.append("workspace image does not match its logged FNV-1a 64")
        bottom = stream["command_bottom"]
        if bottom is None:
            problems.append("command stream address not logged")
        else:
            at = bottom - stream["workspace"]["begin"]
            words = [int.from_bytes(image[at + 4 * index:at + 4 * index + 4], "little")
                     for index in range(len(stream["words"]))]
            if at < 0 or words != stream["words"]:
                problems.append("logged stream words differ from the workspace image")
    calls = stream["calls"]
    if stream["call_count"] != len(calls):
        problems.append(f"{len(calls)} of {stream['call_count']} helper calls logged")
    position = 0
    for call in calls:
        if call["begin"] != position or call["end"] < call["begin"]:
            problems.append(f"helper call {call['name']} spans words {call['begin']}-{call['end']}, "
                            f"not from word {position}")
            break
        position = call["end"]
    else:
        if count is not None and position != count:
            problems.append(f"helper calls cover words 0-{position} of {count}")
    return problems
