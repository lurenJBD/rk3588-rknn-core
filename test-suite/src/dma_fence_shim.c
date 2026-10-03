/*
 * dma_fence_shim.c - LD_PRELOAD ioctl shim to validate RKNPU dma_fence signalling
 * on a REAL operator workload.
 *
 * Why this exists: the standalone async test used regcfg_amount=0 NOP tasks,
 * which never raise a hardware IRQ (handover doc section 4.2), so their
 * FENCE_OUT fences can never signal - a structural test defect, not a driver
 * bug. This shim instead rides on librknnrt's real matmul submit (which builds
 * a real regcmd and does raise an IRQ): it OR-s NONBLOCK|FENCE_OUT into the
 * SUBMIT ioctl, then poll()s the returned fence_fd after the call. A fence that
 * becomes POLLIN is signalled by the real completion IRQ - the actual thing
 * left unverified.
 *
 * Build:
 *   gcc -shared -fPIC -O2 -o dma_fence_shim.so dma_fence_shim.c -ldl
 * Use:
 *   RKNPU_FENCE_SHIM=1 LD_PRELOAD=./fence_shim.so ./rknn_matmul_api_demo 2 4,64,32 0 0 5 1 0 0
 *
 * Env:
 *   RKNPU_FENCE_SHIM=1        enable (default: pass-through if unset)
 *   RKNPU_FENCE_KEEP_BLOCKING=1  add FENCE_OUT only, keep blocking submit
 *                                (verifies fence install+signal on sync path)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <drm/drm.h>

/* Mirror of driver ABI (driver/include/rknpu_ioctl.h) */
#define RKNPU_SUBMIT 0x01
#define RKNPU_JOB_PC        (1u << 0)
#define RKNPU_JOB_NONBLOCK  (1u << 1)
#define RKNPU_JOB_FENCE_IN  (1u << 3)
#define RKNPU_JOB_FENCE_OUT (1u << 4)

struct rknpu_subcore_task { uint32_t task_start; uint32_t task_number; };
struct rknpu_submit {
	uint32_t flags, timeout, task_start, task_number, task_counter;
	int32_t  priority;
	uint64_t task_obj_addr;
	uint32_t iommu_domain_id, reserved;
	uint64_t task_base_addr;
	int64_t  hw_elapse_time;
	uint32_t core_mask;
	int32_t  fence_fd;
	struct rknpu_subcore_task subcore_task[5];
};

#define DRM_IOCTL_RKNPU_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + RKNPU_SUBMIT, struct rknpu_submit)

static int (*real_ioctl)(int, unsigned long, ...);
static int   g_enabled;
static int   g_keep_blocking;
static long  g_submits, g_fenced, g_signalled, g_timeout, g_nofd;

static void shim_init(void) __attribute__((constructor));
static void shim_init(void)
{
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	g_enabled = getenv("RKNPU_FENCE_SHIM") != NULL;
	g_keep_blocking = getenv("RKNPU_FENCE_KEEP_BLOCKING") != NULL;
	if (g_enabled)
		fprintf(stderr, "[fence_shim] enabled (keep_blocking=%d)\n",
			g_keep_blocking);
}

static void shim_report(void) __attribute__((destructor));
static void shim_report(void)
{
	if (!g_enabled)
		return;
	fprintf(stderr,
		"[fence_shim] SUBMITs=%ld fenced=%ld signalled=%ld timeout=%ld no_fd=%ld\n",
		g_submits, g_fenced, g_signalled, g_timeout, g_nofd);
	if (g_fenced == 0)
		fprintf(stderr, "[fence_shim] VERDICT: INCONCLUSIVE (no fenced submits seen)\n");
	else if (g_signalled == g_fenced && g_timeout == 0)
		fprintf(stderr, "[fence_shim] VERDICT: PASS (all %ld fences signalled by IRQ)\n",
			g_fenced);
	else
		fprintf(stderr, "[fence_shim] VERDICT: FAIL (%ld/%ld signalled, %ld timeouts)\n",
			g_signalled, g_fenced, g_timeout);
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!g_enabled || request != DRM_IOCTL_RKNPU_SUBMIT)
		return real_ioctl(fd, request, arg);

	struct rknpu_submit *s = arg;
	uint32_t orig_flags = s->flags;

	/* Only touch PC jobs; leave anything odd alone. */
	if (!(orig_flags & RKNPU_JOB_PC))
		return real_ioctl(fd, request, arg);

	g_submits++;
	s->flags |= RKNPU_JOB_FENCE_OUT;
	if (!g_keep_blocking)
		s->flags |= RKNPU_JOB_NONBLOCK;
	s->fence_fd = -1;

	int ret = real_ioctl(fd, request, arg);
	if (ret != 0) {
		/* restore for the caller's view and report */
		s->flags = orig_flags;
		return ret;
	}

	int ffd = s->fence_fd;
	if (ffd < 0) {
		g_nofd++;
	} else {
		g_fenced++;
		struct pollfd pfd = { .fd = ffd, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, 3000);
		if (pr > 0 && (pfd.revents & POLLIN))
			g_signalled++;
		else
			g_timeout++;
		close(ffd);
		s->fence_fd = -1; /* caller (librknnrt) did not ask for it */
	}
	return ret;
}
