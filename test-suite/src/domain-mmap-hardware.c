#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <drm/drm.h>
#include "rknpu_ioctl.h"

struct snapshot { int id; unsigned cores,active; unsigned long long objs,bytes,reclaimed; };
static struct snapshot state(int id) {
    FILE *fp=fopen("/proc/rknpu/iommu_domains","r");
    if(!fp) fp=fopen("/sys/kernel/debug/rknpu/iommu_domains","r");
    assert(fp); struct snapshot s={.id=id},line;char buf[256];
    while(fgets(buf,sizeof(buf),fp)) {
        if(sscanf(buf,"domain=%d cores=%x active=%u objs=%llu bytes=%llu reclaimed=%llu",&line.id,&line.cores,&line.active,&line.objs,&line.bytes,&line.reclaimed)==6 && (id<0 ? line.id>=16 && line.objs>0 : line.id==id)){s=line;break;}
    }
    fclose(fp);return s;
}
static char device_path[64];
static int open_npu(void) {
    if(*device_path) {int fd=open(device_path,O_RDWR|O_CLOEXEC);assert(fd>=0);return fd;}
    for(int i=0;i<8;i++) {
        snprintf(device_path,sizeof(device_path),i<4?"/dev/dri/card%d":"/dev/dri/renderD%d",i<4?i:128+i-4);
        int fd=open(device_path,O_RDWR|O_CLOEXEC);if(fd<0)continue;
        char name[32]={0};struct drm_version version={.name_len=sizeof(name)-1,.name=name};
        if(ioctl(fd,DRM_IOCTL_VERSION,&version)==0 && strcmp(name,"rknpu")==0)return fd;
        close(fd);
    }
    fprintf(stderr,"NPU DRM device not found\n");exit(2);
}
static struct rknpu_mem_create create(int fd) {
    struct rknpu_mem_create c={.size=0x20000,.flags=1|4|16,.core_mask=7};
    assert(ioctl(fd,DRM_IOCTL_RKNPU_MEM_CREATE,&c)==0);return c;
}
static void destroy(int fd,unsigned handle) {
    struct rknpu_mem_destroy d={.handle=handle};assert(ioctl(fd,DRM_IOCTL_RKNPU_MEM_DESTROY,&d)==0);
}
static void empty(int id,unsigned long long minimum) {
    for(int i=0;i<500;i++) {
        struct snapshot s=state(id);
        if(!s.cores && !s.active && !s.objs && s.reclaimed>=minimum)return;
        usleep(10000);
    }
    assert(!"domains not reclaimed");
}
static int export_fd(int fd,unsigned handle) {
    struct drm_prime_handle p={.handle=handle,.flags=DRM_CLOEXEC|DRM_RDWR};assert(ioctl(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&p)==0);return p.fd;
}
int main(void) {
    int fd,id=-1;unsigned long long freed=0;
    for(int i=0;i<100;i++) {
        fd=open_npu();struct rknpu_mem_create c=create(fd);struct snapshot s=state(-1);id=s.id;
        assert(id>=16 && s.cores==7 && s.objs==1 && s.active==0);freed=s.reclaimed;
        destroy(fd,c.handle);s=state(id);
        assert(s.objs==0 && s.cores==7 && s.reclaimed==freed);
        close(fd);empty(id,freed+3);
    }
    printf("100 client-owned empty-cache and final-close reclamation cycles PASS domain=%d\n",id);
    fd=open_npu();struct rknpu_mem_create old=create(fd);struct snapshot s=state(-1);id=s.id;freed=s.reclaimed;
    int dma=export_fd(fd,old.handle);destroy(fd,old.handle);close(fd);
    s=state(id);assert(s.cores==7 && s.objs==1 && s.reclaimed==freed);
    puts("exported dma-buf pins domain after original FD closes PASS");
    fd=open_npu();struct rknpu_mem_create next=create(fd);s=state(id);
    assert(s.objs==2 && next.dma_addr!=old.dma_addr && s.cores==7);
    destroy(fd,next.handle);close(fd);s=state(id);assert(s.objs==1 && s.reclaimed==freed);
    puts("domain ID reuse with retained old GEM preserves IOVA isolation PASS");
    unsigned char *map=mmap(NULL,old.size,PROT_READ|PROT_WRITE,MAP_SHARED,dma,0);assert(map!=MAP_FAILED);
    memset(map,0x5a,old.size);close(dma);s=state(id);assert(s.objs==1 && s.cores==7);
    assert(map[0]==0x5a && map[old.size-1]==0x5a);
    munmap(map,old.size);empty(id,freed+3);puts("VMA pins GEM; last munmap reclaims domain PASS");
    for(int i=0;i<50;i++){fd=open_npu();struct rknpu_mem_create c=create(fd);(void)c;s=state(-1);close(fd);empty(s.id,s.reclaimed+3);}
    puts("50 client close and ID reuse cycles PASS");
    int heap=open("/dev/dma_heap/default_cma_region",O_RDWR|O_CLOEXEC);assert(heap>=0);
    struct dma_heap_allocation_data a={.len=0x20000,.fd_flags=O_RDWR|O_CLOEXEC};assert(ioctl(heap,DMA_HEAP_IOCTL_ALLOC,&a)==0);close(heap);
    fd=open_npu();struct drm_prime_handle p={.fd=a.fd};assert(ioctl(fd,DRM_IOCTL_PRIME_FD_TO_HANDLE,&p)==0);
    struct rknpu_mem_map m={.handle=p.handle};errno=0;assert(ioctl(fd,DRM_IOCTL_RKNPU_MEM_MAP,&m)==-1 && errno==EINVAL);
    int again=export_fd(fd,p.handle);
    map=mmap(NULL,a.len,PROT_READ|PROT_WRITE,MAP_SHARED,again,0);assert(map!=MAP_FAILED);
    memset(map,0xa5,a.len);assert(map[0]==0xa5 && map[a.len-1]==0xa5);
    munmap(map,a.len);close(again);close(a.fd);close(fd);
    puts("imported handle rejects MEM_MAP; original exporter re-export mmap works PASS");
    puts("OVERALL PASS");return 0;
}
