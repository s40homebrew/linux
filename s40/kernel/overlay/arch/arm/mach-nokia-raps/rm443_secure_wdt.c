// SPDX-License-Identifier: GPL-2.0-only
/* Continue the RM-443 PA_SWD service using the captured token and ROM state. */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>
#include "rm443_power.h"
#include "rm443_secure_wdt_abi.h"
#include "rm443_probe_bridge.h"

extern const u8 rm443_swd_bridge_start[], rm443_swd_bridge_end[];
extern const u8 rm443_swd_callback[];

static struct rm443_swd_state *swd;
static struct delayed_work swd_work;
static struct kobject *swd_kobject;
static unsigned int swd_calls, swd_successes, swd_timeout, swd_next;
static unsigned int swd_result, swd_returned_timeout, swd_returned_token;
static int swd_error;
static unsigned long swd_last_ping;
static bool swd_stopped;
DEFINE_MUTEX(rm443_rom_lock);

/* Each page stores four ARMv5 coarse L2 tables. Keeping them in whole
 * allocated pages guarantees 1 KiB physical alignment without requiring
 * any large contiguous allocation. One maximum-size object needs 8 pages. */
#define PROBE_MAP_TABLE_PAGES (RM443_PROBE_MAP_SLOT_BYTES / (4 * SZ_1M))
struct rm443_probe_mapping {
	u32 slot, bytes, pages, table_count;
	u32 *tables[PROBE_MAP_TABLE_PAGES];
};
static struct rm443_probe_mapping *probe_mappings[RM443_PROBE_MAX_BUFFERS];
static bool probe_mappings_active;
static_assert(PAGE_SIZE == SZ_4K);
static_assert(sizeof(struct rm443_probe_mapping) <= PAGE_SIZE);

static_assert(sizeof(struct rm443_swd_state) == 0xac);
static_assert(SWD_LINUX_FCSE >= RM443_SWD_RESPONSE_OFFSET + 8);
static_assert(SWD_LINUX_CONTEXT + sizeof(u32) <= SWD_PROBE_MODE);
static_assert(offsetof(struct rm443_swd_probe_state, regs) + SWD_PROBE_MODE == SWD_PROBE_REGS);
static_assert(offsetof(struct rm443_swd_probe_state, stack) + SWD_PROBE_MODE == SWD_PROBE_STACK);
static_assert(offsetof(struct rm443_swd_probe_state, descriptor) + SWD_PROBE_MODE == SWD_PROBE_DESC);
static_assert(offsetof(struct rm443_swd_probe_state, result) + SWD_PROBE_MODE == SWD_PROBE_RESULT);
static_assert(offsetof(struct rm443_swd_probe_state, cpsr) + SWD_PROBE_MODE == SWD_PROBE_CPSR);
static_assert(offsetof(struct rm443_swd_probe_state, count) + SWD_PROBE_MODE == SWD_PROBE_COUNT);
static_assert(offsetof(struct rm443_swd_probe_state, overflow) + SWD_PROBE_MODE == SWD_PROBE_OVERFLOW);
static_assert(SWD_PROBE_MODE + sizeof(struct rm443_swd_probe_state) <= SWD_PROBE_RECORDS);
static_assert(sizeof(struct rm443_probe_callback) == SWD_PROBE_RECORD_BYTES);
static_assert(RM443_PROBE_CALLBACKS == SWD_PROBE_RECORD_MAX);
static_assert(SWD_PROBE_RECORDS + RM443_PROBE_CALLBACKS * sizeof(struct rm443_probe_callback) <= RM443_SWD_FFC_MAP_OFFSET);

static bool swd_ram_range(u32 address, u32 bytes)
{
	return bytes && address >= 0x10200000 && address < 0x12000000 &&
	       bytes <= 0x12000000 - address;
}

static bool swd_flash_range(u32 address, u32 bytes)
{
	return bytes && address < 0x04000000 && bytes <= 0x04000000 - address;
}

static bool swd_reserved(u32 address, u32 bytes, const char *compatible)
{
	struct device_node *node;
	bool found = false;

	for_each_compatible_node(node, NULL, compatible) {
		struct reserved_mem *memory = of_reserved_mem_lookup(node);
		if (memory && address >= memory->base &&
		    (u64)address + bytes <= memory->base + memory->size) {
			found = true;
			of_node_put(node);
			break;
		}
	}
	return found;
}

static int swd_callback_veneer(u8 *page, u32 offset, u32 target)
{
	u32 arm_offset = ALIGN(offset + 4, 4);

	if ((offset & 1) || offset >= SZ_4K || arm_offset + 8 > SZ_4K ||
	    (target & 3))
		return -EINVAL;
	/* BX PC must itself be word-aligned so its Thumb PC+4 is an ARM
	 * instruction boundary. A leading Thumb NOP handles entries at +2.
	 * LDR PC then jumps to the physical callback without borrowing any
	 * argument register, changing flags, or touching the callback stack. */
	*(u16 *)(page + offset) = 0x46c0; /* mov r8, r8 (ARMv5 Thumb NOP) */
	*(u16 *)(page + arm_offset - 4) = 0x4778; /* bx pc */
	*(u16 *)(page + arm_offset - 2) = 0x46c0; /* skipped Thumb padding */
	*(u32 *)(page + arm_offset) = 0xe51ff004; /* ldr pc, [pc, #-4] */
	*(u32 *)(page + arm_offset + 4) = target;
	return 0;
}

static int __init swd_prepare(struct reserved_mem *memory)
{
	u32 *l1, *callback_l2, *sram_l2, *rom_map, *io_map, *ffc_map, *ffe_map;
	u32 pa, va, callback_offset;
	u8 *block, *callback_page;
	size_t code_bytes = rm443_swd_bridge_end - rm443_swd_bridge_start;
	u32 callback_delta = rm443_swd_callback - rm443_swd_bridge_start;
	unsigned int i;
	bool watchdog_alias = false;

	if (memory->size != RM443_SWD_BYTES || (memory->base & 0x3fff) ||
	    !swd_ram_range(memory->base, memory->size))
		return -EINVAL;
	block = __va(memory->base);
	swd = (void *)block;
	if (swd->magic != RM443_SWD_MAGIC || swd->version != RM443_SWD_VERSION ||
	    swd->capture_status || swd->rom_entry != 0xffff0809 ||
	    !swd_flash_range(swd->descriptor_pa, 16) ||
	    !swd_flash_range(swd->pa_header_pa, swd->pa_header_bytes) ||
	    !swd_ram_range(swd->work_pa, swd->work_bytes) ||
	    swd->work_bytes != 8192 ||
	    !swd_reserved(swd->work_pa, swd->work_bytes, "nokia,rm443-secure-work") ||
	    !(swd->callback_va & 1) || swd->callback_va < 0xf0000000 ||
	    swd->callback_va >= 0xff000000 ||
	    !(swd->firmware_sctlr & 1) || (swd->firmware_sctlr & BIT(7)) ||
	    code_bytes > SZ_4K || callback_delta >= code_bytes)
		return -EINVAL;
	for (i = 0; i < RM443_SWD_POOL_SLOTS; ++i) {
		if (!swd->firmware_alloc_va[i] && !swd->firmware_alloc_pa[i] &&
		    !swd->firmware_alloc_bytes[i])
			continue;
		if (!swd->firmware_alloc_va[i] ||
		    !swd_ram_range(swd->firmware_alloc_pa[i], swd->firmware_alloc_bytes[i]) ||
		    (!swd_reserved(swd->firmware_alloc_pa[i], swd->firmware_alloc_bytes[i],
				   "nokia,rm443-secure-allocation") &&
		     !swd_reserved(swd->firmware_alloc_pa[i], swd->firmware_alloc_bytes[i],
				   "nokia,rm443-secure-work")))
			return -EINVAL;
	}
	rom_map = (void *)(block + RM443_SWD_ROM_MAP_OFFSET);
	io_map = (void *)(block + RM443_SWD_IO_MAP_OFFSET);
	ffc_map = (void *)(block + RM443_SWD_FFC_MAP_OFFSET);
	ffe_map = (void *)(block + RM443_SWD_FFE_MAP_OFFSET);
	for (i = 0; i < 1024; ++i) {
		/* Native capture normalizes every valid mapping to an uncached
		 * ARMv5 tiny page; raw old-table addresses are never accepted. */
		if ((rom_map[i] && (rom_map[i] & 0x3ff) != 0x13) ||
		    (io_map[i] && (io_map[i] & 0x3ff) != 0x13) ||
		    (ffc_map[i] && (ffc_map[i] & 0x3ff) != 0x13) ||
		    (ffe_map[i] && (ffe_map[i] & 0x3ff) != 0x13))
			return -EINVAL;
		if ((io_map[i] & 0xfffffc00) == 0x20040400)
			watchdog_alias = true;
	}
	if (!watchdog_alias ||
	    (rom_map[(swd->rom_entry & 0xfffff) >> 10] & 0xfffffc00) != 0xffff0800)
		return -EINVAL;
	/* The ROM, callback and IO mappings are private to the synchronous call.
	 * Linux continues to own its normal tables and all interrupt handlers. */
	l1 = (void *)(block + RM443_SWD_L1_OFFSET);
	memset(l1, 0, SZ_16K);
	for (pa = 0; pa < 0x04000000; pa += SZ_1M)
		l1[pa >> 20] = pa | 0xc12;
	for (pa = 0x10000000; pa < 0x12000000; pa += SZ_1M) {
		l1[pa >> 20] = pa | 0xc12;
		va = (u32)__va(pa);
		l1[va >> 20] = pa | 0xc12;
	}
	l1[0x200] = 0x20000c12;
	l1[0xfff] = (memory->base + RM443_SWD_ROM_MAP_OFFSET) | 0x13;
	l1[0xffd] = (memory->base + RM443_SWD_IO_MAP_OFFSET) | 0x13;
	l1[0xffc] = (memory->base + RM443_SWD_FFC_MAP_OFFSET) | 0x13;
	l1[0xffe] = (memory->base + RM443_SWD_FFE_MAP_OFFSET) | 0x13;
	/* Keep the known 16KiB internal SRAM accessible at its physical address;
	 * it is outside Linux DDR and was not overwritten during takeover. */
	sram_l2 = (void *)(block + RM443_SWD_SRAM_L2);
	memset(sram_l2, 0, SZ_1K);
	for (i = 0; i < 4; ++i)
		sram_l2[i] = (0x08000000 + i * SZ_4K) | 0xff2;
	l1[0x080] = (memory->base + RM443_SWD_SRAM_L2) | 0x11;
	/* Firmware uses a Thumb callback. Replace its mapping only inside this
	 * table with a Thumb-to-ARM veneer into the copied callback implementation. */
	callback_l2 = (void *)(block + RM443_SWD_CALLBACK_L2);
	memset(callback_l2, 0, SZ_1K);
	callback_l2[(swd->callback_va >> 12) & 255] =
		(memory->base + RM443_SWD_CALLBACK_OFFSET) | 0xff2;
	l1[swd->callback_va >> 20] =
		(memory->base + RM443_SWD_CALLBACK_L2) | 0x11;
	callback_page = block + RM443_SWD_CALLBACK_OFFSET;
	memset(callback_page, 0, SZ_4K);
	callback_offset = swd->callback_va & 0xffe;
	if (swd_callback_veneer(callback_page, callback_offset, memory->base +
			       RM443_SWD_CODE_OFFSET + callback_delta))
		return -EINVAL;
	memcpy(block + RM443_SWD_CODE_OFFSET, rm443_swd_bridge_start, code_bytes);
	memset(block + SWD_PROBE_MODE, 0,
	       SWD_PROBE_RECORDS + RM443_PROBE_CALLBACKS * sizeof(struct rm443_probe_callback) - SWD_PROBE_MODE);
	swd->block_pa = memory->base;
	flush_icache_range((unsigned long)block + RM443_SWD_CODE_OFFSET,
			   (unsigned long)block + RM443_SWD_CALLBACK_OFFSET + SZ_4K);
	return 0;
}

static void swd_refresh(struct work_struct *work)
{
	u8 *request = (u8 *)swd + RM443_SWD_REQUEST_OFFSET;
	u8 *response = (u8 *)swd + RM443_SWD_RESPONSE_OFFSET;
	u32 result, transport, callback_errors;
	unsigned int timeout, token;
	int (*call)(void *, u32) = (void *)swd + RM443_SWD_CODE_OFFSET;

	mutex_lock(&rm443_rom_lock);
	if (swd_stopped)
		goto unlock;
	swd_error = rm443_power_refresh();
	if (swd_error) {
		swd_stopped = true;
		pr_err("RM-443 secure watchdog: ordinary watchdog refresh failed: %d\n", swd_error);
		goto unlock;
	}
	memset(request, 0, 8);
	memset(response, 0, 8);
	request[2] = 0x1f;
	*(u16 *)(request + 4) = swd->token;
	callback_errors = swd->callback_error;
	++swd_calls;
	transport = call(swd, swd->block_pa);
	result = *(u32 *)response;
	timeout = *(u16 *)(response + 4);
	token = *(u16 *)(response + 6);
	swd->stage = 40;
	swd_result = result;
	swd_returned_timeout = timeout;
	swd_returned_token = token;
	pr_info("RM-443 secure watchdog: call %u transport %u status %u timeout %u token %u callbacks %u\n",
		swd_calls, transport, result, timeout, token, swd->callback_calls);
	if (transport != 1 || result != 1 || !timeout ||
	    swd->callback_error != callback_errors) {
		swd_error = -EIO;
		swd_stopped = true;
		pr_err("RM-443 secure watchdog: service failed; stage %u callback errors %u; no retry with an uncertain token\n",
		       swd->stage, swd->callback_error);
		goto unlock;
	}
	swd->token = token;
	swd_timeout = timeout;
	swd_next = timeout > 30 ? timeout - 30 : max(1U, timeout / 2);
	swd_last_ping = jiffies;
	++swd_successes;
	if (swd->persist_calls)
		pr_warn_once("RM-443 secure watchdog: ROM requested persistence; secure state retained in RAM, no PMM writer available\n");
	schedule_delayed_work(&swd_work, swd_next * HZ);
unlock:
	mutex_unlock(&rm443_rom_lock);
}

int rm443_probe_bridge_info(struct rm443_probe_info *info)
{
	lockdep_assert_held(&rm443_rom_lock);
	if (!swd || !swd_successes || swd_stopped)
		return -ENODEV;
	info->rom_entry = swd->rom_entry;
	info->descriptor_pa = swd->descriptor_pa;
	info->pa_header_pa = swd->pa_header_pa;
	info->callback_va = swd->callback_va;
	info->work_pa = swd->work_pa;
	info->work_bytes = swd->work_bytes;
	info->bridge_pa = swd->block_pa;
	info->bridge_bytes = RM443_SWD_BYTES;
	return 0;
}

static u32 *probe_mapping_table(const struct rm443_probe_mapping *mapping,
			       u32 index)
{
	return mapping->tables[index / 4] + (index % 4) * 256;
}

u32 rm443_probe_mapping_cost(u32 bytes)
{
	if (!bytes || bytes > RM443_PROBE_MAP_SLOT_BYTES)
		return 0;
	/* Charge a full page for the small descriptor as a conservative upper
	 * bound, in addition to the exact number of table pages. */
	return PAGE_SIZE + DIV_ROUND_UP(bytes, 4 * SZ_1M) * PAGE_SIZE;
}

u32 rm443_probe_mapping_address(const struct rm443_probe_mapping *mapping)
{
	return RM443_PROBE_MAP_BASE + mapping->slot * RM443_PROBE_MAP_SLOT_BYTES;
}

int rm443_probe_mapping_create(unsigned int slot, u32 bytes,
			       rm443_probe_page_physical get_page, void *opaque,
			       struct rm443_probe_mapping **result)
{
	struct rm443_probe_mapping *mapping;
	u32 index, allocated = 0;
	int error = -ENOMEM;

	lockdep_assert_held(&rm443_rom_lock);
	*result = NULL;
	if (slot >= RM443_PROBE_MAX_BUFFERS || !rm443_probe_mapping_cost(bytes) ||
	    !get_page)
		return -EINVAL;
	if (probe_mappings_active || probe_mappings[slot])
		return -EBUSY;
	mapping = kzalloc(sizeof(*mapping),
			  GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);
	if (!mapping)
		return -ENOMEM;
	mapping->slot = slot;
	mapping->bytes = bytes;
	mapping->pages = DIV_ROUND_UP(bytes, PAGE_SIZE);
	mapping->table_count = DIV_ROUND_UP(bytes, SZ_1M);
	for (index = 0; index < DIV_ROUND_UP(mapping->table_count, 4); index++) {
		mapping->tables[index] = (void *)get_zeroed_page(
			GFP_KERNEL | __GFP_NORETRY | __GFP_NOMEMALLOC | __GFP_NOWARN);
		if (!mapping->tables[index])
			goto failed;
		allocated++;
	}
	for (index = 0; index < mapping->pages; index++) {
		u32 physical;

		error = get_page(opaque, index, &physical);
		if (error)
			goto failed;
		if (physical & (PAGE_SIZE - 1)) {
			error = -EINVAL;
			goto failed;
		}
		probe_mapping_table(mapping, index / 256)[index % 256] = physical | 0xff2;
	}
	probe_mappings[slot] = mapping;
	*result = mapping;
	return 0;
failed:
	while (allocated)
		free_page((unsigned long)mapping->tables[--allocated]);
	kfree(mapping);
	return error;
}

void rm443_probe_mapping_detach_all(void)
{
	u32 slot, index, *l1;

	lockdep_assert_held(&rm443_rom_lock);
	if (!probe_mappings_active)
		return;
	l1 = (void *)swd + RM443_SWD_L1_OFFSET;
	for (slot = 0; slot < RM443_PROBE_MAX_BUFFERS; slot++) {
		struct rm443_probe_mapping *mapping = probe_mappings[slot];

		if (!mapping)
			continue;
		for (index = 0; index < mapping->table_count; index++)
			l1[(rm443_probe_mapping_address(mapping) >> 20) + index] = 0;
	}
	probe_mappings_active = false;
	/* Linux's TTBR is active here. The next bridge entry cleans caches and
	 * invalidates its TLB, so these detached entries cannot survive a call. */
}

void rm443_probe_mapping_destroy(struct rm443_probe_mapping *mapping)
{
	u32 index;

	lockdep_assert_held(&rm443_rom_lock);
	if (!mapping)
		return;
	/* Normally already detached. This also makes close/error cleanup safe
	 * if its caller is unwinding an attached batch in Linux context. */
	rm443_probe_mapping_detach_all();
	probe_mappings[mapping->slot] = NULL;
	for (index = 0; index < DIV_ROUND_UP(mapping->table_count, 4); index++)
		free_page((unsigned long)mapping->tables[index]);
	kfree(mapping);
}

int rm443_probe_mapping_update(struct rm443_probe_mapping *mapping,
			       u32 index, u32 physical)
{
	lockdep_assert_held(&rm443_rom_lock);
	if (!mapping || index >= mapping->pages || (physical & (PAGE_SIZE - 1)))
		return -EINVAL;
	if (probe_mappings_active)
		return -EBUSY;
	probe_mapping_table(mapping, index / 256)[index % 256] = physical | 0xff2;
	return 0;
}

int rm443_probe_mapping_attach_all(void)
{
	u32 slot, index, *l1;

	lockdep_assert_held(&rm443_rom_lock);
	if (!swd || !swd_successes || swd_stopped)
		return -ENODEV;
	if (probe_mappings_active)
		return -EBUSY;
	l1 = (void *)swd + RM443_SWD_L1_OFFSET;
	/* Validate the whole batch before changing any entry. The fixed window
	 * has no permanent mappings; refusing collisions preserves watchdog,
	 * callback, ROM, IO and Linux alias mappings exactly. */
	for (slot = 0; slot < RM443_PROBE_MAX_BUFFERS; slot++) {
		struct rm443_probe_mapping *mapping = probe_mappings[slot];

		if (!mapping)
			continue;
		for (index = 0; index < mapping->table_count; index++)
			if (l1[(rm443_probe_mapping_address(mapping) >> 20) + index])
				return -EBUSY;
	}
	for (slot = 0; slot < RM443_PROBE_MAX_BUFFERS; slot++) {
		struct rm443_probe_mapping *mapping = probe_mappings[slot];

		if (!mapping)
			continue;
		for (index = 0; index < mapping->table_count; index++)
			l1[(rm443_probe_mapping_address(mapping) >> 20) + index] =
				virt_to_phys(probe_mapping_table(mapping, index)) | 0x11;
	}
	probe_mappings_active = true;
	return 0;
}

static u32 *probe_active_mapping_table(u32 address, u32 descriptor, u32 *remaining)
{
	struct rm443_probe_mapping *mapping;
	u32 slot, offset;

	if (!probe_mappings_active || address < RM443_PROBE_MAP_BASE ||
	    address - RM443_PROBE_MAP_BASE >=
		RM443_PROBE_MAX_BUFFERS * RM443_PROBE_MAP_SLOT_BYTES)
		return NULL;
	slot = (address - RM443_PROBE_MAP_BASE) / RM443_PROBE_MAP_SLOT_BYTES;
	mapping = probe_mappings[slot];
	if (!mapping)
		return NULL;
	offset = address - rm443_probe_mapping_address(mapping);
	if (offset >= mapping->bytes)
		return NULL;
	if (descriptor != (virt_to_phys(probe_mapping_table(mapping, offset >> 20)) | 0x11))
		return NULL;
	*remaining = mapping->bytes - offset;
	return probe_mapping_table(mapping, offset >> 20);
}

int rm443_probe_translate(u32 address, u32 *physical, u32 *contiguous)
{
	u32 *l1, *l2, descriptor, table, offset, mask;
	u32 remaining = ~0U;

	lockdep_assert_held(&rm443_rom_lock);
	if (!swd || !swd_successes || swd_stopped)
		return -ENODEV;
	l1 = (void *)swd + RM443_SWD_L1_OFFSET;
	descriptor = l1[address >> 20];
	if (address >= RM443_PROBE_MAP_BASE &&
	    address - RM443_PROBE_MAP_BASE <
		RM443_PROBE_MAX_BUFFERS * RM443_PROBE_MAP_SLOT_BYTES) {
		/* A view address must resolve through exactly its registered table,
		 * never through an arbitrary replacement section/table descriptor. */
		l2 = probe_active_mapping_table(address, descriptor, &remaining);
		if (!l2)
			return -EFAULT;
		descriptor = l2[(address >> 12) & 255];
		if ((descriptor & 3) != 2)
			return -EFAULT;
		mask = SZ_4K - 1;
		goto resolved;
	}
	if ((descriptor & 3) == 2) {
		mask = SZ_1M - 1;
	} else if ((descriptor & 3) == 1 || (descriptor & 3) == 3) {
		/* Only the private block's tables are accepted. Never follow a
		 * descriptor into arbitrary physical memory while inspecting it. */
		bool fine = (descriptor & 3) == 3;
		u32 bytes = fine ? SZ_4K : SZ_1K;

		table = descriptor & ~(bytes - 1);
		if (table < swd->block_pa ||
		    table - swd->block_pa > RM443_SWD_BYTES - bytes)
			return -EFAULT;
		l2 = (void *)swd + table - swd->block_pa;
		descriptor = l2[fine ? (address >> 10) & 1023 : (address >> 12) & 255];
		if (fine && (descriptor & 3) == 3)
			mask = SZ_1K - 1;
		else if (!fine && (descriptor & 3) == 2)
			mask = SZ_4K - 1;
		else
			return -EFAULT;
	} else {
		return -EFAULT;
	}
resolved:
	offset = address & mask;
	*physical = (descriptor & ~mask) | offset;
	*contiguous = min(mask + 1 - offset, remaining);
	return 0;
}

int rm443_probe_execute(struct rm443_probe_call *call)
{
	struct rm443_swd_probe_state *probe;
	unsigned long irq_flags;
	u32 saved_stage, saved_transport;
	u64 started;
	int error;
	int (*bridge)(void *, u32);

	lockdep_assert_held(&rm443_rom_lock);
	if (!swd || !swd_successes || swd_stopped)
		return -ENODEV;
	if (call->stack_words > RM443_PROBE_STACK_WORDS ||
	    call->flags & ~(RM443_PROBE_CALL_ROM | RM443_PROBE_CALL_VIRTUAL) ||
	    call->flags == (RM443_PROBE_CALL_ROM | RM443_PROBE_CALL_VIRTUAL) || call->reserved)
		return -EINVAL;
	if (!call->entry) {
		if (!(call->flags & RM443_PROBE_CALL_ROM))
			return -EINVAL;
		call->entry = RM443_PROBE_ROM_ENTRY;
	}
	if (!(call->entry & 1) && (call->entry & 3))
		return -EINVAL;
	probe = (void *)swd + SWD_PROBE_MODE;
	memset(probe, 0, sizeof(*probe));
	memset((void *)swd + SWD_PROBE_RECORDS, 0, sizeof(call->callbacks));
	probe->entry = call->entry;
	memcpy(probe->regs, call->regs, sizeof(probe->regs));
	memcpy(probe->stack, call->stack, call->stack_words * sizeof(u32));
	memcpy(probe->descriptor, call->descriptor, sizeof(probe->descriptor));
	if (call->flags & RM443_PROBE_CALL_ROM)
		probe->regs[0] = swd->block_pa + SWD_PROBE_DESC;
	/* Ordinary watchdog feeding and all staging happen with interrupts
	 * enabled. No allocation, user copy or Linux API runs in the bridge. */
	error = rm443_power_refresh();
	if (error)
		return error;
	saved_stage = swd->stage;
	saved_transport = swd->transport;
	probe->mode = 1;
	bridge = (void *)swd + RM443_SWD_CODE_OFFSET;
	preempt_disable();
	local_irq_save(irq_flags);
	started = ktime_get_ns();
	bridge(swd, swd->block_pa);
	call->elapsed_ns = ktime_get_ns() - started;
	local_irq_restore(irq_flags);
	preempt_enable();
	probe->mode = 0;
	memcpy(call->returned, probe->result, sizeof(call->returned));
	call->cpsr = probe->cpsr;
	call->stage = swd->stage;
	call->callback_count = probe->count;
	call->callback_overflow = probe->overflow;
	memcpy(call->callbacks, (void *)swd + SWD_PROBE_RECORDS, sizeof(call->callbacks));
	swd->stage = saved_stage;
	swd->transport = saved_transport;
	return 0;
}

static ssize_t status_show(struct kobject *kobject, struct kobj_attribute *attr,
			   char *buffer)
{
	return sysfs_emit(buffer,
		"calls=%u successes=%u stopped=%u error=%d transport=%u stage=%u\n"
		"returned_status=%u returned_timeout=%u returned_token=%u\n"
		"timeout=%u next_interval=%u token=%u last_success_age=%lu\n"
		"callbacks=%u callback_errors=%u persistence_requests=%u\n",
		swd_calls, swd_successes, swd_stopped, swd_error, swd->transport, swd->stage,
		swd_result, swd_returned_timeout, swd_returned_token,
		swd_timeout, swd_next, swd->token,
		swd_successes ? (jiffies - swd_last_ping) / HZ : 0,
		swd->callback_calls, swd->callback_error, swd->persist_calls);
}
static struct kobj_attribute swd_status = __ATTR_RO(status);

static int swd_shutdown(struct notifier_block *block, unsigned long action,
			void *unused)
{
	mutex_lock(&rm443_rom_lock);
	swd_stopped = true;
	mutex_unlock(&rm443_rom_lock);
	cancel_delayed_work_sync(&swd_work);
	return NOTIFY_DONE;
}
static struct notifier_block swd_reboot = { .notifier_call = swd_shutdown };

static int __init rm443_secure_wdt_init(void)
{
	struct device_node *node;
	struct reserved_mem *memory;
	int error;

	node = of_find_compatible_node(NULL, NULL, "nokia,rm443-secure-watchdog");
	if (!node) {
		pr_warn("RM-443 secure watchdog: no captured firmware state\n");
		return -ENODEV;
	}
	memory = of_reserved_mem_lookup(node);
	of_node_put(node);
	if (!memory)
		return -EINVAL;
	error = swd_prepare(memory);
	if (error) {
		pr_err("RM-443 secure watchdog: handoff state rejected: %d\n", error);
		return error;
	}
	INIT_DELAYED_WORK(&swd_work, swd_refresh);
	error = register_reboot_notifier(&swd_reboot);
	if (error)
		return error;
	swd_kobject = kobject_create_and_add("rm443_secure_watchdog", kernel_kobj);
	if (swd_kobject && sysfs_create_file(swd_kobject, &swd_status.attr))
		pr_warn("RM-443 secure watchdog: status file unavailable\n");
	pr_info("RM-443 secure watchdog: retained token %u, work buffer %08x+%x; initial service before peripheral startup\n",
		swd->token, swd->work_pa, swd->work_bytes);
	/* USB is ready, but MMC and DRM have not started. Complete the first
	 * ROM transition here instead of interrupting their initialization or
	 * an in-flight transfer one second later. The successful call queues
	 * its normal renewal, using the timeout returned by the ROM. */
	swd_refresh(&swd_work.work);
	return swd_error;
}
device_initcall(rm443_secure_wdt_init);
