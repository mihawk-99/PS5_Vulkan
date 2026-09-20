/*
 * PS5 Vulkan compatibility probe - PS5 gaps of the shader compiler archive.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phases A1 and B1 (docs/M5_PHASE_A.md, docs/M5_PHASE_B.md).
 * libpsbc.ps5.a and Mesa's Vulkan runtime reference functions that neither
 * the archives nor the console runtime provides. ps5-opengl supplies the
 * compiler's from its own title runtime; titles here get them from this
 * file, through libpsbc_support.ps5.a (tools/build-psbc-ps5.sh). Mesa's log.c
 * stays excluded: it pulls in syslog and file logging, which neither needs.
 * The logging functions below write what its file logger writes, to stderr.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/log.h"
#include "util/simple_mtx.h"

/* FreeBSD's assert() failure hook; NIR keeps assertions (the C sources build
 * without NDEBUG). Report, then stop like the C library would. */
void __assert(const char *function, const char *file, int line, const char *expression)
{
    fprintf(stderr, "assertion failed: %s (%s:%d, %s)\n", expression, file, line, function);
    abort();
}

/* Only ACO's optional disassembly (aco::print_asm) runs an external objdump.
 * Titles cannot start processes. */
FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;
    errno = ENOSYS;
    return -1;
}

/* Arch's Clang ships a glibc-targeted compiler-rt builtins archive whose
 * emutls.c.o is built against fortified headers and calls __memset_chk. The
 * console's C library and the payload SDK stubs export no fortified
 * interfaces, and psbc-link.sh links that archive for __emutls_get_address,
 * so the reference has to resolve here. emutls derives the length from its
 * own storage, and a violation gets glibc's __chk_fail behaviour: stop. */
void *__memset_chk(void *dest, int ch, size_t len, size_t destlen)
{
    if (len > destlen)
        __builtin_trap();
    return __builtin_memset(dest, ch, len);
}

/* Upstream log.c's level names, as its file logger prints them. */
static const char *log_level_name(enum mesa_log_level level)
{
    static const char *const names[] = {"error", "warning", "info", "debug"};
    return (unsigned)level < sizeof(names) / sizeof(names[0]) ? names[level] : "log";
}

/* Upstream log.c logs a multi-line text one line at a time; its file logger
 * writes "tag: level: line". NIR uses this to print annotated shaders. */
void _mesa_log_multiline(enum mesa_log_level level, const char *tag, const char *lines)
{
    const char *const name = log_level_name(level);
    while (lines && *lines) {
        const char *end = strchr(lines, '\n');
        const int length = end ? (int)(end - lines) : (int)strlen(lines);
        fprintf(stderr, "%s: %s: %.*s\n", tag, name, length, lines);
        lines += length + (end ? 1 : 0);
    }
}

/* Upstream log.c's file logger: "tag: level: message", with a newline added
 * when the message has none. The Vulkan runtime's vk_log reports through
 * mesa_log. */
void mesa_log_v(enum mesa_log_level level, const char *tag, const char *format, va_list va)
{
    fprintf(stderr, "%s: %s: ", tag, log_level_name(level));
    vfprintf(stderr, format, va);
    const size_t length = strlen(format);
    if (length == 0 || format[length - 1] != '\n')
        fputc('\n', stderr);
}

void mesa_log(enum mesa_log_level level, const char *tag, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    mesa_log_v(level, tag, format, va);
    va_end(va);
}

/* The console's C library exports localtime() but not localtime_r(), which
 * the Vulkan runtime's RMV capture dump uses for its file name. localtime_s()
 * is exported too, but no SDK header declares it, so its argument order is
 * unknown. Serialising localtime() and copying its result into the caller's
 * buffer gives localtime_r()'s contract to every caller that comes here. The
 * lock is Mesa's statically initialised simple_mtx, which the shader compiler
 * already uses on the console. */
struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
    simple_mtx_lock(&lock);
    const struct tm *const shared = localtime(timer);
    if (shared)
        *result = *shared;
    simple_mtx_unlock(&lock);
    return shared ? result : NULL;
}
