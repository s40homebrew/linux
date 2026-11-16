#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a new ext4 file for loop mounting on an existing FAT card.

This creates only the named regular file, never formats a disk/card, never
overwrites an existing path, and does not install or switch a Linux rootfs.
"""
import argparse
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--size-mib', type=int, default=64)
    parser.add_argument('--mke2fs', help='explicit e2fsprogs mke2fs executable')
    args = parser.parse_args()
    if not 8 <= args.size_mib <= 4095:
        parser.error('size must be 8..4095 MiB (below FAT individual-file limits)')
    candidates = [args.mke2fs, shutil.which('mke2fs'),
                  '/opt/homebrew/opt/e2fsprogs/sbin/mke2fs', '/usr/sbin/mke2fs',
                  str(Path.home() / 'Library/Android/sdk/platform-tools/mke2fs')]
    formatter = next((str(Path(p).resolve()) for p in candidates if p and Path(p).is_file()), None)
    if not formatter:
        parser.error('mke2fs not found; pass --mke2fs /path/to/mke2fs')
    # Exclusive creation also refuses device nodes, symlinks and existing images.
    with args.output.open('xb') as stream:
        stream.truncate(args.size_mib * 1024 * 1024)
    subprocess.run([formatter, '-t', 'ext4', '-F', '-q', '-b', '1024',
                    '-L', 'Linux6303', '-E', 'lazy_itable_init=0,lazy_journal_init=0',
                    str(args.output.resolve())], check=True)
    print(f'Created {args.output.resolve()} ({args.size_mib} MiB ext4).')
    print('Copy this file onto the card; mount it explicitly with mount -t ext4 -o loop.')


if __name__ == '__main__':
    main()
