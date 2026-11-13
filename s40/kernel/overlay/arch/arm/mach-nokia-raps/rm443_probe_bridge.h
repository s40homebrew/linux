/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RM443_PROBE_BRIDGE_H
#define RM443_PROBE_BRIDGE_H

#include <linux/mutex.h>
#include <uapi/linux/rm443_probe.h>

/* Context/translation/execution APIs require this mutex, also held by the watchdog worker.
 * The uploaded routine must return in System mode with its supplied SP and
 * the private mappings intact. A hang or abort inside it is not recoverable. */
extern struct mutex rm443_rom_lock;
int rm443_probe_bridge_info(struct rm443_probe_info *info);
int rm443_probe_translate(u32 address, u32 *physical, u32 *contiguous);
int rm443_probe_execute(struct rm443_probe_call *call);

/* Session mappings use fixed, disjoint 32 MiB slots in a previously empty
 * part of the private table. They never describe contiguous physical RAM.
 * Cost/address queries do not change state. Other mapping APIs require the
 * mutex. Create/update/destroy happen while Linux is running, with the lock
 * held. Attach all maps immediately before a call or bridge-address read;
 * detach them before releasing the lock, including on error paths.
 * Flash COW is explicit in the driver: these privileged mappings do not
 * enforce read-only flash or service demand-allocation faults. */
#define RM443_PROBE_MAP_BASE 0x40000000U
#define RM443_PROBE_MAP_SLOT_BYTES RM443_PROBE_MAX_OBJECT
struct rm443_probe_mapping;
typedef int (*rm443_probe_page_physical)(void *opaque, u32 index, u32 *physical);
u32 rm443_probe_mapping_cost(u32 bytes);
int rm443_probe_mapping_create(unsigned int slot, u32 bytes,
			       rm443_probe_page_physical get_page, void *opaque,
			       struct rm443_probe_mapping **result);
void rm443_probe_mapping_destroy(struct rm443_probe_mapping *mapping);
int rm443_probe_mapping_update(struct rm443_probe_mapping *mapping,
			       u32 index, u32 physical);
u32 rm443_probe_mapping_address(const struct rm443_probe_mapping *mapping);
int rm443_probe_mapping_attach_all(void);
void rm443_probe_mapping_detach_all(void);

#endif
