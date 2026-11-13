// SPDX-License-Identifier: GPL-2.0-only
/* Explicit, privileged experiments; registration itself performs no probe. */
#include <linux/capability.h>
#include <linux/extable.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/signal.h>
#include <linux/uaccess.h>
#include <uapi/linux/rm443_probe.h>
#include <asm/cacheflush.h>
#include <asm/bug.h>
#include <asm/memory.h>
#include <asm/system_misc.h>
#include "rm443_probe_bridge.h"

struct probe_buffer {
	void *data;
	void **pages;
	struct rm443_probe_mapping *mapping;
	u32 id, kind, bytes, charged, metadata_bytes, page_count;
	u32 flash_pa, bridge_va;
};

struct probe_session {
	/* All ioctls on this file serialize, including descriptors shared by dup.
	 * Lock order is session->lock, then rm443_rom_lock; never the reverse. */
	struct mutex lock;
	struct probe_buffer buffers[RM443_PROBE_MAX_BUFFERS];
	u32 next_id, charged, sequence, budget, reserve;
	bool have_record;
	struct rm443_probe_call record;
};

static atomic_t probe_opened = ATOMIC_INIT(0);

extern int rm443_probe_load(const void *address, u32 width, u32 *value);
extern const u8 rm443_probe_load8[], rm443_probe_load16[], rm443_probe_load32[];

/* ARM926 precise external aborts do not normally consult __ex_table. Only
 * our labelled loads gain recovery: all other faults keep do_bad behavior.
 * No handler is installed for asynchronous/imprecise aborts or hung buses. */
static int probe_external_abort(unsigned long address, unsigned int fsr,
				struct pt_regs *regs)
{
	unsigned long pc = instruction_pointer(regs);

	if (!user_mode(regs) &&
	    (pc == (unsigned long)rm443_probe_load8 ||
	     pc == (unsigned long)rm443_probe_load16 ||
	     pc == (unsigned long)rm443_probe_load32) && fixup_exception(regs))
		return 0;
	return 1;
}

static struct probe_buffer *probe_find(struct probe_session *session, u32 id)
{
	unsigned int i;

	if (!id)
		return NULL;
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++)
		if (session->buffers[i].id == id)
			return &session->buffers[i];
	return NULL;
}

static bool probe_buffer_range(const struct probe_buffer *buffer,
			       u32 offset, u32 bytes)
{
	return offset <= buffer->bytes && bytes <= buffer->bytes - offset;
}

/* The debugger must fail allocation instead of invoking the OOM killer. */
#define PROBE_GFP (GFP_KERNEL | __GFP_ZERO | __GFP_NORETRY | __GFP_NOWARN | __GFP_NOMEMALLOC)

static int probe_room(struct probe_session *session, u32 bytes)
{
	long available = si_mem_available();

	if (session->charged > session->budget || bytes > session->budget - session->charged)
		return -ENOSPC;
	if (available < 0 || (u64)available * PAGE_SIZE < (u64)session->reserve + bytes)
		return -ENOMEM;
	return 0;
}

static u32 probe_page_physical(const struct probe_buffer *buffer, u32 index)
{
	if (buffer->kind == RM443_PROBE_OBJECT_CONTIG)
		return virt_to_phys(buffer->data) + index * PAGE_SIZE;
	if (buffer->pages[index])
		return virt_to_phys(buffer->pages[index]);
	return buffer->flash_pa + index * PAGE_SIZE;
}

static int probe_mapping_page(void *opaque, u32 index, u32 *physical)
{
	struct probe_buffer *buffer = opaque;

	if (index >= buffer->page_count)
		return -EINVAL;
	*physical = probe_page_physical(buffer, index);
	return 0;
}

static bool probe_owned_entry(struct probe_session *session, u32 entry, bool virtual)
{
	u32 address = entry & ~1U, bytes = entry & 1 ? 2 : 4;
	unsigned int i;

	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++) {
		struct probe_buffer *buffer = &session->buffers[i];
		u32 physical;

		if (!buffer->id)
			continue;
		if (virtual && buffer->bridge_va && address >= buffer->bridge_va &&
		    probe_buffer_range(buffer, address - buffer->bridge_va, bytes))
			return true;
		/* A paged object must never acquire an invented physical extent. */
		if (virtual || buffer->kind != RM443_PROBE_OBJECT_CONTIG)
			continue;
		physical = virt_to_phys(buffer->data);
		if (address >= physical && probe_buffer_range(buffer, address - physical, bytes))
			return true;
	}
	return false;
}

static void probe_unmap_buffer(struct probe_session *session, struct probe_buffer *buffer)
{
	u32 bytes;

	if (!buffer->mapping)
		return;
	bytes = rm443_probe_mapping_cost(buffer->bytes);
	rm443_probe_mapping_destroy(buffer->mapping);
	buffer->mapping = NULL;
	buffer->bridge_va = 0;
	buffer->charged -= bytes;
	session->charged -= bytes;
}

static void probe_drop_buffer(struct probe_session *session, struct probe_buffer *buffer)
{
	unsigned int i;

	/* Caller holds the ROM lock; remove every translation before free. */
	probe_unmap_buffer(session, buffer);
	if (buffer->data)
		free_pages_exact(buffer->data, PAGE_ALIGN(buffer->bytes));
	if (buffer->pages) {
		for (i = 0; i < buffer->page_count; i++)
			if (buffer->pages[i])
				free_page((unsigned long)buffer->pages[i]);
		free_pages_exact(buffer->pages, buffer->metadata_bytes);
	}
	session->charged -= buffer->charged;
	memset(buffer, 0, sizeof(*buffer));
}

static bool probe_object_valid(u32 kind, u32 bytes, u32 flash)
{
	if (!bytes || bytes > RM443_PROBE_MAX_OBJECT || kind > RM443_PROBE_OBJECT_FLASH)
		return false;
	if (kind == RM443_PROBE_OBJECT_CONTIG)
		return bytes <= RM443_PROBE_MAX_BUFFER && !flash;
	if (kind == RM443_PROBE_OBJECT_PAGED)
		return !flash;
	return !(flash & (PAGE_SIZE - 1)) && flash < 0x04000000 &&
	       PAGE_ALIGN(bytes) <= 0x04000000 - flash;
}

static int probe_new_buffer(struct probe_session *session, u32 kind, u32 bytes,
			    u32 flash, struct probe_buffer **result)
{
	struct probe_buffer *buffer = NULL;
	u32 needed;
	unsigned int i;
	int error;

	if (!probe_object_valid(kind, bytes, flash))
		return -EINVAL;
	if (!session->next_id)
		return -EOVERFLOW;
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++)
		if (!session->buffers[i].id) {
			buffer = &session->buffers[i];
			break;
		}
	if (!buffer)
		return -ENOSPC;
	buffer->id = session->next_id++;
	buffer->kind = kind;
	buffer->bytes = bytes;
	buffer->page_count = DIV_ROUND_UP(bytes, PAGE_SIZE);
	buffer->flash_pa = flash;
	if (kind == RM443_PROBE_OBJECT_CONTIG) {
		needed = PAGE_ALIGN(bytes);
		error = probe_room(session, needed);
		if (error)
			goto fail;
		buffer->data = alloc_pages_exact(needed, PROBE_GFP);
		if (!buffer->data) {
			error = -ENOMEM;
			goto fail;
		}
		buffer->charged = needed;
		session->charged += needed;
	} else {
		needed = PAGE_ALIGN(buffer->page_count * sizeof(*buffer->pages));
		error = probe_room(session, needed);
		if (error)
			goto fail;
		buffer->pages = alloc_pages_exact(needed, PROBE_GFP);
		if (!buffer->pages) {
			error = -ENOMEM;
			goto fail;
		}
		buffer->metadata_bytes = buffer->charged = needed;
		session->charged += needed;
		if (kind == RM443_PROBE_OBJECT_PAGED) {
			for (i = 0; i < buffer->page_count; i++) {
				error = probe_room(session, PAGE_SIZE);
				if (error)
					goto fail;
				buffer->pages[i] = (void *)__get_free_page(PROBE_GFP);
				if (!buffer->pages[i]) {
					error = -ENOMEM;
					goto fail;
				}
				buffer->charged += PAGE_SIZE;
				session->charged += PAGE_SIZE;
				cond_resched();
			}
		}
	}
	*result = buffer;
	return 0;
fail:
	/* No mapping can exist for an unpublished object. */
	probe_drop_buffer(session, buffer);
	return error;
}

static void probe_object_description(const struct probe_buffer *buffer,
				     struct rm443_probe_object *request)
{
	memset(request, 0, sizeof(*request));
	request->id = buffer->id;
	request->kind = buffer->kind;
	request->bytes = buffer->bytes;
	request->physical = buffer->kind == RM443_PROBE_OBJECT_CONTIG ?
		virt_to_phys(buffer->data) : 0;
	request->bridge_va = buffer->bridge_va;
	request->resident_bytes = buffer->charged;
	request->page_count = buffer->page_count;
	request->backing_physical = buffer->flash_pa;
	request->mapped_bytes = buffer->mapping ? PAGE_ALIGN(buffer->bytes) : 0;
}

static int probe_alloc(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_buffer request;
	struct probe_buffer *buffer;
	int error;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.flags)
		return -EINVAL;
	error = probe_new_buffer(session, RM443_PROBE_OBJECT_CONTIG, request.bytes, 0, &buffer);
	if (error)
		return error;
	request.id = buffer->id;
	request.physical = virt_to_phys(buffer->data);
	if (copy_to_user(argument, &request, sizeof(request))) {
		probe_drop_buffer(session, buffer);
		return -EFAULT;
	}
	return 0;
}

static int probe_object(struct probe_session *session, void __user *argument,
			unsigned int command)
{
	struct rm443_probe_object request;
	struct probe_buffer *buffer;
	u32 cost;
	bool created = false, mapped = false;
	int error;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.flags || request.reserved[0] || request.reserved[1])
		return -EINVAL;
	if (command == RM443_PROBE_OBJECT_CREATE) {
		/* backing_physical is the only accepted flash-base input. */
		if (request.id || request.physical || request.bridge_va ||
		    request.resident_bytes || request.page_count || request.mapped_bytes)
			return -EINVAL;
		error = probe_new_buffer(session, request.kind, request.bytes,
					 request.backing_physical, &buffer);
		if (error)
			return error;
		created = true;
	} else {
		buffer = probe_find(session, request.id);
		if (!buffer)
			return -ENOENT;
	}
	mutex_lock(&rm443_rom_lock);
	if (command == RM443_PROBE_MAP && !buffer->mapping) {
		cost = rm443_probe_mapping_cost(buffer->bytes);
		error = probe_room(session, cost);
		if (error)
			goto unlock;
		error = rm443_probe_mapping_create(buffer - session->buffers, buffer->bytes,
				probe_mapping_page, buffer, &buffer->mapping);
		if (error)
			goto unlock;
		buffer->bridge_va = rm443_probe_mapping_address(buffer->mapping);
		buffer->charged += cost;
		session->charged += cost;
		mapped = true;
	} else if (command == RM443_PROBE_UNMAP) {
		probe_unmap_buffer(session, buffer);
	}
	probe_object_description(buffer, &request);
	error = 0;
unlock:
	mutex_unlock(&rm443_rom_lock);
	if (!error && copy_to_user(argument, &request, sizeof(request)))
		error = -EFAULT;
	if (error && (created || mapped)) {
		mutex_lock(&rm443_rom_lock);
		if (created)
			probe_drop_buffer(session, buffer);
		else
			probe_unmap_buffer(session, buffer);
		mutex_unlock(&rm443_rom_lock);
	}
	return error;
}

static int probe_free(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_buffer request;
	struct probe_buffer *buffer;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.flags)
		return -EINVAL;
	buffer = probe_find(session, request.id);
	if (!buffer)
		return -ENOENT;
	mutex_lock(&rm443_rom_lock);
	probe_drop_buffer(session, buffer);
	mutex_unlock(&rm443_rom_lock);
	return 0;
}

/* Bounded NOR reads use only the precise-fault-safe byte load. */
static int probe_flash_read(u32 physical, void *output, u32 bytes, u32 *done)
{
	void __iomem *mapping;
	u32 i;
	int error = 0;

	*done = 0;
	if (physical >= 0x04000000 || bytes > 0x04000000 - physical)
		return -ERANGE;
	mapping = ioremap(physical, bytes);
	if (!mapping)
		return -ENOMEM;
	pagefault_disable();
	for (i = 0; i < bytes; i++) {
		u32 value;
		error = rm443_probe_load((const void __force *)mapping + i, 1, &value);
		if (error)
			break;
		((u8 *)output)[i] = value;
		++*done;
	}
	pagefault_enable();
	iounmap(mapping);
	return error;
}

static int probe_copy_out(struct probe_buffer *buffer, u32 offset, void *output, u32 bytes)
{
	u32 amount, index, within, done;
	int error;

	while (bytes) {
		index = offset / PAGE_SIZE;
		within = offset % PAGE_SIZE;
		amount = min(bytes, PAGE_SIZE - within);
		if (buffer->kind == RM443_PROBE_OBJECT_CONTIG)
			memcpy(output, (u8 *)buffer->data + offset, amount);
		else if (buffer->pages[index])
			memcpy(output, (u8 *)buffer->pages[index] + within, amount);
		else {
			error = probe_flash_read(buffer->flash_pa + offset, output, amount, &done);
			if (error)
				return error;
		}
		offset += amount;
		output += amount;
		bytes -= amount;
	}
	return 0;
}

static int probe_copy_in(struct probe_session *session, struct probe_buffer *buffer,
			 u32 offset, const void *input, u32 bytes)
{
	u32 added[RM443_PROBE_MAX_TRANSFER / PAGE_SIZE + 1], count = 0;
	u32 first = offset / PAGE_SIZE, last = (offset + bytes - 1) / PAGE_SIZE;
	u32 index, done, amount, within;
	int error;

	/* Prepare every needed COW page before changing a single requested byte.
	 * A fault or allocation failure releases all pages created by this chunk. */
	if (buffer->kind == RM443_PROBE_OBJECT_FLASH) {
		for (index = first; index <= last; index++) {
			if (buffer->pages[index])
				continue;
			error = probe_room(session, PAGE_SIZE);
			if (error)
				goto rollback;
			buffer->pages[index] = (void *)__get_free_page(PROBE_GFP);
			if (!buffer->pages[index]) {
				error = -ENOMEM;
				goto rollback;
			}
			added[count++] = index;
			buffer->charged += PAGE_SIZE;
			session->charged += PAGE_SIZE;
			error = probe_flash_read(buffer->flash_pa + index * PAGE_SIZE,
					 buffer->pages[index], PAGE_SIZE, &done);
			if (error)
				goto rollback;
		}
		for (index = 0; index < count; index++)
			if (buffer->mapping) {
				error = rm443_probe_mapping_update(buffer->mapping, added[index],
					probe_page_physical(buffer, added[index]));
				if (error)
					goto rollback;
			}
	}
	while (bytes) {
		void *destination;

		index = offset / PAGE_SIZE;
		within = offset % PAGE_SIZE;
		amount = min(bytes, PAGE_SIZE - within);
		destination = buffer->kind == RM443_PROBE_OBJECT_CONTIG ?
			(u8 *)buffer->data + offset : (u8 *)buffer->pages[index] + within;
		memcpy(destination, input, amount);
		flush_icache_range((unsigned long)destination, (unsigned long)destination + amount);
		offset += amount;
		input += amount;
		bytes -= amount;
	}
	return 0;
rollback:
	while (count) {
		index = added[--count];
		if (buffer->mapping)
			rm443_probe_mapping_update(buffer->mapping, index,
						    buffer->flash_pa + index * PAGE_SIZE);
		free_page((unsigned long)buffer->pages[index]);
		buffer->pages[index] = NULL;
		buffer->charged -= PAGE_SIZE;
		session->charged -= PAGE_SIZE;
	}
	return error;
}

static int probe_transfer(struct probe_session *session, void __user *argument, bool upload)
{
	struct rm443_probe_transfer request;
	struct probe_buffer *buffer;
	void __user *user;
	void *temporary;
	int error;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.reserved || !request.bytes ||
	    request.bytes > RM443_PROBE_MAX_TRANSFER || request.data > U32_MAX)
		return -EINVAL;
	buffer = probe_find(session, request.id);
	if (!buffer)
		return -ENOENT;
	if (!probe_buffer_range(buffer, request.offset, request.bytes))
		return -ERANGE;
	user = u64_to_user_ptr(request.data);
	temporary = kmalloc(request.bytes, PROBE_GFP);
	if (!temporary)
		return -ENOMEM;
	if (upload && copy_from_user(temporary, user, request.bytes)) {
		kfree(temporary);
		return -EFAULT;
	}
	mutex_lock(&rm443_rom_lock);
	error = upload ? probe_copy_in(session, buffer, request.offset, temporary, request.bytes) :
		probe_copy_out(buffer, request.offset, temporary, request.bytes);
	mutex_unlock(&rm443_rom_lock);
	if (!error && !upload && copy_to_user(user, temporary, request.bytes))
		error = -EFAULT;
	kfree(temporary);
	return error;
}

static int probe_pages(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_pages request;
	struct rm443_probe_page *pages;
	struct probe_buffer *buffer;
	u32 i;
	int error = 0;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.reserved || !request.count || request.count > 256 || request.data > U32_MAX)
		return -EINVAL;
	buffer = probe_find(session, request.id);
	if (!buffer)
		return -ENOENT;
	if (request.start > buffer->page_count)
		return -ERANGE;
	request.count = min(request.count, buffer->page_count - request.start);
	pages = kcalloc(request.count ? request.count : 1, sizeof(*pages), PROBE_GFP);
	if (!pages)
		return -ENOMEM;
	for (i = 0; i < request.count; i++) {
		u32 index = request.start + i;
		pages[i].offset = index * PAGE_SIZE;
		pages[i].physical = probe_page_physical(buffer, index);
		pages[i].kind = buffer->kind == RM443_PROBE_OBJECT_FLASH && !buffer->pages[index] ?
			RM443_PROBE_PAGE_FLASH : RM443_PROBE_PAGE_RAM;
	}
	if (copy_to_user(u64_to_user_ptr(request.data), pages, request.count * sizeof(*pages)) ||
	    copy_to_user(argument, &request, sizeof(request)))
		error = -EFAULT;
	kfree(pages);
	return error;
}

static int probe_budget(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_budget request;
	long available;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.flags & ~RM443_PROBE_BUDGET_SET || request.reserved)
		return -EINVAL;
	if (request.flags & RM443_PROBE_BUDGET_SET) {
		if (request.limit_bytes < session->charged || !request.limit_bytes ||
		    request.limit_bytes > RM443_PROBE_MAX_OBJECT ||
		    request.headroom_bytes < RM443_PROBE_MIN_HEADROOM ||
		    request.headroom_bytes > RM443_PROBE_MAX_OBJECT)
			return -EINVAL;
		session->budget = request.limit_bytes;
		session->reserve = request.headroom_bytes;
	}
	request.limit_bytes = session->budget;
	request.headroom_bytes = session->reserve;
	request.used_bytes = session->charged;
	available = si_mem_available();
	request.available_bytes = available < 0 ? 0 : min_t(u64, (u64)available * PAGE_SIZE, U32_MAX);
	request.max_object_bytes = RM443_PROBE_MAX_OBJECT;
	request.max_contiguous_bytes = RM443_PROBE_MAX_BUFFER;
	return copy_to_user(argument, &request, sizeof(request)) ? -EFAULT : 0;
}

static int probe_copy_phys(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_copy request;
	struct probe_buffer *buffer;
	void *temporary;
	u32 count, done;
	int error = 0;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (request.reserved[0] || request.reserved[1] || !request.bytes ||
	    request.bytes > RM443_PROBE_MAX_TRANSFER ||
	    request.physical >= 0x04000000 || request.bytes > 0x04000000 - request.physical)
		return -EINVAL;
	buffer = probe_find(session, request.id);
	if (!buffer)
		return -ENOENT;
	if (!probe_buffer_range(buffer, request.offset, request.bytes))
		return -ERANGE;
	temporary = kmalloc(PAGE_SIZE, PROBE_GFP);
	if (!temporary)
		return -ENOMEM;
	request.done = 0;
	request.status = 0;
	while (request.done < request.bytes) {
		count = min_t(u32, PAGE_SIZE, request.bytes - request.done);
		mutex_lock(&rm443_rom_lock);
		request.status = probe_flash_read(request.physical + request.done, temporary, count, &done);
		if (!request.status)
			request.status = probe_copy_in(session, buffer, request.offset + request.done, temporary, count);
		mutex_unlock(&rm443_rom_lock);
		if (request.status)
			break;
		request.done += count;
		cond_resched();
	}
	if (copy_to_user(argument, &request, sizeof(request)))
		error = -EFAULT;
	kfree(temporary);
	return error;
}

static int probe_call(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_call *call;
	u32 physical, contiguous;
	unsigned int i;
	int error;

	call = kmalloc(sizeof(*call), PROBE_GFP);
	if (!call)
		return -ENOMEM;
	if (copy_from_user(call, argument, sizeof(*call))) {
		kfree(call);
		return -EFAULT;
	}
	error = -EINVAL;
	if (call->reserved || call->flags & ~(RM443_PROBE_CALL_ROM | RM443_PROBE_CALL_VIRTUAL) ||
	    call->flags == (RM443_PROBE_CALL_ROM | RM443_PROBE_CALL_VIRTUAL) ||
	    call->stack_words > RM443_PROBE_STACK_WORDS)
		goto out;
	if (!call->entry && call->flags & RM443_PROBE_CALL_ROM)
		call->entry = RM443_PROBE_ROM_ENTRY;
	if (!call->entry || (!(call->entry & 1) && (call->entry & 3)))
		goto out;
	if (!(call->flags & RM443_PROBE_CALL_ROM) &&
	    !probe_owned_entry(session, call->entry, call->flags & RM443_PROBE_CALL_VIRTUAL)) {
		error = -EPERM;
		goto out;
	}
	/* Output bytes never come from userspace, including unused record slots. */
	memset(call->returned, 0,
	       sizeof(*call) - offsetof(struct rm443_probe_call, returned));
	if (session->sequence == U32_MAX) {
		error = -EOVERFLOW;
		goto out;
	}
	mutex_lock(&rm443_rom_lock);
	error = rm443_probe_mapping_attach_all();
	if (error)
		goto unlock;
	if (call->flags & RM443_PROBE_CALL_ROM) {
		error = rm443_probe_translate(call->entry & ~1U, &physical, &contiguous);
		if (error)
			goto unlock;
		if ((physical >= 0x04000000 && physical < 0xfffe0000) ||
		    contiguous < (call->entry & 1 ? 2 : 4)) {
			error = -EPERM;
			goto unlock;
		}
	}
	/* A routine may branch into any other uploaded code buffer. The bridge
	 * also drains/invalidates data caches around its uncached physical map. */
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++) {
		struct probe_buffer *buffer = &session->buffers[i];
		u32 page;
		if (buffer->data)
			flush_icache_range((unsigned long)buffer->data,
				(unsigned long)buffer->data + buffer->bytes);
		if (buffer->pages)
			for (page = 0; page < buffer->page_count; page++)
				if (buffer->pages[page])
					flush_icache_range((unsigned long)buffer->pages[page],
						(unsigned long)buffer->pages[page] + PAGE_SIZE);
	}
	call->sequence = ++session->sequence;
	error = rm443_probe_execute(call);
	/* Preserve an execution record even if the caller's output pointer went
	 * away. A pre-entry error has stage zero and did not execute the routine. */
	memcpy(&session->record, call, sizeof(*call));
	session->have_record = true;
unlock:
	rm443_probe_mapping_detach_all();
	mutex_unlock(&rm443_rom_lock);
	if (copy_to_user(argument, call, sizeof(*call)))
		error = -EFAULT;
out:
	kfree(call);
	return error;
}

static bool probe_read_valid(const struct rm443_probe_read *request)
{
	return !request->reserved && request->data <= U32_MAX &&
	       (request->space == RM443_PROBE_READ_PHYSICAL ||
		request->space == RM443_PROBE_READ_BRIDGE) &&
	       (request->width == 1 || request->width == 2 || request->width == 4) &&
	       request->bytes && request->bytes <= RM443_PROBE_MAX_TRANSFER &&
	       !(request->address & (request->width - 1)) &&
	       !(request->bytes & (request->width - 1)) &&
	       (u64)request->address + request->bytes <= (1ULL << 32);
}

static u32 probe_read_count(u32 bytes, u32 contiguous, u32 physical, u32 width)
{
	u32 count = min(bytes, contiguous);

	count = min_t(u32, count, PAGE_SIZE - offset_in_page(physical));
	return count & ~(width - 1);
}

static int probe_read(struct probe_session *session, void __user *argument)
{
	struct rm443_probe_read request;
	void *temporary;
	int error = 0;

	if (copy_from_user(&request, argument, sizeof(request)))
		return -EFAULT;
	if (!probe_read_valid(&request))
		return -EINVAL;
	temporary = kmalloc(request.bytes, PROBE_GFP);
	if (!temporary)
		return -ENOMEM;
	request.done = request.fault_address = 0;
	request.status = 0;
	while (request.done < request.bytes) {
		u32 address = request.address + request.done, physical = address;
		u32 contiguous = PAGE_SIZE - offset_in_page(address), count, offset;
		void __iomem *mapping = NULL;
		const void *source;

		/* Bound each hold to a page so watchdog work can acquire the bridge
		 * lock between pages. Allocation and copy_to_user never disable IRQs. */
		mutex_lock(&rm443_rom_lock);
		if (request.space == RM443_PROBE_READ_BRIDGE) {
			request.status = rm443_probe_mapping_attach_all();
			if (request.status)
				goto page_done;
			request.status = rm443_probe_translate(address, &physical, &contiguous);
			if (request.status)
				goto page_done;
		}
		count = probe_read_count(request.bytes - request.done, contiguous,
					 physical, request.width);
		if (count < request.width || (physical & (request.width - 1))) {
			request.status = -EFAULT;
			goto page_done;
		}
		if (memblock_is_map_memory(physical)) {
			if (!pfn_valid(PHYS_PFN(physical)) ||
			    !virt_addr_valid(__va(physical))) {
				request.status = -EFAULT;
				goto page_done;
			}
			source = __va(physical);
		} else {
			mapping = ioremap(physical, count);
			if (!mapping) {
				request.status = -ENOMEM;
				goto page_done;
			}
			source = (const void __force *)mapping;
		}
		pagefault_disable();
		for (offset = 0; offset < count; offset += request.width) {
			u32 value;

			request.status = rm443_probe_load((const u8 *)source + offset,
							 request.width, &value);
			if (request.status)
				break;
			/* Only the output copy uses memcpy; the hardware access above is
			 * one ldrb/ldrh/ldr. RM-443 is exclusively little-endian. */
			memcpy((u8 *)temporary + request.done, &value, request.width);
			request.done += request.width;
		}
		pagefault_enable();
		if (mapping)
			iounmap(mapping);
page_done:
		if (request.space == RM443_PROBE_READ_BRIDGE)
			rm443_probe_mapping_detach_all();
		mutex_unlock(&rm443_rom_lock);
		if (request.status) {
			request.fault_address = request.address + request.done;
			break;
		}
		cond_resched();
	}
	if (copy_to_user(u64_to_user_ptr(request.data), temporary, request.done))
		error = -EFAULT;
	if (copy_to_user(argument, &request, sizeof(request)))
		error = -EFAULT;
	kfree(temporary);
	return error;
}

static long probe_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct probe_session *session = file->private_data;
	void __user *argument = (void __user *)arg;
	struct rm443_probe_info info = {
		.abi = RM443_PROBE_ABI,
		.max_buffers = RM443_PROBE_MAX_BUFFERS,
		.max_buffer = RM443_PROBE_MAX_BUFFER,
		.max_transfer = RM443_PROBE_MAX_TRANSFER,
		.stack_words = RM443_PROBE_STACK_WORDS,
		.callbacks = RM443_PROBE_CALLBACKS,
	};
	int error;

	/* Recheck on each operation: a privileged fd can be passed to a process
	 * whose current credentials must still authorize arbitrary hardware use. */
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (mutex_lock_interruptible(&session->lock))
		return -ERESTARTSYS;
	switch (command) {
	case RM443_PROBE_INFO:
		info.max_total = session->budget;
		mutex_lock(&rm443_rom_lock);
		error = rm443_probe_bridge_info(&info);
		mutex_unlock(&rm443_rom_lock);
		/* Physical reads and buffer management do not need a captured ROM
		 * context. Zero address fields advertise an unavailable bridge. */
		if (error == -ENODEV)
			error = 0;
		if (!error && copy_to_user(argument, &info, sizeof(info)))
			error = -EFAULT;
		break;
	case RM443_PROBE_ALLOC:
		error = probe_alloc(session, argument);
		break;
	case RM443_PROBE_OBJECT_CREATE:
	case RM443_PROBE_OBJECT_INFO:
	case RM443_PROBE_MAP:
	case RM443_PROBE_UNMAP:
		error = probe_object(session, argument, command);
		break;
	case RM443_PROBE_PAGES:
		error = probe_pages(session, argument);
		break;
	case RM443_PROBE_BUDGET:
		error = probe_budget(session, argument);
		break;
	case RM443_PROBE_COPY_PHYS:
		error = probe_copy_phys(session, argument);
		break;
	case RM443_PROBE_FREE:
		error = probe_free(session, argument);
		break;
	case RM443_PROBE_UPLOAD:
	case RM443_PROBE_DOWNLOAD:
		error = probe_transfer(session, argument, command == RM443_PROBE_UPLOAD);
		break;
	case RM443_PROBE_CALL:
		error = probe_call(session, argument);
		break;
	case RM443_PROBE_READ:
		error = probe_read(session, argument);
		break;
	case RM443_PROBE_RECORD:
		error = !session->have_record ? -ENODATA :
			copy_to_user(argument, &session->record, sizeof(session->record)) ?
			-EFAULT : 0;
		break;
	default:
		error = -ENOTTY;
	}
	mutex_unlock(&session->lock);
	return error;
}

static int probe_open(struct inode *inode, struct file *file)
{
	struct probe_session *session;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (atomic_cmpxchg(&probe_opened, 0, 1))
		return -EBUSY;
	session = kzalloc(sizeof(*session), PROBE_GFP);
	if (!session) {
		atomic_set(&probe_opened, 0);
		return -ENOMEM;
	}
	mutex_init(&session->lock);
	session->next_id = 1;
	session->budget = RM443_PROBE_MAX_TOTAL;
	session->reserve = RM443_PROBE_MIN_HEADROOM;
	file->private_data = session;
	return nonseekable_open(inode, file);
}

static int probe_release(struct inode *inode, struct file *file)
{
	struct probe_session *session = file->private_data;
	unsigned int i;

	mutex_lock(&rm443_rom_lock);
	for (i = 0; i < RM443_PROBE_MAX_BUFFERS; i++)
		if (session->buffers[i].id)
			probe_drop_buffer(session, &session->buffers[i]);
	mutex_unlock(&rm443_rom_lock);
	kfree(session);
	atomic_set(&probe_opened, 0);
	return 0;
}

static const struct file_operations probe_fops = {
	.owner = THIS_MODULE,
	.open = probe_open,
	.release = probe_release,
	.unlocked_ioctl = probe_ioctl,
};

static struct miscdevice probe_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "rm443_probe",
	.fops = &probe_fops,
	.mode = 0600,
};

static int __init rm443_probe_init(void)
{
	static const unsigned int precise_abort[] = { 4, 6, 8, 10, 12, 14 };
	unsigned int i;
	int error = misc_register(&probe_device);

	if (error)
		return error;
	for (i = 0; i < ARRAY_SIZE(precise_abort); i++)
		hook_fault_code(precise_abort[i], probe_external_abort, SIGBUS, 0,
				"external abort (RM-443 probe fixup available)");
	return 0;
}
device_initcall(rm443_probe_init);
