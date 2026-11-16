#!/usr/bin/env python3
"""Reconnect to the physical Linux CDC shell and preserve reboot evidence."""
import argparse
import array
import datetime
import fcntl
import glob
import json
import os
from pathlib import Path
import plistlib
import select
import subprocess
import sys
import termios
import time
import tty


def devices():
    if sys.platform == "darwin":
        result = subprocess.run(
            ["ioreg", "-r", "-c", "IOUSBHostDevice", "-a"],
            capture_output=True, timeout=5)
        if not result.stdout.strip():
            return [], []
        tree = plistlib.loads(result.stdout)
        ports, identities = set(), set()

        def visit(node, linux=False):
            if "idVendor" in node:
                vid, pid = node["idVendor"], node.get("idProduct", 0)
                linux = (vid, pid) == (0x0525, 0xa4aa)
                if linux or vid == 0x0421:
                    identities.add((vid, pid, node.get("USB Product Name", ""),
                                    node.get("sessionID", 0)))
            if linux and "IOCalloutDevice" in node:
                ports.add(node["IOCalloutDevice"])
            for child in node.get("IORegistryEntryChildren", []):
                visit(child, linux)

        for root in tree:
            visit(root)
        # Some macOS registry exports omit the serial descendants. Only use
        # the unambiguous modem fallback while the Linux gadget is present.
        modems = glob.glob("/dev/cu.usbmodem*")
        if not ports and len(modems) == 1 and any(
                identity[:2] == (0x0525, 0xa4aa) for identity in identities):
            ports.update(modems)
        return sorted(ports), sorted(identities)
    ports = []
    for port in glob.glob("/sys/class/tty/ttyACM*"):
        for parent in (Path(port) / "device").resolve().parents:
            if (parent / "idVendor").exists():
                if ((parent / "idVendor").read_text().strip(),
                    (parent / "idProduct").read_text().strip()) == ("0525", "a4aa"):
                    ports.append("/dev/" + Path(port).name)
                break
    return sorted(ports), ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    commands = args.output / "commands"
    commands.mkdir(exist_ok=True)
    lock = (args.output / "capture.lock").open("w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    (args.output / "capture.pid").write_text(str(os.getpid()) + "\n")
    raw = (args.output / "serial.raw").open("ab", buffering=0)
    events = (args.output / "events.jsonl").open("a", buffering=1)
    transcript = (args.output / "serial.log").open("a", buffering=1)

    def record(kind, message):
        stamp = datetime.datetime.now().astimezone().isoformat(timespec="milliseconds")
        events.write(json.dumps(dict(time=stamp, monotonic=time.monotonic(),
                                     kind=kind, message=message)) + "\n")
        transcript.write(f"\n[{stamp} {kind}] {message}")
        if kind != "RX":
            print(stamp, kind, message, flush=True)

    record("START", "Waiting for Nokia Linux 0525:a4aa; 115200 8N1, no flow control")
    fd = None
    previous = None
    next_scan = 0
    prompt = b""
    ready = False
    initialized = False
    port = None
    payload = b""
    command = ""
    try:
        while True:
            now = time.monotonic()
            if fd is None and now >= next_scan:
                try:
                    ports, identities = devices()
                except (OSError, subprocess.SubprocessError, ValueError) as error:
                    record("SCAN_ERROR", str(error))
                    time.sleep(0.5)
                    continue
                state = (ports, identities)
                if state != previous:
                    record("USB", state)
                    previous = state
                next_scan = now + 0.5
                if fd is not None and port not in ports:
                    record("DISCONNECT", port)
                    os.close(fd)
                    fd = None
                if fd is None and ports:
                    port = ports[0]
                    try:
                        record("OPEN_BEGIN", port)
                        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                        record("OPEN_DONE", port)
                        fcntl.ioctl(fd, termios.TIOCEXCL)
                        record("CONFIG_BEGIN", port)
                        attrs = termios.tcgetattr(fd)
                        tty.cfmakeraw(attrs)
                        attrs[2] |= termios.CLOCAL | termios.CREAD
                        attrs[2] &= ~(termios.HUPCL | getattr(termios, "CRTSCTS", 0))
                        attrs[4] = attrs[5] = termios.B115200
                        termios.tcsetattr(fd, termios.TCSANOW, attrs)
                        record("CONFIG_DONE", port)
                        record("DTR_BEGIN", port)
                        fcntl.ioctl(fd, termios.TIOCMBIS,
                                    array.array("i", [termios.TIOCM_DTR | termios.TIOCM_RTS]))
                        record("CONNECT", port)
                        os.write(fd, b"\n")
                        record("TX", "initial newline")
                        prompt, ready, initialized = b"", False, False
                    except OSError as error:
                        record("OPEN_ERROR", str(error))
                        if fd is not None:
                            os.close(fd)
                        fd = None
            if fd is None:
                time.sleep(0.1)
                continue
            try:
                if select.select([fd], [], [], 0.1)[0]:
                    data = os.read(fd, 65536)
                    if not data:
                        raise OSError("serial EOF")
                    raw.write(data)
                    record("RX", data.decode("utf-8", "backslashreplace"))
                    prompt = (prompt + data)[-4096:]
                    if b"rm443# " in prompt:
                        ready = True
                        prompt = b""
                pending = sorted(commands.glob("*.txt"))
                if not payload and ready and (pending or not initialized):
                    if pending:
                        queued = pending[0]
                        command = queued.read_text().rstrip() + "\n"
                        # Consume before transmission: never repeat a crash test
                        # automatically on the next boot.
                        queued.rename(queued.with_suffix(".sent"))
                    else:
                        # Recover the early ring once on connection; there is
                        # no periodic status or dmesg polling.
                        command = "uname -a; dmesg; cat /proc/uptime\n"
                    payload = command.encode()
                    ready = False
                    record("TX_BEGIN", command)
                if payload and select.select([], [fd], [], 0)[1]:
                    # Retain the unsent suffix across partial writes and EAGAIN.
                    sent = os.write(fd, payload)
                    payload = payload[sent:]
                    if not payload:
                        record("TX", command)
                        initialized = True
            except BlockingIOError:
                pass
            except OSError as error:
                record("DISCONNECT", str(error))
                os.close(fd)
                fd = None
                payload = b""
    finally:
        if fd is not None:
            os.close(fd)
        record("STOP", "Capture stopped")


if __name__ == "__main__":
    main()
