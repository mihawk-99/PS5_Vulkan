/*
 * PS5 Vulkan probe - dlopen control module (E2 smoke test).
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A trivial shared object built exactly as the payload SDK's hello_so sample
 * builds its libraries (`prospero-clang -shared -soname <path>`), deployed
 * beside eboot.bin so the e2-module-load case can tell "this title cannot
 * dlopen a file from its own folder at all" from "the Vulkan module's container
 * is what the loader rejects". It exports one function and imports nothing a
 * title does not already have.
 */

int
ps5vk_dlfcn_control(void)
{
   return 42;
}
