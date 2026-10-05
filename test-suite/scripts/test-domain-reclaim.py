#!/usr/bin/env python3
"""Execute production domain GC, release and attach/detach with lifetime mocks."""
from pathlib import Path
import os, re, subprocess, sys, tempfile
source=Path(sys.argv[1]).read_text()
def function(name):
 match=re.search(r'^(?:void|int) '+name+r'\(',source,re.M)
 if not match and name=='rknpu_iommu_reclaim_domain': return 'void rknpu_iommu_reclaim_domain(struct rknpu_device *d,int id) {(void)d;(void)id;}\n'
 start=match.start()
 brace=source.index('{',start); depth=1; end=brace+1
 while depth:
  depth += (source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]+'\n'
code=function('rknpu_iommu_reclaim_domain')+function('rknpu_iommu_release_iova')+function('rknpu_iommu_domain_attach')+function('rknpu_iommu_domain_detach')
headers=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64;
#define RKNPU_MAX_CORES 3
#define RKNPU_MAX_IOMMU_DOMAIN_NUM 64
#define RKNPU_PER_FD_DOMAIN_START 16
#define READ_ONCE(x) (x)
#define IS_ERR(p) ((intptr_t)(p)<0)
#define PTR_ERR(p) ((intptr_t)(p))
#define LOG_DEV_DBG(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
#define msecs_to_jiffies(x) (x)
struct mutex { int held, rank; };
struct iommu_domain {bool freed; int generation;};
struct drm_mm {int nodes;};
struct drm_mm_node {u64 size; bool allocated; struct drm_mm *mm;};
struct pool {bool ready; struct drm_mm mm;};
struct rknpu_core {bool registered; void *dev, *group;};
struct rknpu_device {
 bool iommu_en,iommu_shared_mm_ready,shutting_down;
 void *dev;
 struct mutex iommu_domain_lock,iommu_shared_mm_lock;
 struct iommu_domain *iommu_domains[3][64];
 struct pool iommu_pools[64];
 struct rknpu_core cores[3];
 int iommu_active_domain[3],iommu_active_wq[3];
 unsigned iommu_active_refcount[3];
 unsigned iommu_domain_fd_users[64];
 u64 iommu_domain_live_objs[64],iommu_domain_live_bytes[64],iommu_domain_reclaims[64];
 u64 iommu_live_bytes,iommu_live_objs,iommu_total_releases;
};
struct rknpu_iommu_domain_ref {struct iommu_domain *domain;void *group;};
static struct rknpu_device *current;
static struct iommu_domain allocations[1024];
static int created,freed,detached,attached;
static bool reserve_on_lock,reclaim_during_wait;
static void mutex_lock(struct mutex *m) {
 assert(!m->held);
 if(m->rank==1) assert(!current->iommu_shared_mm_lock.held);
 m->held=1;
 if(m->rank==2 && reserve_on_lock) {
  reserve_on_lock=false;current->iommu_domain_live_objs[16]=1;current->iommu_pools[16].mm.nodes=1;
 }
}
static void mutex_unlock(struct mutex *m) {assert(m->held);m->held=0;}
static bool drm_mm_clean(struct drm_mm *mm) {return !mm->nodes;}
static bool drm_mm_node_allocated(struct drm_mm_node *n) {return n->allocated;}
static void drm_mm_remove_node(struct drm_mm_node *n) {assert(n->mm->nodes>0);n->mm->nodes--;n->allocated=false;}
static void iommu_domain_free(struct iommu_domain *d) {assert(!d->freed);d->freed=true;freed++;}
static void iommu_detach_group(struct iommu_domain *d,void *g) {assert(!d->freed && g);detached++;}
static int iommu_attach_group(struct iommu_domain *d,void *g) {assert(!d->freed && g);attached++;return 0;}
static int in_interrupt(void) {return 0;}
static void wake_up(int *w) {(void)w;}
static struct iommu_domain *rknpu_iommu_ensure_domain(struct rknpu_device *d,unsigned c,int id) {
 assert(d->iommu_domain_lock.held);
 if(!d->iommu_domains[c][id]) {assert(created<1024);allocations[created].generation=created+1;d->iommu_domains[c][id]=&allocations[created++];}
 return d->iommu_domains[c][id];
}
void rknpu_iommu_reclaim_domain(struct rknpu_device *,int);
static int fake_wait(void) {
 assert(!current->iommu_domain_lock.held);
 if(reclaim_during_wait) {reclaim_during_wait=false;rknpu_iommu_reclaim_domain(current,16);}
 current->iommu_active_refcount[0]=0;return 1;
}
#define wait_event_interruptible_timeout(...) fake_wait()
static void initialise(struct rknpu_device *d,int id) {
 memset(d,0,sizeof(*d));current=d;
 d->iommu_en=d->iommu_shared_mm_ready=true;
 d->iommu_domain_lock.rank=1;d->iommu_shared_mm_lock.rank=2;
 for(int c=0;c<3;c++) {
  d->cores[c].registered=true;d->cores[c].dev=d->cores[c].group=(void*)(uintptr_t)(c+1);
  d->iommu_active_domain[c]=id;
  mutex_lock(&d->iommu_domain_lock);rknpu_iommu_ensure_domain(d,c,id);mutex_unlock(&d->iommu_domain_lock);
 }
 d->iommu_pools[id].ready=true;
}
'''
main=r'''
int main(void) {
 struct rknpu_device d;struct rknpu_iommu_domain_ref ref;int before;
 initialise(&d,16);before=freed;rknpu_iommu_reclaim_domain(&d,16);
 assert(freed==before+3 && d.iommu_domain_reclaims[16]==3);
 for(int c=0;c<3;c++)assert(!d.iommu_domains[c][16] && d.iommu_active_domain[c]==-1);
 puts("empty sticky domains detached and freed PASS");
 for(int id=0;id<16;id++) {initialise(&d,id);before=freed;rknpu_iommu_reclaim_domain(&d,id);assert(freed==before);}
 puts("reserved SDK domains retained PASS");
 initialise(&d,16);before=freed;d.iommu_domain_live_objs[16]=1;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 d.iommu_domain_live_objs[16]=0;d.iommu_pools[16].mm.nodes=1;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 puts("GEM reservation and allocator pinning PASS");
 initialise(&d,16);before=freed;d.iommu_domain_fd_users[16]=1;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 d.iommu_domain_fd_users[16]=0;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before+3);
 puts("open FD retains empty cache; final owner close reclaims PASS");
 initialise(&d,16);before=freed;d.iommu_active_refcount[1]=1;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 ref.domain=d.iommu_domains[1][16];ref.group=d.cores[1].group;rknpu_iommu_domain_detach(&d,1,&ref);
 assert(freed==before+3 && !ref.domain && d.iommu_active_refcount[1]==0);
 puts("active job pins all domains; final detach reclaims PASS");
 initialise(&d,16);before=freed;d.iommu_live_objs=d.iommu_domain_live_objs[16]=1;
 d.iommu_live_bytes=d.iommu_domain_live_bytes[16]=4096;d.iommu_pools[16].mm.nodes=1;
 struct drm_mm_node node={4096,true,&d.iommu_pools[16].mm};
 rknpu_iommu_release_iova(&d,16,&node);assert(freed==before+3 && !d.iommu_live_objs && !d.iommu_domain_live_objs[16]);
 rknpu_iommu_release_iova(&d,16,&node);assert(freed==before+3);
 puts("last reservation release reclaims; double release inert PASS");
 initialise(&d,16);before=freed;reserve_on_lock=true;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 puts("new allocation racing GC pins domain PASS");
 initialise(&d,16);before=freed;d.shutting_down=true;rknpu_iommu_reclaim_domain(&d,16);assert(freed==before);
 puts("shutdown defers cleanup to teardown PASS");
 initialise(&d,16);before=freed;struct iommu_domain *old=d.iommu_domains[0][16];
 d.iommu_active_domain[0]=17;d.iommu_active_refcount[0]=1;
 mutex_lock(&d.iommu_domain_lock);rknpu_iommu_ensure_domain(&d,0,17);mutex_unlock(&d.iommu_domain_lock);
 reclaim_during_wait=true;assert(rknpu_iommu_domain_attach(&d,0,16,&ref)==0);
 assert(old->freed && ref.domain && !ref.domain->freed && ref.domain!=old);
 assert(d.iommu_active_refcount[0]==1);rknpu_iommu_domain_detach(&d,0,&ref);
 puts("attach refreshes target after unlocked wait PASS");
 for(int i=0;i<100;i++) {
  initialise(&d,16);before=freed;
  assert(rknpu_iommu_domain_attach(&d,0,16,&ref)==0);
  rknpu_iommu_domain_detach(&d,0,&ref);assert(freed==before+3);
 }
 puts("repeated reclaim and lazy recreation PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='rknpu-domain-reclaim-') as tmp:
 cpp=Path(tmp)/'test.c';exe=Path(tmp)/'test';cpp.write_text(headers+code+main)
 flags=['-std=gnu11','-O2','-Wall','-Wextra','-Wno-unused-variable']
 if os.getenv('SANITIZE'):flags+=['-g','-fsanitize='+os.environ['SANITIZE']]
 subprocess.run([os.getenv('CC','cc'),*flags,str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
