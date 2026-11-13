// SPDX-License-Identifier: GPL-2.0-only
/* Bounded retained-state diagnostics. Never interpret user supplied addresses. */
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/kobject.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <asm/memory.h>
#include "rm443_inspect.h"
#include "rm443_inspect_cpu.h"
#include "rm443_secure_wdt_abi.h"

#define TRACE_BYTES 128
#define TRACE_OFFSET 0xe000
#define TRANSITION_BYTES 0xf000

static bool enabled;
static struct kobject *inspect_kobj;
static phys_addr_t transition_pa, secure_pa, ramlog_pa;
static u8 trace_snapshot[TRACE_BYTES] __aligned(4);
static const struct rm443_swd_state *secure_state;
static bool trace_valid, ramlog_valid;

static int __init inspect_option(char *value)
{
	return kstrtobool(value, &enabled);
}
early_param("rm443.inspect", inspect_option);

/* Exclude DSP-shared DDR, ROM, internal SRAM and MMIO even if DT is malformed. */
static bool inspect_range(u64 base, u64 bytes)
{
	return base >= 0x10200000 && base < 0x12000000 &&
	       bytes && bytes <= 0x12000000 - base;
}

static bool __init inspect_reserved(struct device_node *node,
				    struct reserved_mem *memory, u64 bytes)
{
	unsigned long pfn, end;

	if (!memory || memory->size != bytes || (memory->base & (PAGE_SIZE - 1)) ||
	    of_property_read_bool(node, "no-map") ||
	    !inspect_range(memory->base, bytes) ||
	    !memblock_is_region_memory(memory->base, bytes))
		return false;
	end = PHYS_PFN(memory->base + bytes);
	for (pfn = PHYS_PFN(memory->base); pfn < end; pfn++)
		if (!pfn_valid(pfn) || !PageReserved(pfn_to_page(pfn)))
			return false;
	return true;
}

static bool inspect_trace(const u32 *trace, u32 base, u32 log)
{
	return trace[0] == 0x4c303453 && trace[1] == 0x3130584e &&
	       (trace[2] == 2 || trace[2] == 3) && trace[6] == log &&
	       trace[15] == base && trace[16] == 0x10000 &&
	       inspect_range(trace[4], trace[5]);
}

static ssize_t inspect_read(struct file *file, struct kobject *kobj,
			    struct bin_attribute *attr, char *buffer,
			    loff_t offset, size_t count)
{
	unsigned long flags;
	void *snapshot;
	int ret;

	if (offset < 0)
		return -EINVAL;
	if ((u64)offset >= attr->size)
		return 0;
	count = min_t(size_t, count, attr->size - offset);
	if (!count)
		return 0;
	if (!strcmp(attr->attr.name, "boot_trace")) {
		memcpy(buffer, trace_snapshot + offset, count);
		return count;
	}
	if (!strcmp(attr->attr.name, "ramlog")) {
		/* Allocate outside the console's IRQ-off lock. A 4KiB read is atomic
		 * with respect to append; separate partial reads are separate samples. */
		snapshot = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!snapshot)
			return -ENOMEM;
		ret = rm443_ram_log_snapshot(snapshot, PAGE_SIZE);
		if (!ret)
			memcpy(buffer, snapshot + offset, count);
		kfree(snapshot);
		return ret ? ret : count;
	}
	/* The PA_SWD service and its Linux caller execute on this one CPU.
	 * Sample only the header (172 bytes) or immutable captured map bytes;
	 * never expose the live secure-call stack, allocator or whole block. */
	local_irq_save(flags);
	memcpy(buffer, (const u8 *)secure_state + (unsigned long)attr->private + offset, count);
	local_irq_restore(flags);
	return count;
}

#define INSPECT_BIN(_name, bytes, displacement) \
	static struct bin_attribute bin_##_name = { \
		.attr = { .name = #_name, .mode = 0400 }, \
		.size = bytes, .read = inspect_read, .private = (void *)(displacement), \
	}
INSPECT_BIN(boot_trace, TRACE_BYTES, 0);
INSPECT_BIN(ramlog, PAGE_SIZE, 0);
INSPECT_BIN(secure_state, sizeof(struct rm443_swd_state), 0);
INSPECT_BIN(rom_map, PAGE_SIZE, RM443_SWD_ROM_MAP_OFFSET);
INSPECT_BIN(io_map, PAGE_SIZE, RM443_SWD_IO_MAP_OFFSET);
INSPECT_BIN(ffc_map, PAGE_SIZE, RM443_SWD_FFC_MAP_OFFSET);
INSPECT_BIN(ffe_map, PAGE_SIZE, RM443_SWD_FFE_MAP_OFFSET);

static struct bin_attribute *inspect_bins[] = {
	&bin_boot_trace, &bin_ramlog, &bin_secure_state,
	&bin_rom_map, &bin_io_map, &bin_ffc_map, &bin_ffe_map,
};

static ssize_t layout_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buffer)
{
	return sysfs_emit(buffer,
		"CPU and retained boot snapshots; read_only=1\n"
		"boot_trace available=%u pa=%pa bytes=%u immutable_boot_snapshot=1\n"
		"ramlog available=%u pa=%pa bytes=%lu console_uncached_alias=1\n"
		"secure_state available=%u pa=%pa bytes=%zu\n"
		"rom_map io_map ffc_map ffe_map: normalized captured page tables, not mapped contents; 4096 bytes each\n"
		"partial reads are separate samples; prefer one complete read per file\n"
		"secure_state is a point-in-time raw header, not a completed-service transaction\n",
		trace_valid, &transition_pa, TRACE_BYTES,
		ramlog_valid, &ramlog_pa, PAGE_SIZE,
		secure_state != NULL, &secure_pa, sizeof(struct rm443_swd_state));
}
static struct kobj_attribute layout_attr = __ATTR(layout, 0400, layout_show, NULL);

static ssize_t cpu_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buffer)
{
	return rm443_inspect_cpu(buffer);
}
static struct kobj_attribute cpu_attr = __ATTR(cpu, 0400, cpu_show, NULL);

static int __init rm443_inspect_init(void)
{
	struct device_node *parent, *node;
	struct reserved_mem *memory;
	unsigned int i, created = 0;
	int ret;

	BUILD_BUG_ON(sizeof(struct rm443_swd_state) != 172);
	if (!enabled)
		return 0;
	parent = of_find_node_by_path("/reserved-memory");
	if (!parent)
		return -ENODEV;
	for_each_available_child_of_node(parent, node) {
		memory = of_reserved_mem_lookup(node);
		if (of_node_name_eq(node, "transition") &&
		    inspect_reserved(node, memory, TRANSITION_BYTES) &&
		    !(memory->base & 0x3fff))
			transition_pa = memory->base;
		else if (of_node_name_eq(node, "ramlog") &&
			 inspect_reserved(node, memory, PAGE_SIZE) &&
			 memory->base == rm443_ram_log_address()) {
			ramlog_pa = memory->base;
			ramlog_valid = true;
		} else if (of_device_is_compatible(node, "nokia,rm443-secure-watchdog") &&
			   inspect_reserved(node, memory, RM443_SWD_BYTES) &&
			   !(memory->base & 0x3fff) &&
			   !(rm443_ram_log_address() >= memory->base &&
			     rm443_ram_log_address() < memory->base + memory->size)) {
			const struct rm443_swd_state *state = __va(memory->base);
			if (READ_ONCE(state->magic) == RM443_SWD_MAGIC &&
			    READ_ONCE(state->version) == RM443_SWD_VERSION &&
			    !READ_ONCE(state->capture_status) &&
			    READ_ONCE(state->block_pa) == memory->base) {
				secure_pa = memory->base;
				secure_state = state;
			}
		}
	}
	of_node_put(parent);
	if (transition_pa && ramlog_valid && ramlog_pa == transition_pa + TRANSITION_BYTES) {
		memcpy(trace_snapshot, __va(transition_pa + TRACE_OFFSET), TRACE_BYTES);
		trace_valid = inspect_trace((const u32 *)trace_snapshot, transition_pa, ramlog_pa);
	}
	/* Report the record address, not the start of its larger reservation. */
	if (transition_pa)
		transition_pa += TRACE_OFFSET;
	inspect_kobj = kobject_create_and_add("rm443_inspect", firmware_kobj);
	if (!inspect_kobj)
		return -ENOMEM;
	ret = sysfs_create_file(inspect_kobj, &layout_attr.attr);
	if (ret)
		goto fail;
	ret = sysfs_create_file(inspect_kobj, &cpu_attr.attr);
	if (ret)
		goto fail;
	for (i = 0; i < ARRAY_SIZE(inspect_bins); i++) {
		if ((i == 0 && !trace_valid) || (i == 1 && !ramlog_valid) ||
		    (i >= 2 && !secure_state))
			continue;
		ret = sysfs_create_bin_file(inspect_kobj, inspect_bins[i]);
		if (ret)
			goto fail;
		created |= BIT(i);
	}
	pr_info("RM-443 inspect: CPU and retained boot snapshots enabled\n");
	return 0;
fail:
	for (i = 0; i < ARRAY_SIZE(inspect_bins); i++)
		if (created & BIT(i))
			sysfs_remove_bin_file(inspect_kobj, inspect_bins[i]);
	sysfs_remove_file(inspect_kobj, &layout_attr.attr);
	sysfs_remove_file(inspect_kobj, &cpu_attr.attr);
	kobject_put(inspect_kobj);
	return ret;
}
late_initcall(rm443_inspect_init);
