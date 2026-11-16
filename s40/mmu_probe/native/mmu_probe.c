/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "s40/runtime.h"
#include "capture.h"

static void line(unsigned row,const char *format,...) {
    char text[33]; va_list ap; va_start(ap,format);
    vsnprintf(text,sizeof(text),format,ap); va_end(ap);
    unsigned n=strlen(text); if(row==7 && n>31)n=31;
    memcpy((char *)(s40_result+16)+32*row,text,n);
}
static int binary(const char *path,const void *data,unsigned bytes) {
    FILE *f=fopen(path,"wb"); if(!f)return 0;
    int ok=fwrite(data,1,bytes,f)==bytes;
    if(fclose(f)!=0)ok=0;
    return ok;
}
static int save(const struct mmu_snapshot *s,const struct mmu_range *range) {
    if(!binary("D:\\linux6303-l1.bin",s->l1,sizeof(s->l1)))return 0;
    for(unsigned i=0;i<MMU_TARGETS;i++) {
        const struct mmu_target *t=&s->targets[i];
        if(!t->table_bytes)continue;
        char path[64]; snprintf(path,sizeof(path),"D:\\linux6303-l2-%u.bin",i);
        if(!binary(path,t->table,t->table_bytes))return 0;
    }
    FILE *f=fopen("D:\\linux6303-mmu.json","wb"); if(!f)return 0;
    fprintf(f,"{\n  \"format\":1, \"board\":\"nokia,rm443\",\n");
#define FIELD(name,value) fprintf(f,"  \"%s\":\"0x%08x\",\n",name,(unsigned)(value))
    FIELD("cpsr",s->cpu.cpsr); FIELD("midr",s->cpu.midr); FIELD("cache_type",s->cpu.cache_type);
    FIELD("control",s->cpu.control); FIELD("ttbr",s->cpu.ttbr); FIELD("domains",s->cpu.domains);
    FIELD("fcse",s->cpu.fcse); FIELD("pc",s->targets[0].va); FIELD("sp",s->targets[1].va);
    FIELD("descriptor_va",s->targets[5].va); FIELD("walker_va",s->walker_va);
    FIELD("cpsr_after",s->cpsr_after); FIELD("domains_after",s->domains_after);
    FIELD("sample_allocation_va",range->va); FIELD("sample_allocation_bytes",range->bytes);
    FIELD("sample_physical_base",range->pa); FIELD("sample_checked_bytes",range->checked_bytes);
    FIELD("sample_first_bad_va",range->first_bad_va); FIELD("sample_range_status",range->status);
#undef FIELD
    fprintf(f,"  \"page_tables_captured\":true, \"takeover_performed\":false,\n"
              "  \"allocation_lifetime\":\"released-before-return-to-Java\",\n  \"targets\":[\n");
    for(unsigned i=0;i<MMU_TARGETS;i++) {
        const struct mmu_target *t=&s->targets[i];
        fprintf(f,"    {\"index\":%u,\"va\":\"0x%08x\",\"pa\":\"0x%08x\","
                  "\"status\":%u,\"l1\":\"0x%08x\",\"l2\":\"0x%08x\","
                  "\"table_pa\":\"0x%08x\",\"table_bytes\":%u}%s\n",i,t->va,t->pa,
                  t->status,t->l1,t->l2,t->table_pa,t->table_bytes,i+1==MMU_TARGETS?"":",");
    }
    fprintf(f,"  ]\n}\n");int ok=!ferror(f);if(fclose(f)!=0)ok=0;return ok;
}

int s40_app_main(void) {
    struct mmu_snapshot *s=calloc(1,sizeof(*s));
    unsigned sample_bytes=6*1024*1024;
    void *sample=malloc(sample_bytes);
    if(!sample){sample_bytes=65536;sample=malloc(sample_bytes);}
    if(!s || !sample)s40_error("MMU probe allocation failed");
    int error=linux6303_mmu_cpu(&s->cpu);
    if(error)s40_error("MMU CPU guard %d MIDR %08x",error,s->cpu.midr);
    s->walker_va=linux6303_find_table_walker();
    if(!s->walker_va)s40_error("MMU walker policy not validated");
    s->targets[0].va=(uint32_t)(uintptr_t)linux6303_capture;
    __asm__ volatile("mov %0, sp" : "=r"(s->targets[1].va));
    s->targets[2].va=(uint32_t)(uintptr_t)sample;
    s->targets[3].va=(uint32_t)(uintptr_t)sample+sample_bytes-1;
    s->targets[4].va=(uint32_t)(uintptr_t)s;
    s->targets[5].va=(uint32_t)(uintptr_t)s40_result;
    linux6303_capture(s);
    if(s->status || s->domains_after!=s->cpu.domains ||
       ((s->cpsr_after^s->cpu.cpsr)&0xff))s40_error("MMU capture restore/state error");
    struct mmu_range range;
    linux6303_translate_range(&s->cpu,(uint32_t)(uintptr_t)sample,sample_bytes,&range);
    int saved=save(s,&range);
    memset((char *)(s40_result+16),' ',255);((char *)(s40_result+16))[255]=0;
    line(0,saved?"MMU captured; files saved to D:":"MMU captured; file not saved");
    line(1,"CTRL %08X TTBR %08X",s->cpu.control,s->cpu.ttbr);
    line(2,"CPSR %08X DACR %08X",s->cpu.cpsr,s->cpu.domains);
    const char *labels[]={"PC","SP","Heap","End"};
    for(unsigned i=0;i<4;i++) {
        struct mmu_target *t=&s->targets[i];
        if(t->status)line(3+i,"%s %08X map error %u",labels[i],t->va,t->status);
        else line(3+i,"%s %08X -> %08X",labels[i],t->va,t->pa);
    }
    line(7,"Arena %uK %08X st%u",sample_bytes/1024,range.pa,range.status);
    free(sample);free(s);return 0;
}
