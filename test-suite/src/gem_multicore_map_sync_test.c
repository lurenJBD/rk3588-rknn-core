#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/drm.h>
#include "rknpu_ioctl.h"

#define CORE_AUTO 0u
#define CORE0 1u
#define CORE1 2u
#define CORE2 4u

static int fail;

static int create(int fd, unsigned int core_mask, unsigned int flags,
		  struct rknpu_mem_create *c)
{
	memset(c, 0, sizeof(*c));
	c->size = 0x1000;
	c->flags = flags;
	c->iommu_domain_id = 0;
	c->core_mask = core_mask;
	errno = 0;
	return ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
}

static int expect_create_flags_ok(int fd, unsigned int core_mask,
				   unsigned int flags,
				   struct rknpu_mem_create *c)
{
	int ret = create(fd, core_mask, flags, c);
	printf("create mask=%-2u ret=%d errno=%d handle=%u dma_addr=%#llx [%s]\n",
	       core_mask, ret, errno, c->handle,
	       (unsigned long long)c->dma_addr,
	       ret == 0 && c->handle && c->dma_addr ? "PASS" : "FAIL");
	if (ret || !c->handle || !c->dma_addr) {
		fail++;
		return -1;
	}
	if (c->size != 0x1000 || c->sram_size != 0 ||
	    c->obj_addr != c->handle) {
		printf("  metadata check FAIL\n");
		fail++;
	}
	return 0;
}

static int sync_test(int fd, unsigned int core_mask, unsigned int flags)
{
	struct rknpu_mem_create c;
	struct rknpu_mem_sync sync;
	struct rknpu_mem_destroy d;
	int ret, i;

	if (expect_create_flags_ok(fd, core_mask, flags, &c))
		return -1;

	for (i = 0; i < 3; i++) {
		unsigned int sync_flags = i + 1;

		memset(&sync, 0, sizeof(sync));
		sync.obj_addr = c.obj_addr;
		sync.flags = sync_flags;
		sync.size = c.size;
		errno = 0;
		ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync);
		printf("  sync mask=%u flags=%u    ret=%d errno=%d [%s]\n",
		       core_mask, sync_flags, ret, errno,
		       ret == 0 ? "PASS" : "FAIL");
		if (ret)
			fail++;
	}

	memset(&d, 0, sizeof(d));
	d.handle = c.handle;
	d.obj_addr = c.obj_addr;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
	printf("  destroy sync-test  ret=%d errno=%d [%s]\n", ret, errno,
	       ret == 0 ? "PASS" : "FAIL");
	if (ret)
		fail++;

	return 0;
}

int main(void)
{
	const unsigned int masks[] = {
		CORE_AUTO, CORE0, CORE1, CORE2,
		CORE0 | CORE1, CORE0 | CORE2, CORE1 | CORE2,
		CORE0 | CORE1 | CORE2
	};
	struct rknpu_mem_create c;
	struct rknpu_mem_destroy d;
	struct rknpu_submit s;
	struct rknpu_mem_sync sync;
	int fd, i, ret;

	fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open renderD129 failed: %s\n", strerror(errno));
		return 2;
	}
	printf("fd=%d\n", fd);

	for (i = 0; i < (int)(sizeof(masks) / sizeof(masks[0])); i++) {
		if (expect_create_flags_ok(fd, masks[i], 0, &c))
			continue;
		memset(&d, 0, sizeof(d));
		d.handle = c.handle;
		d.obj_addr = c.handle;
		errno = 0;
		ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
		printf("  destroy             ret=%d errno=%d [%s]\n", ret,
		       errno, ret == 0 ? "PASS" : "FAIL");
		if (ret)
			fail++;
	}

	/* Runtime-like page-backed IOMMU allocation must also map every core. */
	for (i = 0; i < (int)(sizeof(masks) / sizeof(masks[0])); i++) {
		if (expect_create_flags_ok(fd, masks[i],
					   1 | 4 | 16, &c))
			continue;
		memset(&d, 0, sizeof(d));
		d.handle = c.handle;
		d.obj_addr = c.handle;
		errno = 0;
		ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
		printf("  destroy iommu flags ret=%d errno=%d [%s]\n", ret,
		       errno, ret == 0 ? "PASS" : "FAIL");
		if (ret)
			fail++;
	}

	/* M5.2: sync every mapped core for cacheable page-backed objects. */
	sync_test(fd, CORE0, 2);
	sync_test(fd, CORE1, 2);
	sync_test(fd, CORE2, 2);
	sync_test(fd, CORE0 | CORE1, 2);
	sync_test(fd, CORE1 | CORE2, 2);
	sync_test(fd, CORE0 | CORE1 | CORE2, 2);

	/* Invalid mask and zero size are rejected. */
	ret = create(fd, 0x8, 0, &c);
	printf("create mask=8          ret=%d errno=%d expected=EINVAL [%s]\n",
	       ret, errno,
	       ret < 0 && errno == EINVAL ? "PASS" : "FAIL");
	if (!(ret < 0 && errno == EINVAL))
		fail++;

	memset(&c, 0, sizeof(c));
	c.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, &c);
	printf("create size=0          ret=%d errno=%d expected=EINVAL [%s]\n",
	       ret, errno,
	       ret < 0 && errno == EINVAL ? "PASS" : "FAIL");
	if (!(ret < 0 && errno == EINVAL))
		fail++;

	/* M5.1 does not implement submit yet. */
	memset(&s, 0, sizeof(s));
	s.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	printf("submit                 ret=%d errno=%d expected=EOPNOTSUPP [%s]\n",
	       ret, errno,
	       ret < 0 && errno == EOPNOTSUPP ? "PASS" : "FAIL");
	if (!(ret < 0 && errno == EOPNOTSUPP))
		fail++;

	/* Invalid-handle MEM_SYNC still returns ENOENT. */
	memset(&sync, 0, sizeof(sync));
	sync.obj_addr = 0x12345678;
	sync.flags = 1;
	sync.size = 0x1000;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync);
	printf("sync invalid handle    ret=%d errno=%d expected=ENOENT [%s]\n",
	       ret, errno,
	       ret < 0 && errno == ENOENT ? "PASS" : "FAIL");
	if (!(ret < 0 && errno == ENOENT))
		fail++;

	printf("OVERALL: %s\n", fail ? "FAIL" : "PASS");
	close(fd);
	return fail ? 1 : 0;
}
