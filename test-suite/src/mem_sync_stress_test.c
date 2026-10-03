/*
 * MEM_SYNC tolerance + concurrency stress for RKNPU (PR #5 / merged 5d65058).
 *
 * Edge cases:
 *   must=1  -> call must return 0
 *   must=0  -> call must return <0
 *   must=-1 -> informational (no pass/fail), prints the observed errno
 *
 * Concurrency: threads repeatedly MEM_CREATE / MEM_SYNC (valid, over-range,
 * zero-length) / MEM_DESTROY and record the first few failures with op+errno.
 *
 * Usage: memsync_stress [threads] [iters]
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include "rknpu_ioctl.h"

#define OBJ_FLAGS (RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_CACHEABLE | \
		   RKNPU_MEM_KERNEL_MAPPING | RKNPU_MEM_IOMMU)

static atomic_int failures;
static atomic_long ops_done;

static int open_dev(void)
{
	const char *paths[] = { "/dev/dri/renderD128", "/dev/dri/renderD129" };
	int fd;

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			printf("render=%s fd=%d\n", paths[i], fd);
			return fd;
		}
	}
	printf("open render node failed: %s\n", strerror(errno));
	return -1;
}

static int create_obj(int fd, struct rknpu_mem_create *c, uint32_t size)
{
	memset(c, 0, sizeof(*c));
	c->size = size;
	c->flags = OBJ_FLAGS;
	c->iommu_domain_id = 0;
	c->core_mask = 0;
	errno = 0;
	return ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
}

static int destroy_obj(int fd, struct rknpu_mem_create *c)
{
	struct rknpu_mem_destroy d;

	memset(&d, 0, sizeof(d));
	d.handle = c->handle;
	d.obj_addr = c->obj_addr;
	errno = 0;
	return ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
}

static int sync_obj(int fd, uint64_t obj, uint32_t flags, uint64_t off,
		    uint64_t size)
{
	struct rknpu_mem_sync s;

	memset(&s, 0, sizeof(s));
	s.obj_addr = obj;
	s.flags = flags;
	s.offset = off;
	s.size = size;
	errno = 0;
	return ioctl(fd, DRM_IOCTL_RKNPU_MEM_SYNC, &s);
}

struct edge {
	const char *name;
	uint32_t flags;
	uint64_t off;
	uint64_t size;
	int must; /* 1 = ret==0, 0 = ret<0, -1 = info */
};

static void run_edges(int fd, uint32_t obj_size)
{
	const struct edge edges[] = {
		{ "valid TO_DEVICE", RKNPU_MEM_SYNC_TO_DEVICE, 0, obj_size, 1 },
		{ "valid TO|FROM", RKNPU_MEM_SYNC_TO_DEVICE | RKNPU_MEM_SYNC_FROM_DEVICE, 0, obj_size, 1 },
		{ "over-range clamp (issue#4)", RKNPU_MEM_SYNC_TO_DEVICE, 0, obj_size * 2, 1 },
		{ "over-range FROM_DEVICE", RKNPU_MEM_SYNC_FROM_DEVICE, 0, obj_size * 2, 1 },
		{ "over-range with offset", RKNPU_MEM_SYNC_TO_DEVICE, 0x1000, obj_size, 1 },
		{ "zero-length (offset 0)", RKNPU_MEM_SYNC_TO_DEVICE, 0, 0, 1 },
		{ "offset==size, size 0", RKNPU_MEM_SYNC_FROM_DEVICE, obj_size, 0, -1 },
		{ "offset==size, size>0", RKNPU_MEM_SYNC_TO_DEVICE, obj_size, 0x1000, -1 },
		{ "offset beyond size", RKNPU_MEM_SYNC_TO_DEVICE, obj_size + 0x1000, 0x1000, 0 },
		{ "flags==0", 0, 0, obj_size, 0 },
		{ "invalid flags bit", 0x8, 0, obj_size, 0 },
	};
	struct rknpu_mem_create c;
	int i, ret, bad = 0;

	if (create_obj(fd, &c, obj_size)) {
		printf("create failed: %s\n", strerror(errno));
		atomic_fetch_add(&failures, 1);
		return;
	}
	for (i = 0; i < (int)(sizeof(edges) / sizeof(edges[0])); i++) {
		const struct edge *e = &edges[i];
		const char *verdict;

		ret = sync_obj(fd, c.obj_addr, e->flags, e->off, e->size);
		if (e->must == 1)
			verdict = (ret == 0) ? "PASS" : "FAIL";
		else if (e->must == 0)
			verdict = (ret < 0) ? "PASS" : "FAIL";
		else
			verdict = "INFO";
		printf("  %-30s ret=%d errno=%d %s\n", e->name, ret, errno,
		       verdict);
		if (strcmp(verdict, "FAIL") == 0) {
			bad++;
			atomic_fetch_add(&failures, 1);
		}
	}
	if (destroy_obj(fd, &c)) {
		printf("  destroy after edges FAILED errno=%d\n", errno);
		atomic_fetch_add(&failures, 1);
	}
	printf("edge cases: %s\n", bad ? "FAIL" : "PASS");
}

struct worker_arg {
	int fd;
	int iters;
	int idx;
};

static void note(const char *op, int err)
{
	if (atomic_fetch_add(&failures, 1) < 8)
		printf("hammer failure: op=%s errno=%d(%s)\n", op, err,
		       strerror(err));
}

static void *hammer(void *p)
{
	struct worker_arg *a = p;
	struct rknpu_mem_create c;
	int i;

	for (i = 0; i < a->iters; i++) {
		uint32_t size = 0x1000 + ((i + a->idx) % 5) * 0x2000;

		if (create_obj(a->fd, &c, size)) {
			note("create", errno);
			continue;
		}
		if (sync_obj(a->fd, c.obj_addr, RKNPU_MEM_SYNC_TO_DEVICE, 0, size))
			note("sync-valid", errno);
		if (sync_obj(a->fd, c.obj_addr, RKNPU_MEM_SYNC_FROM_DEVICE, 0, size * 2))
			note("sync-overrange", errno);
		if (sync_obj(a->fd, c.obj_addr, RKNPU_MEM_SYNC_TO_DEVICE, 0, 0))
			note("sync-zero", errno);
		/* bad offset must never succeed */
		if (sync_obj(a->fd, c.obj_addr, RKNPU_MEM_SYNC_TO_DEVICE,
			     size + 0x1000, 0x1000) == 0)
			note("sync-badoffset-unexpected-ok", 0);
		if (destroy_obj(a->fd, &c))
			note("destroy", errno);
		atomic_fetch_add(&ops_done, 5);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int threads = argc > 1 ? atoi(argv[1]) : 8;
	int iters = argc > 2 ? atoi(argv[2]) : 4000;
	uint32_t obj_size = 0x20000;
	pthread_t *tid;
	struct worker_arg *wa;
	int fd, i;

	fd = open_dev();
	if (fd < 0)
		return 2;

	printf("=== edge cases (obj_size=%#x) ===\n", obj_size);
	run_edges(fd, obj_size);

	printf("=== concurrency hammer: %d threads x %d iters ===\n", threads, iters);
	tid = calloc(threads, sizeof(*tid));
	wa = calloc(threads, sizeof(*wa));
	for (i = 0; i < threads; i++) {
		wa[i].fd = fd;
		wa[i].iters = iters;
		wa[i].idx = i;
	}
	for (i = 0; i < threads; i++)
		pthread_create(&tid[i], NULL, hammer, &wa[i]);
	for (i = 0; i < threads; i++)
		pthread_join(tid[i], NULL);

	printf("ops=%ld failures=%d\n", atomic_load(&ops_done), atomic_load(&failures));
	printf("RESULT: %s\n", atomic_load(&failures) ? "FAIL" : "PASS");
	close(fd);
	return atomic_load(&failures) ? 1 : 0;
}
