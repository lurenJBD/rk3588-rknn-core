// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/delay.h>

#include "rknpu_core.h"
#include "rknpu_reset.h"

static inline struct reset_control *rknpu_reset_control_get(struct device *dev,
							    const char *name)
{
	struct reset_control *rst = NULL;

	rst = devm_reset_control_get(dev, name);
	if (IS_ERR(rst))
		LOG_DEV_ERROR(dev,
			      "failed to get rknpu reset control: %s, %ld\n",
			      name, PTR_ERR(rst));

	return rst;
}

int rknpu_reset_get(struct rknpu_device *rknpu_dev)
{
	int i = 0;
	int num_srsts = 0;

	num_srsts = of_count_phandle_with_args(rknpu_dev->dev->of_node,
					       "resets", "#reset-cells");
	if (num_srsts <= 0) {
		LOG_DEV_ERROR(rknpu_dev->dev,
			      "failed to get rknpu resets from dtb\n");
		return num_srsts;
	}

	rknpu_dev->srsts = devm_kcalloc(rknpu_dev->dev, num_srsts,
					sizeof(*rknpu_dev->srsts), GFP_KERNEL);
	if (!rknpu_dev->srsts)
		return -ENOMEM;

	for (i = 0; i < num_srsts; ++i) {
		rknpu_dev->srsts[i] = devm_reset_control_get_exclusive_by_index(
			rknpu_dev->dev, i);
		if (IS_ERR(rknpu_dev->srsts[i])) {
			rknpu_dev->num_srsts = i;
			return PTR_ERR(rknpu_dev->srsts[i]);
		}
	}

	rknpu_dev->num_srsts = num_srsts;

	return num_srsts;
}

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

	if (rknpu_dev->bypass_soft_reset) {
		LOG_WARN("bypass soft reset\n");
		return -EOPNOTSUPP;
	}

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
