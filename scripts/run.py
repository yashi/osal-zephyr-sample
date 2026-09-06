#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run or inspect one OSAL Zephyr stage-harness application."""

import argparse
import errno
import math
import os
import pty
import re
import select
import signal
import subprocess
import sys
import time


START_RE = re.compile(r"HARNESS START (\S+)")
CHECK_RE = re.compile(r"HARNESS CHECK (\S+) (PASS|FAIL)")
RESULT_RE = re.compile(
    r"HARNESS RESULT (\S+) (PASS|FAIL) checks=(\d+) failures=(\d+)"
)


class HarnessRecords:
    def __init__(self, app):
        self.app = app
        self.starts = 0
        self.checks = []
        self.labels = set()
        self.results = 0
        self.result = None
        self.errors = []

    @property
    def result_seen(self):
        return self.results != 0

    def error(self, line_number, message):
        self.errors.append(f"line {line_number}: {message}")

    def feed(self, line, line_number):
        if not line.startswith("HARNESS"):
            return

        match = START_RE.fullmatch(line)
        if match:
            self.starts += 1
            if self.starts != 1 or self.result is not None:
                self.error(line_number, "unexpected additional START record")
            if match.group(1) != self.app:
                self.error(line_number, f"START names foreign app {match.group(1)!r}")
            return

        match = CHECK_RE.fullmatch(line)
        if match:
            label, status = match.groups()
            if self.starts != 1 or self.result is not None:
                self.error(line_number, "CHECK record is out of order")
            if label in self.labels:
                self.error(line_number, f"duplicate check label {label!r}")
            self.labels.add(label)
            self.checks.append(status)
            if status != "PASS":
                self.error(line_number, f"check {label!r} failed")
            return

        match = RESULT_RE.fullmatch(line)
        if match:
            app, status, checks, failures = match.groups()
            self.results += 1
            if self.starts != 1 or self.result is not None:
                self.error(line_number, "RESULT record is out of order or repeated")
            if app != self.app:
                self.error(line_number, f"RESULT names foreign app {app!r}")
            if self.result is None:
                self.result = (status, int(checks), int(failures), line_number)
            return

        self.error(line_number, "malformed HARNESS record")

    def finish(self):
        if self.starts != 1:
            self.errors.append(f"expected one START record, observed {self.starts}")
        if not self.checks:
            self.errors.append("expected at least one CHECK record")
        if self.results != 1:
            self.errors.append(f"expected one RESULT record, observed {self.results}")
        if self.result is not None:
            status, checks, failures, line_number = self.result
            observed_failures = self.checks.count("FAIL")
            if checks != len(self.checks):
                self.error(line_number, f"summary says checks={checks}, observed {len(self.checks)}")
            if failures != observed_failures:
                self.error(
                    line_number,
                    f"summary says failures={failures}, observed {observed_failures}",
                )
            expected_status = "PASS" if observed_failures == 0 else "FAIL"
            if status != expected_status:
                self.error(
                    line_number,
                    f"summary status is {status}, expected {expected_status}",
                )
            if status != "PASS":
                self.error(line_number, "application reported failure")
        return not self.errors


def consume(data, pending, records, line_number, log_file):
    if log_file is not None:
        log_file.write(data)
        log_file.flush()
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()
    pending += data
    while True:
        cr = pending.find(b"\r")
        lf = pending.find(b"\n")
        boundaries = [offset for offset in (cr, lf) if offset >= 0]
        if not boundaries:
            break
        boundary = min(boundaries)
        width = 2 if pending[boundary : boundary + 2] == b"\r\n" else 1
        raw, pending = pending[:boundary], pending[boundary + width :]
        records.feed(raw.decode("utf-8", "replace"), line_number)
        line_number += 1
    return pending, line_number


def stop_process(proc):
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        proc.wait()
        return
    deadline = time.monotonic() + 1
    while time.monotonic() < deadline:
        proc.poll()
        try:
            os.killpg(proc.pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    proc.wait()


def run_command(command, records, timeout, qemu, log_file):
    master = slave = None
    popen_args = {
        "start_new_session": True,
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.PIPE,
        "stderr": subprocess.STDOUT,
    }
    if qemu:
        master, slave = pty.openpty()
        popen_args.update(stdin=slave, stdout=slave)

    try:
        proc = subprocess.Popen(command, **popen_args)
    except Exception:
        if master is not None:
            os.close(master)
        raise
    finally:
        if slave is not None:
            os.close(slave)

    output_fd = master if qemu else proc.stdout.fileno()
    pending = b""
    line_number = 1
    eof = False
    quit_sent = False
    deadline = time.monotonic() + timeout
    try:
        while not (eof and proc.poll() is not None):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                records.errors.append(f"command timed out after {timeout:g} seconds")
                stop_process(proc)
                break
            readable, _, _ = select.select([output_fd], [], [], min(0.2, remaining))
            if readable:
                try:
                    data = os.read(output_fd, 65536)
                except OSError as exc:
                    if qemu and exc.errno == errno.EIO:
                        data = b""
                    else:
                        raise
                if data:
                    pending, line_number = consume(
                        data, pending, records, line_number, log_file
                    )
                else:
                    eof = True
            if qemu and records.result_seen and not quit_sent:
                print(
                    "[harness] QEMU termination is runner-controlled; "
                    "the application result comes from checked HARNESS records.",
                    file=sys.stderr,
                    flush=True,
                )
                try:
                    os.write(master, b"\x01x")
                    quit_sent = True
                except OSError as exc:
                    records.errors.append(f"could not send QEMU quit sequence: {exc}")
                    stop_process(proc)
                    break
        if pending:
            records.feed(pending.rstrip(b"\r").decode("utf-8", "replace"), line_number)
        return_code = proc.wait()
        if return_code != 0:
            records.errors.append(f"command exited with status {return_code}")
    except KeyboardInterrupt:
        stop_process(proc)
        records.errors.append("interrupted by user")
        return 130
    except Exception:
        stop_process(proc)
        raise
    finally:
        if master is not None:
            os.close(master)
    return 0


def validate_log(path, records):
    with open(path, "rb") as source:
        for line_number, raw in enumerate(source.read().splitlines(), 1):
            records.feed(raw.decode("utf-8", "replace"), line_number)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run a command or validate a captured OSAL stage-harness log.",
        allow_abbrev=False,
    )
    parser.add_argument("--app", required=True, help="expected application name")
    parser.add_argument("--qemu", action="store_true", help="quit QEMU after RESULT")
    parser.add_argument("--timeout", type=float, default=30, help="command timeout in seconds")
    logs = parser.add_mutually_exclusive_group()
    logs.add_argument("--log", help="save launched command output to this file")
    logs.add_argument("--log-file", help="validate this existing hardware log")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command after --")
    args = parser.parse_args()
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if args.log_file:
        if args.command:
            parser.error("a command cannot be used with --log-file")
        if args.qemu:
            parser.error("--qemu cannot be used with --log-file")
    elif not args.command:
        parser.error("a command is required unless --log-file is used")
    return args


def main():
    args = parse_args()
    records = HarnessRecords(args.app)
    interrupted = 0
    try:
        if args.log_file:
            validate_log(args.log_file, records)
        else:
            with open(args.log, "wb") if args.log else open(os.devnull, "wb") as output:
                interrupted = run_command(
                    args.command, records, args.timeout, args.qemu, output if args.log else None
                )
    except (OSError, subprocess.SubprocessError) as exc:
        records.errors.append(str(exc))

    valid = records.finish()
    if valid:
        print(f"[harness] PASS: app={args.app} checks={len(records.checks)}", file=sys.stderr)
        return interrupted
    print(f"[harness] FAIL: app={args.app}", file=sys.stderr)
    for error in records.errors:
        print(f"[harness] {error}", file=sys.stderr)
    return interrupted or 1


if __name__ == "__main__":
    sys.exit(main())
