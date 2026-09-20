#!/usr/bin/env python3
# PS5 Vulkan compatibility probe - console development tools.
# Copyright (C) 2026 Mihawk-99
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read test runs straight from the console's klog and publish runner jobs.

Commands:
  klog       Capture klog into Klog_Logs/, follow the next [PS5VK] run and
             summarise it when its run_end record arrives.
  summary    Summarise the [PS5VK] runs in a saved klog file.
  push-jobs  Upload job files to /data/homebrew/<TITLE_ID>/jobs/ over FTP.
  payload    Ask the resident control payload (payload/ps5vkctl) for its state.
  launch     Start the runner title through the control payload.
  kill       Close the runner title through the control payload.
  restart    Close the runner title if it is up, then start it again.
  battery    The whole loop: queue a battery, arm the capture, restart the title
             and summarise the run it produced.

Connection settings come from the environment, then from the ignored .env
file: PS5_HOST, FTP_PORT (default 2121), KLOG_PORT (default 3232),
PS5VKCTL_PORT (default 9111), PS5_FTP_USER (default anonymous) and
PS5_FTP_PASSWORD (default empty).
"""

import argparse
import datetime
import json
import os
import re
import socket
import sys
import time
from ftplib import FTP, error_perm
from pathlib import Path

from ps5vk_log import RunTracker, read_runs

ROOT = Path(__file__).resolve().parent.parent
BACKLOG_QUIET_SECONDS = 0.75
BACKLOG_LIMIT_SECONDS = 5.0


def load_settings():
    values = {}
    env_file = ROOT / ".env"
    if env_file.is_file():
        for raw in env_file.read_text(encoding="utf-8").splitlines():
            line = raw.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()

    def setting(key, default=None):
        value = os.environ.get(key)
        return value if value is not None else values.get(key, default)

    host = setting("PS5_HOST")
    if not host:
        raise SystemExit("PS5_HOST is not set in the environment or .env")
    return {
        "host": host,
        "ftp_port": int(setting("FTP_PORT", "2121")),
        "klog_port": int(setting("KLOG_PORT", "3232")),
        "ctl_port": int(setting("PS5VKCTL_PORT", "9111")),
        "ftp_user": setting("PS5_FTP_USER", "anonymous"),
        "ftp_password": setting("PS5_FTP_PASSWORD", ""),
    }


def format_run(run):
    lines = [f"run pid={run['pid']}: " + ("complete" if run["ended"] else "no run_end record")]
    counts = ", ".join(f"{status} {count}" for status, count in sorted(run["statuses"].items()))
    lines.append(f"  probe statuses: {counts or 'none'}")
    if run["tests"]:
        lines.append("  runner tests:")
        for record in run["tests"]:
            lines.append(f"    {record.get('status', '?'):<12} {record.get('detail', '')}")
    if run["notable"]:
        # Identical records (one per test, say) collapse into one line with a count.
        groups = {}
        for record in run["notable"]:
            key = (record.get("status", "?"), record.get("probe", "?"), record.get("result"),
                   record.get("detail", ""))
            groups[key] = groups.get(key, 0) + 1
        lines.append("  not passing:")
        for (status, probe, result, detail), count in groups.items():
            repeat = f" (x{count})" if count > 1 else ""
            lines.append(f"    {status:<12} {probe} ({result}) {detail}{repeat}")
    if not run["ended"] and run["last"] is not None:
        lines.append(f"  last record: {json.dumps(run['last'])}")
    return "\n".join(lines)


def split_lines(pending, chunk):
    pending += chunk
    *lines, pending = pending.split(b"\n")
    return [raw.decode("utf-8", "replace").rstrip("\r") for raw in lines], pending


def capture_runs(args, on_ready=None):
    settings = load_settings()
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    output = Path(args.output) if args.output else ROOT / "Klog_Logs" / f"klog-{stamp}.log"
    output.parent.mkdir(parents=True, exist_ok=True)
    tracker = RunTracker(echo=lambda text: print(text, flush=True))
    address = (settings["host"], settings["klog_port"])
    with socket.create_connection(address, timeout=10) as connection, \
            output.open("w", encoding="utf-8", newline="\n") as log_file:
        # The klog service replays its buffer on connect. Keep that backlog in
        # the file, but never mistake an earlier run for the one awaited.
        pending = b""
        backlog = 0
        connection.settimeout(BACKLOG_QUIET_SECONDS)
        backlog_end = time.monotonic() + BACKLOG_LIMIT_SECONDS
        while time.monotonic() < backlog_end:
            try:
                chunk = connection.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                raise SystemExit("klog: connection closed by the console")
            lines, pending = split_lines(pending, chunk)
            backlog += len(lines)
            log_file.writelines(line + "\n" for line in lines)
        log_file.flush()
        print(f"klog: {address[0]}:{address[1]} -> {output} ({backlog} backlog lines skipped)")
        if on_ready is None:
            print("ready: launch the title", flush=True)
        else:
            # The capture is armed: whoever asked for it starts the run now, so
            # no record of it is missed between arming and launching.
            on_ready()

        connection.settimeout(1.0)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            try:
                chunk = connection.recv(65536)
            except socket.timeout:
                continue
            if not chunk:
                print("klog: connection closed by the console")
                break
            lines, pending = split_lines(pending, chunk)
            for line in lines:
                log_file.write(line + "\n")
                if tracker.feed(line) == "run_end":
                    print(format_run(tracker.completed[-1]), flush=True)
                    if not args.follow:
                        return 0
            log_file.flush()
    if tracker.run is not None:
        print(format_run(tracker.run))
        return 3
    if tracker.completed:
        return 0
    print(f"klog: no run within {args.timeout} s")
    return 2


def capture_klog(args):
    return capture_runs(args)


def summarise(args):
    runs = read_runs(args.log)
    if not runs:
        print("no [PS5VK] run_start record found")
        return 2
    print("\n".join(format_run(run) for run in runs))
    return 0 if all(run["ended"] for run in runs) else 3


def delete_if_present(ftp, name):
    try:
        # ftpsrv answers a successful DELE with 226, which ftplib's delete()
        # rejects; sendcmd accepts any 2xx completion.
        ftp.sendcmd(f"DELE {name}")
        return True
    except error_perm as error:
        text = str(error).lower()
        if str(error).startswith("550") and ("no such" in text or "not found" in text):
            return False
        raise


def push_jobs(args):
    if not re.fullmatch(r"PPSA\d{5}", args.title):
        raise SystemExit("TITLE_ID must look like PPSA12345")
    files = [Path(name) for name in args.files]
    for path in files:
        if not path.is_file():
            raise SystemExit(f"missing job file: {path}")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", path.name) or path.name.startswith("."):
            raise SystemExit(f"unsafe job file name: {path.name}")
    return upload_jobs(load_settings(), args.title, files, args.replace)


def upload_jobs(settings, title, files, replace):
    """Upload job files to /data/homebrew/<TITLE_ID>/jobs/."""
    with FTP() as ftp:
        ftp.connect(settings["host"], settings["ftp_port"], timeout=15)
        ftp.login(settings["ftp_user"], settings["ftp_password"])
        # ftpsrv ignores MLSD path arguments, so every listing changes directory.
        try:
            ftp.cwd(f"/data/homebrew/{title}")
        except error_perm:
            raise SystemExit(f"{title} is not deployed under /data/homebrew")
        try:
            ftp.mkd("jobs")
        except error_perm as error:
            if not str(error).startswith("550"):
                raise
        ftp.cwd("jobs")
        if replace:
            for name, facts in list(ftp.mlsd()):
                if name not in {".", ".."} and facts.get("type") == "file":
                    delete_if_present(ftp, name)
                    print(f"removed jobs/{name}")
        for path in files:
            temporary = f".{path.name}.upload"
            delete_if_present(ftp, temporary)
            with path.open("rb") as source:
                ftp.storbinary(f"STOR {temporary}", source)
            delete_if_present(ftp, path.name)
            ftp.rename(temporary, path.name)
            print(f"uploaded jobs/{path.name} ({path.stat().st_size} bytes)")
        names = sorted(name for name, _ in ftp.mlsd() if name not in {".", ".."})
        print(f"/data/homebrew/{title}/jobs: {', '.join(names) or 'empty'}")
        try:
            ftp.quit()
        except (EOFError, OSError):
            pass
    return 0


def ps5vkctl_command(settings, command, timeout=60.0):
    """Send one command to the resident control payload and return its reply.

    The payload (payload/ps5vkctl) is loaded on the console once per boot and
    answers on PS5VKCTL_PORT; docs/DEPLOYMENT.md describes the loop.
    """
    address = (settings["host"], settings["ctl_port"])
    try:
        with socket.create_connection(address, timeout=timeout) as connection:
            connection.settimeout(timeout)
            connection.sendall(command.encode("ascii") + b"\n")
            pending = b""
            while b"\n" not in pending:
                chunk = connection.recv(256)
                if not chunk:
                    break
                pending += chunk
    except OSError as error:
        raise SystemExit(f"ps5vkctl: {address[0]}:{address[1]} did not answer ({error}); load "
                         f"build/ps5vkctl/ps5vkctl.elf on the console first")
    return pending.decode("ascii", "replace").strip()


def payload_status(args):
    settings = load_settings()
    print(f"ps5vkctl: {ps5vkctl_command(settings, 'ping')}")
    print(f"ps5vkctl: {ps5vkctl_command(settings, 'status')}")
    return 0


def title_command(command):
    """Build a handler for a payload command that names a title id."""

    def handler(args):
        if not re.fullmatch(r"PPSA\d{5}", args.title):
            raise SystemExit("TITLE_ID must look like PPSA12345")
        reply = ps5vkctl_command(load_settings(), f"{command} {args.title}", args.timeout)
        print(f"ps5vkctl: {reply}")
        return 0 if reply.startswith("ok") else 1

    return handler


def deploy_payload(args):
    """Upload the control payload so the console's loader can run it."""
    source = Path(args.payload) if args.payload else ROOT / "build/ps5vkctl/ps5vkctl.elf"
    if not source.is_file():
        raise SystemExit(f"missing payload: {source} (run tools/build-ps5vkctl.sh)")
    settings = load_settings()
    with FTP() as ftp:
        ftp.connect(settings["host"], settings["ftp_port"], timeout=15)
        ftp.login(settings["ftp_user"], settings["ftp_password"])
        for directory in ("/data/homebrew", "/data/etaHEN/payloads"):
            try:
                ftp.cwd(directory)
            except error_perm:
                # The second location is etaHEN's payload folder; a console that
                # does not have one simply keeps the first copy.
                print(f"{directory}: not available")
                continue
            with source.open("rb") as payload:
                ftp.storbinary("STOR ps5vkctl.elf", payload)
            print(f"{directory}/ps5vkctl.elf ({source.stat().st_size} bytes)")
        try:
            ftp.quit()
        except (EOFError, OSError):
            pass
    print("load it once on the console -- with the console's own payload loader, or with the "
          "payload SDK's")
    print("prospero-deploy -h <host> -p 9021 -- and check it answers with: "
          "python3 tools/ps5_console.py payload")
    return 0


def battery(args):
    """Queue a battery, arm the capture, restart the title, summarise the run."""
    if not re.fullmatch(r"PPSA\d{5}", args.title):
        raise SystemExit("TITLE_ID must look like PPSA12345")
    queue = Path(args.queue)
    if not queue.is_file():
        raise SystemExit(f"missing job file: {queue}")
    settings = load_settings()
    upload_jobs(settings, args.title, [queue], replace=True)

    def restart():
        reply = ps5vkctl_command(settings, f"restart {args.title}", args.timeout)
        print(f"ps5vkctl: {reply}", flush=True)
        if not reply.startswith("ok"):
            raise SystemExit(f"the control payload could not start {args.title}: {reply}")

    return capture_runs(args, on_ready=restart)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    klog = commands.add_parser("klog", help="capture klog and summarise the next run")
    klog.add_argument("--output", help="klog file (default Klog_Logs/klog-<time>.log)")
    klog.add_argument("--timeout", type=float, default=900.0,
                      help="seconds to wait for the run to finish (default 900)")
    klog.add_argument("--follow", action="store_true",
                      help="keep capturing after run_end and summarise every run")
    klog.set_defaults(handler=capture_klog)

    summary = commands.add_parser("summary", help="summarise the runs in a saved klog file")
    summary.add_argument("log")
    summary.set_defaults(handler=summarise)

    jobs = commands.add_parser("push-jobs", help="upload job files for a deployed title")
    jobs.add_argument("title", metavar="TITLE_ID")
    jobs.add_argument("files", nargs="+", metavar="FILE")
    jobs.add_argument("--replace", action="store_true",
                      help="remove the title's existing job files first")
    jobs.set_defaults(handler=push_jobs)

    payload = commands.add_parser("payload", help="ask the control payload for its state")
    payload.set_defaults(handler=payload_status)

    upload = commands.add_parser("deploy-payload",
                                 help="upload the control payload for the console's loader")
    upload.add_argument("--payload", help="payload to upload (default build/ps5vkctl/ps5vkctl.elf)")
    upload.set_defaults(handler=deploy_payload)

    for name, help_text in (("launch", "start the runner title"),
                            ("kill", "close the runner title"),
                            ("restart", "close the runner title, then start it again")):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("title", metavar="TITLE_ID")
        command.add_argument("--timeout", type=float, default=120.0,
                             help="seconds to wait for the payload's answer (default 120)")
        command.set_defaults(handler=title_command(name))

    run = commands.add_parser("battery", help="queue, launch and summarise one battery")
    run.add_argument("title", metavar="TITLE_ID")
    run.add_argument("queue", metavar="QUEUE", help="job queue to upload and run")
    run.add_argument("--output", help="klog file (default Klog_Logs/klog-<time>.log)")
    run.add_argument("--timeout", type=float, default=900.0,
                     help="seconds to wait for the run to finish (default 900)")
    run.set_defaults(handler=battery, follow=False)

    args = parser.parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
