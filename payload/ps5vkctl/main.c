/*
 * PS5 Vulkan compatibility probe - resident control payload.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every probe battery costs one launch of the runner title, and until this
 * payload existed each of them cost a hand on the console as well: a title
 * cannot launch itself (the system refuses to launch an app that is already
 * running) and closing one is not something the title can be trusted to do after
 * a GPU fault. So the console keeps an agent instead. This payload stays
 * resident once it is loaded and answers one command per connection on port
 * 9111:
 *
 *   ping              is the agent there, and which process is it
 *   status            which application the console is running, and its title
 *   users             what the user service answers: init, foreground, logged-in
 *   procs             the pids of that application's processes
 *   launch <TITLEID>  start a title (sceLncUtilLaunchApp)
 *   kill <TITLEID>    close a title: suspend, SIGKILL its processes, then
 *                     sceLncUtilKillApp, which is the sequence
 *                     ps5-payload-manager's process manager uses
 *   restart <TITLEID> close it if it is the running title, then start it again
 *   quit              leave the console
 *
 * The launch and kill calls are the ones AirPSX (ApplicationRunHandler) and
 * ps5-payload-manager (ps5_launcher.c) already make from a payload on a
 * jailbroken console; this agent makes them reachable without a dashboard.
 * Closing names a title id and refuses to act unless the console's running
 * application really is that title, so a typo cannot close the wrong
 * application, and PPSA99999 -- the default profile this project never launches
 * for a run -- is refused outright.
 *
 * Build with tools/build-ps5vkctl.sh, which needs PS5_PAYLOAD_SDK, and load
 * build/ps5vkctl/ps5vkctl.elf once per console boot with whatever payload
 * loader the console runs. tools/ps5_console.py is the client; docs/DEPLOYMENT.md
 * describes the loop it serves.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

/* The agent's port, the longest command it answers, and its own log. */
#define PS5VKCTL_PORT 9111
#define PS5VKCTL_LINE 256
#define PS5VKCTL_LOG "/data/ps5vkctl.log"
#define PS5VKCTL_MAX_PIDS 16
#define PS5VKCTL_TITLE 9

/* libSceSystemService reports these for a title that is already up, and for no
 * application at all. */
#define PS5VKCTL_ALREADY_RUNNING 0x8094000cu
#define PS5VKCTL_NO_APP 0xffffffffu

/* The default profile: never launched, and never closed, by a run. */
static const char *const refused_title = "PPSA99999";

/* What sceUserServiceInitialize returned at startup, reported by "users". */
static int g_user_init;

/* The console's own record of a process, as sceKernelGetAppInfo fills it (the
 * layout ps5-payload-manager's process manager reads). */
typedef struct app_info
{
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} app_info_t;

/* The launch parameter etaHEN's launcher passes, field for field. */
typedef struct lnc_app_param
{
    uint32_t size;
    int user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    uint64_t check_flag;
} lnc_app_param_t;

/* The user ids the console has logged in, -1 for an empty slot (the layout
 * AirPSX reads). */
typedef struct login_user_id_list
{
    int user_id[4];
} login_user_id_list_t;

/* libSceSystemService, through the payload SDK's stub library. */
extern uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
extern int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
extern int sceLncUtilSuspendApp(uint32_t app_id);
extern uint32_t sceLncUtilKillApp(uint32_t app_id);
extern int sceLncUtilLaunchApp(const char *title_id, const char *argv[], lnc_app_param_t *param);
extern int sceSystemServiceLaunchApp(const char *title_id, const char *argv[],
                                     lnc_app_param_t *param);
/* libSceUserService. The service wants initializing before its getters answer,
 * and a launch needs the user it runs as. */
extern int sceUserServiceInitialize(void *param);
extern int sceUserServiceGetForegroundUser(int *user_id);
extern int sceUserServiceGetLoginUserIdList(login_user_id_list_t *list);
/* libkernel. */
extern int sceKernelGetAppInfo(int pid, app_info_t *info);

// Log one line to stdout (the console's klog carries a payload's output) and to
// the log file the FTP server can hand back.
static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    vprintf(format, arguments);
    printf("\n");
    fflush(stdout);
    FILE *const file = fopen(PS5VKCTL_LOG, "a");
    if (file != NULL)
    {
        vfprintf(file, format, copy);
        fputc('\n', file);
        fclose(file);
    }
    va_end(copy);
    va_end(arguments);
}

// Answer one line, newline terminated.
static void reply(int client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void reply(int client, const char *format, ...)
{
    char line[PS5VKCTL_LINE];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    const size_t length = strlen(line);
    if (send(client, line, length, 0) < 0 || send(client, "\n", 1, 0) < 0)
        log_line("ps5vkctl: reply failed: %s", strerror(errno));
}

// The application the console is running, with its title id ("" when the id
// cannot be read). PS5VKCTL_NO_APP when nothing is running.
static uint32_t running_app(char *title, size_t size)
{
    title[0] = '\0';
    const uint32_t app_id = sceLncUtilGetAppIdOfRunningBigApp();
    if (app_id == PS5VKCTL_NO_APP || app_id == 0)
        return PS5VKCTL_NO_APP;
    char buffer[32] = {0};
    if (sceLncUtilGetAppTitleId(app_id, buffer) != 0)
        buffer[0] = '\0';
    snprintf(title, size, "%s", buffer);
    return app_id;
}

// The pids of the processes that belong to one application, this agent's own
// process excluded.
static int app_pids(uint32_t app_id, int *pids, int capacity)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t bytes = 0;
    if (sysctl(mib, 4, NULL, &bytes, NULL, 0) != 0 || bytes == 0)
        return 0;
    void *const buffer = malloc(bytes);
    if (buffer == NULL)
        return 0;
    int count = 0;
    if (sysctl(mib, 4, buffer, &bytes, NULL, 0) == 0)
    {
        for (const char *cursor = buffer;
             cursor + sizeof(struct kinfo_proc) <= (const char *)buffer + bytes &&
             count < capacity;)
        {
            const struct kinfo_proc *const process = (const struct kinfo_proc *)cursor;
            if (process->ki_structsize <= 0 ||
                (size_t)process->ki_structsize < sizeof(struct kinfo_proc))
                break;
            cursor += process->ki_structsize;
            if (process->ki_pid == getpid())
                continue;
            app_info_t info;
            memset(&info, 0, sizeof(info));
            if (sceKernelGetAppInfo(process->ki_pid, &info) != 0)
                continue;
            if (info.app_id == app_id)
                pids[count++] = process->ki_pid;
        }
    }
    free(buffer);
    return count;
}

// Close a title. Returns 0 when it is gone, 1 when it was not running, and -1
// when it could not be closed or is not the running application.
static int close_title(const char *title, char *detail, size_t size)
{
    char running[32] = {0};
    const uint32_t app_id = running_app(running, sizeof(running));
    if (app_id == PS5VKCTL_NO_APP)
    {
        snprintf(detail, size, "no application is running");
        return 1;
    }
    if (strncmp(running, title, PS5VKCTL_TITLE) != 0)
    {
        snprintf(detail, size, "the running application is %s", running);
        return -1;
    }
    log_line("ps5vkctl: closing %s (app id %u): suspend, then SIGKILL", title, app_id);
    (void)sceLncUtilSuspendApp(app_id);
    sleep(2);
    int pids[PS5VKCTL_MAX_PIDS] = {0};
    const int count = app_pids(app_id, pids, PS5VKCTL_MAX_PIDS);
    for (int index = 0; index < count; index++)
        if (kill(pids[index], SIGKILL) == 0)
            log_line("ps5vkctl: SIGKILL to pid %d", pids[index]);
    const uint32_t result = sceLncUtilKillApp(app_id);
    log_line("ps5vkctl: sceLncUtilKillApp(%u) -> 0x%08x", app_id, result);
    for (int wait = 0; wait < 50; wait++)
    {
        char after[32] = {0};
        if (running_app(after, sizeof(after)) == PS5VKCTL_NO_APP)
        {
            snprintf(detail, size, "%s is gone (app id %u, %d processes killed, result 0x%08x)",
                     title, app_id, count, result);
            return 0;
        }
        usleep(100000);
    }
    snprintf(detail, size, "%s is still running (app id %u, result 0x%08x)", title, app_id, result);
    return -1;
}

// The foreground user's id, or -1 when the console does not name one.
static int foreground_user(void)
{
    int user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) != 0)
        return -1;
    return user_id;
}

// The first logged-in user's id, or -1 when nobody is logged in.
static int login_user(void)
{
    login_user_id_list_t users;
    memset(&users, 0, sizeof(users));
    if (sceUserServiceGetLoginUserIdList(&users) != 0)
        return -1;
    for (int index = 0; index < 4; index++)
        if (users.user_id[index] >= 0)
            return users.user_id[index];
    return -1;
}

// One way of asking the console to start a title, and the user it runs as.
typedef struct launch_attempt
{
    const char *name;
    int (*call)(const char *title_id, const char *argv[], lnc_app_param_t *param);
    int user_id;
    uint64_t check_flag;
} launch_attempt_t;

// Start a title. A payload's launch call needs a user id and the right entry
// point, and which of them the console accepts is not worth guessing: AirPSX
// launches through sceSystemServiceLaunchApp with a logged-in user and etaHEN
// through sceLncUtilLaunchApp with the foreground one, so try both users through
// both calls -- and the skip-check flag last -- and report what each returned.
// Returns 0 when the title is up or already was, with the winning attempt and
// its result in *name and *code.
static int start_title(const char *title, const char **name, uint32_t *code, char *detail,
                       size_t size)
{
    const int foreground = foreground_user();
    const int login = login_user();
    char report[384] = {0};
    const launch_attempt_t attempts[] = {
        {"lncUtilApp/foreground", sceLncUtilLaunchApp, foreground, 0},
        {"lncUtilApp/login", sceLncUtilLaunchApp, login, 0},
        {"systemServiceApp/login", sceSystemServiceLaunchApp, login, 0},
        {"lncUtilApp/login+skipCheck", sceLncUtilLaunchApp, login, 1},
    };
    for (size_t index = 0; index < sizeof(attempts) / sizeof(attempts[0]); index++)
    {
        const launch_attempt_t *const attempt = &attempts[index];
        if (attempt->user_id < 0)
            continue;
        lnc_app_param_t param;
        memset(&param, 0, sizeof(param));
        param.size = (uint32_t)sizeof(param);
        param.user_id = attempt->user_id;
        param.check_flag = attempt->check_flag;
        *code = (uint32_t)attempt->call(title, NULL, &param);
        log_line("ps5vkctl: launch %s as %s user %d -> 0x%08x", title, attempt->name,
                 attempt->user_id, *code);
        char entry[96] = {0};
        snprintf(entry, sizeof(entry), "%s%s(user %d)=0x%08x", report[0] == '\0' ? "" : ", ",
                 attempt->name, attempt->user_id, *code);
        strncat(report, entry, sizeof(report) - strlen(report) - 1);
        // The console decides, not the return code: a launch that has started
        // the title reads as success even when the call reports something else
        // (a console that already had the title coming up answers the next
        // attempt with "already running"), so ask which application is running
        // before trying another way of asking for it.
        char running[32] = {0};
        bool started = false;
        for (int wait = 0; wait < 20 && !started; wait++)
        {
            started = running_app(running, sizeof(running)) != PS5VKCTL_NO_APP &&
                      strncmp(running, title, PS5VKCTL_TITLE) == 0;
            if (!started)
                usleep(250000);
        }
        if (*code == 0 || *code == PS5VKCTL_ALREADY_RUNNING || started)
        {
            *name = attempt->name;
            snprintf(detail, size, "via %s as user %d%s", attempt->name, attempt->user_id,
                     started && *code != 0 && *code != PS5VKCTL_ALREADY_RUNNING
                         ? " (the console reports the title running)"
                         : "");
            return 0;
        }
    }
    *name = NULL;
    snprintf(detail, size, "foreground user %d, login user %d, attempts: %s", foreground, login,
             report[0] == '\0' ? "none (no user id to launch as)" : report);
    return -1;
}

// A title id is nine characters, as the console's own routes require.
static int valid_title(const char *title)
{
    return title != NULL && strlen(title) == PS5VKCTL_TITLE;
}

// Answer one command. Returns 0 to keep serving, -1 to leave the console.
static int handle_command(int client, char *line)
{
    char *const command = strtok(line, " \t");
    if (command == NULL)
    {
        reply(client, "err empty");
        return 0;
    }
    const char *const title = strtok(NULL, " \t");
    if (strcmp(command, "ping") == 0)
    {
        reply(client, "ok ps5vkctl 1 pid=%d", (int)getpid());
        return 0;
    }
    if (strcmp(command, "status") == 0)
    {
        char running[32] = {0};
        const uint32_t app_id = running_app(running, sizeof(running));
        if (app_id == PS5VKCTL_NO_APP)
            reply(client, "ok idle");
        else
            reply(client, "ok running app=%u title=%s", app_id, running);
        return 0;
    }
    if (strcmp(command, "users") == 0)
    {
        login_user_id_list_t users;
        memset(&users, 0, sizeof(users));
        const int list_result = sceUserServiceGetLoginUserIdList(&users);
        char list[64] = {0};
        if (list_result == 0)
        {
            for (int index = 0; index < 4; index++)
            {
                char entry[12] = {0};
                snprintf(entry, sizeof(entry), "%s%d", index == 0 ? "" : ",", users.user_id[index]);
                strncat(list, entry, sizeof(list) - strlen(list) - 1);
            }
        }
        reply(client, "ok users init=0x%08x foreground=%d list=0x%08x [%s]", g_user_init,
              foreground_user(), (uint32_t)list_result, list);
        return 0;
    }
    if (strcmp(command, "procs") == 0)
    {
        char running[32] = {0};
        const uint32_t app_id = running_app(running, sizeof(running));
        int pids[PS5VKCTL_MAX_PIDS] = {0};
        const int count = app_id == PS5VKCTL_NO_APP ? 0 : app_pids(app_id, pids, PS5VKCTL_MAX_PIDS);
        char list[PS5VKCTL_MAX_PIDS * 12] = {0};
        for (int index = 0; index < count; index++)
        {
            char entry[16] = {0};
            snprintf(entry, sizeof(entry), "%s%d", index == 0 ? "" : ",", pids[index]);
            strncat(list, entry, sizeof(list) - strlen(list) - 1);
        }
        reply(client, "ok procs app=%u title=%s count=%d pids=%s", app_id,
              app_id == PS5VKCTL_NO_APP ? "" : running, count, list);
        return 0;
    }
    if (strcmp(command, "quit") == 0)
    {
        reply(client, "ok bye");
        return -1;
    }
    if (!valid_title(title))
    {
        reply(client, "err usage: %s needs a nine-character title id", command);
        return 0;
    }
    if (strcmp(title, refused_title) == 0)
    {
        reply(client, "err refused %s is the default profile", title);
        return 0;
    }
    if (strcmp(command, "launch") == 0)
    {
        const char *name = NULL;
        uint32_t code = 0;
        char detail[160] = {0};
        if (start_title(title, &name, &code, detail, sizeof(detail)) == 0)
            reply(client, "ok launched %s result=0x%08x %s", title, code, detail);
        else
            reply(client, "err launch %s %s", title, detail);
        return 0;
    }
    if (strcmp(command, "kill") == 0)
    {
        char detail[160] = {0};
        const int status = close_title(title, detail, sizeof(detail));
        if (status == 0)
            reply(client, "ok killed %s: %s", title, detail);
        else if (status == 1)
            reply(client, "ok idle %s: %s", title, detail);
        else
            reply(client, "err kill %s: %s", title, detail);
        return 0;
    }
    if (strcmp(command, "restart") == 0)
    {
        char detail[160] = {0};
        const int status = close_title(title, detail, sizeof(detail));
        if (status < 0)
        {
            reply(client, "err restart %s: %s", title, detail);
            return 0;
        }
        const char *name = NULL;
        uint32_t code = 0;
        if (start_title(title, &name, &code, detail, sizeof(detail)) == 0)
            reply(client, "ok restarted %s result=0x%08x %s", title, code, detail);
        else
            reply(client, "err restart %s %s", title, detail);
        return 0;
    }
    reply(client, "err unknown %s", command);
    return 0;
}

// Read one command line and answer it. Returns 0 to keep serving, -1 to leave.
static int serve_client(int client)
{
    char line[PS5VKCTL_LINE] = {0};
    size_t used = 0;
    while (used + 1 < sizeof(line))
    {
        const ssize_t bytes = recv(client, line + used, sizeof(line) - 1 - used, 0);
        if (bytes <= 0)
            break;
        used += (size_t)bytes;
        line[used] = '\0';
        if (strchr(line, '\n') != NULL)
            break;
    }
    while (used > 0 && (line[used - 1] == '\n' || line[used - 1] == '\r'))
        line[--used] = '\0';
    if (used == 0)
        return 0;
    log_line("ps5vkctl: command %s", line);
    return handle_command(client, line);
}

int main(int argc, char *argv[])
{
    unsigned port = PS5VKCTL_PORT;
    if (argc > 1)
    {
        const unsigned long requested = strtoul(argv[1], NULL, 10);
        if (requested > 0 && requested < 65536)
            port = (unsigned)requested;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    // The launch calls need the user service, and an uninitialized one answers
    // nothing: initialize it once, whatever the result, and report it.
    g_user_init = sceUserServiceInitialize(NULL);
    log_line("ps5vkctl: starting, port %u, pid %d, user service 0x%08x", port, (int)getpid(),
             (uint32_t)g_user_init);

    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
    {
        log_line("ps5vkctl: socket: %s", strerror(errno));
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 4) != 0)
    {
        log_line("ps5vkctl: bind or listen failed: %s", strerror(errno));
        close(server);
        return 1;
    }
    log_line("ps5vkctl: listening");
    for (;;)
    {
        const int client = accept(server, NULL, NULL);
        if (client < 0)
        {
            if (errno == EINTR)
                continue;
            log_line("ps5vkctl: accept failed: %s", strerror(errno));
            break;
        }
        const int leaving = serve_client(client);
        close(client);
        if (leaving != 0)
        {
            log_line("ps5vkctl: leaving");
            break;
        }
    }
    close(server);
    return 0;
}
