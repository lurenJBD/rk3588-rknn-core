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
#define TASK_SIZE (sizeof(struct rknpu_task) * 4)

static int fail;

static int open_render(void)
{
	const char *paths[] = {
		"/dev/dri/renderD129", "/dev/dri/renderD128"
	};
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		int fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			printf("render=%s fd=%d\n", paths[i], fd);
			return fd;
		}
	}
	printf("open render failed: %s\n", strerror(errno));
	return -1;
}

static int expect_errno(const char *name, int ret, int expected)
{
	int saved = errno;
	int ok = ret < 0 && saved == expected;
	printf("%-32s ret=%d errno=%d(%s) expected=%d [%s]\n", name, ret,
	       saved, strerror(saved), expected, ok ? "PASS" : "FAIL");
	if (!ok)
		fail++;
	return ok;
}

static void create_task(int fd, unsigned int core_mask,
			struct rknpu_mem_create *c)
{
	memset(c, 0, sizeof(*c));
	c->size = TASK_SIZE;
	c->flags = 1 | 8 | 16; /* noncontig + kernel map + IOMMU */
	c->iommu_domain_id = 0;
	c->core_mask = core_mask;
	errno = 0;
	int ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
	printf("create task mask=%u             ret=%d errno=%d handle=%u dma=%#llx [%s]\n",
	       core_mask, ret, errno, c->handle,
	       (unsigned long long)c->dma_addr,
	       ret == 0 && c->handle && c->dma_addr ? "PASS" : "FAIL");
	if (ret || !c->handle || !c->dma_addr)
		fail++;
}

static void submit(int fd, struct rknpu_mem_create *c,
		   unsigned int core_mask, uint64_t base_addr,
		   uint32_t task_start, uint32_t task_number,
		   uint32_t flags, struct rknpu_submit *s)
{
	int ret;
	memset(s, 0, sizeof(*s));
	s->flags = flags;
	s->timeout = 1000;
	s->task_start = task_start;
	s->task_number = task_number;
	s->task_obj_addr = c->obj_addr;
	s->task_base_addr = base_addr;
	s->core_mask = core_mask;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, s);
	printf("submit mask=%u base=%#llx       ret=%d errno=%d counter=%u retbase=%#llx [%s]\n",
	       core_mask, (unsigned long long)base_addr, ret, errno,
	       s->task_counter, (unsigned long long)s->task_base_addr,
	       ret == 0 ? "PASS" : "FAIL");
	if (ret)
		fail++;
}

int main(void)
{
	struct rknpu_mem_create c;
	struct rknpu_submit s;
	struct rknpu_mem_destroy d;
	int fd = open_render();
	int ret;

	if (fd < 0)
		return 2;

	/* Valid single-core submits, including AUTO resolved to core0. */
	unsigned int masks[] = {CORE_AUTO, CORE0, CORE1, CORE2};
	for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
		create_task(fd, masks[i] ? masks[i] : CORE0, &c);
		if (!c.handle)
			continue;
		uint64_t expected = c.dma_addr;
		submit(fd, &c, masks[i], expected, 0, 4, 1, &s);
		if (s.task_base_addr != expected || s.task_counter != 4 ||
		    (masks[i] && s.core_mask != masks[i])) {
			printf("  returned metadata mismatch\n");
			fail++;
		}
		memset(&d, 0, sizeof(d));
		d.handle = c.handle;
		d.obj_addr = c.handle;
		errno = 0;
		ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
		printf("  destroy                      ret=%d errno=%d [%s]\n",
		       ret, errno, ret == 0 ? "PASS" : "FAIL");
		if (ret)
			fail++;
	}

	/* Multi-core submit remains intentionally unsupported in M5.3. */
	create_task(fd, CORE0 | CORE1 | CORE2, &c);
	memset(&s, 0, sizeof(s));
	s.flags = 1;
	s.timeout = 1000;
	s.task_number = 4;
	s.task_obj_addr = c.obj_addr;
	s.task_base_addr = c.dma_addr;
	s.core_mask = CORE0 | CORE1 | CORE2;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	expect_errno("submit mask=7", ret, EOPNOTSUPP);
	memset(&d, 0, sizeof(d));
	d.handle = c.handle;
	d.obj_addr = c.handle;
	ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);

	/* Buffer must be mapped on the selected core. */
	create_task(fd, CORE0, &c);
	memset(&s, 0, sizeof(s));
	s.flags = 1;
	s.timeout = 1000;
	s.task_number = 4;
	s.task_obj_addr = c.obj_addr;
	s.task_base_addr = c.dma_addr;
	s.core_mask = CORE2;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	expect_errno("submit wrong core mapping", ret, EINVAL);
	memset(&d, 0, sizeof(d));
	d.handle = c.handle;
	d.obj_addr = c.handle;
	ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);

	/* Base address mismatch is rejected. */
	create_task(fd, CORE1, &c);
	memset(&s, 0, sizeof(s));
	s.flags = 1;
	s.timeout = 1000;
	s.task_number = 4;
	s.task_obj_addr = c.obj_addr;
	s.task_base_addr = c.dma_addr + 0x1000;
	s.core_mask = CORE1;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	expect_errno("submit base mismatch", ret, EINVAL);
	memset(&d, 0, sizeof(d));
	d.handle = c.handle;
	d.obj_addr = c.handle;
	ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);

	/* Invalid task range is rejected. */
	create_task(fd, CORE2, &c);
	memset(&s, 0, sizeof(s));
	s.flags = 1;
	s.timeout = 1000;
	s.task_start = 103;
	s.task_number = 2;
	s.task_obj_addr = c.obj_addr;
	s.task_base_addr = c.dma_addr;
	s.core_mask = CORE2;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	expect_errno("submit task range", ret, EINVAL);
	memset(&d, 0, sizeof(d));
	d.handle = c.handle;
	d.obj_addr = c.handle;
	ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);

	/* Invalid handle remains ENOENT. */
	memset(&s, 0, sizeof(s));
	s.flags = 1;
	s.task_number = 1;
	s.task_obj_addr = 0x12345678;
	s.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	expect_errno("submit invalid handle", ret, ENOENT);

	printf("OVERALL: %s\n", fail ? "FAIL" : "PASS");
	close(fd);
	return fail ? 1 : 0;
}
