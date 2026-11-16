#!/usr/bin/env python3
"""Exercise persistent logger lifecycle without a phone or block device."""
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SdlogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="sdlog-build-")
        cls.binary = Path(cls.build.name) / "sdlog"
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-O2", str(ROOT / "kernel/userspace/sdlog.c"),
                        "-o", str(cls.binary)], check=True)
        # Model /dev/kmsg record reads rather than regular-file 8 KiB chunks.
        shim = Path(cls.build.name) / "record_read.c"
        shim.write_text(r'''
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
ssize_t sdlog_test_read(int fd, void *buf, size_t size) {
    struct stat st;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode)) {
        if (getenv("SDLOG_TEST_DELAY")) usleep(50000);
        if (getenv("SDLOG_TEST_OVERRUN")) { errno = EPIPE; return -1; }
        if (size > 24) size = 24;
        if (getenv("SDLOG_TEST_FLOOD")) { memset(buf, 'x', size); return size; }
    }
    return read(fd, buf, size);
}
''')
        obj = Path(cls.build.name) / "sdlog.o"
        cls.record_binary = Path(cls.build.name) / "sdlog-records"
        subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror",
                        "-O2", "-Dread=sdlog_test_read", "-c",
                        str(ROOT / "kernel/userspace/sdlog.c"), "-o", str(obj)], check=True)
        subprocess.run([os.environ.get("CC", "cc"), str(obj), str(shim),
                        "-o", str(cls.record_binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.workspace = tempfile.TemporaryDirectory(prefix="sdlog-test-")
        self.path = Path(self.workspace.name)
        self.directory = self.path / "logs"
        self.input = self.path / "kmsg"
        self.input.write_bytes(b"6,1,100,-;early kernel record\n")
        self.socket = self.path / "control.sock"
        self.process = None

    def tearDown(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=5)
        if self.process:
            self.process.stdout.close()
            self.process.stderr.close()
        self.workspace.cleanup()

    def start(self, binary=None):
        self.process = subprocess.Popen(
            [str(binary or self.binary), "--directory", str(self.directory),
             "--input", str(self.input), "--socket", str(self.socket)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertTrue(select.select([self.process.stdout], [], [], 5)[0])
        self.assertEqual(self.process.stdout.read(1), b"R")

    def control(self, option):
        return subprocess.run([str(self.binary), "--socket", str(self.socket), option],
                              capture_output=True, timeout=5)

    def test_backlog_checkpoint_stop_and_unique_boot(self):
        self.start()
        first = self.directory / "boot-000001.kmsg"
        self.assertIn(b"early kernel record", first.read_bytes())
        with self.input.open("ab", buffering=0) as stream:
            stream.write(b"6,2,200,-;before DMA\n")
        self.assertEqual(self.control("--checkpoint").returncode, 0)
        self.assertIn(b"before DMA", first.read_bytes())
        self.assertEqual(self.control("--stop").returncode, 0)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertFalse(self.socket.exists())
        saved = first.read_bytes()
        self.process.stdout.close()
        self.process.stderr.close()
        self.start()
        self.assertEqual(first.read_bytes(), saved)
        self.assertTrue((self.directory / "boot-000002.kmsg").exists())
        self.assertEqual(self.control("--stop").returncode, 0)
        self.assertEqual(self.process.wait(timeout=5), 0)
        self.assertEqual(self.control("--stop").returncode, 0)

    def test_storage_budget_refuses_without_overwrite(self):
        self.directory.mkdir()
        saved = self.directory / "boot-000001.kmsg"
        with saved.open("wb") as stream:
            stream.truncate(16 * 1024 * 1024)
        result = subprocess.run(
            [str(self.binary), "--directory", str(self.directory),
             "--input", str(self.input), "--socket", str(self.socket)],
            capture_output=True, timeout=5)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(b"R", result.stdout)
        self.assertEqual(saved.stat().st_size, 16 * 1024 * 1024)
        self.assertEqual(len(list(self.directory.iterdir())), 1)
        self.assertFalse(self.socket.exists())

    def test_per_boot_limit_stops_capture(self):
        self.start()
        with self.input.open("ab", buffering=0) as stream:
            stream.write(b"x" * (1024 * 1024 + 1))
        self.assertNotEqual(self.process.wait(timeout=5), 0)
        self.assertLessEqual((self.directory / "boot-000001.kmsg").stat().st_size,
                             1024 * 1024)
        self.assertFalse(self.socket.exists())

    def test_ready_and_checkpoint_drain_more_than_256_records(self):
        self.input.write_bytes(b"initial record\n" * 700 + b"LAST_INITIAL_RECORD\n")
        self.start(self.record_binary)
        log = self.directory / "boot-000001.kmsg"
        self.assertIn(b"LAST_INITIAL_RECORD", log.read_bytes())
        with self.input.open("ab", buffering=0) as stream:
            stream.write(b"new record\n" * 700 + b"BEFORE_DMA_CHECKPOINT\n")
        self.assertEqual(self.control("--checkpoint").returncode, 0)
        self.assertIn(b"BEFORE_DMA_CHECKPOINT", log.read_bytes())

    def test_flood_and_ring_overrun_never_acknowledge_ready(self):
        for flag in ("SDLOG_TEST_FLOOD", "SDLOG_TEST_OVERRUN"):
            with self.subTest(flag=flag):
                result = subprocess.run(
                    [str(self.record_binary), "--directory", str(self.directory),
                     "--input", str(self.input), "--socket", str(self.socket)],
                    capture_output=True, timeout=5, env={**os.environ, flag: "1"})
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"R", result.stdout)
                self.assertFalse(self.socket.exists())

    def test_cancel_initial_drain_never_acknowledges_ready(self):
        self.input.write_bytes(b"initial record\n" * 700)
        self.process = subprocess.Popen(
            [str(self.record_binary), "--directory", str(self.directory),
             "--input", str(self.input), "--socket", str(self.socket)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env={**os.environ, "SDLOG_TEST_DELAY": "1"})
        deadline = time.monotonic() + 5
        while not (self.directory / "boot-000001.kmsg").exists():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.01)
        self.process.terminate()
        self.assertNotEqual(self.process.wait(timeout=5), 0)
        self.assertNotIn(b"R", self.process.stdout.read())
        self.assertFalse(self.socket.exists())


if __name__ == "__main__":
    unittest.main()
