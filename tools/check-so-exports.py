#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - check the signed shared object's payload.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the payload of a signed PS5 shared object.

tools/build-driver.sh signs build/driver/ps5/libvulkan.so.1 with the FSELF
container, whose payload drops the section header table: `prospero-nm` cannot
read its dynamic symbols, so the strings the dynamic segment and the symbol
table carry are what this checks. It prints "ok", or "missing <names>".

Usage: python3 tools/check-so-exports.py <signed.so> [soname]
"""

import sys

ENTRY_POINTS = [
    "vkGetInstanceProcAddr",
    "vk_icdGetInstanceProcAddr",
    "vk_icdGetPhysicalDeviceProcAddr",
    "vk_icdNegotiateLoaderICDInterfaceVersion",
]


def main(argv):
    if len(argv) not in (2, 3):
        raise SystemExit(__doc__)
    # The soname is the file name the payload has to name, and the soname-as-path
    # copy carries the absolute path instead.
    names = [argv[2] if len(argv) == 3 else "libvulkan.so.1", *ENTRY_POINTS]
    with open(argv[1], "rb") as stream:
        data = stream.read()
    missing = [name for name in names if name.encode() not in data]
    print("ok" if not missing else "missing " + " ".join(missing))
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
