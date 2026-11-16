#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture LCD boot stages from the running emulator."""
import argparse
import json
import shutil
import sys
import time
from pathlib import Path

from port_paths import EMULATOR_ROOT
sys.path.insert(0, str(EMULATOR_ROOT / 'scripts'))
from capture_nor import QMP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qmp', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=float, default=25)
    parser.add_argument('--start-key', help='optional QEMU key, e.g. ret on the Enter Linux menu item')
    args = parser.parse_args()
    if not 0 < args.seconds <= 60:
        parser.error('--seconds must be in (0,60]')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    frames = []
    previous = None
    start = time.monotonic()
    qmp = QMP(args.qmp)
    try:
        if args.start_key:
            qmp.command('send-key', keys=[{'type': 'qcode', 'data': args.start_key}],
                        **{'hold-time': 100})
        while time.monotonic() - start < args.seconds:
            current = out / 'current.png'
            qmp.command('screendump', filename=str(current), format='png')
            data = current.read_bytes()
            if data != previous:
                name = f'frame-{len(frames):03d}.png'
                shutil.copyfile(current, out / name)
                frames.append({'seconds': round(time.monotonic() - start, 3), 'file': name})
                (out / 'frames.json').write_text(json.dumps(frames, indent=2) + '\n')
                previous = data
            time.sleep(.08)
    finally:
        qmp.close()
    print(f'Captured {len(frames)} changed frames in {out}')


if __name__ == '__main__':
    main()
