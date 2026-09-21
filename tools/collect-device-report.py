#!/usr/bin/env python3
# PS5 Vulkan - the device's reported capability set, as a committed inventory.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Phase E1 (docs/CTS.md): capture what the device reports, and check it is whole.

The CTS is judged only against the capability set the device itself reports, so
the first artefact of the campaign is that set in a machine-readable form, from
the same build the runs use. The runner's `device-report` case walks the
reporting surface with public Vulkan calls and writes one JSON record per value
(src/diagnostics.cpp); this tool turns those records into
`conformance_inventory/device_report.json` and refuses an inventory that is
missing a member of VkPhysicalDeviceLimits or VkPhysicalDeviceFeatures, read
from the Vulkan headers the build uses. A member nobody emitted would otherwise
read as "the device does not report it", which is exactly the kind of hole the
selection must not have.

Two sources, one shape:

  tools/collect-device-report.py                 run the host runner and collect
  tools/collect-device-report.py --log FILE      collect from a saved log, e.g.
                                                 the console's Klog_Logs/*.log

The console source is the authoritative one for a run's selection; the host run
is what makes the tool part of the host gates.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LOG = ROOT / "build" / "driver" / "runner-cases" / "device-report.log"
RUNNER = ROOT / "build" / "host" / "runner_host_driver"
GOLDEN = ROOT / "golden" / "c7-mip-tiled" / "run-1.json"
OUT = ROOT / "conformance_inventory" / "device_report.json"

HEADER_GLOBS = [
    ".deps/native/opengl-sdk/third_party/Vulkan-Headers/include/vulkan/vulkan_core.h",
    ".deps/native/ps5-payload-sdk/**/vulkan/vulkan_core.h",
]

# The record types the case writes, and the section each lands in.
SECTIONS = {
    "device_report_limit": "limits",
    "device_report_feature": "features",
    "device_report_queue": "queue_families",
    "device_report_memory": "memory",
    "device_report_extension_version": "device_extension_versions",
}


def header_members(struct):
    """The member names of a struct, from the Vulkan header the build uses."""
    for pattern in HEADER_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            text = path.read_text()
            match = re.search(r"typedef struct " + struct + r" \{(.*?)\} " + struct + r";",
                              text, re.S)
            if not match:
                continue
            names = []
            for line in match.group(1).splitlines():
                line = line.split("//")[0].strip()
                member = re.match(
                    r"^(?:VkBool32|float|uint32_t|uint64_t|VkDeviceSize|VkSampleCountFlags)\s+"
                    r"(\w+)(?::\d+)?;", line)
                if member:
                    names.append(member.group(1))
            return names
    raise SystemExit("no vulkan_core.h found under .deps; run make deps")


def header_formats():
    """Every format the 1.0 core enumeration names, by value."""
    for pattern in HEADER_GLOBS:
        for path in sorted(ROOT.glob(pattern)):
            text = path.read_text()
            start = text.find("typedef enum VkFormat {")
            end = text.find("} VkFormat;", start)
            if start < 0 or end < 0:
                continue
            by_value = {}
            for name, value in re.findall(r"(VK_FORMAT_[A-Za-z0-9_]+)\s*=\s*(\d+)\s*,", text[start:end]):
                number = int(value)
                if 1 <= number <= 184:
                    by_value.setdefault(number, name)
            return by_value
    raise SystemExit("no vulkan_core.h VkFormat enum found under .deps; run make deps")


def run_host_runner():
    """The runner's `device-report` case through the driver, on this host."""
    if not (ROOT / "build/driver/host/libps5vk.a").exists():
        subprocess.run(["bash", str(ROOT / "tools/build-driver.sh")], check=True)
    subprocess.run(["bash", str(ROOT / "tools/build-host-runner.sh"), "--driver"], check=True)
    work = LOG.parent
    work.mkdir(parents=True, exist_ok=True)
    queue = work / "device-report.queue"
    queue.write_text("capture\ndevice-report\n")
    replay = work / "device-report.replay"
    subprocess.run(["python3", str(ROOT / "tools/golden.py"), "replay", str(GOLDEN), str(replay)],
                   check=True)
    with LOG.open("w") as log:
        subprocess.run([str(RUNNER), "--replay", str(replay), "--memory", "free", "--cases",
                        "driver", "--app0", str(ROOT), "--download0", str(work),
                        "--queue", str(queue)], stdout=log, stderr=subprocess.STDOUT, check=False)
    return LOG


def collect(path):
    records = []
    text = path.read_text(errors="replace")
    for line in text.splitlines():
        at = line.find('{"schema"')
        if at < 0:
            continue
        try:
            record = json.loads(line[at:])
        except json.JSONDecodeError:
            continue
        if record.get("probe", "").startswith("device_report"):
            records.append(record)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", help="collect from a saved runner log instead of running the host")
    parser.add_argument("--out", help=f"where to write the inventory (default {OUT.name})")
    args = parser.parse_args()

    path = pathlib.Path(args.log) if args.log else run_host_runner()
    records = collect(path)
    if not records:
        raise SystemExit(f"{path}: no device_report records; did the case run?")

    inventory = {
        "source": str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path),
        "collected_by": "tools/collect-device-report.py",
        "properties": {},
        "limits": {},
        "features": {},
        "queue_families": {},
        "memory": {},
        "formats": {},
        "image_format_combos": [],
        "image_format_properties": {},
        "instance_extensions": [],
        "device_extensions": [],
        "device_extension_versions": {},
        "detail": {},
    }
    for record in records:
        probe = record["probe"]
        # Only the value-carrying records feed the inventory; the case's own
        # PASS/FAIL event is recorded separately.
        if record.get("event") not in ("value", "hex", "text"):
            if probe == "device_report":
                inventory["detail"]["result"] = record.get("status", "")
                inventory["detail"]["detail"] = record.get("detail", "")
            continue
        if probe == "device_report" and record.get("event") == "text":
            if record["field"] == "device_name":
                inventory["properties"]["device_name"] = record["value"]
            continue
        if probe == "device_report_format_optimal":
            inventory["formats"].setdefault(record["field"], {})["optimal"] = record["value"]
            continue
        if probe == "device_report_format_linear":
            inventory["formats"].setdefault(record["field"], {})["linear"] = record["value"]
            continue
        if probe == "device_report_format_buffer":
            inventory["formats"].setdefault(record["field"], {})["buffer"] = record["value"]
            continue
        if probe == "device_report_image_format":
            inventory["image_format_properties"][record["field"]] = record["value"]
            continue
        if probe == "device_report_image_combo":
            inventory["image_format_combos"].append(record["value"])
            continue
        if probe == "device_report_extension":
            inventory["instance_extensions" if record["field"] == "instance"
                      else "device_extensions"].append(record["value"])
            continue
        if probe == "device_report":
            inventory["properties"][record["field"]] = record["value"]
            continue
        if probe in SECTIONS:
            inventory[SECTIONS[probe]][record["field"]] = record["value"]
            continue
        inventory["detail"].setdefault(probe, {})[record.get("field", "")] = record.get("value")

    missing = []
    for struct, section in (("VkPhysicalDeviceLimits", "limits"),
                            ("VkPhysicalDeviceFeatures", "features")):
        for member in header_members(struct):
            if member not in inventory[section]:
                missing.append(f"{section}.{member}")
    for value, name in header_formats().items():
        if name not in inventory["formats"]:
            missing.append(f"formats.{name} (VkFormat {value})")
    if missing:
        print(f"{len(missing)} member(s) of the reported structures are not in the inventory:",
              file=sys.stderr)
        for name in missing[:20]:
            print(f"  {name}", file=sys.stderr)
        return 1

    out = pathlib.Path(args.out) if args.out else OUT
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(inventory, indent=2, sort_keys=True) + "\n")
    nonzero = sum(1 for value in inventory["features"].values() if value)
    print(f"== the device's reporting, from {inventory['source']}")
    print(f"   api_version       {inventory['properties'].get('api_version')}")
    print(f"   device            {inventory['properties'].get('device_name', '')!r} "
          f"(type {inventory['properties'].get('device_type')})")
    print(f"   limits            {len(inventory['limits'])} members, all reported")
    print(f"   features          {len(inventory['features'])} members, {nonzero} true")
    print(f"   queue families    {inventory['properties'].get('queue_family_count')}")
    print(f"   memory            {inventory['properties'].get('memory_type_count')} types, "
          f"{inventory['properties'].get('memory_heap_count')} heaps")
    print(f"   extensions        {len(inventory['instance_extensions'])} instance, "
          f"{len(inventory['device_extensions'])} device")
    supported = sum(1 for entry in inventory["formats"].values()
                    if entry.get("optimal") not in (None, "0x0") or
                    entry.get("linear") not in (None, "0x0") or
                    entry.get("buffer") not in (None, "0x0"))
    print(f"   formats          {len(inventory['formats'])} of {len(header_formats())} probed, "
          f"{supported} with any feature")
    print(f"   image combos      {len(inventory['image_format_properties'])} supported of "
          f"{len(inventory['image_format_combos'])} probed per format")
    print(f"   written           {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
