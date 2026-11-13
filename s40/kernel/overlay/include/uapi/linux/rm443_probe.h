/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RM443_PROBE_H
#define _UAPI_LINUX_RM443_PROBE_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define RM443_PROBE_ABI 2
#define RM443_PROBE_MAX_BUFFERS 32
#define RM443_PROBE_MAX_BUFFER (1024U * 1024U)
#define RM443_PROBE_MAX_TOTAL (4U * 1024U * 1024U)
#define RM443_PROBE_MAX_TRANSFER (64U * 1024U)
#define RM443_PROBE_STACK_WORDS 16
#define RM443_PROBE_CALLBACKS 32
#define RM443_PROBE_ROM_ENTRY 0xffff0809U

/* CALL_ROM supplies the descriptor's physical address in r0. Otherwise all
 * thirteen registers are supplied unchanged. Entry bit 0 set selects Thumb. */
#define RM443_PROBE_CALL_ROM 1U
#define RM443_PROBE_CALL_VIRTUAL 2U /* Raw execution at an owned bridge VA. */
#define RM443_PROBE_READ_PHYSICAL 0U
#define RM443_PROBE_READ_BRIDGE 1U

/* ABI and limits remain available without a working ROM bridge. In that
 * case all eight runtime address/size fields starting at rom_entry are zero. */
struct rm443_probe_info {
	__u32 abi, max_buffers, max_buffer, max_total;
	__u32 max_transfer, stack_words, callbacks, reserved;
	__u32 rom_entry, descriptor_pa, pa_header_pa, callback_va;
	__u32 work_pa, work_bytes, bridge_pa, bridge_bytes;
};

/* Buffers are zeroed, physically contiguous, and owned by this open file.
 * Close releases them; addresses must not be retained by ROM after close. */
struct rm443_probe_buffer {
	__u32 bytes, flags, id, physical;
};

struct rm443_probe_transfer {
	__u32 id, offset, bytes, reserved;
	__aligned_u64 data;
};

struct rm443_probe_callback {
	__u32 args[4], result, stage, reserved[2];
};

struct rm443_probe_call {
	__u32 entry, flags;
	__u32 regs[13];
	__u32 stack_words, stack[RM443_PROBE_STACK_WORDS];
	__u32 descriptor[4];
	__u32 returned[13], cpsr, stage, callback_count, callback_overflow;
	__u32 sequence, reserved;
	__aligned_u64 elapsed_ns;
	struct rm443_probe_callback callbacks[RM443_PROBE_CALLBACKS];
};

/* Width is exactly 1, 2 or 4; address and bytes must be width-aligned.
 * done and fault_address describe a partial read; status is 0 or -errno.
 * A recoverable CPU abort returns status=-EFAULT. A stalled bus is not a
 * recoverable fault and cannot be bounded by a Linux timeout. */
struct rm443_probe_read {
	__u32 address, space, width, bytes;
	__aligned_u64 data;
	__u32 done, fault_address;
	__s32 status;
	__u32 reserved;
};

/* Large objects have no single physical address. Flash views materialize
 * RAM pages only on explicit upload/copy; execution cannot fault in COW pages.
 * No mapping here changes the physical-pointer contract of secure ROM. */
#define RM443_PROBE_OBJECT_CONTIG 0U
#define RM443_PROBE_OBJECT_PAGED 1U
#define RM443_PROBE_OBJECT_FLASH 2U
#define RM443_PROBE_MAX_OBJECT (32U * 1024U * 1024U)
#define RM443_PROBE_MIN_HEADROOM (4U * 1024U * 1024U)
#define RM443_PROBE_PAGE_RAM 1U
#define RM443_PROBE_PAGE_FLASH 2U

struct rm443_probe_object {
	__u32 id, kind, bytes, flags;
	__u32 physical, bridge_va, resident_bytes, page_count;
	__u32 backing_physical, mapped_bytes, reserved[2];
};

struct rm443_probe_page {
	__u32 offset, physical, kind, reserved;
};

/* Start/count are page indexes/capacity; count returns entries written.
 * data points to count rm443_probe_page entries, max 256 per operation. */
struct rm443_probe_pages {
	__u32 id, start, count, reserved;
	__aligned_u64 data;
};

#define RM443_PROBE_BUDGET_SET 1U
struct rm443_probe_budget {
	__u32 flags, limit_bytes, headroom_bytes, used_bytes;
	__u32 available_bytes, max_object_bytes, max_contiguous_bytes, reserved;
};

/* Copy a bounded physical NOR range into an object, without a userspace
 * intermediate image. done/status report partial progress. */
struct rm443_probe_copy {
	__u32 id, offset, physical, bytes;
	__u32 done;
	__s32 status;
	__u32 reserved[2];
};

#define RM443_PROBE_IOC_MAGIC 'R'
#define RM443_PROBE_INFO _IOR(RM443_PROBE_IOC_MAGIC, 0x40, struct rm443_probe_info)
#define RM443_PROBE_ALLOC _IOWR(RM443_PROBE_IOC_MAGIC, 0x41, struct rm443_probe_buffer)
#define RM443_PROBE_FREE _IOW(RM443_PROBE_IOC_MAGIC, 0x42, struct rm443_probe_buffer)
#define RM443_PROBE_UPLOAD _IOW(RM443_PROBE_IOC_MAGIC, 0x43, struct rm443_probe_transfer)
#define RM443_PROBE_DOWNLOAD _IOW(RM443_PROBE_IOC_MAGIC, 0x44, struct rm443_probe_transfer)
#define RM443_PROBE_CALL _IOWR(RM443_PROBE_IOC_MAGIC, 0x45, struct rm443_probe_call)
#define RM443_PROBE_READ _IOWR(RM443_PROBE_IOC_MAGIC, 0x46, struct rm443_probe_read)
#define RM443_PROBE_RECORD _IOR(RM443_PROBE_IOC_MAGIC, 0x47, struct rm443_probe_call)
#define RM443_PROBE_OBJECT_CREATE _IOWR(RM443_PROBE_IOC_MAGIC, 0x48, struct rm443_probe_object)
#define RM443_PROBE_OBJECT_INFO _IOWR(RM443_PROBE_IOC_MAGIC, 0x49, struct rm443_probe_object)
#define RM443_PROBE_MAP _IOWR(RM443_PROBE_IOC_MAGIC, 0x4a, struct rm443_probe_object)
#define RM443_PROBE_UNMAP _IOWR(RM443_PROBE_IOC_MAGIC, 0x4b, struct rm443_probe_object)
#define RM443_PROBE_PAGES _IOWR(RM443_PROBE_IOC_MAGIC, 0x4c, struct rm443_probe_pages)
#define RM443_PROBE_BUDGET _IOWR(RM443_PROBE_IOC_MAGIC, 0x4d, struct rm443_probe_budget)
#define RM443_PROBE_COPY_PHYS _IOWR(RM443_PROBE_IOC_MAGIC, 0x4e, struct rm443_probe_copy)

#endif
