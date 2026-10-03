#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <drm/drm.h>
#include "rknpu_ioctl.h"

static int create(int fd, unsigned int core, struct rknpu_mem_create *c)
{
	memset(c, 0, sizeof(*c));
	c->size = 0x20000; /* 128 KiB, likely enough for >1 page scatter */
	c->flags = 1 | 4 | 16; /* noncontig + WC + IOMMU */
	c->core_mask = core;
	return ioctl(fd, DRM_IOCTL_RKNPU_MEM_CREATE, c);
}

int main(void)
{
	struct rknpu_mem_create c[3][2];
	unsigned int cores[3] = {1, 2, 4};
	int fd, i, j, ret;
	int pass = 1;

	fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open"); return 2; }

	/* Keep two objects per core allocated simultaneously. */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < 2; j++) {
			ret = create(fd, cores[i], &c[i][j]);
			printf("core%u[%d] create ret=%d handle=%u dma=%#llx size=%llu\n",
			       i, j, ret, c[i][j].handle,
			       (unsigned long long)c[i][j].dma_addr,
			       (unsigned long long)c[i][j].size);
			if (ret || !c[i][j].dma_addr) pass = 0;
		}
		if (c[i][0].dma_addr == c[i][1].dma_addr) {
			printf("FAIL: core%u concurrent objects have same IOVA\n", i);
			pass = 0;
		} else {
			printf("PASS: core%u concurrent IOVAs distinct\n", i);
		}
	}
	if (c[0][0].dma_addr == c[1][0].dma_addr && c[1][0].dma_addr == c[2][0].dma_addr)
		printf("NOTE: all cores reuse first IOVA=%#llx (consistent with independent domains)\n",
		       (unsigned long long)c[0][0].dma_addr);

	printf("OVERALL: %s\n", pass ? "PASS" : "FAIL");
	close(fd);
	return pass ? 0 : 1;
}
