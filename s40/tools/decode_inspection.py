#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode an offline hwinspect collection; never accesses the phone."""
import argparse
import json
import struct
from pathlib import Path

from boot_trace import decode_traces
from read_ramlog import decode as decode_ramlog


def decode_map(data, virtual_base):
    """Coalesce the bridge's normalized ARMv5 1-KiB page descriptors."""
    if len(data) != 4096:
        raise ValueError("expected a 4096-byte normalized map")
    regions = []
    for index, descriptor in enumerate(struct.unpack("<1024I", data)):
        if not descriptor:
            continue
        if descriptor & 0x3ff != 0x13:
            raise ValueError(f"invalid normalized descriptor at index {index}")
        va, pa = virtual_base + index * 1024, descriptor & 0xfffffc00
        if (regions and regions[-1]['va'] + regions[-1]['bytes'] == va
                and regions[-1]['pa'] + regions[-1]['bytes'] == pa):
            regions[-1]['bytes'] += 1024
        else:
            regions.append(dict(va=va, pa=pa, bytes=1024))
    return [dict(va=f"0x{r['va']:08x}", pa=f"0x{r['pa']:08x}", bytes=r['bytes'])
            for r in regions]


def decode_directory(directory):
    result = {'scope': 'Offline retained diagnostic RAM; maps are not ROM bytes.'}
    errors = {}
    for name in ('boot_trace', 'ramlog', 'secure_state', 'rom_map', 'io_map', 'ffc_map', 'ffe_map'):
        path = directory / (name + '.bin')
        if not path.exists():
            continue
        data = path.read_bytes()
        try:
            if name == 'boot_trace':
                if len(data) != 128:
                    raise ValueError('expected a 128-byte boot trace')
                records = decode_traces(data, physical_base=0)
                if len(records) != 1:
                    raise ValueError('invalid or unsupported boot trace')
                record = records[0]
                # A file offset is not a physical address. The trace ABI carries
                # its own transition base, allowing the actual location below.
                record.pop('trace_pa')
                if record['format'] >= 2:
                    record['trace_pa'] = record['scratch_pa'] + 0xe000
                result[name] = {k: f'0x{v:08x}' for k, v in record.items()}
            elif name == 'ramlog':
                result[name] = decode_ramlog(data).decode('utf-8', errors='backslashreplace')
            elif name == 'secure_state':
                if len(data) != 172:
                    raise ValueError('expected a 172-byte secure state header')
                words = struct.unpack('<43I', data)
                if words[:2] != (0x53574431, 1):
                    raise ValueError('unknown secure state header')
                fields = {'rom_entry': 2, 'block_pa': 11, 'work_pa': 6,
                          'work_bytes': 7, 'captured_firmware_sctlr': 8,
                          'linux_sctlr': 13, 'linux_ttbr': 14, 'linux_dacr': 15,
                          'linux_cpsr': 16, 'transport': 18, 'stage': 19,
                          'callback_calls': 20, 'callback_error': 21,
                          'persistence_requests': 22}
                result[name] = {k: f'0x{words[i]:08x}' for k, i in fields.items()}
            else:
                base = {'rom_map': 0xfff00000, 'io_map': 0xffd00000,
                        'ffc_map': 0xffc00000, 'ffe_map': 0xffe00000}[name]
                result[name] = decode_map(data, base)
        except ValueError as error:
            errors[name] = str(error)
    if errors:
        result['errors'] = errors
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error('collection directory does not exist')
    result = decode_directory(args.directory)
    print(json.dumps(result, indent=2))
    return int(bool(result.get('errors')))


if __name__ == '__main__':
    raise SystemExit(main())
