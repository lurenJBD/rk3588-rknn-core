#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Userspace control-flow model; does not load or exercise a kernel module.
from pathlib import Path
import re
import subprocess
import sys
import os
import tempfile

root=Path(__file__).resolve().parents[2]
repo=Path(__file__).resolve().parents[2]
_tmp=tempfile.TemporaryDirectory(prefix="rknpu-regression-")
out=Path(_tmp.name)
source=(Path(sys.argv[1]) if len(sys.argv)>1 else repo/'src/rknpu_gem.c').read_text()
fn=source[source.index('int rknpu_gem_sync_ioctl('):source.index('MODULE_IMPORT_NS("DMA_BUF")')]
stats=sorted(set(re.findall(r'RKNPU_MEM_STAT_\w+',fn)))
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t u64;
#define unlikely(x) (x)
#define RKNPU_MEM_SYNC_TO_DEVICE 1
#define RKNPU_MEM_SYNC_FROM_DEVICE 2
#define RKNPU_MEM_SYNC_MASK 3
#define RKNPU_MEM_CACHEABLE 2
#define DMA_BIDIRECTIONAL 0
#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define LOG_DEV_ERROR(...) ((void)0)
struct rknpu_device {int dummy;};
struct drm_device {void *dev_private;void *dev;};
struct drm_file {int dummy;};
struct drm_gem_object_funcs {int dummy;};
struct dma_buf_ops {int dummy;};
struct dma_buf {const struct dma_buf_ops *ops;void *priv;};
struct dma_buf_attachment {struct dma_buf *dmabuf;};
struct drm_gem_object {struct drm_device *dev;const struct drm_gem_object_funcs *funcs;struct dma_buf_attachment *import_attach;struct dma_buf *dma_buf;};
struct rknpu_gem_object {struct drm_gem_object base;unsigned int flags;unsigned long size,dma_addr;bool pages_backed;};
struct rknpu_mem_sync {unsigned int flags;u64 offset,size,obj_addr;};
static const struct drm_gem_object_funcs rknpu_gem_object_funcs={0},other_funcs={1};
static const struct dma_buf_ops drm_gem_prime_dmabuf_ops={0},foreign_ops={1};
static bool mem_profile;
static int puts_count,core_calls,begin_calls,end_calls,core_ret,begin_ret;
static struct rknpu_gem_object *lookup;
static unsigned long observed_offset,observed_size;
static unsigned int observed_flags;
static u64 ktime_get_ns(void) {return 0;}
static void rknpu_mem_stat_add(struct rknpu_device *d,int stat,u64 n) {(void)d;(void)stat;(void)n;}
static struct rknpu_gem_object *rknpu_gem_object_find_token(struct drm_device *d,struct drm_file *f,u64 t) {(void)d;(void)f;(void)t;return lookup;}
static void rknpu_gem_object_put(struct drm_gem_object *o) {(void)o;puts_count++;}
static bool rknpu_gem_sync_is_valid(unsigned int flags,u64 offset,u64 size,size_t object_size) {
    if(!flags || (flags & ~RKNPU_MEM_SYNC_MASK) || !size) return false;
    return offset<=object_size && size<=object_size-offset;
}
// Linux v6.18 drm_prime.c helper: ops identity gates the GEM dereference.
static bool drm_gem_is_prime_exported_dma_buf(struct drm_device *dev,struct dma_buf *dma_buf) {
    struct drm_gem_object *obj=dma_buf->priv;
    return (dma_buf->ops==&drm_gem_prime_dmabuf_ops) && (obj->dev==dev);
}
#define to_rknpu_obj(x) ((struct rknpu_gem_object *)(x))
static int rknpu_gem_sync_core_maps(struct rknpu_gem_object *o,unsigned int f,unsigned long off,unsigned long size) {core_calls++;if(!size && off>=o->size)return -EIO;observed_flags=f;observed_offset=off;observed_size=size;return core_ret;}
static int dma_buf_begin_cpu_access(struct dma_buf *b,int dir) {(void)b;(void)dir;begin_calls++;return begin_ret;}
static int dma_buf_end_cpu_access(struct dma_buf *b,int dir) {(void)b;(void)dir;end_calls++;return 0;}
static void dma_sync_single_range_for_device(void*d,unsigned long addr,u64 off,u64 size,int dir) {(void)d;(void)addr;(void)off;(void)size;(void)dir;}
static void dma_sync_single_range_for_cpu(void*d,unsigned long addr,u64 off,u64 size,int dir) {(void)d;(void)addr;(void)off;(void)size;(void)dir;}
'''
main=r'''
int main(int argc,char **argv) {
    int which=argc>1?atoi(argv[1]):0;
    struct rknpu_device device={0};struct drm_device dev={&device,0},other={&device,0};struct drm_file file={0};
    struct rknpu_gem_object exporter={.base={.dev=&dev,.funcs=&rknpu_gem_object_funcs},.flags=2,.size=256,.dma_addr=4096,.pages_backed=true};
    void *tiny=malloc(1);
    struct dma_buf db={.ops=&foreign_ops,.priv=tiny};
    struct dma_buf_attachment attach={.dmabuf=&db};
    struct rknpu_gem_object imported={.base={.dev=&dev,.import_attach=&attach,.dma_buf=&db},.size=256};lookup=&imported;
    struct rknpu_mem_sync args={.flags=1,.offset=64,.size=64,.obj_addr=1};
    int expected=-EOPNOTSUPP,expected_core=0,expected_begin=0,expected_end=0;
    switch(which) {
      case 0: break; // foreign one-byte private object
      case 1: db.priv=NULL;break;
      case 2: db.priv=(void*)1;break;
      case 3: db.ops=&drm_gem_prime_dmabuf_ops;db.priv=&exporter.base;exporter.base.dev=&other;break;
      case 4: db.ops=&drm_gem_prime_dmabuf_ops;db.priv=&exporter.base;exporter.base.funcs=&other_funcs;break;
      case 5: db.ops=&drm_gem_prime_dmabuf_ops;db.priv=&exporter.base;expected=0;expected_core=1;break;
      case 6: args.offset=0;args.size=256;expected=0;expected_begin=expected_end=1;break;
      case 7: args.offset=0;args.size=256;begin_ret=-EIO;expected=-EIO;expected_begin=1;break;
      case 8: imported.base.import_attach=NULL;imported.flags=2;imported.dma_addr=4096;imported.pages_backed=true;expected=0;expected_core=1;break;
      case 9: imported.base.import_attach=NULL;imported.flags=2;expected=-ENXIO;break;
      case 10: db.ops=&drm_gem_prime_dmabuf_ops;db.priv=&exporter.base;core_ret=-EIO;expected=-EIO;expected_core=1;break;
      case 11: attach.dmabuf=NULL;break;
      case 12: args.flags=8;expected=-EINVAL;break;
      case 14: args.offset=0;args.size=0;expected=0;break;
      case 15: args.offset=256;args.size=0;expected=0;break;
      case 16: imported.base.import_attach=NULL;imported.flags=2;imported.dma_addr=4096;imported.pages_backed=true;args.offset=256;args.size=0;expected=0;break;
      case 17: args.flags=8;args.size=0;expected=-EINVAL;break;
      case 18: args.offset=257;args.size=0;expected=-EINVAL;break;
      case 13: db.ops=&drm_gem_prime_dmabuf_ops;db.priv=&exporter.base;args.flags=3;expected=0;expected_core=1;break;
      default: return 2;
    }
    int ret=rknpu_gem_sync_ioctl(&dev,&args,&file);
    bool ok=ret==expected && puts_count==1 && core_calls==expected_core && begin_calls==expected_begin && end_calls==expected_end;
    if(core_calls) ok &= observed_flags==args.flags && observed_offset==args.offset && observed_size==args.size;
    printf("case=%d ret=%d puts=%d core=%d begin=%d end=%d %s\n",which,ret,puts_count,core_calls,begin_calls,end_calls,ok?"PASS":"FAIL");
    free(tiny);return ok?0:1;
}
'''
cpp=out/'sync_regression.c'
cpp.write_text(prefix+'\nenum {'+','.join(stats)+'};\n'+fn+main)
subprocess.run([os.getenv('CC','cc'),'-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(out/'sync_regression')],check=True)
results=[];failures=0
for case in range(19):
    r=subprocess.run([str(out/'sync_regression'),str(case)],capture_output=True,text=True)
    failures+=r.returncode!=0
    results.append(r.stdout+r.stderr)
    if r.returncode and not r.stdout: print(f'case={case} failed: '+r.stderr.splitlines()[0])
    else:print(r.stdout,end='')
summary=f'cases=19 failures={failures}\n';print(summary,end='')
(out/('sync-baseline-results.txt' if len(sys.argv)>1 else 'sync-fixed-results.txt')).write_text(''.join(results)+summary)
sys.exit(bool(failures))
