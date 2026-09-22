/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __LINUX_RKNPU_JOB_H_
#define __LINUX_RKNPU_JOB_H_

#include <linux/spinlock.h>
#include <linux/dma-fence.h>
#include <linux/irq.h>
#include <linux/refcount.h>

#include <drm/drm_device.h>

#include "rknpu_ioctl.h"

#define RKNPU_MAX_CORES 3

#define RKNPU_JOB_DONE (1 << 0)
#define RKNPU_JOB_ASYNC (1 << 1)
#define RKNPU_JOB_FINALIZED (1 << 2)
#define RKNPU_JOB_RECOVERY_PENDING (1 << 3)
#define RKNPU_JOB_PUBLISHED (1 << 4)
#define RKNPU_JOB_CLEANED (1 << 5)
#define RKNPU_JOB_SHUTDOWN_PENDING (1 << 6)
/*
 * Guards rknpu_job_release_holds() against running twice.
 *
 * On the *completion* path that function is reachable from two threads at
 * once - the IRQ thread's rknpu_job_finish() and the submitting thread's
 * rknpu_job_cleanup() - because finalize wakes the waiter before finish()
 * has released the holds. Claiming the release under the job lock makes it
 * idempotent regardless of which thread arrives first.
 */
#define RKNPU_JOB_HOLDS_RELEASED (1 << 7)

#define RKNPU_CORE_AUTO_MASK 0x00
#define RKNPU_CORE0_MASK 0x01
#define RKNPU_CORE1_MASK 0x02
#define RKNPU_CORE2_MASK 0x04

struct rknpu_gem_object;
struct sync_file;

struct rknpu_iommu_domain_ref {
	struct iommu_domain *domain;
	struct iommu_group *group;
};

static inline bool rknpu_task_range_is_valid(size_t object_size,
					      u32 task_start, u32 task_number)
{
	size_t task_capacity = object_size / sizeof(struct rknpu_task);

	return task_number && task_start < task_capacity &&
	       task_number <= task_capacity - task_start;
}

static inline bool rknpu_task_token_is_valid(u64 token)
{
	return token && token <= U32_MAX;
}

struct rknpu_job {
	struct rknpu_device *rknpu_dev;
	struct list_head head[RKNPU_MAX_CORES];
	struct list_head device_node;
	struct work_struct cleanup_work;
	struct work_struct recovery_work;
	struct delayed_work timeout_work;
	bool core_done[RKNPU_MAX_CORES];
	unsigned int flags;
	int ret;
	struct rknpu_submit *args;
	bool args_owner;
	bool domain_held;
	struct rknpu_iommu_domain_ref iommu_ref[RKNPU_MAX_CORES];
	bool power_held;
	struct rknpu_gem_object *task_obj;
	struct rknpu_task *first_task;
	struct rknpu_task *last_task;
	uint32_t int_mask[RKNPU_MAX_CORES];
	uint32_t int_status[RKNPU_MAX_CORES];
	struct dma_fence *fence;
	struct sync_file *sync_file;
	int fence_fd;
	ktime_t timestamp;
	uint32_t use_core_num;
	atomic_t run_count;
	atomic_t interrupt_count;
	refcount_t refcount;
	ktime_t hw_commit_time;
	ktime_t hw_recoder_time[RKNPU_MAX_CORES];
	ktime_t hw_elapse_time;
	atomic_t submit_count[RKNPU_MAX_CORES];
	int iommu_domain_id;
};

irqreturn_t rknpu_core0_irq_handler(int irq, void *data);
irqreturn_t rknpu_core1_irq_handler(int irq, void *data);
irqreturn_t rknpu_core2_irq_handler(int irq, void *data);

int rknpu_submit_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv);

int rknpu_job_cache_init(void);
void rknpu_job_cache_fini(void);

int rknpu_job_recover_all(struct rknpu_device *rknpu_dev, int ret);
int rknpu_job_shutdown(struct rknpu_device *rknpu_dev);
bool rknpu_jobs_idle(struct rknpu_device *rknpu_dev);

int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version);

int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, uint32_t *priority,
			  uint32_t *expect, uint32_t *tw);

int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, uint32_t priority,
			  uint32_t expect, uint32_t tw);

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev);

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr,
			uint32_t *dt_rd, uint32_t *wd_rd);

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount);

/*
 * Core rail voltage in microvolts, or 0 when this device exposes none.
 * See rknpu_get_volt() in rknpu_job.c for why it is not simply
 * regulator_get_voltage(rknpu_dev->vdd) the way vendor's action handler has it.
 */
int rknpu_get_volt(struct rknpu_device *rknpu_dev, uint32_t *value);

#endif /* __LINUX_RKNPU_JOB_H_ */
