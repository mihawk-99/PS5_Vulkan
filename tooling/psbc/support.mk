# PS5 Vulkan compatibility probe - flags for the support archive's util objects.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 5 Phase B1 (docs/M5_PHASE_B.md). Read after the SDK's
# Makefile.opengnm-psbc-ps5 by tools/build-psbc-ps5.sh, which leaves that
# makefile unmodified.
#
# os_time.c calls usleep() and u_process.c getprogname(). The console exports
# both, but the payload SDK's FreeBSD headers hide them under the compiler
# tree's -D_XOPEN_SOURCE=700: getprogname is declared only when __BSD_VISIBLE
# (stdlib.h), and usleep, which POSIX 2008 removed, only when __XSI_VISIBLE is
# at most 600 or __BSD_VISIBLE (unistd.h). sys/cdefs.h sets __XSI_VISIBLE to
# 700 and __BSD_VISIBLE to 0 whenever _XOPEN_SOURCE is 700. These two objects
# therefore compile without it; FreeBSD's default environment exposes POSIX
# 2008, XSI, BSD and C11 interfaces, a superset. tools/build-vulkan-runtime.sh
# drops the define for the Vulkan runtime for the same reason.

src/util/os_time.ps5.o src/util/u_process.ps5.o: CFLAGS := $(filter-out -D_XOPEN_SOURCE=700,$(CFLAGS))
