#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Control the user's existing RM-443 emulator handset for Linux development.

Uses existing emulator QMP/keypad/storage tools and firmware/phone storage.
Stop the frontend first; the shared phone lock prevents concurrent use.
"""
import argparse
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

from port_paths import EMULATOR_ROOT as EMU, STATE_ROOT
sys.path.insert(0, str(EMU / "scripts"))
from capture_nor import QMP

STATE = STATE_ROOT / "session.json"
PHONE = EMU / "firmware/phone"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("start", "status", "key", "screen", "stop", "pause", "resume", "dump", "regs"))
    parser.add_argument("args", nargs="*")
    args = parser.parse_args()
    if args.command == "start":
        firmware = Path(args.args[0]).resolve() if args.args else None
        phone = firmware / "phone" if firmware else PHONE
        if len(args.args) > 1 or not (phone / "phone.json").is_file():
            parser.error("start accepts one existing firmware directory; an existing phone profile is required")
        if STATE.exists():
            old = json.loads(STATE.read_text())
            try:
                prior = QMP(old["qmp"])
            except OSError:
                pass
            else:
                prior.close()
                parser.error("session already running")
        STATE.parent.mkdir(parents=True, exist_ok=True)
        qmp_port, gdb_port = free_port(), free_port()
        log = STATE.parent / "qemu.log"
        command = [sys.executable, str(EMU / "scripts/run.py"), "--headless", "--mute",
                   "--phone", str(phone), "--gdb", str(gdb_port)]
        if firmware:
            command += ["--firmware", str(firmware)]
        command += ["--",
                   "-qmp", f"tcp:127.0.0.1:{qmp_port},server=on,wait=off"]
        with log.open("wb") as output:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True)
        state = {"pid": process.pid, "qmp": qmp_port, "gdb": gdb_port,
                 "phone": str(phone), "command": command, "started": time.time()}
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(log.read_text())
            try:
                connection = QMP(qmp_port)
                connection.close()
                # A profile-lock or launch failure must not replace the last
                # known session with ports that never opened.
                STATE.write_text(json.dumps(state, indent=2) + "\n")
                print(json.dumps(state, indent=2))
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("QMP not ready; inspect emulator/qemu.log")
    state = json.loads(STATE.read_text())
    connection = QMP(state["qmp"])
    try:
        if args.command == "status":
            print(json.dumps(connection.command("query-status")))
        elif args.command == "key":
            for key in args.args:
                connection.command("send-key", keys=[{"type": "qcode", "data": key}], **{"hold-time": 100})
                time.sleep(0.3)
        elif args.command == "screen":
            path = Path(args.args[0]).resolve() if args.args else STATE.parent / "screen.png"
            connection.command("screendump", filename=str(path), format="png")
            print(path)
        elif args.command == "regs":
            print(connection.command("human-monitor-command", **{"command-line": "info registers"}))
        elif args.command == "dump":
            if len(args.args) != 3:
                parser.error("dump requires PHYSICAL_BASE SIZE OUTPUT")
            base, length = (int(text, 0) for text in args.args[:2])
            path = Path(args.args[2]).resolve()
            running = connection.command("query-status")["running"]
            connection.command("stop")
            try:
                connection.command("pmemsave", val=base, size=length, filename=str(path))
            finally:
                if running:
                    connection.command("cont")
            print(path)
        else:
            connection.command({"stop": "quit", "pause": "stop", "resume": "cont"}[args.command])
    finally:
        connection.close()


if __name__ == "__main__":
    main()
