/* SPDX-License-Identifier: MIT */
/* No firmware calls, allocation, MMIO or table writes in this file. */
#include "capture.h"

__attribute__((noinline)) int linux6303_mmu_cpu(struct mmu_cpu *s) {
    __asm__ volatile("mrs %0, cpsr" : "=r"(s->cpsr));
    if ((s->cpsr & 31) != 31 && (s->cpsr & 31) != 19) return 1;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(s->midr));
    if ((s->midr & 0xff0ffff0u) != 0x41069260u) return 2;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(s->cache_type));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(s->control));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(s->ttbr));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(s->domains));
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 0" : "=r"(s->fcse));
    __asm__ volatile("mov %0, pc" : "=r"(s->pc));
    __asm__ volatile("mov %0, sp" : "=r"(s->sp));
    if (!(s->control & 1) || (s->control & 128)) return 3;
    if ((s->ttbr & 16383) || s->ttbr < 0x10000000u || s->ttbr > 0x11ffc000u) return 4;
    return 0;
}

/* Whole audited ARMv5 walker, with only its six relative calls masked below.
 * Exact bytes are shared by 08.90 and 10.12. The matched address is discovered;
 * no firmware routine address is imported or called. */
static const uint8_t walker[] = {
    0xf7,0xb5,0x00,0x25,0x06,0x00,0xed,0x43,0x82,0xb0,0x03,0xf0,0xb7,0xf8,
    0x07,0x00,0x03,0xf0,0xa8,0xf8,0xc0,0x07,0x03,0xd0,0x0d,0x20,0x00,0x07,
    0xc4,0x1b,0x00,0xe0,0x00,0x24,0x03,0xf0,0xb7,0xf8,0x01,0x90,0x80,0x08,
    0x80,0x00,0x40,0x1c,0x03,0xf0,0xb7,0xf8,0x03,0xf0,0xb6,0xe9,0x31,0x0d,
    0x38,0x19,0x89,0x00,0x40,0x58,0x03,0x99,0x08,0x60,0x81,0x07,0x89,0x0f,
    0x01,0x29,0x0b,0xd0,0x02,0x29,0x07,0xd0,0x03,0x29,0x11,0xd1,0x00,0x0b,
    0x00,0x03,0x00,0x19,0x31,0x03,0x89,0x0d,0x06,0xe0,0x00,0x25,0x09,0xe0,
    0x80,0x0a,0x80,0x02,0x00,0x19,0x31,0x03,0x09,0x0e,0x89,0x00,0x41,0x58,
    0x04,0x98,0x01,0x25,0x01,0x60,0x01,0x98,0x03,0xf0,0x92,0xf8,0x28,0x00,
    0x05,0xb0,0xf0,0xbd
};
static const unsigned calls[] = {10,16,34,46,50,120};
static const uint32_t helper_insn[] = {
    0xee120f10, 0xee110f10, 0xee130f10, 0xee030f10, 0xee071f9a, 0xee030f10
};

static uint32_t call_target(uint32_t pc, int arm) {
    const volatile uint16_t *p = (const volatile uint16_t *)(uintptr_t)pc;
    uint32_t first=p[0], second=p[1];
    if ((first & 0xf800) != 0xf000 || (second & 0xf800) != (arm ? 0xe800u : 0xf800u)) return 0;
    if (arm && (second & 1)) return 0;
    int32_t delta=(int32_t)(((first & 2047)<<12)|((second & 2047)<<1));
    if (delta & 0x400000) delta -= 0x800000;
    uint32_t target=pc+4+(uint32_t)delta;
    return arm ? target & ~3u : target;
}

__attribute__((noinline)) uint32_t linux6303_find_table_walker(void) {
    /* Shared RM-443 resolver validates this same initial ROM window first. */
    const uint32_t begin=0xf0100400u, end=0xf0110400u;
    uint32_t found=0;
    for (uint32_t address=begin; address<=end-sizeof(walker); address+=2) {
        const volatile uint8_t *p=(const volatile uint8_t *)(uintptr_t)address;
        if (p[0]!=0xf7 || p[1]!=0xb5 || p[2]!=0 || p[3]!=0x25) continue;
        unsigned i;
        for (i=0;i<sizeof(walker);i++) {
            int masked=0;
            for (unsigned j=0;j<6;j++) if(i>=calls[j] && i<calls[j]+4) masked=1;
            if (!masked && p[i]!=walker[i]) break;
        }
        if (i!=sizeof(walker)) continue;
        uint32_t write_target=0;
        for (i=0;i<6;i++) {
            uint32_t target=call_target(address+calls[i],i==4);
            if (target<begin || target>end-12 || (target&3)) break;
            if (i!=4) {
                if (*(const volatile uint32_t *)(uintptr_t)target!=0x46c04778u) break;
                target+=4;
            }
            if (*(const volatile uint32_t *)(uintptr_t)target!=helper_insn[i] ||
                *(const volatile uint32_t *)(uintptr_t)(target+4)!=0xe12fff1eu) break;
            if (i==3) write_target=target;
            if (i==5 && target!=write_target) break;
        }
        if (i!=6) continue;
        if (found) return 0; /* Ambiguity is a failure, never first-match wins. */
        found=address;
    }
    return found;
}

/* Executed only after CPU and ROM-policy validation. All memory is owned RAM,
 * the verified L1 alias, or RAM-backed L2 aliases used by that firmware walker.
 * Error exits always restore DACR and IRQ/FIQ masks; a hardware data abort is
 * outside this contract and is why this remains an emulator-first probe. */
__attribute__((noinline)) void linux6303_capture(struct mmu_snapshot *s) {
    uint32_t cpsr, mask, domains, ttbr, control, zero=0;
    __asm__ volatile("mrs %0, cpsr\norr %1, %0, #0xc0\nmsr cpsr_c, %1"
        : "=&r"(cpsr), "=&r"(mask) : : "memory", "cc");
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(domains));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    s->status=1;
    if (ttbr != s->cpu.ttbr || control != s->cpu.control || domains != s->cpu.domains) goto restore_masks;
    uint32_t client=(domains & ~3u)|1u;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(client), "r"(zero) : "memory");
    const volatile uint32_t *first=(const volatile uint32_t *)0xd0000000u;
    for (unsigned i=0;i<4096;i++) s->l1[i]=first[i];
    s->status=0;
    for (unsigned k=0;k<MMU_TARGETS;k++) {
        struct mmu_target *t=&s->targets[k];
        uint32_t va=t->va;
        if (va < 0x02000000u) va |= s->cpu.fcse & 0xfe000000u;
        uint32_t l1=s->l1[va>>20], kind=l1&3;
        t->l1=l1; t->l2=0; t->pa=0; t->status=2; t->table_pa=0; t->table_bytes=0;
        if (!kind || ((l1>>5)&15)==3) continue;
        if (kind==2) { t->pa=(l1&0xfff00000u)|(va&0xfffffu); t->status=0; continue; }
        uint32_t table=l1&(kind==3 ? 0xfffff000u : 0xfffffc00u);
        uint32_t bytes=kind==3 ? 4096u : 1024u;
        /* The audited builder requires L2 allocations >= TTBR; reject anything
         * outside real DDR, so a descriptor can never direct this probe to MMIO. */
        if (table<ttbr || table>0x12000000u-bytes) { t->status=3; continue; }
        uint32_t alias=0xd0000000u+(table-ttbr);
        const volatile uint32_t *second=(const volatile uint32_t *)(uintptr_t)alias;
        for (unsigned i=0;i<bytes/4;i++) t->table[i]=second[i];
        t->table_pa=table; t->table_bytes=bytes;
        uint32_t l2=t->table[kind==3 ? (va>>10)&1023 : (va>>12)&255];
        t->l2=l2;
        uint32_t size;
        if ((l2&3)==1) size=65536;
        else if ((l2&3)==2) size=4096;
        else if ((l2&3)==3 && kind==3) size=1024;
        else { t->status=4; continue; }
        t->pa=(l2&~(size-1))|(va&(size-1)); t->status=0;
    }
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(domains), "r"(zero) : "memory");
restore_masks:
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory", "cc");
    __asm__ volatile("mrs %0, cpsr" : "=r"(s->cpsr_after));
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(s->domains_after));
}

static __attribute__((noinline)) void translate_mapping(
        const struct mmu_cpu *cpu, uint32_t va, uint32_t bytes,
        struct mmu_range *out, int ddr_only) {
    out->va=va; out->bytes=bytes; out->pa=0; out->checked_bytes=0;
    out->first_bad_va=va; out->status=1;
    if(!bytes || va<0x02000000u || bytes>0xffffffffu-va) return;
    uint32_t cpsr,mask,domains,ttbr,control,zero=0;
    __asm__ volatile("mrs %0, cpsr\norr %1, %0, #0xc0\nmsr cpsr_c, %1"
        : "=&r"(cpsr),"=&r"(mask) : : "memory","cc");
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(domains));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    if(ttbr!=cpu->ttbr || domains!=cpu->domains || control!=cpu->control) goto restore_masks;
    uint32_t client=(domains&~3u)|1u;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(client),"r"(zero) : "memory");
    const volatile uint32_t *first=(const volatile uint32_t *)0xd0000000u;
    uint32_t current=va,end=va+bytes;
    while(current<end) {
        uint32_t l1=first[current>>20],kind=l1&3,pa,size;
        out->first_bad_va=current; out->status=2;
        if(!kind || ((l1>>5)&15)==3) break;
        if(kind==2) {size=0x100000;pa=(l1&0xfff00000u)|(current&0xfffffu);}
        else {
            uint32_t table=l1&(kind==3?0xfffff000u:0xfffffc00u);
            uint32_t table_bytes=kind==3?4096:1024;
            out->status=3;
            if(table<ttbr || table>0x12000000u-table_bytes) break;
            uint32_t index=kind==3?(current>>10)&1023:(current>>12)&255;
            uint32_t alias=0xd0000000u+(table-ttbr)+4*index;
            uint32_t l2=*(const volatile uint32_t *)(uintptr_t)alias;
            out->status=4;
            if((l2&3)==1)size=65536;
            else if((l2&3)==2)size=4096;
            else if((l2&3)==3 && kind==3)size=1024;
            else break;
            pa=(l2&~(size-1))|(current&(size-1));
        }
        uint32_t step=size-(current&(size-1));
        /* Check every smallest ARMv5 page/subpage boundary, including repeated
         * descriptors of large pages and fine-table tiny pages. */
        uint32_t subpage=1024-(current&1023);
        if(step>subpage)step=subpage;
        if(step>end-current)step=end-current;
        out->status=5;
        if(ddr_only && (pa<0x10000000u || pa>0x12000000u-step))break;
        if(current==va)out->pa=pa;
        out->status=6;
        if(pa!=out->pa+(current-va))break;
        current+=step;out->checked_bytes=current-va;
    }
    if(current==end){out->status=0;out->first_bad_va=0;}
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(domains),"r"(zero) : "memory");
restore_masks:
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory","cc");
}

void linux6303_translate_range(const struct mmu_cpu *cpu, uint32_t va,
                              uint32_t bytes, struct mmu_range *out) {
    translate_mapping(cpu, va, bytes, out, 1);
}

void linux6303_translate_mapping(const struct mmu_cpu *cpu, uint32_t va,
                                uint32_t bytes, struct mmu_range *out) {
    translate_mapping(cpu, va, bytes, out, 0);
}

/* Resolve MMIO through the same audited table-RAM alias, without relaxing the
 * DDR-only allocation checks above. The captured 10.12 LCD mapping is a coarse
 * small page (FFD22200 -> 20022200), so section-only discovery is insufficient.
 * No mappings are installed and no peripheral is read while finding an alias.
 */
__attribute__((noinline)) uint32_t linux6303_mmio_alias(
        const struct mmu_cpu *cpu, uint32_t expected_pa) {
    uint32_t result=0;
    if(expected_pa&3u) return 0;
    uint32_t cpsr,mask,domains,ttbr,control,fcse,zero=0;
    __asm__ volatile("mrs %0, cpsr\norr %1, %0, #0xc0\nmsr cpsr_c, %1"
        : "=&r"(cpsr),"=&r"(mask) : : "memory","cc");
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(domains));
    __asm__ volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(ttbr));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 0" : "=r"(fcse));
    if(ttbr!=cpu->ttbr || domains!=cpu->domains || control!=cpu->control ||
       fcse || cpu->fcse || !(control&1u)) goto restore_masks;
    uint32_t client=(domains&~3u)|1u;
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(client),"r"(zero) : "memory");
    const volatile uint32_t *first=(const volatile uint32_t *)0xd0000000u;
    /* Prefer sections, then coarse small pages. Skip the FCSE window and
     * require access under the original DACR, not temporary table-read access.
     */
    for(unsigned pass=0;pass<2 && !result;pass++) {
        for(unsigned i=32;i<4096 && !result;i++) {
            uint32_t l1=first[i],kind=l1&3u,domain=(l1>>5)&15u;
            uint32_t access=(domains>>(domain*2))&3u;
            if(domain==3 || (access!=1 && access!=3)) continue;
            if(!pass) {
                if(kind==2 && !(l1&12u) && ((l1>>10)&3u) &&
                   (l1&0xfff00000u)==(expected_pa&0xfff00000u))
                    result=(i<<20)|(expected_pa&0xfffffu);
                continue;
            }
            if(kind!=1) continue;
            uint32_t table=l1&0xfffffc00u;
            if(table<ttbr || table>0x12000000u-1024u) continue;
            const volatile uint32_t *second=(const volatile uint32_t *)(uintptr_t)
                (0xd0000000u+(table-ttbr));
            for(unsigned j=0;j<256;j++) {
                uint32_t l2=second[j];
                unsigned ap=(l2>>(4+2*((expected_pa>>10)&3u)))&3u;
                if((l2&3u)==2 && !(l2&12u) && ap &&
                   (l2&0xfffff000u)==(expected_pa&0xfffff000u)) {
                    result=(i<<20)|(j<<12)|(expected_pa&0xfffu);
                    break;
                }
            }
        }
    }
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\nmcr p15, 0, %1, c7, c10, 4"
        : : "r"(domains),"r"(zero) : "memory");
restore_masks:
    __asm__ volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory","cc");
    return result;
}
