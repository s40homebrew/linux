#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only stream framing and queue/ack tests; no serial port is opened."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("probe_stream", ROOT / "tools/probe_stream.py")
STREAM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(STREAM)


class Streaming(unittest.TestCase):
    def test_bounded_commands_and_offsets(self):
        data = bytes(range(256)) * 4
        commands = list(STREAM.commands(io.BytesIO(data), "x" * 31, 4095))
        rebuilt = bytearray()
        for command, ack in commands:
            op, name, offset, hexdata = command.split()
            self.assertEqual(op, "puthex")
            self.assertEqual(int(offset), 4095 + len(rebuilt))
            self.assertLess(len(STREAM.shell_command(command, 9)), 500)
            rebuilt.extend(bytes.fromhex(hexdata))
            self.assertEqual(ack, f"uploaded {name} offset={offset} bytes={len(hexdata)//2}")
        self.assertEqual(rebuilt, data)
        with self.assertRaises(ValueError):
            list(STREAM.commands(io.BytesIO(b"123"), "bad;name"))
        with self.assertRaises(ValueError):
            list(STREAM.commands(io.BytesIO(b"123"), "data", 0xFFFFFFFF))
        with self.assertRaises(ValueError):
            list(STREAM.commands(io.BytesIO(b"123"), "data", chunk=193))

    def queue_fixture(self, error=False):
        with tempfile.TemporaryDirectory(prefix="probe-stream-test-") as directory:
            capture = Path(directory)
            queue = capture / "commands"
            queue.mkdir()
            raw, events = capture / "serial.raw", capture / "events.jsonl"
            raw.write_bytes(b"")
            events.write_text("")
            received = []
            stop = threading.Event()
            failure = []

            def consume():
                try:
                    while not stop.is_set():
                        paths = list(queue.glob("*.txt"))
                        if not paths:
                            time.sleep(0.005)
                            continue
                        self.assertEqual(len(paths), 1)
                        path = paths[0]
                        text = path.read_text()
                        path.rename(path.with_suffix(".sent"))
                        # Shell command quotes are deterministic for these
                        # generated names/data; no shell is executed.
                        command = text.split("'puthex ", 1)[1].split("'", 1)[0]
                        name, offset, hexdata = command.split()
                        received.append(bytes.fromhex(hexdata))
                        if error:
                            with events.open("a") as output:
                                output.write(json.dumps({"kind": "DISCONNECT"}) + "\n")
                            return
                        with raw.open("ab") as output:
                            output.write(f"rm443# uploaded {name} offset={offset} bytes={len(hexdata)//2}\r\n".encode())
                except BaseException as exception:
                    failure.append(exception)

            worker = threading.Thread(target=consume)
            worker.start()
            try:
                if error:
                    with self.assertRaisesRegex(RuntimeError, "disconnected"):
                        STREAM.queue_upload(io.BytesIO(b"a" * 600), "data", 0, 192, queue, 9, 2)
                    self.assertEqual(len(received), 1)
                else:
                    data = bytes(range(256)) * 3
                    STREAM.queue_upload(io.BytesIO(data), "data", 4095, 192, queue, 9, 2)
                    self.assertEqual(b"".join(received), data)
                self.assertFalse(list(queue.glob("*.txt")))
                self.assertEqual(len(list(queue.glob("*.sent"))), len(received))
            finally:
                stop.set()
                worker.join(3)
            self.assertFalse(worker.is_alive())
            if failure:
                raise failure[0]

    def test_queue_paces_chunks_by_ack(self):
        self.queue_fixture()

    def test_disconnect_stops_without_replay(self):
        self.queue_fixture(error=True)


if __name__ == "__main__":
    unittest.main()
