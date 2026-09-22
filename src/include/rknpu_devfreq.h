/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Finley Xiao <finley.xiao@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __LINUX_RKNPU_DEVFREQ_H
#define __LINUX_RKNPU_DEVFREQ_H

#include <linux/types.h>

struct rknpu_device;
struct device;
struct clk;
struct regulator;

extern int rknpu_target_freq_mhz;

struct clk *rknpu_get_npu_clk(struct rknpu_device *rknpu_dev);
struct regulator *rknpu_get_npu_regulator(struct rknpu_device *rknpu_dev);
int rknpu_opp_set_freq(struct rknpu_device *rknpu_dev, unsigned long target_freq);

#ifdef CONFIG_PM_DEVFREQ
void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev);
void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev);
int rknpu_devfreq_init(struct rknpu_device *rknpu_dev);
void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev);
int rknpu_devfreq_runtime_suspend(struct device *dev);
int rknpu_devfreq_runtime_resume(struct device *dev);
#else
static inline int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	return -EOPNOTSUPP;
}

static inline void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
}

static inline void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
}

static inline void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
}

static inline int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	return -EOPNOTSUPP;
}

static inline int rknpu_devfreq_runtime_resume(struct device *dev)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_PM_DEVFREQ */

#endif /* __LINUX_RKNPU_DEVFREQ_H */
