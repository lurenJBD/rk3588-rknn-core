#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>

#include "rknpu_ioctl.h"

#define CORE0 1u
#define CORE1 2u
#define CORE2 4u
#define TASK_SIZE (sizeof(struct rknpu_task) * 4)

static int fail;

static int open_render(void)
{
	const char *paths[] = {
		"/dev/dri/renderD129",
		"/dev/dri/renderD128",
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

	printf("%-34s ret=%d errno=%d(%s) expected=%d [%s]\n", name, ret,
	       saved, strerror(saved), expected, ok ? "PASS" : "FAIL");
	if (!ok)
		fail++;
	return ok;
}

static int create_task(int fd, unsigned int core_mask,
		       struct rknpu_mem_create *c)
{
	int ret;

	memset(c, 0, sizeof(*c));
	c->size = TASK_SIZE;
	c->flags = 1 | 2 | 8 | 16; /* noncontig + cacheable + kernel map + IOMMU */
	c->iommu_domain_id = 0;
	c->core_mask = core_mask;

	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
	printf("create task mask=%u                    ret=%d errno=%d handle=%u dma=%#llx [%s]\n",
	       core_mask, ret, errno, c->handle,
	       (unsigned long long)c->dma_addr,
	       ret == 0 && c->handle && c->dma_addr ? "PASS" : "FAIL");
	if (ret || !c->handle || !c->dma_addr)
		fail++;
	return ret;
}

static int submit_dma(int fd, struct rknpu_mem_create *c,
		      unsigned int core_mask)
{
	struct rknpu_submit s;
	int ret;

	memset(&s, 0, sizeof(s));
	s.flags = RKNPU_JOB_PC;
	s.timeout = 1000;
	s.task_number = 4;
	s.task_obj_addr = c->dma_addr;
	s.task_base_addr = c->dma_addr;
	s.core_mask = core_mask;

	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	printf("submit dma mask=%u                    ret=%d errno=%d counter=%u retbase=%#llx [%s]\n",
	       core_mask, ret, errno, s.task_counter,
	       (unsigned long long)s.task_base_addr,
	       ret == 0 && s.task_counter == 4 &&
			       s.task_base_addr == c->dma_addr ?
		       "PASS" : "FAIL");
	if (ret || s.task_counter != 4 || s.task_base_addr != c->dma_addr)
		fail++;
	return ret;
}

static int sync_dma(int fd, struct rknpu_mem_create *c)
{
	struct rknpu_mem_sync s;
	int ret;

	memset(&s, 0, sizeof(s));
	s.flags = RKNPU_MEM_SYNC_TO_DEVICE;
	s.obj_addr = c->dma_addr;
	s.size = c->size;

	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &s);
	printf("sync dma                              ret=%d errno=%d [%s]\n",
	       ret, errno, ret == 0 ? "PASS" : "FAIL");
	if (ret)
		fail++;
	return ret;
}

static int destroy(int fd, struct rknpu_mem_create *c)
{
	struct rknpu_mem_destroy d;
	int ret;

	memset(&d, 0, sizeof(d));
	d.handle = c->handle;
	d.obj_addr = c->handle;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
	printf("destroy handle                        ret=%d errno=%d [%s]\n",
	       ret, errno, ret == 0 ? "PASS" : "FAIL");
	if (ret)
		fail++;
	return ret;
}

static void test_single_core_dma_token(int fd, unsigned int core_mask)
{
	struct rknpu_mem_create c;

	if (create_task(fd, core_mask, &c))
		return;

	sync_dma(fd, &c);
	submit_dma(fd, &c, core_mask);
	destroy(fd, &c);

	/* Destroy removes all tokens: the same DMA address must not resolve. */
	{
		struct rknpu_mem_sync s;
		int ret;

		memset(&s, 0, sizeof(s));
		s.flags = RKNPU_MEM_SYNC_TO_DEVICE;
		s.obj_addr = c.dma_addr;
		s.size = c.size;
		errno = 0;
		ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &s);
		expect_errno("sync destroyed dma token", ret, ENOENT);
	}
}

static void test_handle_compatibility(int fd)
{
	struct rknpu_mem_create c;
	struct rknpu_mem_sync sync;
	struct rknpu_submit submit;
	int ret;

	if (create_task(fd, CORE1, &c))
		return;

	memset(&sync, 0, sizeof(sync));
	sync.flags = RKNPU_MEM_SYNC_TO_DEVICE;
	sync.obj_addr = c.handle;
	sync.size = c.size;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync);
	printf("sync handle compatibility             ret=%d errno=%d [%s]\n",
	       ret, errno, ret == 0 ? "PASS" : "FAIL");
	if (ret)
		fail++;

	memset(&submit, 0, sizeof(submit));
	submit.flags = RKNPU_JOB_PC;
	submit.timeout = 1000;
	submit.task_number = 4;
	submit.task_obj_addr = c.handle;
	submit.task_base_addr = c.dma_addr;
	submit.core_mask = CORE1;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &submit);
	printf("submit handle compatibility           ret=%d errno=%d counter=%u [%s]\n",
	       ret, errno, submit.task_counter,
	       ret == 0 && submit.task_counter == 4 ? "PASS" : "FAIL");
	if (ret || submit.task_counter != 4)
		fail++;

	destroy(fd, &c);
}

static void test_invalid_dma_token(int fd)
{
	struct rknpu_mem_sync sync;
	struct rknpu_submit submit;
	int ret;

	memset(&sync, 0, sizeof(sync));
	sync.flags = RKNPU_MEM_SYNC_TO_DEVICE;
	sync.obj_addr = 0x1234567800000000ULL;
	sync.size = TASK_SIZE;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &sync);
	expect_errno("sync invalid dma token", ret, ENOENT);

	memset(&submit, 0, sizeof(submit));
	submit.flags = RKNPU_JOB_PC;
	submit.timeout = 1000;
	submit.task_number = 4;
	submit.task_obj_addr = 0x1234567800000000ULL;
	submit.task_base_addr = 0x1234567800000000ULL;
	submit.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &submit);
	expect_errno("submit invalid dma token", ret, ENOENT);
}

int main(void)
{
	int fd = open_render();

	if (fd < 0)
		return 2;

	test_single_core_dma_token(fd, CORE0);
	test_single_core_dma_token(fd, CORE1);
	test_single_core_dma_token(fd, CORE2);
	test_handle_compatibility(fd);
	test_invalid_dma_token(fd);

	printf("OVERALL: %s\n", fail ? "FAIL" : "PASS");
	close(fd);
	return fail ? 1 : 0;
}
