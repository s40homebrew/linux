#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode boot traces from a physical RAM capture."""
import struct

FIELDS = ('magic0', 'magic1', 'format', 'stage', 'base_pa', 'bytes', 'log_pa',
          'image_pa', 'stub_va', 'stub_pa', 'ttbr_pa', 'dtb_pa', 'old_ttbr', 'old_domains')
EXTENDED = ('source_pa', 'scratch_pa', 'scratch_bytes', 'quiesce_status', 'copied_bytes',
            'relocation_flags', 'dsp_before', 'dsp_after', 'quiesce_done',
            'entry_cpsr', 'entry_sctlr', 'entry_ttbr', 'entry_dacr', 'entry_fcse')
INTEGRITY = ('expected_hash', 'native_hash', 'source_hash', 'destination_hash')


def decode_traces(memory, physical_base=0x10000000):
    records = []
    start = 0
    while (position := memory.find(b'S40LNX01', start)) >= 0:
        start = position + 1
        if position + 56 > len(memory):
            continue
        header = struct.unpack_from('<14I', memory, position)
        if header[2] not in (1, 2, 3) or not 1 <= header[3] <= 4:
            continue
        record = dict(zip(FIELDS, header))
        if header[2] >= 2:
            if position + 112 > len(memory):
                continue
            record.update(zip(EXTENDED, struct.unpack_from('<14I', memory, position + 56)))
        if header[2] >= 3:
            if position + 128 > len(memory):
                continue
            record.update(zip(INTEGRITY, struct.unpack_from('<4I', memory, position + 112)))
        record['trace_pa'] = physical_base + position
        records.append(record)
    return records


def relocation_checks(plan, image_bytes):
    if plan.get('format') not in (2, 3):
        return {'versioned_relocation_trace': False}
    start, size = plan['scratch_pa'], plan['scratch_bytes']
    checks = {
        'versioned_relocation_trace': True,
        'declared_ddr_bank': (plan['base_pa'], plan['bytes']) in
            ((0x10000000, 0x2000000), (0x10200000, 0x1e00000)),
        'kernel_relocated_to_bank_base': plan['image_pa'] == plan['base_pa'] + 0x8000,
        'private_transition_retained': size == 65536 and start >= 0x10000000
            and start + size <= 0x12000000
            and plan['trace_pa'] == start + 0xe000
            and plan['dtb_pa'] == start and plan['log_pa'] == start + 0xf000,
        'physical_image_copy_complete': plan['copied_bytes'] == image_bytes and plan['relocation_flags'] == 3,
        'early_quiesce_complete': plan['quiesce_status'] == 0 and plan['quiesce_done'] == 15,
        'modeled_dsp_held': plan['dsp_after'] == plan['dsp_before'] & ~1,
        'kernel_entry_cpu_contract': plan['entry_cpsr'] & 0xff == 0xd3
            and plan['entry_sctlr'] & 0x100f == 0 and plan['entry_fcse'] == 0
            and plan['entry_dacr'] == 1 and plan['entry_ttbr'] == plan['ttbr_pa'],
    }
    if plan['format'] >= 3:
        checks['image_integrity_at_each_boundary'] = all(
            plan[name] == plan['expected_hash'] for name in INTEGRITY[1:])
        checks['dsp_low_bank_excluded'] = plan['base_pa'] >= 0x10200000
    return checks
