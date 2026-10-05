#!/usr/bin/env python3
"""Run the production mmap callback with GEM reference and exporter mocks."""
from pathlib import Path
import os,re,subprocess,sys,tempfile
source=Path(sys.argv[1]).read_text()
start=re.search(r'^int rknpu_gem_mmap_obj\(',source,re.M).start();brace=source.index('{',start);end=brace+1;depth=1
while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
callback=source[start:end]
headers=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#define RKNPU_MEM_CACHEABLE 1
#define RKNPU_MEM_WRITE_COMBINE 2
#define LOG_DEBUG(...) ((void)0)
struct drm_gem_object {void *import_attach;int refs;};
struct rknpu_gem_object {struct drm_gem_object base;int flags;};
struct vm_area_struct {int vm_flags,vm_page_prot;};
static int mappings,result;
static struct rknpu_gem_object *to_rknpu_obj(struct drm_gem_object *o) {return (void*)o;}
static int vm_get_page_prot(int f){return f;}
static int pgprot_writecombine(int p){return p+10;}
static int pgprot_noncached(int p){return p+20;}
static int rknpu_gem_mmap_buffer(struct rknpu_gem_object *o,struct vm_area_struct *v){(void)o;(void)v;mappings++;return result;}
'''
main=r'''
static int core_mmap(struct drm_gem_object *o,struct vm_area_struct *v) {
 o->refs++;int ret=rknpu_gem_mmap_obj(o,v);if(ret)o->refs--;return ret;
}
int main(void) {
 struct rknpu_gem_object o={.base={.refs=1}};struct vm_area_struct v={.vm_flags=7,.vm_page_prot=99};
 o.base.import_attach=&o;assert(core_mmap(&o.base,&v)==-EINVAL);
 assert(mappings==0 && o.base.refs==1 && v.vm_page_prot==99);
 puts("import rejected without mapping or changing exporter cache policy PASS");
 o.base.import_attach=NULL;
 for(int flags=0;flags<3;flags++) {
  o.flags=flags;result=0;assert(core_mmap(&o.base,&v)==0);assert(o.base.refs==2);o.base.refs--;
  assert(v.vm_page_prot==7+(flags==1?0:flags==2?10:20));
  result=-ENOMEM;assert(core_mmap(&o.base,&v)==-ENOMEM && o.base.refs==1);
 }
 puts("native cache modes and mapping error references PASS");
}
'''
with tempfile.TemporaryDirectory(prefix='rknpu-mmap-guard-') as tmp:
 c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(headers+callback+main)
 flags=['-std=gnu11','-O2','-Wall','-Wextra']
 if os.getenv('SANITIZE'):flags+=['-g','-fsanitize='+os.environ['SANITIZE']]
 subprocess.run([os.getenv('CC','cc'),*flags,str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
for name in ('rknpu_gem_mmap','rknpu_gem_prime_mmap'):
 if re.search(r'^int '+name+r'\(',source,re.M):raise RuntimeError('dead wrapper remains: '+name)
print('dead mmap wrappers removed PASS')
