#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Userspace control-flow model; does not load or exercise a kernel module.
from pathlib import Path
import re
import subprocess
import sys
import os
import tempfile

repo=Path(__file__).resolve().parents[2]
_tmp=tempfile.TemporaryDirectory(prefix="rknpu-regression-")
out=Path(_tmp.name)
source=(Path(sys.argv[1]) if len(sys.argv)>1 else repo/'src/rknpu_job.c').read_text()
start=source.index('static inline int rknpu_job_subcore_commit_pc(')
fn=source[start:source.index('static inline int rknpu_job_subcore_commit(',start)]
regs=sorted(set(re.findall(r'RKNPU_CORE_REG_\w+',fn)))
prefix=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;typedef uint32_t u32;
#define LOG_DBG(...) ((void)0)
#define RKNPU_JOB_PC 1
#define RKNPU_JOB_PINGPONG 2
#define RKNPU_JOB_FINALIZED 4
#define RKNPU_JOB_RECOVERY_PENDING 8
#define RKNPU_MEM_KERNEL_MAPPING 16
#define RKNPU_PC_DATA_EXTRA_AMOUNT 4
#define U32_MAX UINT32_MAX
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define atomic_read(x) (*(x))
#define spin_lock_irqsave(lock,flags) do {*(lock)=1;(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock,flags) do {*(lock)=0;(void)(flags);} while(0)
struct rknpu_job;
struct rknpu_config {int pc_data_amount_scale,pc_task_number_bits,num_irqs,pc_dma_ctrl;u64 max_submit_number;};
struct rknpu_device {struct rknpu_config *config;int irq_lock;bool soft_reseting,reset_failed,shutting_down,iommu_en;struct {struct rknpu_job *job;} subcore_datas[3];};
struct rknpu_task {u64 regcmd_addr;u32 regcfg_amount,int_mask;};
struct rknpu_gem_object {void *kv_addr;unsigned int flags;size_t size;};
struct rknpu_submit {u32 task_start,task_number,flags;struct {u32 task_start,task_number;} subcore_task[5];};
struct rknpu_job {struct rknpu_device *rknpu_dev;struct rknpu_submit *args;struct rknpu_gem_object *task_obj;u32 flags,use_core_num;int submit_count[3],ret,iommu_domain_id;struct {void *domain;} iommu_ref[3];struct rknpu_task *first_task,*last_task;u32 int_mask[3];};
static int writes,unlocked;
static void write_reg(struct rknpu_device *d,u64 v,int r) {(void)v;(void)r;writes++;unlocked+=!d->irq_lock;}
#define REG_WRITE(value,reg) write_reg(rknpu_dev,value,reg)
static int rknpu_iommu_domain_attach(struct rknpu_device *d,int core,int id,void *ref) {(void)d;(void)core;(void)id;(void)ref;return 0;}
static u64 rknpu_pc_arm_step(struct rknpu_device *d) {(void)d;return 1;}
static bool rknpu_task_range_is_valid(size_t size,u32 start,u32 number) {size_t capacity=size/sizeof(struct rknpu_task);return number&&start<capacity&&number<=capacity-start;}
static void rknpu_arm_one_section(u32 *n,u32 *end,struct rknpu_task **last,struct rknpu_task *first,u32 start) {if(*n>1){*n=1;*end=start;*last=first;}}
'''
main=r'''
int main(void) {
    int failed=0;
    for(int mode=0;mode<6;mode++) {
        struct rknpu_config cfg={.pc_data_amount_scale=2,.pc_task_number_bits=12,.num_irqs=3,.max_submit_number=4095};
        struct rknpu_device dev={.config=&cfg,.iommu_en=true};
        struct rknpu_task task={.regcmd_addr=4096,.regcfg_amount=4,.int_mask=1};
        struct rknpu_gem_object obj={.kv_addr=&task,.flags=RKNPU_MEM_KERNEL_MAPPING,.size=sizeof(task)};
        struct rknpu_submit args={.task_number=1,.flags=RKNPU_JOB_PC};args.subcore_task[0].task_number=1;
        struct rknpu_job job={.rknpu_dev=&dev,.args=&args,.task_obj=&obj,.use_core_num=1};
        job.iommu_ref[0].domain=(void*)1;dev.subcore_datas[0].job=&job;
        if(mode==1)dev.soft_reseting=true;
        if(mode==2)job.flags=RKNPU_JOB_RECOVERY_PENDING;
        if(mode==3)job.flags=RKNPU_JOB_FINALIZED;
        if(mode==4)dev.subcore_datas[0].job=NULL;
        if(mode==5)dev.shutting_down=true;
        writes=unlocked=0;
        int ret=rknpu_job_subcore_commit_pc(&job,0);
        bool ok=ret==0 && dev.irq_lock==0 && (mode?writes==0:writes>0&&unlocked==0);
        printf("mode=%d ret=%d writes=%d unlocked=%d %s\n",mode,ret,writes,unlocked,ok?"PASS":"FAIL");failed+=!ok;
    }
    return failed!=0;
}
'''
c=out/'pc_regression.c'
c.write_text(prefix+'\nenum {'+','.join(regs)+'};\n'+fn+main)
subprocess.run([os.getenv('CC','cc'),'-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(out/'pc_regression')],check=True)
r=subprocess.run([str(out/'pc_regression')],capture_output=True,text=True)
print(r.stdout,end='');print(r.stderr,end='',file=sys.stderr)
(out/('pc-baseline-results.txt' if len(sys.argv)>1 else 'pc-fixed-results.txt')).write_text(r.stdout+r.stderr)
sys.exit(r.returncode)
