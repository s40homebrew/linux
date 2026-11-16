#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit bounded rmprobe upload commands, or queue them with per-chunk ACKs.

Creates neither objects nor a session. In --queue mode a running rmprobe FIFO
session and its shell writer FD must already exist. Never opens the USB port.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shlex
import sys
import time
import uuid


def number(text):
    value = int(text, 16 if text.lower().startswith("0x") else 10)
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("expected an unsigned 32-bit number")
    return value


def commands(source, name, offset=0, chunk=192):
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]{0,30}", name):
        raise ValueError("buffer name must match rmprobe's name syntax")
    if not 1 <= chunk <= 192:
        raise ValueError("chunk must be between 1 and 192 bytes")
    while data := source.read(chunk):
        if offset + len(data) > 0x100000000:
            raise ValueError("upload exceeds the 32-bit offset range")
        yield f"puthex {name} {offset} {data.hex()}", f"uploaded {name} offset={offset} bytes={len(data)}"
        offset += len(data)


def shell_command(command, fd):
    if not 3 <= fd <= 9:
        raise ValueError("shell writer FD must be between 3 and 9")
    line = f"printf '%s\\n' {shlex.quote(command)} >&{fd}"
    if len(line.encode()) >= 500:
        raise ValueError("shell command exceeds the serial line budget")
    return line


def queue_upload(source, name, offset, chunk, directory, fd, timeout):
    directory = Path(directory)
    raw = directory.parent / "serial.raw"
    events = directory.parent / "events.jsonl"
    if not directory.is_dir() or not raw.is_file() or not events.is_file():
        raise ValueError("queue requires an existing capture commands directory, serial.raw and events.jsonl")
    prefix = f"probe-upload-{time.time_ns()}-{uuid.uuid4().hex[:8]}"
    with raw.open("rb") as capture, events.open("r") as event_log:
        capture.seek(0, os.SEEK_END)
        event_log.seek(0, os.SEEK_END)
        pending_event = ""
        for index, (command, ack) in enumerate(commands(source, name, offset, chunk)):
            deadline = time.monotonic() + timeout
            # Respect unrelated pending commands and let the logger retain its
            # existing one-command-per-shell-prompt transmission policy.
            while any(directory.glob("*.txt")):
                if time.monotonic() >= deadline:
                    raise TimeoutError("another queued command did not complete")
                time.sleep(0.05)
            target = directory / f"{prefix}-{index:08d}.txt"
            temporary = target.with_suffix(".part")
            temporary.write_text(shell_command(command, fd) + "\n")
            os.replace(temporary, target)
            received = b""
            try:
                while True:
                    received = (received + capture.read())[-65536:]
                    pending_event += event_log.read()
                    lines = pending_event.split("\n")
                    pending_event = lines.pop()
                    for line in lines:
                        if line and json.loads(line).get("kind") in {"DISCONNECT", "STOP"}:
                            raise RuntimeError("capture disconnected; upload stopped without retry")
                    if b"rmprobe:" in received or b"Bad file descriptor" in received or b"Broken pipe" in received:
                        raise RuntimeError("phone reported a stream error; upload stopped without retry")
                    if re.search(re.escape(ack.encode()) + rb"\r?\n", received):
                        break
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"no acknowledgement for chunk {index}; not retried")
                    time.sleep(0.05)
            except BaseException:
                # Cancel only our still-pending command. A .sent file remains
                # as the logger's evidence and is never replayed or removed.
                target.unlink(missing_ok=True)
                raise
            print(ack, file=sys.stderr, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument("input", type=Path)
    parser.add_argument("--offset", type=number, default=0)
    parser.add_argument("--chunk", type=int, default=192)
    parser.add_argument("--shell-fd", type=int, choices=range(3, 10))
    parser.add_argument("--queue", type=Path, help="capture commands directory; waits for each rmprobe upload acknowledgement")
    parser.add_argument("--timeout", type=float, default=30, help="seconds to wait per queued chunk")
    args = parser.parse_args()
    if args.queue and args.shell_fd is None:
        parser.error("--queue requires --shell-fd for an already-open FIFO writer")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive")
    try:
        with args.input.open("rb") as source:
            if args.queue:
                queue_upload(source, args.name, args.offset, args.chunk, args.queue, args.shell_fd, args.timeout)
            else:
                for command, _ in commands(source, args.name, args.offset, args.chunk):
                    print(shell_command(command, args.shell_fd) if args.shell_fd is not None else command)
    except (OSError, ValueError, TimeoutError, RuntimeError) as error:
        parser.exit(1, f"probe_stream: {error}\n")


if __name__ == "__main__":
    main()
