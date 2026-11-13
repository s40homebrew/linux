// SPDX-License-Identifier: GPL-2.0-only
/* Read the real ARM926 register state in the calling sysfs task. */
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/sysfs.h>
#include <asm/cp15.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include "rm443_inspect_cpu.h"

ssize_t rm443_inspect_cpu(char *buffer)
{
	u32 cpsr, midr, cachetype, sctlr, ttbr, dacr, fcse, context;
	u32 dfsr, dfar, caller_cpsr = 0;
	bool caller_valid;

	/* Preemption control does not mask IRQ/FIQ or change CPSR's mode.
	 * Interrupts can run between these reads and restore their CPU state;
	 * this is a register sample, not a stop-the-world operation. */
	preempt_disable();
	asm volatile("mrs %0, cpsr" : "=r" (cpsr));
	asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r" (midr));
	asm volatile("mrc p15, 0, %0, c0, c0, 1" : "=r" (cachetype));
	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r" (sctlr));
	asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r" (ttbr));
	asm volatile("mrc p15, 0, %0, c3, c0, 0" : "=r" (dacr));
	asm volatile("mrc p15, 0, %0, c13, c0, 0" : "=r" (fcse));
	asm volatile("mrc p15, 0, %0, c13, c0, 1" : "=r" (context));
	asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r" (dfsr));
	asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r" (dfar));
	caller_valid = current->mm && user_mode(task_pt_regs(current));
	if (caller_valid)
		caller_cpsr = task_pt_regs(current)->ARM_cpsr;
	preempt_enable();

	return sysfs_emit(buffer,
		"context=kernel sysfs read; registers sampled without masking IRQ/FIQ\n"
		"midr=%08x cachetype=%08x\n"
		"cpsr=%08x mode=%02x thumb=%u irq_masked=%u fiq_masked=%u\n"
		"sctlr=%08x mmu=%u alignment_abort=%u dcache=%u write_buffer=%u "
		"big_endian=%u system_protection=%u rom_protection=%u icache=%u high_vectors=%u\n"
		"ttbr=%08x ttbr_base=%08x dacr=%08x fcse=%08x context_id=%08x\n"
		"last_dfsr=%08x last_dfar=%08x\n"
		"caller_frame_valid=%u caller_cpsr=%08x caller_mode=%02x\n",
		midr, cachetype, cpsr, cpsr & 0x1f, !!(cpsr & PSR_T_BIT),
		!!(cpsr & PSR_I_BIT), !!(cpsr & PSR_F_BIT), sctlr,
		!!(sctlr & CR_M), !!(sctlr & CR_A), !!(sctlr & CR_C),
		!!(sctlr & CR_W), !!(sctlr & CR_B), !!(sctlr & CR_S),
		!!(sctlr & CR_R), !!(sctlr & CR_I), !!(sctlr & CR_V),
		ttbr, ttbr & 0xffffc000, dacr, fcse, context, dfsr, dfar,
		caller_valid, caller_cpsr, caller_cpsr & 0x1f);
}
