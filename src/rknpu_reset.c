// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 *
 * Modified by lurenJBD <31967654+lurenJBD@users.noreply.github.com>.
 */

#include <linux/delay.h>

#include "rknpu_core.h"
#include "rknpu_reset.h"

static int rknpu_reset_assert(struct reset_control *rst)
{
	int ret = -EINVAL;

	if (!rst)
		return -EINVAL;

	ret = reset_control_assert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to assert rknpu reset: %d\n", ret);
		return ret;
	}

	return 0;
}

static int rknpu_reset_deassert(struct reset_control *rst)
{
	int ret = -EINVAL;

	if (!rst)
		return -EINVAL;

	ret = reset_control_deassert(rst);
	if (ret < 0) {
		LOG_ERROR("failed to deassert rknpu reset: %d\n", ret);
		return ret;
	}

	return 0;
}

int rknpu_reset_begin(struct rknpu_device *rknpu_dev)
{
	unsigned long flags;

	mutex_lock(&rknpu_dev->reset_lock);
	if (READ_ONCE(rknpu_dev->reset_failed)) {
		mutex_unlock(&rknpu_dev->reset_lock);
		return -EIO;
	}
	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	WRITE_ONCE(rknpu_dev->soft_reseting, true);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	return 0;
}

int rknpu_reset_execute(struct rknpu_device *rknpu_dev)
{
	int ret = 0;
	int core;
	int i;

	msleep(100);
	LOG_INFO("soft reset active cores: %#x\n",
		 rknpu_dev->available_cores);

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		struct rknpu_core *rk_core = &rknpu_dev->cores[core];

		if (!rk_core->registered)
			continue;
		for (i = 0; i < rk_core->num_srsts; ++i)
			ret |= rknpu_reset_assert(rk_core->srsts[i]);
	}

	udelay(10);

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		struct rknpu_core *rk_core = &rknpu_dev->cores[core];

		if (!rk_core->registered)
			continue;
		for (i = 0; i < rk_core->num_srsts; ++i)
			ret |= rknpu_reset_deassert(rk_core->srsts[i]);
	}

	udelay(10);

	if (ret)
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to soft reset for rknpu: %d\n", ret);
	else if (rknpu_dev->config->state_init)
		rknpu_dev->config->state_init(rknpu_dev);

	return ret;
}

void rknpu_reset_end(struct rknpu_device *rknpu_dev)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	WRITE_ONCE(rknpu_dev->soft_reseting, false);
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	mutex_unlock(&rknpu_dev->reset_lock);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++)
		wake_up_all(&rknpu_dev->subcore_datas[i].job_done_wq);
}

void rknpu_reset_fail(struct rknpu_device *rknpu_dev)
{
	int i;

	WRITE_ONCE(rknpu_dev->reset_failed, true);
	mutex_unlock(&rknpu_dev->reset_lock);

	for (i = 0; i < rknpu_dev->config->num_irqs; i++)
		wake_up_all(&rknpu_dev->subcore_datas[i].job_done_wq);
}

int rknpu_soft_reset(struct rknpu_device *rknpu_dev)
{
	if (!rknpu_jobs_idle(rknpu_dev))
		return -EBUSY;

	return rknpu_job_recover_all(rknpu_dev, -ECANCELED);
}
