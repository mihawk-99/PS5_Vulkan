#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - audit the Vulkan 1.0 command surface.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""PS5 Vulkan compatibility probe - audit the Vulkan 1.0 command surface.

Phase B2 (docs/M5_REFERENCE.md, "1.0: make the current claim true"). A device
that reports Vulkan 1.0 must answer every 1.0 command: either the driver has an
implementation, or the runtime's own shared entry points do (Mesa supplies a
great deal of the API, `vk_common_*`), or the driver refuses the command **by
name** with the phase that would implement it. Three sweeps over the core
commands -- one call per fork, then every crash re-checked with the handles a
valid call needs -- are what found the commands that were none of the three
(docs/M5_PHASE_B.md, 2026-09-18). Those sweeps were manual; this script is the
audit that keeps the answer complete.

For every command the `VK_VERSION_1_0` feature names, it looks for:

  driver    driver/*.c defines `ps5vk_<name>` (or the promoted spelling the
            runtime forwards to, `...2`, `...KHR` or `...2KHR`)
  refused   driver/ps5vk_refusals.c defines it, which ends the recording with
            VK_ERROR_UNKNOWN and a message naming the phase
  runtime   Mesa's runtime defines `vk_common_<name>`, so the shared dispatch
            table answers the call even though the driver does not
  gap       none of the three: a command whose call reaches an empty slot

Usage: python3 tools/command_audit.py [--check]
  --check  exit 1 while any 1.0 command is a gap, for a gate
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REGISTRY = sorted(ROOT.glob(".deps/native/mesa/*/src/vulkan/registry/vk.xml"))
RUNTIME = ROOT / ".deps/native/vulkan-runtime/lib/libvk_runtime.a"
DRIVER_SOURCES = sorted((ROOT / "driver").glob("*.c"))
REFUSALS = ROOT / "driver/ps5vk_refusals.c"
FEATURE = "VK_VERSION_1_0"


def blocks_of(text, tag):
    """The registry's <tag name=...> blocks: name, what each depends on, and its
    body. Parsed by tag boundaries rather than by pairing an opening tag with the
    next closing one, because the registry declares a few extensions without a
    body, and those pair with the following extension's end."""
    blocks = {}
    for match in re.finditer(rf"<{tag}\b([^>]*)>", text):
        attributes = match.group(1)
        name = re.search(r'name="([^"]+)"', attributes)
        if name is None:
            continue
        depends = re.search(r'depends="([^"]*)"', attributes)
        if attributes.rstrip().endswith("/"):
            body = ""
        else:
            end = text.find(f"</{tag}>", match.end())
            body = text[match.end() : end] if end != -1 else ""
        blocks[name.group(1)] = (
            (depends.group(1) if depends else "").replace(",", " ").split(),
            body,
        )
    return blocks


def required_commands(body, satisfied):
    """The commands a body requires, and the ones it requires only when a
    dependency is met: a <require depends="VK_VERSION_1_1"> block's commands are
    conditional for a device that reports 1.0 and exposes no device groups
    (VK_KHR_swapchain's three presentation ones are), so they are not gaps."""
    commands, conditional = [], []
    for match in re.finditer(r"<require([^>]*)>(.*?)</require>", body, re.S):
        attributes, inner = match.group(1), match.group(2)
        needed = [
            dependency
            for group in re.findall(r'depends="([^"]*)"', attributes)
            for dependency in group.replace(",", " ").split()
        ]
        names = re.findall(r'<command name="([^"]+)"', inner)
        (commands if all(dependency in satisfied for dependency in needed) else conditional).extend(
            names
        )
    return commands, conditional


def commands_of(text, name, satisfied):
    """The commands a feature or extension requires, its dependencies included.

    The registry splits the core feature: VK_VERSION_1_0 depends on
    VK_GRAPHICS_VERSION_1_0, and the commands are in the feature that declares
    them, so the chain is followed."""
    blocks = blocks_of(text, "feature")
    if name not in blocks:
        raise SystemExit(f"{name} is not in the registry")
    names, conditional = [], []
    pending = [name]
    seen = set()
    while pending:
        current = pending.pop()
        if current in seen or current not in blocks:
            continue
        seen.add(current)
        depends, body = blocks[current]
        pending.extend(depends)
        required, only_if = required_commands(body, satisfied)
        names.extend(required)
        conditional.extend(only_if)
    # A command can be listed by more than one dependency; the surface is the
    # distinct set.
    return sorted(set(names)), sorted(set(conditional))


def symbol_of(name):
    """The driver's and the runtime's spelling of a command: the `vk` prefix is
    what the entry point's `ps5vk_`/`vk_common_` replaces, so `vkCmdDraw` is
    `ps5vk_CmdDraw` and `vk_common_CmdDraw`."""
    return name[2:] if name.startswith("vk") else name


def definition(path, name):
    """The body of the driver's entry point for a command, or None. The body
    reaches to the closing brace in the first column."""
    match = re.search(
        rf"^ps5vk_{re.escape(symbol_of(name))}\([^)]*\)\s*\n?\s*\{{(.*?)^\}}",
        path.read_text(encoding="utf-8"),
        re.M | re.S,
    )
    return match.group(1) if match else None


def runtime_symbols():
    """Every symbol the runtime archive defines: `vk_common_*` among them."""
    symbols = set()
    for archive in sorted(RUNTIME.parent.glob("libvk_runtime*.a")):
        result = subprocess.run(
            ["nm", "--defined-only", str(archive)], capture_output=True, text=True, check=True
        )
        for line in result.stdout.splitlines():
            parts = line.split()
            if parts:
                symbols.add(parts[-1])
    return symbols


def exposed_extensions():
    """The extension names the driver's own tables turn on, as the registry
    spells them: ps5vk_instance_extensions' `.KHR_surface = true` is
    VK_KHR_surface."""
    names = set()
    for source in (ROOT / "driver/ps5vk_instance.c", ROOT / "driver/ps5vk_physical_device.c"):
        text = source.read_text(encoding="utf-8")
        for table in re.finditer(
            r"ps5vk_(?:instance|device)_extensions = \{(.*?)\n\};", text, re.S
        ):
            for field in re.finditer(r"\.([A-Za-z0-9_]+) = true", table.group(1)):
                names.add("VK_" + field.group(1))
    return sorted(names)


def extension_commands(text, name, satisfied):
    """The commands an extension requires, its dependencies included."""
    blocks = blocks_of(text, "extension")
    if name not in blocks:
        raise SystemExit(f"{name} is not in the registry")
    names, conditional = [], []
    pending = [name]
    seen = set()
    while pending:
        current = pending.pop()
        if current in seen or current not in blocks:
            continue
        seen.add(current)
        depends, body = blocks[current]
        pending.extend(depends)
        required, only_if = required_commands(body, satisfied)
        names.extend(required)
        conditional.extend(only_if)
    return names, conditional


def classify(name, symbols):
    spellings = [name]
    for suffix in ("KHR", "2", "2KHR"):
        spellings.append(name + suffix)
    # An extension's command is often the KHR spelling of a core one the driver
    # implements under its core name (vkGetPhysicalDeviceFeatures2KHR against
    # vkGetPhysicalDeviceFeatures2), so the stripped name counts too.
    if name.endswith("KHR"):
        spellings.append(name[: -len("KHR")])
    bodies = [
        body
        for source in DRIVER_SOURCES
        for spelling in spellings
        if (body := definition(source, spelling)) is not None
    ]
    if bodies:
        for source in DRIVER_SOURCES:
            for spelling in spellings:
                body = definition(source, spelling)
                if body is not None and source == REFUSALS and ("refuse" in body or "vk_errorf" in body):
                    return "refused"
        return "driver"
    if any(f"vk_common_{symbol_of(spelling)}" in symbols for spelling in spellings):
        return "runtime"
    return "gap"


# What a command list cannot show, one line each, with the runner case that
# covers it: the request that found R1, R2 and R3 asked the audits to be as
# honest about what they cannot see as the runtime is about what it will not do
# (PS5_VULKAN_REQUESTS.md, R4).
BLIND_SPOTS = (
    ("pipeline state: cullMode, rasterizerDiscardEnable, polygon offset",
     "v0-cull (jobs/v0-cull/queue.txt)"),
    ("sampler state: address modes, filters, LOD bias, anisotropy",
     "v0-sampler-address (jobs/v0-sampler-address/queue.txt)"),
    ("the pixels a clear wrote, before anything draws over them",
     "v0-stencil-clear (jobs/v0-stencil-clear/queue.txt)"),
    ("how a submission splits into steps, and what a step's capture holds",
     "v0-two-passes (jobs/v0-two-passes/queue.txt)"),
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="exit 1 on any gap")
    args = parser.parse_args()

    if not REGISTRY:
        raise SystemExit("the Mesa registry is not fetched; run tools/fetch-mesa.sh")
    if not DRIVER_SOURCES or not REFUSALS.is_file():
        raise SystemExit("the driver sources are missing")
    registry = REGISTRY[0].read_text(encoding="utf-8")
    symbols = runtime_symbols()
    exposed = exposed_extensions()
    # What the device reports: 1.0 and the extensions it exposes.
    satisfied = {"VK_VERSION_1_0"} | set(exposed)
    names, core_conditional = commands_of(registry, FEATURE, satisfied)

    classified = {"driver": [], "refused": [], "runtime": [], "gap": []}
    for name in names:
        # A definition in ps5vk_refusals.c that refuses (or reports) is a
        # refusal; the file also holds the no-op destructors of objects the
        # driver does implement, and those are implementations.
        classified[classify(name, symbols)].append(name)

    print(f"{len(names)} commands are required by {FEATURE}")
    if core_conditional:
        print(
            f"  ({len(core_conditional)} more are conditional on a version or an "
            f"extension this device does not report: {', '.join(sorted(set(core_conditional)))})"
        )
    for kind in ("driver", "refused", "runtime"):
        print(f"  {len(classified[kind]):3} {kind}")
    print(f"  {len(classified['gap']):3} gap")
    # A refusal's contract is that its message names what would implement it:
    # a phase, or the reference that carries the step. A refusal that does not is
    # a dead end for whoever meets it, so the audit reports it.
    unpointed = []
    if classified["refused"]:
        print("\nrefused by name (the message names the phase that implements each):")
        for name in classified["refused"]:
            bodies = [
                body
                for source in [REFUSALS]
                for spelling in (name, name + "KHR", name + "2", name + "2KHR")
                if (body := definition(source, spelling)) is not None
            ]
            message = " ".join(bodies)
            pointed = any(
                pointer in message
                for pointer in ("Phase ", "docs/M5_REFERENCE.md", "docs/M5_PHASE", "V0-", "D1", "D2")
            )
            print(f"  {name}" + ("" if pointed else "   <- names no phase or reference"))
            if not pointed:
                unpointed.append(name)

    # The extensions the driver exposes are part of the same claim: an extension
    # whose commands reach an empty slot is a promise the device cannot keep.
    extension_gaps = []
    if exposed:
        print(f"\n{len(exposed)} extensions are exposed:")
        for extension in exposed:
            commands, conditional = extension_commands(registry, extension, satisfied)
            counted = {"driver": 0, "refused": 0, "runtime": 0, "gap": 0}
            for name in commands:
                kind = classify(name, symbols)
                counted[kind] += 1
                if kind == "gap":
                    extension_gaps.append((extension, name))
            print(
                f"  {extension:45} {len(commands):3} commands: "
                f"{counted['driver']} driver, {counted['refused']} refused, "
                f"{counted['runtime']} runtime, {counted['gap']} gap"
                + (f", {len(conditional)} conditional" if conditional else "")
            )
    if extension_gaps:
        print("\nthe commands of an exposed extension that are unaccounted for:")
        for extension, name in extension_gaps:
            print(f"  {extension}: {name}")
    if classified["gap"]:
        print("\nthe core commands that are unaccounted for:")
        for name in classified["gap"]:
            print(f"  {name}")
    # What this audit cannot see, and the probes that do see it. A command list
    # says nothing about pipeline state, sampler state or the pixels a clear
    # wrote: R1 lived in the first, R2 in the second and R3 in the third, and
    # none of them showed up here (PS5_VULKAN_REQUESTS.md, R4). The cases named
    # beside each blind spot are its coverage, and each is a console-proved
    # runner case rather than a one-off test.
    print("\nnot visible here, and the case that covers it:")
    for blind, case in BLIND_SPOTS:
        print(f"  {blind}: {case}")
    if args.check and (classified["gap"] or extension_gaps or unpointed):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
