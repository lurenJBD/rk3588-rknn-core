/* SPDX-License-Identifier: GPL-2.0 */
/* Userspace lifetime/index model exercising extracted, unmodified driver functions.
 * This is not KUnit and does not validate real DRM, DMA or kernel locking. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gem-token-stats.inc"

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint64_t dma_addr_t;
#define U32_MAX UINT32_MAX
#define RKNPU_GEM_MAX_CORE_MAPS 3
#define RKNPU_MAX_IOMMU_DOMAIN_NUM 64
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define GFP_KERNEL 0
#define LOG_DEV_ERROR(dev, ...) ((void)(dev))
struct kref { atomic_int refs; };
struct entry { unsigned long key; void *value; };
struct xarray { pthread_mutex_t lock; struct entry entries[64]; int fail_after; };
struct rknpu_device { struct xarray gem_dma_xa; atomic_uint_fast64_t mem_stats[RKNPU_MEM_STAT_COUNT]; };
static bool mem_profile;
#define unlikely(x) (x)
static u64 ktime_get_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (u64)t.tv_sec*1000000000+t.tv_nsec; }
static void rknpu_mem_stat_add(struct rknpu_device *d, enum rknpu_mem_stat stat, u64 value) { if (mem_profile) atomic_fetch_add(&d->mem_stats[stat],value); }
struct drm_device { struct rknpu_device *dev_private; void *dev; };
struct drm_gem_object { struct drm_device *dev; struct kref refcount; };
struct rknpu_file_priv { int domain_id; };
struct drm_file { pthread_mutex_t table_lock; struct drm_gem_object *object_idr[16]; void *driver_priv; };
struct rknpu_gem_object {
    struct drm_gem_object base;
    int iommu_domain_id;
    dma_addr_t dma_addr;
    struct { dma_addr_t dma_addr; bool mapped; } core_maps[3];
};
static atomic_int freed;
#define spin_lock(p) assert(!pthread_mutex_lock(p))
#define spin_unlock(p) assert(!pthread_mutex_unlock(p))
#define xa_lock(x) spin_lock(&(x)->lock)
#define xa_unlock(x) spin_unlock(&(x)->lock)
#define idr_for_each_entry(table, obj, id) \
    for ((id)=0; (id)<16; (id)++) if (((obj)=(*(table))[id]) != NULL)
/* xa_load is used under xa_lock in the new driver's lifetime-critical paths. */
static void *xa_load(struct xarray *x, unsigned long key) {
    for (int i=0; i<64; i++) if (x->entries[i].value && x->entries[i].key==key) return x->entries[i].value;
    return NULL;
}
static bool xa_empty(struct xarray *x) {
    bool empty=true; xa_lock(x);
    for (int i=0; i<64; i++) if (x->entries[i].value) empty=false;
    xa_unlock(x); return empty;
}
static void __xa_erase(struct xarray *x, unsigned long key) {
    for (int i=0; i<64; i++) if (x->entries[i].key==key) x->entries[i].value=NULL;
}
static int xa_insert(struct xarray *x, unsigned long key, void *value, int flags) {
    (void)flags; xa_lock(x);
    if (xa_load(x,key)) { xa_unlock(x); return -EBUSY; }
    if (x->fail_after==0) { xa_unlock(x); return -ENOMEM; }
    if (x->fail_after>0) x->fail_after--;
    for (int i=0; i<64; i++) if (!x->entries[i].value) {
        x->entries[i]=(struct entry){key,value}; xa_unlock(x); return 0;
    }
    abort();
}
static bool kref_get_unless_zero(struct kref *k) {
    int old=atomic_load(&k->refs);
    while (old) if (atomic_compare_exchange_weak(&k->refs,&old,old+1)) return true;
    return false;
}
static void rknpu_gem_object_put(struct drm_gem_object *base);
static void rknpu_gem_object_get(struct drm_gem_object *base) {
    assert(kref_get_unless_zero(&base->refcount));
}
static struct rknpu_gem_object *rknpu_gem_object_find(struct drm_file *f, unsigned int handle) {
    struct drm_gem_object *o=NULL;
    spin_lock(&f->table_lock);
    if (handle<16 && (o=f->object_idr[handle])) rknpu_gem_object_get(o);
    spin_unlock(&f->table_lock); return (struct rknpu_gem_object *)o;
}
bool rknpu_gem_object_is_file_handle(struct drm_file *, struct rknpu_gem_object *);
/* Generated directly from the checked-out rknpu_gem.c. */
#include "gem-token-under-test.inc"
static void rknpu_gem_object_put(struct drm_gem_object *base) {
    int old=atomic_fetch_sub(&base->refcount.refs,1); assert(old>0);
    if (old==1) {
        rknpu_gem_dma_token_remove(base->dev->dev_private,(struct rknpu_gem_object *)base);
        atomic_fetch_add(&freed,1); free(base);
    }
}
static struct rknpu_gem_object *new_obj(struct drm_device *dev, int domain, u64 addr) {
    struct rknpu_gem_object *o=calloc(1,sizeof(*o)); assert(o);
    o->base.dev=dev; atomic_init(&o->base.refcount.refs,1);
    o->iommu_domain_id=domain; o->dma_addr=addr; return o;
}
static void own(struct drm_file *f, int handle, struct rknpu_gem_object *o) {
    spin_lock(&f->table_lock); assert(!f->object_idr[handle]); f->object_idr[handle]=&o->base; spin_unlock(&f->table_lock);
}
static void close_handle(struct drm_file *f, int handle) {
    spin_lock(&f->table_lock); struct drm_gem_object *o=f->object_idr[handle]; f->object_idr[handle]=NULL; spin_unlock(&f->table_lock);
    assert(o); rknpu_gem_object_put(o);
}
static void expect(struct drm_device *dev, struct drm_file *f, u64 token, struct rknpu_gem_object *want) {
    struct rknpu_gem_object *got=rknpu_gem_object_find_token(dev,f,token); assert(got==want);
    if (got) rknpu_gem_object_put(&got->base);
}
struct race { struct drm_device *dev; struct drm_file *file; atomic_bool stop; atomic_int hits; };
static void *reader(void *arg) {
    struct race *r=arg;
    while (!atomic_load(&r->stop)) {
        struct rknpu_gem_object *o=rknpu_gem_object_find_token(r->dev,r->file,((u64)7<<32)|0x9000);
        if (o) { assert(o->dma_addr==0x9000); atomic_fetch_add(&r->hits,1); rknpu_gem_object_put(&o->base); }
    }
    return NULL;
}
int main(void) {
    mem_profile=getenv("MEM_PROFILE") != NULL;
    struct rknpu_device rd={.gem_dma_xa={.lock=PTHREAD_MUTEX_INITIALIZER,.fail_after=-1}};
    struct drm_device dev={.dev_private=&rd};
    struct rknpu_file_priv fp={.domain_id=7};
    struct drm_file f={.table_lock=PTHREAD_MUTEX_INITIALIZER,.driver_priv=&fp};
    struct rknpu_gem_object *a=new_obj(&dev,0,0x1000), *b=new_obj(&dev,7,0x1000);
    assert(!rknpu_gem_dma_token_insert(&rd,a)); assert(!rknpu_gem_dma_token_insert(&rd,b)); own(&f,1,b);
    expect(&dev,&f,1,b); expect(&dev,&f,0x1000,b); expect(&dev,&f,((u64)7<<32)|0x1000,b);
    expect(&dev,&f,((u64)2<<32)|0x1000,NULL); /* encoded domain cannot fall through */
    close_handle(&f,1); expect(&dev,&f,0x1000,NULL); /* foreign domain0 remains unauthorized */
    b=new_obj(&dev,9,0x1000); assert(!rknpu_gem_dma_token_insert(&rd,b)); own(&f,1,b);
    expect(&dev,&f,0x1000,b); /* must continue beyond foreign early domains */
    close_handle(&f,1); rknpu_gem_object_put(&a->base);
    puts("PASS: handle, encoded token, domain collision, ownership and handle reuse");

    a=new_obj(&dev,1,0x1000); a->core_maps[0].dma_addr=0x2000; a->core_maps[0].mapped=true;
    a->core_maps[1]=a->core_maps[0]; a->core_maps[2].dma_addr=0x3000; a->core_maps[2].mapped=true;
    assert(!rknpu_gem_dma_token_insert(&rd,a)); own(&f,1,a);
    expect(&dev,&f,((u64)1<<32)|0x3000,a); /* duplicate aliases deduplicated */
    b=new_obj(&dev,1,0x4000); b->core_maps[0].dma_addr=0x2000; b->core_maps[0].mapped=true;
    assert(rknpu_gem_dma_token_insert(&rd,b)==-EEXIST);
    assert(!xa_load(&rd.gem_dma_xa,((u64)1<<32)|0x4000));
    expect(&dev,&f,((u64)1<<32)|0x2000,a); rknpu_gem_object_put(&b->base);
    expect(&dev,&f,((u64)1<<32)|0x2000,a);
    close_handle(&f,1); assert(xa_empty(&rd.gem_dma_xa));
    puts("PASS: alias deduplication, collision rollback preserves original object");

    a=new_obj(&dev,2,0x1000);
    for (int i=0;i<3;i++) { a->core_maps[i].dma_addr=0x2000+i*0x1000; a->core_maps[i].mapped=true; }
    assert(!rknpu_gem_dma_token_insert(&rd,a)); own(&f,1,a); expect(&dev,&f,((u64)2<<32)|0x4000,a);
    close_handle(&f,1); assert(xa_empty(&rd.gem_dma_xa));
    for (int fail=0;fail<4;fail++) {
        a=new_obj(&dev,2,0x1000);
        for (int i=0;i<3;i++) { a->core_maps[i].dma_addr=0x2000+i*0x1000; a->core_maps[i].mapped=true; }
        rd.gem_dma_xa.fail_after=fail; assert(rknpu_gem_dma_token_insert(&rd,a)==-ENOMEM);
        assert(xa_empty(&rd.gem_dma_xa)); rknpu_gem_object_put(&a->base);
    }
    rd.gem_dma_xa.fail_after=-1;
    puts("PASS: canonical plus three aliases, allocation-failure rollback");

    a=new_obj(&dev,7,0x1000); assert(!rknpu_gem_dma_token_insert(&rd,a));
    atomic_store(&a->base.refcount.refs,0); expect(&dev,&f,((u64)7<<32)|0x1000,NULL);
    atomic_store(&a->base.refcount.refs,1); rknpu_gem_object_put(&a->base);
    puts("PASS: zero-reference object is not resurrected");
    a=new_obj(&dev,7,0x1000); assert(!rknpu_gem_dma_token_insert(&rd,a));
    rknpu_gem_object_get(&a->base); rknpu_gem_object_put(&a->base);
    assert(atomic_load(&a->base.refcount.refs)==1);
    rknpu_gem_object_put(&a->base); assert(xa_empty(&rd.gem_dma_xa));
    puts("PASS: creator rollback does not bypass outstanding references");

    struct race r={.dev=&dev,.file=&f}; pthread_t thread[2];
    for (int i=0;i<2;i++) assert(!pthread_create(&thread[i],NULL,reader,&r));
    for (int i=0;i<10000;i++) {
        a=new_obj(&dev,7,0x9000);
        /* Previous in-flight readers may still hold the old object. */
        int ret=rknpu_gem_dma_token_insert(&rd,a);
        if (ret) { assert(ret==-EEXIST); rknpu_gem_object_put(&a->base); continue; }
        own(&f,1,a); sched_yield(); close_handle(&f,1);
    }
    atomic_store(&r.stop,true);
    for (int i=0;i<2;i++) assert(!pthread_join(thread[i],NULL));
    assert(atomic_load(&r.hits)>0); assert(xa_empty(&rd.gem_dma_xa));
    printf("PASS: 10000 create/close attempts with two concurrent DMA-token readers; hits=%d frees=%d\n",atomic_load(&r.hits),atomic_load(&freed));
    if (mem_profile) {
        assert(atomic_load(&rd.mem_stats[RKNPU_MEM_STAT_HANDLE_HIT])>0);
        assert(atomic_load(&rd.mem_stats[RKNPU_MEM_STAT_DMA_PROBES])>0);
        assert(atomic_load(&rd.mem_stats[RKNPU_MEM_STAT_LOOKUP_NS])>0);
    } else {
        for (int i=0;i<RKNPU_MEM_STAT_COUNT;i++) assert(!atomic_load(&rd.mem_stats[i]));
    }
    printf("PASS: profile=%d counter gating\n",mem_profile);
    return 0;
}
