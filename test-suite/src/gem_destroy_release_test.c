/*
 * MEM_DESTROY must actually release the GEM handle and its IOVA mapping.
 *
 * The bug this covers: rknpu_gem_destroy_ioctl() used to reject the call with
 * -EINVAL unless args->obj_addr equalled args->handle. librknnrt sends
 * obj_addr == 0 (it zeroes the whole struct and fills in handle only), so
 * every real MEM_DESTROY was refused. Nothing leaked visibly in the demo
 * because the process then exited and closing the DRM fd dropped all handles,
 * which is exactly why a single-run test could not see it.
 *
 * What this test does instead: stay inside one process and loop
 * create/destroy. Two independent things then become observable.
 *
 *   1. Every destroy must return 0, including the obj_addr == 0 form that
 *      librknnrt actually sends. Against the old code these all failed with
 *      EINVAL.
 *
 *   2. The IOVA allocator must hand the same address back on the next
 *      allocate. drm_mm returns the lowest-fitting free gap, so a released
 *      1 MiB node comes straight back. If destroy were a silent no-op the
 *      node would stay allocated and the addresses would keep advancing,
 *      which is the leak, made visible without needing any counters.
 *
 * The second check is what makes this a real test rather than a restatement
 * of the fix: reverting the driver change makes it fail.
 *
 * Requires the rknpu_native module loaded with RKNPU_MODE=full and the NPU
 * powered on (see artifacts/tools/run-mem-destroy-test.sh).
 */

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
/* noncontiguous + cacheable + kernel-mapped + IOMMU */
#define CREATE_FLAGS (1u | 2u | 8u | 16u)
#define BUF_SIZE (1024u * 1024u)
#define ROUNDS 8

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

static int create_buf(int fd, struct rknpu_mem_create *c)
{
	int ret;

	memset(c, 0, sizeof(*c));
	c->size = BUF_SIZE;
	c->flags = CREATE_FLAGS;
	c->iommu_domain_id = 0;
	c->core_mask = CORE0;

	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
	if (ret) {
		printf("  create FAIL ret=%d errno=%d(%s)\n", ret, errno,
		       strerror(errno));
		fail++;
	}
	return ret;
}

/*
 * Send exactly what librknnrt sends: handle filled in, obj_addr left at 0.
 * The driver must accept it and release the handle.
 */
static int destroy_like_runtime(int fd, unsigned int handle)
{
	struct rknpu_mem_destroy d;
	int ret;

	memset(&d, 0, sizeof(d));
	d.handle = handle;
	/* d.obj_addr stays 0 */

	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
	if (ret || errno) {
		printf("  destroy FAIL ret=%d errno=%d(%s)\n", ret, errno,
		       strerror(errno));
		fail++;
	}
	return ret;
}

/* A handle that was just destroyed must no longer resolve. */
static void expect_destroyed_handle_rejected(int fd, unsigned int handle)
{
	struct rknpu_mem_destroy d;
	int ret;

	memset(&d, 0, sizeof(d));
	d.handle = handle;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
	printf("  reuse destroyed handle                ret=%d errno=%d(%s) [%s]\n",
	       ret, errno, strerror(errno),
	       ret < 0 && errno == EINVAL ? "PASS" : "FAIL");
	if (!(ret < 0 && errno == EINVAL))
		fail++;
}

/*
 * The core check. Run create/destroy repeatedly in one process and print the
 * IOVA each round. A working destroy must let the allocator reuse the address.
 */
static void test_release_and_reuse(int fd)
{
	struct rknpu_mem_create c;
	uint64_t prev_dma = 0;
	unsigned int prev_handle = 0;
	int reused = 0;
	int i;

	printf("round  handle  dma_addr            iova_reused\n");

	for (i = 0; i < ROUNDS; i++) {
		if (create_buf(fd, &c))
			return;

		printf("%5d  %-7u %#018llx  %s\n", i, c.handle,
		       (unsigned long long)c.dma_addr,
		       prev_dma && c.dma_addr == prev_dma ? "yes" : "no");

		if (prev_dma && c.dma_addr == prev_dma)
			reused++;

		prev_dma = c.dma_addr;
		prev_handle = c.handle;

		if (destroy_like_runtime(fd, c.handle))
			return;
	}

	/*
	 * Every round after the first must land on the address the previous
	 * round released. Anything less means the node is still allocated.
	 */
	printf("iova reuse: %d/%d rounds [%s]\n", reused, ROUNDS - 1,
	       reused == ROUNDS - 1 ? "PASS" : "FAIL");
	if (reused != ROUNDS - 1)
		fail++;

	expect_destroyed_handle_rejected(fd, prev_handle);
}

int main(void)
{
	int fd = open_render();

	if (fd < 0)
		return 2;

	test_release_and_reuse(fd);

	printf("OVERALL: %s\n", fail ? "FAIL" : "PASS");
	close(fd);
	return fail ? 1 : 0;
}
