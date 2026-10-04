#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Userspace control-flow model; does not load or exercise a kernel module.
from pathlib import Path
import subprocess
import sys
import os
import tempfile

repo=Path(__file__).resolve().parents[2]
_tmp=tempfile.TemporaryDirectory(prefix="rknpu-regression-")
out=Path(_tmp.name)
source=(Path(sys.argv[1]) if len(sys.argv)>1 else repo/'src/rknpu_job.c').read_text()
a=source.index('static void rknpu_job_timeout_work(')
timeout=source[a:source.index('static inline struct rknpu_job *rknpu_job_alloc(',a)]
a=source.index('static void rknpu_job_schedule(')
schedule=source[a:source.index('static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index);',a)]
old='timeout_armed = schedule_delayed_work' in source
prefix=r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#define RKNPU_JOB_DONE 1
#define RKNPU_JOB_ASYNC 2
#define RKNPU_JOB_FINALIZED 4
#define RKNPU_JOB_RECOVERY_PENDING 8
#define RKNPU_JOB_PUBLISHED 16
#define RKNPU_CORE_AUTO_MASK 0
#define RKNPU_CORE0_MASK 1
#define READ_ONCE(x) (x)
#define hweight32(x) __builtin_popcount(x)
#define atomic_set(x,v) (*(x)=(v))
#define refcount_inc(x) (++*(x))
#define msecs_to_jiffies(x) (x)
#define container_of(p,t,m) ((t*)((char*)(p)-offsetof(t,m)))
struct rknpu_job;
struct work_struct {int dummy;};
struct delayed_work {struct work_struct work;struct rknpu_job *owner;};
struct list_head {int member;};
struct rknpu_config {int num_irqs;};
struct rknpu_subcore_data {struct list_head todo_list;int task_num;struct rknpu_job *job;};
struct rknpu_device {struct rknpu_config *config;bool shutting_down,reset_failed,soft_reseting;int irq_lock;struct list_head jobs;struct rknpu_subcore_data subcore_datas[3];};
struct rknpu_submit {unsigned int core_mask,timeout;struct {int task_start,task_number;} subcore_task[5];};
struct rknpu_job {struct rknpu_device *rknpu_dev;struct rknpu_submit *args;int ret;unsigned int flags,use_core_num;int run_count,interrupt_count,iommu_domain_id,refcount;bool domain_held;struct {void *domain;} iommu_ref[3];struct list_head device_node,head[3];struct delayed_work timeout_work;};
static bool fire_on_unlock;
static int timer_calls,timer_published,timer_locked,dispatches;
static struct rknpu_job *pending;
static void rknpu_job_timeout_work(struct work_struct *work);
static void unlock(int *lock) {*lock=0;if(fire_on_unlock&&pending){struct rknpu_job *j=pending;pending=NULL;fire_on_unlock=false;rknpu_job_timeout_work(&j->timeout_work.work);}}
#define spin_lock_irqsave(lock,flags) do {*(lock)=1;(flags)=0;} while(0)
#define spin_unlock_irqrestore(lock,flags) do {(void)(flags);unlock(lock);} while(0)
static struct delayed_work *to_delayed_work(struct work_struct *w) {return container_of(w,struct delayed_work,work);}
static void list_add_tail(struct list_head *n,struct list_head *h) {(void)h;n->member=1;}
static void list_del_init(struct list_head *n) {n->member=0;}
static int rknpu_schedule_core_index(struct rknpu_device *d) {(void)d;return 0;}
static unsigned int rknpu_core_mask(int i) {return 1U<<i;}
static int rknpu_iommu_domain_get_and_switch(struct rknpu_device *d,int id) {(void)d;(void)id;return 0;}
static int rknpu_iommu_domain_attach(struct rknpu_device*d,int i,int id,void*r) {(void)d;(void)i;(void)id;(void)r;return 0;}
static void rknpu_job_release_holds(struct rknpu_job *j) {(void)j;}
static int rknpu_get_task_number(struct rknpu_job*j,int i) {(void)j;(void)i;return 1;}
static void rknpu_job_put(struct rknpu_job*j) {j->refcount--;}
static bool rknpu_job_is_active_locked(struct rknpu_job*j) {return j->rknpu_dev->subcore_datas[0].job==j;}
static void rknpu_job_detach_locked(struct rknpu_job*j) {j->head[0].member=0;}
static void rknpu_job_abort(struct rknpu_job*j) {j->flags|=RKNPU_JOB_FINALIZED|RKNPU_JOB_DONE;j->flags&=~RKNPU_JOB_PUBLISHED;j->head[0].member=0;j->device_node.member=0;j->refcount--;}
static int rknpu_job_recover_device(struct rknpu_device*d,struct rknpu_job*j,int err) {(void)d;(void)j;(void)err;return 0;}
static bool schedule_delayed_work(struct delayed_work*w,unsigned int delay) {
    (void)delay;struct rknpu_job*j=w->owner;timer_calls++;timer_published=!!(j->flags&RKNPU_JOB_PUBLISHED);timer_locked=j->rknpu_dev->irq_lock;
    if(fire_on_unlock){if(timer_locked)pending=j;else{fire_on_unlock=false;rknpu_job_timeout_work(&w->work);}}
    return true;
}
static void rknpu_job_next(struct rknpu_device*d,int i) {(void)i;struct rknpu_job*j=d->jobs.member?(struct rknpu_job*)d->jobs.member:NULL;(void)j;dispatches++;}
'''
# The test's dispatch stub records calls, without submitting hardware work.
prefix=prefix.replace('struct rknpu_job*j=d->jobs.member?(struct rknpu_job*)d->jobs.member:NULL;(void)j;', '(void)d;')
main=r'''
int main(void) {
    int failed=0;
    for(int mode=0;mode<2;mode++) {
        struct rknpu_config cfg={1};struct rknpu_device dev={.config=&cfg};
        struct rknpu_submit args={.core_mask=1,.timeout=0};
        struct rknpu_job job={.rknpu_dev=&dev,.args=&args,.flags=RKNPU_JOB_ASYNC,.refcount=2};job.timeout_work.owner=&job;
        timer_calls=timer_published=timer_locked=dispatches=0;fire_on_unlock=mode==1;pending=NULL;
#if OLD_TIMER
        refcount_inc(&job.refcount);
        schedule_delayed_work(&job.timeout_work,msecs_to_jiffies(args.timeout));
#endif
        rknpu_job_schedule(&job);
        bool ok=timer_calls==1&&timer_published&&timer_locked;
        if(mode)ok &= job.ret==-ETIMEDOUT && (job.flags&RKNPU_JOB_FINALIZED) && job.refcount==1;
        else ok &= job.ret==0 && !(job.flags&RKNPU_JOB_RECOVERY_PENDING) && job.refcount==3;
        printf("mode=%d published_at_arm=%d locked_at_arm=%d ret=%d finalized=%d refs=%d %s\n",mode,timer_published,timer_locked,job.ret,!!(job.flags&RKNPU_JOB_FINALIZED),job.refcount,ok?"PASS":"FAIL");failed+=!ok;
    }
    return failed!=0;
}
'''
c=out/'timer_regression.c'
c.write_text(prefix+'\n#define OLD_TIMER '+str(int(old))+'\n'+timeout+schedule+main)
subprocess.run([os.getenv('CC','cc'),'-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(out/'timer_regression')],check=True)
r=subprocess.run([str(out/'timer_regression')],capture_output=True,text=True)
print(r.stdout,end='');print(r.stderr,end='',file=sys.stderr)
(out/('timer-baseline-results.txt' if len(sys.argv)>1 else 'timer-fixed-results.txt')).write_text(r.stdout+r.stderr)
sys.exit(r.returncode)
