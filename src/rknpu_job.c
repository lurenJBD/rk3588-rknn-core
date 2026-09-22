// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/slab.h>
#include <linux/irqreturn.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/sync_file.h>
#include <linux/io.h>

#include "rknpu_ioctl.h"
#include "rknpu_drv.h"
#include "rknpu_core.h"
#include "rknpu_reset.h"
#include "rknpu_gem.h"
#include "rknpu_fence.h"
#include "rknpu_iommu.h"
#include "rknpu_job.h"

static struct kmem_cache *rknpu_job_cache;

int rknpu_job_cache_init(void)
{
	rknpu_job_cache = kmem_cache_create("rknpu_job",
					    sizeof(struct rknpu_job), 0,
					    SLAB_HWCACHE_ALIGN, NULL);
	if (!rknpu_job_cache)
		return -ENOMEM;
	return 0;
}

void rknpu_job_cache_fini(void)
{
	kmem_cache_destroy(rknpu_job_cache);
	rknpu_job_cache = NULL;
}


/*
 * The task descriptor is provided by userspace runtime: the executor
 * batch routine writes enable_mask, int_mask and int_clear (0x1ffff),
 * and the driver programs them into PC_INT_MASK / PC_INT_CLEAR.
 */

#define _REG_READ(base, offset) readl(base + (offset))
#define _REG_WRITE(base, value, offset) writel(value, base + (offset))

#define REG_READ(reg) rknpu_core_read(rknpu_dev, core_index, reg)
#define REG_WRITE(value, reg) rknpu_core_write(rknpu_dev, core_index, reg, value)
#define REG_READ_OFFSET(offset) \
	rknpu_core_read_dynamic(rknpu_dev, core_index, offset)
#define REG_WRITE_OFFSET(value, offset) \
	rknpu_core_write_dynamic(rknpu_dev, core_index, offset, value)

/*
 * One section per arming: the driver arms each section sequentially,
 * advancing PC.BASE_ADDRESS on each completion until the full submit
 * range has completed.
 */

/*
 * How many task descriptors one arming covers (always one per step).
 */

static inline u64 rknpu_pc_arm_step(struct rknpu_device *rknpu_dev)
{
	return 1;
}

static inline void rknpu_arm_one_section(u32 *task_number, u32 *task_end,
					 struct rknpu_task **last_task,
					 struct rknpu_task *first_task,
					 u32 task_start)
{
	if (*task_number <= 1)
		return;

	/*
	 * PC_TASK_CONTROL's count field becomes "retire one task", and the
	 * interrupt mask is taken from the one descriptor actually armed.
	 */
	*task_number = 1;
	*task_end = task_start;
	*last_task = first_task;
}

static int rknpu_wait_core_index(int core_mask)
{
	int index = 0;

	switch (core_mask) {
	case RKNPU_CORE0_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		index = 0;
		break;
	case RKNPU_CORE1_MASK:
		index = 1;
		break;
	case RKNPU_CORE2_MASK:
		index = 2;
		break;
	default:
		break;
	}

	return index;
}

static int rknpu_core_mask(int core_index)
{
	int core_mask = RKNPU_CORE_AUTO_MASK;

	switch (core_index) {
	case 0:
		core_mask = RKNPU_CORE0_MASK;
		break;
	case 1:
		core_mask = RKNPU_CORE1_MASK;
		break;
	case 2:
		core_mask = RKNPU_CORE2_MASK;
		break;
	default:
		break;
	}

	return core_mask;
}

static u32 rknpu_get_task_number(struct rknpu_job *job, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	u32 task_num = job->args->task_number;

	if (core_index >= RKNPU_MAX_CORES || core_index < 0) {
		LOG_ERROR("invalid rknpu core index: %d", core_index);
		return 0;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		if (job->use_core_num == 1 || job->use_core_num == 2)
			task_num = job->args->subcore_task[core_index].task_number;
		else if (job->use_core_num == 3)
			task_num = job->args->subcore_task[core_index + 2]
					   .task_number;
	}

	return task_num;
}

static void rknpu_job_free(struct rknpu_job *job)
{
	WARN_ON_ONCE(job->flags & RKNPU_JOB_PUBLISHED);
	WARN_ON_ONCE(job->domain_held || job->power_held);
	WARN_ON_ONCE(!list_empty(&job->device_node));
	rknpu_fence_discard_fd(job);

	if (job->task_obj)
		rknpu_gem_object_put(&job->task_obj->base);

	if (job->fence)
		dma_fence_put(job->fence);

	if (job->args_owner)
		kfree(job->args);

	kmem_cache_free(rknpu_job_cache, job);
}

static void rknpu_job_put(struct rknpu_job *job)
{
	if (refcount_dec_and_test(&job->refcount))
		rknpu_job_free(job);
}

static void rknpu_job_release_holds(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	unsigned long flags;
	int i;

	/*
	 * Claim the release under irq_lock to make it idempotent regardless of
	 * whether rknpu_job_finish() or rknpu_job_cleanup() arrives first.
	 */
	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (job->flags & RKNPU_JOB_HOLDS_RELEASED) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}
	job->flags |= RKNPU_JOB_HOLDS_RELEASED;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	for (i = 0; i < RKNPU_MAX_CORES; i++)
		rknpu_iommu_domain_detach(job->rknpu_dev, i,
					  &job->iommu_ref[i]);

	if (job->domain_held) {
		rknpu_iommu_domain_put(job->rknpu_dev);
		job->domain_held = false;
	}
	if (job->power_held) {
		rknpu_power_put_delay(job->rknpu_dev);
		job->power_held = false;
	}
}

static void rknpu_job_unpublish(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	unsigned long flags;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (job->flags & RKNPU_JOB_PUBLISHED) {
		list_del_init(&job->device_node);
		job->flags &= ~RKNPU_JOB_PUBLISHED;
		wake_up_all(&rknpu_dev->commit_wq);
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
}

static int rknpu_job_cleanup(struct rknpu_job *job)
{
	unsigned long flags;

	spin_lock_irqsave(&job->rknpu_dev->irq_lock, flags);
	if (job->flags & RKNPU_JOB_CLEANED) {
		spin_unlock_irqrestore(&job->rknpu_dev->irq_lock, flags);
		return 0;
	}
	job->flags |= RKNPU_JOB_CLEANED;
	spin_unlock_irqrestore(&job->rknpu_dev->irq_lock, flags);

	rknpu_job_unpublish(job);
	rknpu_job_release_holds(job);
	rknpu_job_put(job);

	return 0;
}

static bool rknpu_job_finalize_locked(struct rknpu_job *job, int ret)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	int i;

	lockdep_assert_held(&rknpu_dev->irq_lock);

	if (job->flags & RKNPU_JOB_FINALIZED)
		return false;

	job->flags |= RKNPU_JOB_FINALIZED | RKNPU_JOB_DONE;
	job->ret = ret;
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i))
			wake_up(&rknpu_dev->subcore_datas[i].job_done_wq);
	}

	return true;
}

static bool rknpu_job_get(struct rknpu_job *job)
{
	return refcount_inc_not_zero(&job->refcount);
}

static void rknpu_job_detach_locked(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data;
	int i;

	lockdep_assert_held(&rknpu_dev->irq_lock);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (!(job->args->core_mask & rknpu_core_mask(i)))
			continue;

		subcore_data = &rknpu_dev->subcore_datas[i];
		if (job == subcore_data->job) {
			subcore_data->job = NULL;
			subcore_data->task_num -= rknpu_get_task_number(job, i);
			if (!job->core_done[i]) {
				job->core_done[i] = true;
				atomic_dec(&job->interrupt_count);
			}
		} else if (!list_empty(&job->head[i])) {
			list_del_init(&job->head[i]);
			subcore_data->task_num -= rknpu_get_task_number(job, i);
			if (!job->core_done[i]) {
				job->core_done[i] = true;
				atomic_dec(&job->interrupt_count);
			}
		}
	}
}

static int rknpu_job_recover_device(struct rknpu_device *rknpu_dev,
				     struct rknpu_job *trigger, int ret);
static void rknpu_job_abort(struct rknpu_job *job);
static void rknpu_job_finish(struct rknpu_job *job);
static void rknpu_job_finish_irq(struct rknpu_job *job);

static void rknpu_job_cleanup_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(work, struct rknpu_job, cleanup_work);

	rknpu_job_cleanup(job);
}

static void rknpu_job_recovery_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(work, struct rknpu_job, recovery_work);
	int ret;

	ret = rknpu_job_recover_device(job->rknpu_dev, job,
				       job->ret ? job->ret : -EIO);
	if (ret) {
		LOG_ERROR("failed to recover invalid IRQ: %d\n", ret);
		rknpu_job_abort(job);
	}
	rknpu_job_put(job);
}

static void rknpu_job_timeout_work(struct work_struct *work)
{
	struct rknpu_job *job =
		container_of(to_delayed_work(work), struct rknpu_job, timeout_work);
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	unsigned long flags;
	bool recover = false;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (!(job->flags & (RKNPU_JOB_FINALIZED |
			    RKNPU_JOB_RECOVERY_PENDING))) {
		if (!job->ret)
			job->ret = -ETIMEDOUT;
		job->flags |= RKNPU_JOB_RECOVERY_PENDING;
		recover = true;
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (recover) {
		if (rknpu_job_recover_device(rknpu_dev, job, -ETIMEDOUT))
			rknpu_job_abort(job);
	}
	rknpu_job_put(job);
}

static inline struct rknpu_job *rknpu_job_alloc(struct rknpu_device *rknpu_dev,
						struct rknpu_submit *args,
						struct rknpu_gem_object *task_obj)
{
	struct rknpu_job *job = NULL;
	int i = 0;

	job = kmem_cache_zalloc(rknpu_job_cache, GFP_KERNEL);
	if (!job)
		return NULL;

	job->timestamp = ktime_get();
	job->rknpu_dev = rknpu_dev;
	job->fence_fd = -1;
	refcount_set(&job->refcount, 1);
	job->use_core_num = hweight32(args->core_mask);
	atomic_set(&job->run_count, job->use_core_num);
	atomic_set(&job->interrupt_count, job->use_core_num);
	job->iommu_domain_id = args->iommu_domain_id;
	/*
	 * Warn if task buffer's domain differs from the submit's domain_id.
	 */
	if (task_obj && task_obj->iommu_domain_id != args->iommu_domain_id)
		dev_warn_ratelimited(
			rknpu_dev->dev,
			"RKNPU: domain mismatch - submit domain=%d but task_obj domain=%d\n",
			args->iommu_domain_id, task_obj->iommu_domain_id);
	INIT_LIST_HEAD(&job->device_node);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		INIT_LIST_HEAD(&job->head[i]);
		atomic_set(&job->submit_count[i], 0);
	}
	job->task_obj = task_obj;
	rknpu_gem_object_get(&task_obj->base);

	job->args = kmemdup(args, sizeof(*args), GFP_KERNEL);
	if (!job->args) {
		rknpu_job_free(job);
		return NULL;
	}
	job->args_owner = true;

	INIT_WORK(&job->cleanup_work, rknpu_job_cleanup_work);
	INIT_WORK(&job->recovery_work, rknpu_job_recovery_work);
	INIT_DELAYED_WORK(&job->timeout_work, rknpu_job_timeout_work);

	return job;
}

static inline int rknpu_job_wait(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_task *last_task = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	int core_index = rknpu_wait_core_index(job->args->core_mask);
	unsigned long flags;
	int wait_count = 0;
	bool continue_wait = false;
	int ret = -EINVAL;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	do {
		ret = wait_event_timeout(subcore_data->job_done_wq,
					 job->flags & RKNPU_JOB_DONE ||
						 rknpu_dev->soft_reseting ||
						 rknpu_dev->reset_failed,
					 msecs_to_jiffies(args->timeout));

		if (++wait_count >= 3)
			break;

		if (ret == 0) {
			int64_t elapse_time_us = 0;
			spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
			elapse_time_us = ktime_us_delta(ktime_get(),
							job->hw_commit_time);
			continue_wait =
				job->hw_commit_time == 0 ?
					true :
					(elapse_time_us < args->timeout * 1000);
			spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
			LOG_ERROR(
				"job: %p, mask: %#x, job iommu domain id: %d, dev iommu domain id: %d, wait_count: %d, continue wait: %d, commit elapse time: %lldus, wait time: %lldus, timeout: %uus\n",
				job, args->core_mask, job->iommu_domain_id,
				rknpu_dev->iommu_domain_id, wait_count,
				continue_wait,
				(job->hw_commit_time == 0 ? 0 : elapse_time_us),
				ktime_us_delta(ktime_get(), job->timestamp),
				args->timeout * 1000);
		}
	} while (ret == 0 && continue_wait);

	if (READ_ONCE(job->flags) & RKNPU_JOB_FINALIZED)
		return READ_ONCE(job->ret);
	if (READ_ONCE(rknpu_dev->reset_failed))
		return -EIO;

	last_task = job->last_task;
	if (!last_task) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		rknpu_job_detach_locked(job);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		LOG_ERROR("job commit failed: %d\n", job->ret);
		return job->ret ? job->ret : (ret < 0 ? ret : -EINVAL);
	}

	last_task->int_status = job->int_status[core_index];

	if (ret <= 0) {
		uint32_t int_raw_status = REG_READ(RKNPU_CORE_REG_INT_RAW_STATUS);
		uint32_t int_status = REG_READ(RKNPU_CORE_REG_INT_STATUS);
		uint32_t pc_op_en = REG_READ(RKNPU_CORE_REG_PC_OP_EN);
		/* Read CNA/CORE status to diagnose timeout state. */
		u32 cna_s_status = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CNA_S_STATUS);
		u32 cna_s_pointer = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CNA_S_POINTER);
		u32 cna_op_en = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CNA_OP_EN);
		u32 core_s_status = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CORE_S_STATUS);
		u32 core_s_pointer = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CORE_S_POINTER);
		u32 core_op_en = rknpu_core_read_block(
			rknpu_dev, core_index, RKNPU_CORE_BLOCK_CORE_OP_EN);

		args->task_counter = 0;
		if (args->flags & RKNPU_JOB_PC) {
			uint32_t task_status = REG_READ_OFFSET(
				rknpu_dev->config->pc_task_status_offset);
			args->task_counter =
				(task_status &
				 rknpu_dev->config->pc_task_number_mask);
		}

		LOG_ERROR(
			"failed to wait job, task counter: %d, flags: %#x, ret = %d, elapsed time: %lldus, int_raw_status: %#x, int_status: %#x, op_en: %#x\n",
			args->task_counter, args->flags, ret,
			ktime_us_delta(ktime_get(), job->timestamp),
			int_raw_status, int_status, pc_op_en);
		LOG_ERROR(
			"job timeout block state core=%u: CNA s_status=%#x s_pointer=%#x op_en=%#x, CORE s_status=%#x s_pointer=%#x op_en=%#x\n",
			core_index, cna_s_status, cna_s_pointer, cna_op_en,
			core_s_status, core_s_pointer, core_op_en);
		LOG_ERROR(
			"job timeout blocks core=%u: DPU(%#x/%#x/%#x) DPU.DATA_FORMAT=%#x [out_prec=%u in_prec=%u proc=%u] DPU_RDMA(%#x/%#x/%#x) PPU(%#x/%#x/%#x) PPU_RDMA(%#x/%#x/%#x) GLOBAL_OP_EN=%#x\n",
			core_index,
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_S_STATUS),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_S_POINTER),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_OP_EN),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_DATA_FORMAT),
			(rknpu_core_read_block(rknpu_dev, core_index,
					       RKNPU_CORE_BLOCK_DPU_DATA_FORMAT) >>
			 29) &
				0x7,
			(rknpu_core_read_block(rknpu_dev, core_index,
					       RKNPU_CORE_BLOCK_DPU_DATA_FORMAT) >>
			 26) &
				0x7,
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_DATA_FORMAT) &
				0x7,
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_RDMA_S_STATUS),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_RDMA_S_POINTER),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_DPU_RDMA_OP_EN),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_S_STATUS),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_S_POINTER),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_OP_EN),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_RDMA_S_STATUS),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_RDMA_S_POINTER),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_PPU_RDMA_OP_EN),
			rknpu_core_read_block(rknpu_dev, core_index,
					      RKNPU_CORE_BLOCK_GLOBAL_OP_EN));
		return ret < 0 ? ret : -ETIMEDOUT;
	}

	if (!(job->flags & RKNPU_JOB_DONE))
		return -EINVAL;

	args->task_counter = args->task_number;
	args->hw_elapse_time = job->hw_elapse_time;

	return 0;
}

static inline int rknpu_job_subcore_commit_pc(struct rknpu_job *job,
					      int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	struct rknpu_gem_object *task_obj = job->task_obj;
	struct rknpu_task *task_base = NULL;
	struct rknpu_task *first_task = NULL;
	struct rknpu_task *last_task = NULL;
	u32 task_start = args->task_start;
	u32 task_number = args->task_number;
	u32 task_end;
	int task_pp_en = args->flags & RKNPU_JOB_PINGPONG ? 1 : 0;
	int pc_data_amount_scale = rknpu_dev->config->pc_data_amount_scale;
	int pc_task_number_bits = rknpu_dev->config->pc_task_number_bits;
	u64 completed, batch_start, batch_number;
	u64 arm_step;
	int i = 0;
	int submit_index = atomic_read(&job->submit_count[core_index]);
	u64 max_submit_number = rknpu_dev->config->max_submit_number;
	unsigned long flags;
	int ret;

	if (!task_obj || !task_obj->kv_addr ||
	    !(task_obj->flags & RKNPU_MEM_KERNEL_MAPPING) ||
	    submit_index < 0 || !max_submit_number) {
		job->ret = -EINVAL;
		return job->ret;
	}

	if (rknpu_dev->config->num_irqs > 1) {
		/*
		 * Both writes take the value 0xe, i.e. the "executer" and
		 * ping-pong bits of CNA/CORE S_POINTER (the two registers
		 * rknpu_core.c maps MULTICORE_CFG/MULTICORE_CFG2 onto).
		 */
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (i == core_index) {
				REG_WRITE((0xe + 0x10000000 * i), RKNPU_CORE_REG_MULTICORE_CFG);
				REG_WRITE((0xe + 0x10000000 * i), RKNPU_CORE_REG_MULTICORE_CFG2);
			}
		}

		switch (job->use_core_num) {
		case 1:
		case 2:
			task_start = args->subcore_task[core_index].task_start;
			task_number = args->subcore_task[core_index].task_number;
			break;
		case 3:
			task_start = args->subcore_task[core_index + 2].task_start;
			task_number = args->subcore_task[core_index + 2].task_number;
			break;
		default:
			job->ret = -EINVAL;
			return job->ret;
		}
	}

	/*
	 * Attach once per core per job. This function runs again for every
	 * section when walking a submit's range, and iommu_attach_group()
	 * fails if the group is already attached to a domain, so a repeated
	 * attach would abort the walk after the first section. A non-NULL ref
	 * means this job already holds the domain for this core.
	 */
	if (!job->iommu_ref[core_index].domain) {
		ret = rknpu_iommu_domain_attach(rknpu_dev, core_index,
						job->iommu_domain_id,
						&job->iommu_ref[core_index]);
		if (ret) {
			job->ret = ret;
			return job->ret;
		}
	}

	/*
	 * The batch stride is the arming step (one section), not
	 * max_submit_number: the k-th completion has covered k sections.
	 */
	arm_step = rknpu_pc_arm_step(rknpu_dev);

	completed = (u64)submit_index * arm_step;
	if (completed >= task_number) {
		job->ret = -EINVAL;
		return job->ret;
	}

	batch_start = (u64)task_start + completed;
	batch_number = min_t(u64, (u64)task_number - completed,
			     arm_step);
	if (batch_start > U32_MAX || batch_number > U32_MAX ||
	    !rknpu_task_range_is_valid(task_obj->size, batch_start,
				       batch_number)) {
		job->ret = -EINVAL;
		return job->ret;
	}

	task_start = batch_start;
	task_number = batch_number;
	task_end = task_start + task_number - 1;
	task_base = (struct rknpu_task *)task_obj->kv_addr;
	first_task = &task_base[task_start];
	last_task = &task_base[task_end];

	/* Arm one section at batch_start, advancing across successive completions. */
	rknpu_arm_one_section(&task_number, &task_end, &last_task, first_task,
			      task_start);


	if (rknpu_dev->config->pc_dma_ctrl) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		REG_WRITE(first_task->regcmd_addr, RKNPU_CORE_REG_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		REG_WRITE(first_task->regcmd_addr, RKNPU_CORE_REG_PC_DATA_ADDR);
	}

	REG_WRITE((first_task->regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT +
		   pc_data_amount_scale - 1) /
			  pc_data_amount_scale -
			  1,
		  RKNPU_CORE_REG_PC_DATA_AMOUNT);

	REG_WRITE(last_task->int_mask, RKNPU_CORE_REG_INT_MASK);
	REG_WRITE(first_task->int_mask, RKNPU_CORE_REG_INT_CLEAR);
	REG_WRITE(((0x6 | task_pp_en) << pc_task_number_bits) | task_number,
		  RKNPU_CORE_REG_PC_TASK_CONTROL);
	REG_WRITE(0, RKNPU_CORE_REG_PC_DMA_BASE_ADDR);

	job->first_task = first_task;
	job->last_task = last_task;
	job->int_mask[core_index] = last_task->int_mask;

	/* Release the PC and immediately clear enable (pulse OPERATION_ENABLE 1 -> 0) */
	REG_WRITE(0x1, RKNPU_CORE_REG_PC_OP_EN);
	REG_WRITE(0x0, RKNPU_CORE_REG_PC_OP_EN);

	LOG_DBG(
		"PC arm: core=%u task=%u..%u regcmd=%#llx amount=%u int_mask=%#x int_clear=%#x control=%#x base=%pad tf=%#x op_idx=%u enable=%#x roff=%u\n",
		core_index, task_start, task_end,
		(unsigned long long)first_task->regcmd_addr,
		first_task->regcfg_amount, last_task->int_mask,
		first_task->int_clear,
		((0x6 | task_pp_en) << pc_task_number_bits) | task_number,
		&task_obj->core_maps[core_index].dma_addr,
		first_task->flags, first_task->op_idx,
		first_task->enable_mask, first_task->regcfg_offset);


	return 0;
}

static inline int rknpu_job_subcore_commit(struct rknpu_job *job,
					   int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_submit *args = job->args;
	unsigned long flags;

	// switch to slave mode
	if (rknpu_dev->config->pc_dma_ctrl) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		REG_WRITE(0x1, RKNPU_CORE_REG_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		REG_WRITE(0x1, RKNPU_CORE_REG_PC_DATA_ADDR);
	}

	if (!(args->flags & RKNPU_JOB_PC)) {
		job->ret = -EINVAL;
		return job->ret;
	}

	return rknpu_job_subcore_commit_pc(job, core_index);
}

static void rknpu_job_commit(struct rknpu_job *job)
{
	int ret = 0;

	switch (job->args->core_mask) {
	case RKNPU_CORE0_MASK:
		ret = rknpu_job_subcore_commit(job, 0);
		break;
	case RKNPU_CORE1_MASK:
		ret = rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE2_MASK:
		ret = rknpu_job_subcore_commit(job, 2);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK:
		ret = rknpu_job_subcore_commit(job, 0);
		if (!ret)
			ret = rknpu_job_subcore_commit(job, 1);
		break;
	case RKNPU_CORE0_MASK | RKNPU_CORE1_MASK | RKNPU_CORE2_MASK:
		ret = rknpu_job_subcore_commit(job, 0);
		if (!ret)
			ret = rknpu_job_subcore_commit(job, 1);
		if (!ret)
			ret = rknpu_job_subcore_commit(job, 2);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	if (ret)
		job->ret = ret;
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index)
{
	struct rknpu_job *job = NULL;
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;

	if (READ_ONCE(rknpu_dev->shutting_down) ||
	    READ_ONCE(rknpu_dev->soft_reseting) ||
	    READ_ONCE(rknpu_dev->reset_failed))
		return;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

	if (subcore_data->job || list_empty(&subcore_data->todo_list)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}

	job = list_first_entry(&subcore_data->todo_list, struct rknpu_job,
			       head[core_index]);

	list_del_init(&job->head[core_index]);
	subcore_data->job = job;
	job->hw_commit_time = ktime_get();
	job->hw_recoder_time[core_index] = job->hw_commit_time;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (atomic_dec_and_test(&job->run_count))
		rknpu_job_commit(job);
}

static void rknpu_job_done(struct rknpu_job *job, int ret, int core_index)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	ktime_t now;
	unsigned long flags;
	u64 task_number = rknpu_get_task_number(job, core_index);
	u64 submit_count = atomic_inc_return(&job->submit_count[core_index]);
	bool finalized = false;

	/*
	 * Re-arm while the submit's range still has sections left. The stride
	 * is one section, so a single large batch runs to the end.
	 */
	if (!ret &&
	    submit_count <
		    DIV_ROUND_UP_ULL(task_number, rknpu_pc_arm_step(rknpu_dev))) {
		job->hw_recoder_time[core_index] = ktime_get();
		ret = rknpu_job_subcore_commit(job, core_index);
		if (!ret)
			return;
	}

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (WARN_ON_ONCE(job->core_done[core_index] ||
			 subcore_data->job != job)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return;
	}
	job->core_done[core_index] = true;
	subcore_data->job = NULL;
	subcore_data->task_num -= rknpu_get_task_number(job, core_index);
	if (ret && !job->ret)
		job->ret = ret;
	now = ktime_get();
	job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
	subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time[core_index]);
	if (atomic_dec_and_test(&job->interrupt_count))
		finalized = rknpu_job_finalize_locked(job, job->ret);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (finalized)
		rknpu_job_finish_irq(job);

	rknpu_job_next(rknpu_dev, core_index);
}

static int rknpu_schedule_core_index(struct rknpu_device *rknpu_dev)
{
	int core_num = rknpu_dev->config->num_irqs;
	int64_t task_num;
	unsigned long flags;
	int core_index = 0;
	int i = 0;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	task_num = rknpu_dev->subcore_datas[0].task_num;
	for (i = 1; i < core_num; i++) {
		if (task_num > rknpu_dev->subcore_datas[i].task_num) {
			core_index = i;
			task_num = rknpu_dev->subcore_datas[i].task_num;
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	return core_index;
}

static void rknpu_job_schedule(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	struct rknpu_subcore_data *subcore_data = NULL;
	int i = 0, core_index = 0;
	unsigned long flags;

	if (READ_ONCE(rknpu_dev->shutting_down) ||
	    READ_ONCE(rknpu_dev->reset_failed)) {
		job->ret = -ENODEV;
		return;
	}

	if (job->args->core_mask == RKNPU_CORE_AUTO_MASK) {
		core_index = rknpu_schedule_core_index(rknpu_dev);
		job->args->core_mask = rknpu_core_mask(core_index);
		job->use_core_num = 1;
		atomic_set(&job->run_count, job->use_core_num);
		atomic_set(&job->interrupt_count, job->use_core_num);
	}

	job->ret = rknpu_iommu_domain_get_and_switch(rknpu_dev,
						 job->iommu_domain_id);
	if (job->ret)
		return;
	job->domain_held = true;

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			job->ret = rknpu_iommu_domain_attach(
				rknpu_dev, i, job->iommu_domain_id,
				&job->iommu_ref[i]);
			if (job->ret) {
				rknpu_job_release_holds(job);
				return;
			}
		}
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (rknpu_dev->shutting_down || rknpu_dev->soft_reseting ||
	    rknpu_dev->reset_failed) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		job->ret = -EBUSY;
		return;
	}
	job->flags |= RKNPU_JOB_PUBLISHED;
	list_add_tail(&job->device_node, &rknpu_dev->jobs);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			list_add_tail(&job->head[i], &subcore_data->todo_list);
			subcore_data->task_num += rknpu_get_task_number(job, i);
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (job->args->core_mask & rknpu_core_mask(i))
			rknpu_job_next(rknpu_dev, i);
	}
}

static void rknpu_job_next(struct rknpu_device *rknpu_dev, int core_index);

static bool rknpu_job_is_active_locked(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	int i;

	lockdep_assert_held(&rknpu_dev->irq_lock);
	for (i = 0; i < rknpu_dev->config->num_irqs; i++)
		if (rknpu_dev->subcore_datas[i].job == job)
			return true;

	return false;
}

static void rknpu_job_complete(struct rknpu_job *job)
{
	if (job->fence) {
		if (job->ret)
			dma_fence_set_error(job->fence, job->ret);
		dma_fence_signal(job->fence);
	}
	if (job->flags & RKNPU_JOB_ASYNC)
		queue_work(job->rknpu_dev->job_wq, &job->cleanup_work);
}

static void rknpu_job_cancel_timeout(struct rknpu_job *job)
{
	if ((job->flags & RKNPU_JOB_ASYNC) &&
	    cancel_delayed_work(&job->timeout_work))
		rknpu_job_put(job);
}

static void rknpu_job_finish(struct rknpu_job *job)
{
	rknpu_job_cancel_timeout(job);

	rknpu_job_unpublish(job);
	rknpu_job_release_holds(job);
	rknpu_job_complete(job);
}

/*
 * Interrupt-context completion: only IRQ-safe operations (canceling timeouts,
 * unpublishing the job). Releasing holds requires mutexes and is deferred
 * to process context (submitting thread or cleanup workqueue).
 */
static void rknpu_job_finish_irq(struct rknpu_job *job)
{
	rknpu_job_cancel_timeout(job);
	rknpu_job_unpublish(job);
	rknpu_job_complete(job);
}

static int rknpu_job_recover_device(struct rknpu_device *rknpu_dev,
				     struct rknpu_job *trigger, int ret)
{
	struct rknpu_job *jobs[RKNPU_MAX_CORES];
	unsigned long flags;
	bool manual = !trigger && !READ_ONCE(rknpu_dev->shutting_down);
	int count = 0;
	int reset_ret;
	int i, j;

	reset_ret = rknpu_reset_begin(rknpu_dev);
	if (reset_ret)
		return reset_ret;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (manual) {
		for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
			if (rknpu_dev->subcore_datas[i].job ||
			    !list_empty(&rknpu_dev->subcore_datas[i].todo_list)) {
				spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
				rknpu_reset_end(rknpu_dev);
				return -EBUSY;
			}
		}
	}
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		struct rknpu_job *job = rknpu_dev->subcore_datas[i].job;

		if (!job)
			continue;
		for (j = 0; j < count; j++)
			if (jobs[j] == job)
				break;
		if (j != count)
			continue;
		if (!rknpu_job_get(job))
			continue;
		jobs[count++] = job;
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	reset_ret = rknpu_reset_execute(rknpu_dev);
	if (reset_ret) {
		/* Keep every DMA mapping pinned when hardware quiescence is unknown. */
		rknpu_reset_fail(rknpu_dev);
		for (i = 0; i < count; i++)
			rknpu_job_put(jobs[i]);
		return reset_ret;
	}

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	for (i = 0; i < count; i++) {
		struct rknpu_job *job = jobs[i];

		rknpu_job_detach_locked(job);
		job->flags &= ~RKNPU_JOB_RECOVERY_PENDING;
		if (!job->ret)
			job->ret = trigger && job != trigger ? -EIO : ret;
		rknpu_job_finalize_locked(job, job->ret);
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	rknpu_reset_end(rknpu_dev);

	for (i = 0; i < count; i++) {
		rknpu_job_finish(jobs[i]);
		rknpu_job_put(jobs[i]);
	}
	if (!READ_ONCE(rknpu_dev->shutting_down))
		for (i = 0; i < rknpu_dev->config->num_irqs; i++)
			rknpu_job_next(rknpu_dev, i);

	return 0;
}

int rknpu_job_recover_all(struct rknpu_device *rknpu_dev, int ret)
{
	return rknpu_job_recover_device(rknpu_dev, NULL, ret);
}

int rknpu_job_shutdown(struct rknpu_device *rknpu_dev)
{
	struct rknpu_job *job;
	unsigned long flags;
	bool active = false;
	int ret;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	list_for_each_entry(job, &rknpu_dev->jobs, device_node) {
		job->flags |= RKNPU_JOB_SHUTDOWN_PENDING;
		if (rknpu_job_is_active_locked(job))
			active = true;
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (active) {
		ret = rknpu_job_recover_all(rknpu_dev, -ENODEV);
		if (ret)
			return ret;
	}

	for (;;) {
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		job = list_first_entry_or_null(&rknpu_dev->jobs,
					       struct rknpu_job, device_node);
		if (job && !rknpu_job_get(job))
			job = NULL;
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		if (!job)
			break;

		cancel_work_sync(&job->recovery_work);
		cancel_delayed_work_sync(&job->timeout_work);

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		if (!(job->flags & RKNPU_JOB_FINALIZED)) {
			rknpu_job_detach_locked(job);
			job->flags &= ~RKNPU_JOB_RECOVERY_PENDING;
			rknpu_job_finalize_locked(job, -ENODEV);
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		rknpu_job_finish(job);
		cancel_work_sync(&job->cleanup_work);
		rknpu_job_cleanup(job);
		rknpu_job_put(job);
	}

	flush_workqueue(rknpu_dev->job_wq);
	return rknpu_jobs_idle(rknpu_dev) ? 0 : -EBUSY;
}

bool rknpu_jobs_idle(struct rknpu_device *rknpu_dev)
{
	unsigned long flags;
	bool idle = true;
	int i;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (!list_empty(&rknpu_dev->jobs))
		idle = false;
	for (i = 0; idle && i < rknpu_dev->config->num_irqs; i++) {
		if (rknpu_dev->subcore_datas[i].job ||
		    !list_empty(&rknpu_dev->subcore_datas[i].todo_list)) {
			idle = false;
			break;
		}
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	return idle;
}

static void rknpu_job_abort(struct rknpu_job *job)
{
	struct rknpu_device *rknpu_dev = job->rknpu_dev;
	unsigned long flags;
	bool active;
	bool finalized = false;
	int ret;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (job->flags & RKNPU_JOB_FINALIZED) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		goto out_cleanup;
	}
	active = rknpu_job_is_active_locked(job);
	if (!active) {
		rknpu_job_detach_locked(job);
		finalized = rknpu_job_finalize_locked(job,
						      job->ret ? job->ret : -EIO);
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (active) {
		ret = rknpu_job_recover_device(rknpu_dev, job,
					       job->ret ? job->ret : -EIO);
		if (ret) {
			LOG_ERROR("failed to quiesce aborted job: %d\n", ret);
			return;
		}
		goto out_cleanup;
	}

	if (finalized)
		rknpu_job_finish(job);

out_cleanup:
	if (!(job->flags & RKNPU_JOB_ASYNC))
		rknpu_job_cleanup(job);
}
static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
	uint32_t fuzz_status = 0;

	if ((status & 0x3) != 0)
		fuzz_status |= 0x3;

	if ((status & 0xc) != 0)
		fuzz_status |= 0xc;

	if ((status & 0x30) != 0)
		fuzz_status |= 0x30;

	if ((status & 0xc0) != 0)
		fuzz_status |= 0xc0;

	if ((status & 0x300) != 0)
		fuzz_status |= 0x300;

	if ((status & 0xc00) != 0)
		fuzz_status |= 0xc00;

	return fuzz_status;
}

static inline irqreturn_t rknpu_irq_handler(int irq, void *data, int core_index)
{
	struct rknpu_device *rknpu_dev = data;
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	uint32_t status = 0;
	unsigned long flags;
	bool complete = false;
	bool recover = false;
	bool queued;

	subcore_data = &rknpu_dev->subcore_datas[core_index];

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	job = subcore_data->job;
	if (!job) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return IRQ_NONE;
	}

	status = REG_READ(RKNPU_CORE_REG_INT_STATUS);
	if (!status) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return IRQ_NONE;
	}

	if (!rknpu_job_get(job)) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		return IRQ_NONE;
	}
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	job->int_status[core_index] = status;

	if (rknpu_fuzz_status(status) != job->int_mask[core_index]) {
		LOG_ERROR(
			"invalid irq status: %#x, raw status: %#x, require mask: %#x, task counter: %#x\n",
			status, REG_READ(RKNPU_CORE_REG_INT_RAW_STATUS),
			job->int_mask[core_index],
			(REG_READ_OFFSET(rknpu_dev->config->pc_task_status_offset) &
			 rknpu_dev->config->pc_task_number_mask));
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_CORE_REG_INT_CLEAR);
		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		if (!(job->flags & (RKNPU_JOB_FINALIZED |
				    RKNPU_JOB_RECOVERY_PENDING)) &&
		    subcore_data->job == job) {
			job->ret = job->ret ? job->ret : -EIO;
			job->flags |= RKNPU_JOB_RECOVERY_PENDING;
			recover = true;
		}
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		if (recover) {
			queued = queue_work(rknpu_dev->job_wq,
					    &job->recovery_work);
			if (queued)
				return IRQ_HANDLED;
		}
		rknpu_job_put(job);
		return IRQ_HANDLED;
	}

	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_CORE_REG_INT_CLEAR);

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	if (subcore_data->job == job && !job->core_done[core_index])
		complete = true;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	if (complete)
		rknpu_job_done(job, 0, core_index);

	LOG_DBG("PC irq: core=%u status=%#x expected=%#x counter=%#x\n",
		  core_index, status, job->int_mask[core_index],
		  REG_READ_OFFSET(rknpu_dev->config->pc_task_status_offset));

	rknpu_job_put(job);

	return IRQ_HANDLED;
}

irqreturn_t rknpu_core0_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 0);
}

irqreturn_t rknpu_core1_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 1);
}

irqreturn_t rknpu_core2_irq_handler(int irq, void *data)
{
	return rknpu_irq_handler(irq, data, 2);
}

static bool rknpu_core_mask_is_supported(struct rknpu_device *rknpu_dev,
					 unsigned int core_mask)
{
	if (core_mask & ~rknpu_dev->config->core_mask)
		return false;

	if (core_mask == RKNPU_CORE_AUTO_MASK ||
	    core_mask == RKNPU_CORE0_MASK ||
	    core_mask == RKNPU_CORE1_MASK ||
	    core_mask == RKNPU_CORE2_MASK ||
	    core_mask == (RKNPU_CORE0_MASK | RKNPU_CORE1_MASK) ||
	    core_mask == (RKNPU_CORE0_MASK | RKNPU_CORE1_MASK |
			 RKNPU_CORE2_MASK))
		return true;

	return false;
}

static bool rknpu_submit_task_base_is_valid(struct rknpu_submit *args,
					    struct rknpu_gem_object *task_obj)
{
	unsigned int core_index;

	if (hweight32(args->core_mask) != 1)
		return false;

	core_index = ffs(args->core_mask) - 1;
	return task_obj->core_maps[core_index].mapped &&
	       args->task_base_addr ==
			task_obj->core_maps[core_index].dma_addr;
}

static int rknpu_submit_full(struct rknpu_device *rknpu_dev,
			      struct rknpu_submit *args,
			      struct rknpu_gem_object *task_obj);
static int rknpu_submit(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args,
			struct rknpu_gem_object *task_obj);

static int __maybe_unused rknpu_submit_full(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args,
			struct rknpu_gem_object *task_obj)
{
	struct rknpu_job *job = NULL;
	bool timeout_armed;
	int ret = -EINVAL;

	if (!task_obj || task_obj->base.dev != rknpu_dev->drm_dev ||
	    !task_obj->kv_addr || !task_obj->pages ||
	    !(task_obj->flags & RKNPU_MEM_KERNEL_MAPPING) ||
	    !rknpu_submit_task_base_is_valid(args, task_obj))
		return -EINVAL;

	if (!args->task_number || (args->flags & ~RKNPU_JOB_MASK) ||
	    !(args->flags & RKNPU_JOB_PC)) {
		LOG_ERROR("invalid rknpu job flags or task number!\n");
		return -EINVAL;
	}

	if (!rknpu_dev->config->max_submit_number ||
	    rknpu_dev->config->max_submit_number > U32_MAX)
		return -EINVAL;

	if (!rknpu_task_range_is_valid(task_obj->size, args->task_start,
				       args->task_number)) {
		LOG_ERROR("invalid rknpu task range!\n");
		return -EINVAL;
	}

	if (!rknpu_core_mask_is_supported(rknpu_dev, args->core_mask)) {
		LOG_ERROR("invalid rknpu core mask: %#x", args->core_mask);
		return -EINVAL;
	}

	if (rknpu_dev->config->num_irqs > 1 &&
	    args->core_mask != RKNPU_CORE_AUTO_MASK) {
		int core_index;
		int use_core_num = hweight32(args->core_mask);

		for (core_index = 0; core_index < rknpu_dev->config->num_irqs;
		     core_index++) {
			struct rknpu_subcore_task *subcore;

			if (!(args->core_mask & rknpu_core_mask(core_index)))
				continue;
			subcore = use_core_num == 3 ?
				  &args->subcore_task[core_index + 2] :
				  &args->subcore_task[core_index];
			if (!rknpu_task_range_is_valid(task_obj->size,
						       subcore->task_start,
						       subcore->task_number)) {
				LOG_ERROR("invalid rknpu subcore task range!\n");
				return -EINVAL;
			}
		}
	}

	job = rknpu_job_alloc(rknpu_dev, args, task_obj);
	if (!job) {
		LOG_ERROR("failed to allocate rknpu job!\n");
		return -ENOMEM;
	}

	if (args->flags & RKNPU_JOB_NONBLOCK)
		job->flags |= RKNPU_JOB_ASYNC;

	if (args->flags & RKNPU_JOB_FENCE_IN) {
		struct dma_fence *in_fence;

		in_fence = sync_file_get_fence(args->fence_fd);
		if (!in_fence) {
			LOG_ERROR("invalid fence in fd, fd: %d\n", args->fence_fd);
			ret = -EINVAL;
			goto err_job;
		}
		args->fence_fd = -1;

		unsigned long timeout_jiffies =
			msecs_to_jiffies(args->timeout ? args->timeout : 3000);

		ret = dma_fence_wait_timeout(in_fence, true, timeout_jiffies);
		dma_fence_put(in_fence);
		if (ret == 0) {
			ret = -ETIMEDOUT;
			LOG_ERROR("Timed out waiting for input fence!\n");
			goto err_job;
		}
		if (ret < 0) {
			if (ret != -ERESTARTSYS)
				LOG_ERROR("Error (%d) waiting for fence!\n", ret);
			goto err_job;
		}
	}

	if (args->flags & RKNPU_JOB_FENCE_OUT) {
		ret = rknpu_fence_alloc(job);
		if (ret)
			goto err_job;
		ret = rknpu_fence_prepare_fd(job);
		if (ret < 0)
			goto err_job;
		job->args->fence_fd = ret;
		args->fence_fd = ret;
	}

	if (args->flags & RKNPU_JOB_NONBLOCK) {
		ret = rknpu_power_get(rknpu_dev);
		if (ret)
			goto err_job;
		job->power_held = true;
		if (!rknpu_job_get(job)) {
			ret = -EIO;
			goto err_job;
		}
		refcount_inc(&job->refcount);
		timeout_armed = schedule_delayed_work(&job->timeout_work,
						msecs_to_jiffies(args->timeout));
		if (!timeout_armed) {
			rknpu_job_put(job);
			ret = -EIO;
			rknpu_job_put(job);
			goto err_job;
		}
		rknpu_job_schedule(job);
		ret = job->ret;
		if (ret) {
			rknpu_job_cancel_timeout(job);
			rknpu_job_cleanup(job);
			rknpu_job_put(job);
			return ret;
		}
		rknpu_fence_install_fd(job);
		rknpu_job_put(job);
	} else {
		rknpu_job_schedule(job);
		if (job->ret) {
			ret = job->ret;
			rknpu_job_cleanup(job);
			return ret;
		}
		job->ret = rknpu_job_wait(job);

		args->task_counter = job->args->task_counter;
		ret = job->ret;
		rknpu_fence_install_fd(job);
		if (!ret)
			rknpu_job_cleanup(job);
		else
			rknpu_job_abort(job);
	}

	return ret;

err_job:
	rknpu_job_cleanup(job);
	return ret;
}

/*
 * Validate the runtime ABI, select the per-core mapping, and continue into
 * the full job path. Keep this helper below rknpu_submit(): calling the
 * internal implementation directly is essential because the public entry
 * dispatches back to this helper.
 */
static int rknpu_submit_checked(struct rknpu_device *rknpu_dev,
			   struct rknpu_submit *args,
			   struct rknpu_gem_object *task_obj)
{
	unsigned int core_mask = args->core_mask;
	unsigned int core_index;
	dma_addr_t dma_addr = 0;

	if (!task_obj || task_obj->base.dev != rknpu_dev->drm_dev)
		return -EINVAL;

	/*
	 * AUTO is resolved by the scheduler only in the full job path. Resolve
	 * it deterministically to core0 here, matching the canonical GEM
	 * mapping returned to userspace.
	 */
	if (core_mask == RKNPU_CORE_AUTO_MASK)
		core_mask = RKNPU_CORE0_MASK;

	if (hweight32(core_mask) != 1 ||
	    core_mask & ~rknpu_dev->config->core_mask) {
		LOG_ERROR("one target core per submit is supported, mask=%#x\n",
			  args->core_mask);
		return -EOPNOTSUPP;
	}
	core_index = ffs(core_mask) - 1;

	if (!args->task_number || (args->flags & ~RKNPU_JOB_MASK) ||
	    !(args->flags & RKNPU_JOB_PC)) {
		LOG_ERROR("invalid rknpu job flags or task number!\n");
		return -EINVAL;
	}

	if (!rknpu_task_range_is_valid(task_obj->size, args->task_start,
				       args->task_number)) {
		LOG_ERROR("invalid rknpu task range! start=%u number=%u size=%lu\n",
			  args->task_start, args->task_number, task_obj->size);
		return -EINVAL;
	}

	if (!(task_obj->flags & RKNPU_MEM_KERNEL_MAPPING) ||
	    !task_obj->pages_backed || !task_obj->kv_addr) {
		LOG_ERROR("task buffer lacks kernel-mapped backing pages\n");
		return -EINVAL;
	}

	/* Select target core based on whether the buffer is mapped on that core. */
	if (!task_obj->core_maps[core_index].mapped ||
	    !task_obj->core_maps[core_index].sgt) {
		LOG_ERROR("task buffer is not mapped on core %u, buffer mask=%#x\n",
			  core_index, task_obj->core_mask);
		return -EINVAL;
	}

	dma_addr = task_obj->core_maps[core_index].dma_addr;
	if (args->task_base_addr && args->task_base_addr != dma_addr) {
		LOG_ERROR("task base address mismatch: %#llx != %#llx\n",
			  args->task_base_addr, dma_addr);
		return -EINVAL;
	}

	/* Nonblock and fence synchronization are handled by rknpu_submit_full(). */

	args->core_mask = core_mask;
	args->task_base_addr = dma_addr;

	/*
	 * On multi-core hardware, use subcore_task[core_index] as authoritative.
	 * Only fall back to top-level fields when the per-core slot is empty.
	 */
	if (rknpu_dev->config->num_irqs > 1) {
		struct rknpu_subcore_task *subcore =
			&args->subcore_task[core_index];

		if (!subcore->task_number) {
			LOG_WARN("subcore_task[%u] empty, using top-level task range %u..%u\n",
				 core_index, args->task_start,
				 args->task_number);
			subcore->task_start = args->task_start;
			subcore->task_number = args->task_number;
		} else if (!rknpu_task_range_is_valid(task_obj->size,
						      subcore->task_start,
						      subcore->task_number)) {
			LOG_ERROR("subcore_task[%u] out of range: start=%u number=%u buffer_size=%lu\n",
				  core_index, subcore->task_start,
				  subcore->task_number, task_obj->size);
			return -EINVAL;
		}
	} else {
		args->subcore_task[core_index].task_start = args->task_start;
		args->subcore_task[core_index].task_number = args->task_number;
	}

	/* Log submit descriptor details before arming. */
	LOG_DBG(
		"submit probe: flags=%#x core_mask=%#x use_core=%u task_start=%u task_number=%u subcore=[%u/%u %u/%u %u/%u %u/%u %u/%u] pingpong=%d\n",
		args->flags, args->core_mask, hweight32(args->core_mask),
		args->task_start, args->task_number,
		args->subcore_task[0].task_start, args->subcore_task[0].task_number,
		args->subcore_task[1].task_start, args->subcore_task[1].task_number,
		args->subcore_task[2].task_start, args->subcore_task[2].task_number,
		args->subcore_task[3].task_start, args->subcore_task[3].task_number,
		args->subcore_task[4].task_start, args->subcore_task[4].task_number,
		args->flags & RKNPU_JOB_PINGPONG ? 1 : 0);

	return rknpu_submit_full(rknpu_dev, args, task_obj);
}

static int rknpu_submit(struct rknpu_device *rknpu_dev,
			struct rknpu_submit *args,
			struct rknpu_gem_object *task_obj)
{
	return rknpu_submit_checked(rknpu_dev, args, task_obj);
}

int rknpu_submit_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;

	struct rknpu_submit *args = data;
	struct rknpu_gem_object *task_obj;
	int ret;

	task_obj = rknpu_gem_object_find_token(dev, file_priv,
					       args->task_obj_addr);
	if (!task_obj)
		return -ENOENT;

	/*
	 * Align submit domain with task_obj domain:
	 * If submit domain_id was not explicitly specified (>0), inherit
	 * the domain_id from the task buffer (which respects per-FD isolation).
	 */
	if (args->iommu_domain_id == 0 && task_obj)
		args->iommu_domain_id = task_obj->iommu_domain_id;

	ret = rknpu_submit(rknpu_dev, args, task_obj);
	rknpu_gem_object_put(&task_obj->base);

	return ret;
}


int rknpu_get_hw_version(struct rknpu_device *rknpu_dev, uint32_t *version)
{
	if (version == NULL)
		return -EINVAL;

	*version = rknpu_core_read(rknpu_dev, 0, RKNPU_CORE_REG_VERSION) +
		   (rknpu_core_read(rknpu_dev, 0, RKNPU_CORE_REG_VERSION_NUM) &
		    0xffff);

	return 0;
}

int rknpu_get_bw_priority(struct rknpu_device *rknpu_dev, uint32_t *priority,
			  uint32_t *expect, uint32_t *tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);

	if (priority != NULL)
		*priority = _REG_READ(base, 0x0);

	if (expect != NULL)
		*expect = _REG_READ(base, 0x8);

	if (tw != NULL)
		*tw = _REG_READ(base, 0xc);

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_set_bw_priority(struct rknpu_device *rknpu_dev, uint32_t priority,
			  uint32_t expect, uint32_t tw)
{
	void __iomem *base = rknpu_dev->bw_priority_base;

	if (!base)
		return -EINVAL;

	spin_lock(&rknpu_dev->lock);

	if (priority != 0)
		_REG_WRITE(base, priority, 0x0);

	if (expect != 0)
		_REG_WRITE(base, expect, 0x8);

	if (tw != 0)
		_REG_WRITE(base, tw, 0xc);

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_clear_rw_amount(struct rknpu_device *rknpu_dev)
{
	unsigned int core_index = 0;
	const struct rknpu_config *config = rknpu_dev->config;
	unsigned long flags;

	if (config->amount_top == NULL) {
		LOG_WARN("Clear rw_amount is not supported on this device!\n");
		return 0;
	}

	if (config->pc_dma_ctrl) {
		uint32_t pc_data_addr = 0;

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
		pc_data_addr = REG_READ(RKNPU_CORE_REG_PC_DATA_ADDR);

		REG_WRITE(0x1, RKNPU_CORE_REG_PC_DATA_ADDR);
		REG_WRITE_OFFSET(0x80000101,
				 config->amount_top->offset_clr_all);
		REG_WRITE_OFFSET(0x00000101,
				 config->amount_top->offset_clr_all);
		if (config->amount_core) {
			REG_WRITE_OFFSET(0x80000101,
					 config->amount_core->offset_clr_all);
			REG_WRITE_OFFSET(0x00000101,
					 config->amount_core->offset_clr_all);
		}
		REG_WRITE(pc_data_addr, RKNPU_CORE_REG_PC_DATA_ADDR);
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	} else {
		spin_lock(&rknpu_dev->lock);
		REG_WRITE_OFFSET(0x80000101,
				 config->amount_top->offset_clr_all);
		REG_WRITE_OFFSET(0x00000101,
				 config->amount_top->offset_clr_all);
		if (config->amount_core) {
			REG_WRITE_OFFSET(0x80000101,
					 config->amount_core->offset_clr_all);
			REG_WRITE_OFFSET(0x00000101,
					 config->amount_core->offset_clr_all);
		}
		spin_unlock(&rknpu_dev->lock);
	}

	return 0;
}

int rknpu_get_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *dt_wr,
			uint32_t *dt_rd, uint32_t *wd_rd)
{
	unsigned int core_index = 0;
	const struct rknpu_config *config = rknpu_dev->config;
	int amount_scale = config->pc_data_amount_scale;

	if (config->amount_top == NULL) {
		LOG_WARN("Get rw_amount is not supported on this device!\n");
		return 0;
	}

	spin_lock(&rknpu_dev->lock);

	if (dt_wr != NULL) {
		*dt_wr = REG_READ_OFFSET(config->amount_top->offset_dt_wr) *
			 amount_scale;
		if (config->amount_core) {
			*dt_wr += REG_READ_OFFSET(config->amount_core->offset_dt_wr) *
				  amount_scale;
		}
	}

	if (dt_rd != NULL) {
		*dt_rd = REG_READ_OFFSET(config->amount_top->offset_dt_rd) *
			 amount_scale;
		if (config->amount_core) {
			*dt_rd += REG_READ_OFFSET(config->amount_core->offset_dt_rd) *
				  amount_scale;
		}
	}

	if (wd_rd != NULL) {
		*wd_rd = REG_READ_OFFSET(config->amount_top->offset_wt_rd) *
			 amount_scale;
		if (config->amount_core) {
			*wd_rd += REG_READ_OFFSET(config->amount_core->offset_wt_rd) *
				  amount_scale;
		}
	}

	spin_unlock(&rknpu_dev->lock);

	return 0;
}

int rknpu_get_total_rw_amount(struct rknpu_device *rknpu_dev, uint32_t *amount)
{
	const struct rknpu_config *config = rknpu_dev->config;
	uint32_t dt_wr = 0;
	uint32_t dt_rd = 0;
	uint32_t wd_rd = 0;
	int ret = -EINVAL;

	if (config->amount_top == NULL) {
		LOG_WARN(
			"Get total_rw_amount is not supported on this device!\n");
		return 0;
	}

	ret = rknpu_get_rw_amount(rknpu_dev, &dt_wr, &dt_rd, &wd_rd);

	if (amount != NULL)
		*amount = dt_wr + dt_rd + wd_rd;

	return ret;
}

/*
 * Report the NPU core rail voltage in microvolts, or 0 if this device exposes
 * none. Returns 0 rather than -EINVAL when there is no regulator.
 */
int rknpu_get_volt(struct rknpu_device *rknpu_dev, uint32_t *value)
{
	unsigned int c, r;

	if (value == NULL)
		return -EINVAL;

	*value = 0;

	for (c = 0; c < RKNPU_MAX_CORES; c++) {
		struct rknpu_core *core = &rknpu_dev->cores[c];
		int microvolts = -EINVAL;

		if (!core->registered)
			continue;

		for (r = 0; r < core->num_regulators; r++) {
			if (!core->regulators[r].consumer)
				continue;
			microvolts = regulator_get_voltage(
				core->regulators[r].consumer);
			if (microvolts > 0)
				break;
		}

		if (microvolts > 0) {
			*value = (uint32_t)microvolts;
			break;
		}
	}

	return 0;
}
