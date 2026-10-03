#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <drm/drm.h>
#include "rknpu_ioctl.h"

#define CORE_AUTO 0u
#define CORE0 1u
#define CORE1 2u
#define CORE2 4u

static int expect_errno(const char *name, int ret, int expected)
{
	int saved = errno;
	int ok = ret < 0 && saved == expected;
	printf("%-28s ret=%2d errno=%2d(%s) expected=%d [%s]\n",
	       name, ret, saved, strerror(saved), expected, ok ? "PASS" : "FAIL");
	return ok;
}

static int expect_ok(const char *name, int ret)
{
	int saved = errno;
	printf("%-28s ret=%2d errno=%2d(%s) [%s]\n", name, ret, saved,
	       ret ? strerror(saved) : "-", ret == 0 ? "PASS" : "FAIL");
	return ret == 0;
}

static int create(int fd, unsigned int core_mask, unsigned int flags,
		  struct rknpu_mem_create *c)
{
	int ret;
	memset(c, 0, sizeof(*c));
	c->size = 0x1000;
	c->flags = flags;
	c->iommu_domain_id = 0;
	c->core_mask = core_mask;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
	return ret;
}

int main(void)
{
	struct rknpu_mem_create c;
	struct rknpu_mem_map map;
	struct rknpu_mem_destroy d;
	struct rknpu_submit s;
	unsigned int core_masks[] = {CORE_AUTO, CORE0, CORE1, CORE2};
	unsigned int flags[] = {0, 1 | 4 | 16, 1 | 4 | 8 | 16};
	int fd, ret, i, j, pass = 1;
	void *p;

	fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("open renderD129 failed: %s\n", strerror(errno));
		return 2;
	}
	printf("fd=%d\n", fd);

	/* Single-core successful allocation matrix. */
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 3; j++) {
			char name[64];
			errno = 0;
			ret = create(fd, core_masks[i], flags[j], &c);
			snprintf(name, sizeof(name), "create mask=%u flags=%u",
				 core_masks[i], flags[j]);
			printf("%-46s ret=%d errno=%d(%s) handle=%u dma_addr=%#llx size=%llu obj=%llu [%s]\n",
			       name, ret, errno, ret ? strerror(errno) : "-",
			       c.handle, (unsigned long long)c.dma_addr,
			       (unsigned long long)c.size,
			       (unsigned long long)c.obj_addr,
			       ret == 0 && c.handle && c.dma_addr ? "PASS" : "FAIL");
			if (ret != 0 || !c.handle || !c.dma_addr)
				pass = 0;

			/* Query by handle, verify returned metadata is stable. */
			if (ret == 0) {
				struct rknpu_mem_create q;
				memset(&q, 0, sizeof(q));
				q.handle = c.handle;
				errno = 0;
				ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, &q);
				printf("  query handle=%u          ret=%d errno=%d dma_addr=%#llx size=%llu [%s]\n",
				       c.handle, ret, errno, (unsigned long long)q.dma_addr,
				       (unsigned long long)q.size,
				       ret == 0 && q.dma_addr == c.dma_addr &&
					       q.size == c.size ? "PASS" : "FAIL");
				if (ret != 0 || q.dma_addr != c.dma_addr || q.size != c.size)
					pass = 0;
			}

			/* mmap one page and write/read it. */
			if (ret == 0) {
				memset(&map, 0, sizeof(map));
				map.handle = c.handle;
				errno = 0;
				ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_MAP, &map);
				printf("  map handle=%u             ret=%d errno=%d offset=%#llx [%s]\n",
				       c.handle, ret, errno, (unsigned long long)map.offset,
				       ret == 0 ? "PASS" : "FAIL");
				if (ret != 0) pass = 0;
				if (ret == 0) {
					p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE,
						 MAP_SHARED, fd, map.offset);
					printf("  mmap                     ret=%p errno=%d [%s]\n",
					       p, errno, p != MAP_FAILED ? "PASS" : "FAIL");
					if (p == MAP_FAILED) {
						pass = 0;
					} else {
						*(volatile uint32_t *)p = 0x13572468u;
						if (*(volatile uint32_t *)p != 0x13572468u) {
							printf("  mmap readback            FAIL\n");
							pass = 0;
						} else {
							printf("  mmap readback            PASS\n");
						}
						munmap(p, 0x1000);
					}
				}
			}

			/* Explicit destroy. */
			if (c.handle) {
				memset(&d, 0, sizeof(d));
				d.handle = c.handle;
				d.obj_addr = c.handle;
				errno = 0;
				ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_DESTROY, &d);
				if (!expect_ok("destroy explicit", ret)) pass = 0;
			}
		}
	}

	/* Multi-core masks remain unsupported in M4. */
	errno = 0;
	ret = create(fd, CORE0 | CORE1, 0, &c);
	pass &= expect_errno("create mask=3", ret, EOPNOTSUPP);
	errno = 0;
	ret = create(fd, CORE0 | CORE1 | CORE2, 0, &c);
	pass &= expect_errno("create mask=7", ret, EOPNOTSUPP);

	/* Invalid size. */
	memset(&c, 0, sizeof(c));
	c.size = 0;
	c.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, &c);
	pass &= expect_errno("create size=0", ret, EINVAL);

	/* SUBMIT remains unsupported. */
	memset(&s, 0, sizeof(s));
	s.core_mask = CORE0;
	errno = 0;
	ret = ioctl(fd, DRM_IOCTL_RKNPU_SUBMIT, &s);
	pass &= expect_errno("submit", ret, EOPNOTSUPP);

	/* Leave one object allocated for close()-driven release. */
	ret = create(fd, CORE2, 1 | 4 | 16, &c);
	if (!expect_ok("create close-test", ret)) pass = 0;

	printf("OVERALL: %s\n", pass ? "PASS" : "FAIL");
	close(fd);
	printf("closed fd=%d; close-path GEM release follows\n", fd);
	return pass ? 0 : 1;
}
