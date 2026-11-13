/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RM443_SECURE_WDT_ABI_H
#define RM443_SECURE_WDT_ABI_H

/* Shared by the S40 capture, Linux driver and copied ARM926 call bridge. */
#define RM443_SWD_MAGIC             0x53574431
#define RM443_SWD_VERSION           1
#define RM443_SWD_BYTES             0x20000
#define RM443_SWD_FFC_MAP_OFFSET    0x1000
#define RM443_SWD_FFE_MAP_OFFSET    0x2000
#define RM443_SWD_L1_OFFSET         0x4000
#define RM443_SWD_ROM_MAP_OFFSET    0x8000
#define RM443_SWD_IO_MAP_OFFSET     0x9000
#define RM443_SWD_CALLBACK_L2       0xa000
#define RM443_SWD_SRAM_L2           0xa400
#define RM443_SWD_CODE_OFFSET       0xc000
#define RM443_SWD_CALLBACK_OFFSET   0xd000
#define RM443_SWD_STACK_TOP         0x10000
#define RM443_SWD_POOL_OFFSET       0x10000
#define RM443_SWD_POOL_SLOT_BYTES   0x4000
#define RM443_SWD_POOL_SLOTS        4
#define RM443_SWD_REQUEST_OFFSET    0x100
#define RM443_SWD_RESPONSE_OFFSET   0x108

#define SWD_MAGIC          0x00
#define SWD_VERSION        0x04
#define SWD_ROM_ENTRY      0x08
#define SWD_DESCRIPTOR     0x0c
#define SWD_PA_HEADER      0x10
#define SWD_CALLBACK_VA    0x14
#define SWD_WORK_PA        0x18
#define SWD_WORK_BYTES     0x1c
#define SWD_FW_CONTROL     0x20
#define SWD_TOKEN          0x24
#define SWD_CAPTURE_STATUS 0x28
#define SWD_BLOCK_PA       0x2c
#define SWD_LINUX_SP       0x30
#define SWD_LINUX_CONTROL  0x34
#define SWD_LINUX_TTBR     0x38
#define SWD_LINUX_DOMAINS  0x3c
#define SWD_LINUX_CPSR     0x40
#define SWD_RETURN_VA      0x44
#define SWD_TRANSPORT      0x48
#define SWD_STAGE          0x4c
#define SWD_CALLBACK_CALLS 0x50
#define SWD_CALLBACK_ERROR 0x54
#define SWD_PERSIST_CALLS  0x58
#define SWD_ALLOCATIONS    0x60
#define SWD_FIRMWARE_ALLOC_VA 0x74
#define SWD_FIRMWARE_ALLOC_PA 0x84
#define SWD_SYSTEM_SP      0xa4
#define SWD_SYSTEM_LR      0xa8
#define SWD_LINUX_FCSE      0x180
#define SWD_LINUX_CONTEXT   0x184

/* Optional experiment state occupies only unused space in the first page.
 * Existing capture, retained work, callback pools and page tables are intact. */
#define SWD_PROBE_MODE      0x200
#define SWD_PROBE_ENTRY     0x204
#define SWD_PROBE_REGS      0x208
#define SWD_PROBE_STACK     0x23c
#define SWD_PROBE_DESC      0x27c
#define SWD_PROBE_RESULT    0x28c
#define SWD_PROBE_CPSR      0x2c0
#define SWD_PROBE_COUNT     0x2c4
#define SWD_PROBE_OVERFLOW  0x2c8
#define SWD_PROBE_RECORDS   0x400
#define SWD_PROBE_RECORD_MAX 32
#define SWD_PROBE_RECORD_BYTES 32

#ifndef __ASSEMBLY__
#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 rm443_swd_u32;
#else
#include <stdint.h>
typedef uint32_t rm443_swd_u32;
#endif
struct rm443_swd_state {
	rm443_swd_u32 magic, version, rom_entry, descriptor_pa;
	rm443_swd_u32 pa_header_pa, callback_va, work_pa, work_bytes;
	rm443_swd_u32 firmware_sctlr, token, capture_status, block_pa;
	rm443_swd_u32 linux_sp, linux_control, linux_ttbr, linux_domains;
	rm443_swd_u32 linux_cpsr, return_va, transport, stage;
	rm443_swd_u32 callback_calls, callback_error, persist_calls, reserved;
	rm443_swd_u32 allocations[RM443_SWD_POOL_SLOTS];
	rm443_swd_u32 pa_header_bytes;
	rm443_swd_u32 firmware_alloc_va[RM443_SWD_POOL_SLOTS];
	rm443_swd_u32 firmware_alloc_pa[RM443_SWD_POOL_SLOTS];
	rm443_swd_u32 firmware_alloc_bytes[RM443_SWD_POOL_SLOTS];
	rm443_swd_u32 system_sp, system_lr;
};

struct rm443_swd_probe_state {
	rm443_swd_u32 mode, entry, regs[13], stack[16], descriptor[4];
	rm443_swd_u32 result[13], cpsr, count, overflow;
};
#endif
#endif
