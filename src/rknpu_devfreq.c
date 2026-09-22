// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Finley Xiao <finley.xiao@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/devfreq.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>

#include "rknpu_drv.h"
#include "rknpu_core.h"
#include "rknpu_devfreq.h"

#define RKNPU_FREQ_MIN 200000000UL /* 200 MHz - boot default / idle rate */
#define RKNPU_FREQ_MAX 800000000UL  /* 800 MHz - safe hardware limit on mainline */
#define RKNPU_FREQ_1GHZ 1000000000UL /* 1 GHz - reserved, pending GRF read margin */
#define RKNPU_BASELINE_VOLT 800000 /* 800 mV (0.80 V) - board boot default */
#define RKNPU_HIGH_FREQ_VOLT 850000 /* 850 mV (0.85 V) - reserved for 1 GHz */
#define RKNPU_MAX_VOLT 950000 /* 950 mV (0.95 V) - PMIC safe limit */

int rknpu_target_freq_mhz = 800;
module_param_named(target_freq_mhz, rknpu_target_freq_mhz, int, 0644);
MODULE_PARM_DESC(target_freq_mhz,
		 "Target NPU frequency in MHz (default: 800, safe range: 200-800)");

static DEFINE_MUTEX(rknpu_opp_lock);

struct clk *rknpu_get_npu_clk(struct rknpu_device *rknpu_dev)
{
	unsigned int i;

	if (!rknpu_dev || !rknpu_dev->cores[0].dev)
		return NULL;

	for (i = 0; i < rknpu_dev->cores[0].num_clks; i++) {
		if (rknpu_dev->cores[0].clks[i].id &&
		    strcmp(rknpu_dev->cores[0].clks[i].id, "npu") == 0)
			return rknpu_dev->cores[0].clks[i].clk;
	}

	if (rknpu_dev->cores[0].num_clks > 2)
		return rknpu_dev->cores[0].clks[2].clk;

	return NULL;
}

struct regulator *rknpu_get_npu_regulator(struct rknpu_device *rknpu_dev)
{
	unsigned int i;

	if (!rknpu_dev || !rknpu_dev->cores[0].dev)
		return NULL;

	for (i = 0; i < rknpu_dev->cores[0].num_regulators; i++) {
		if (rknpu_dev->cores[0].regulators[i].consumer)
			return rknpu_dev->cores[0].regulators[i].consumer;
	}

	return NULL;
}

static unsigned long rknpu_round_opp_freq(unsigned long freq)
{
	if (freq <= 200000000UL)
		return 200000000UL;
	if (freq <= 400000000UL)
		return 400000000UL;
	if (freq <= 600000000UL)
		return 600000000UL;
	return 800000000UL;
}

int rknpu_opp_set_freq(struct rknpu_device *rknpu_dev, unsigned long target_freq)
{
	struct clk *npu_clk;
	struct regulator *vdd;
	int cur_volt = 0;
	int target_volt = 0;
	int ret = 0;

	if (!rknpu_dev)
		return -ENODEV;

	mutex_lock(&rknpu_opp_lock);

	/* Allow MHz input (e.g. 800) */
	if (target_freq < 2000UL)
		target_freq *= 1000000UL;

	/* Clamp target frequency within supported operating range. */
	if (target_freq < RKNPU_FREQ_MIN)
		target_freq = RKNPU_FREQ_MIN;
	if (target_freq > RKNPU_FREQ_MAX)
		target_freq = RKNPU_FREQ_MAX;

	/* Select target operating voltage. */
	if (target_freq > 900000000UL)
		target_volt = RKNPU_HIGH_FREQ_VOLT; /* 850 mV (reserved) */
	else
		target_volt = RKNPU_BASELINE_VOLT;  /* 800 mV (safe 200-800 MHz) */

	/* Defer clock change until power domain is active. */
	if (!rknpu_dev->cores[0].power_held) {
		rknpu_dev->ondemand_freq = target_freq;
		mutex_unlock(&rknpu_opp_lock);
		return 0;
	}

	npu_clk = rknpu_get_npu_clk(rknpu_dev);
	if (!npu_clk) {
		mutex_unlock(&rknpu_opp_lock);
		return -ENODEV;
	}

	vdd = rknpu_get_npu_regulator(rknpu_dev);
	if (vdd)
		cur_volt = regulator_get_voltage(vdd);
	if (cur_volt <= 0)
		cur_volt = RKNPU_BASELINE_VOLT;

	/* DVFS Sequence: If voltage needs to increase, step voltage up first */
	if (vdd && target_volt > cur_volt) {
		ret = regulator_set_voltage(vdd, target_volt, RKNPU_MAX_VOLT);
		if (ret) {
			LOG_ERROR("failed to set NPU voltage to %d uV: %d\n",
				  target_volt, ret);
			mutex_unlock(&rknpu_opp_lock);
			return ret;
		}
		udelay(100);
	}

	/* Step clock frequency */
	ret = clk_set_rate(npu_clk, target_freq);
	if (ret) {
		LOG_ERROR("failed to set NPU clk to %lu Hz: %d\n",
			  target_freq, ret);
		/* If voltage was increased but clock failed, rollback voltage */
		if (vdd && target_volt > cur_volt)
			regulator_set_voltage(vdd, cur_volt, RKNPU_MAX_VOLT);
		mutex_unlock(&rknpu_opp_lock);
		return ret;
	}

	/* DVFS Sequence: If voltage needs to decrease, step voltage down after */
	if (vdd && target_volt < cur_volt) {
		ret = regulator_set_voltage(vdd, target_volt, RKNPU_MAX_VOLT);
		if (ret) {
			LOG_ERROR("failed to restore NPU voltage to %d uV: %d\n",
				  target_volt, ret);
		}
		udelay(100);
	}

	rknpu_dev->current_freq = clk_get_rate(npu_clk);
	if (vdd) {
		int v = regulator_get_voltage(vdd);
		if (v > 0)
			rknpu_dev->current_volt = v;
	} else {
		rknpu_dev->current_volt = target_volt;
	}

	LOG_DBG("NPU freq set to %lu Hz (%lu MHz), volt %lu uV\n",
		 rknpu_dev->current_freq, rknpu_dev->current_freq / 1000000UL,
		 rknpu_dev->current_volt);

	mutex_unlock(&rknpu_opp_lock);
	return 0;
}

static int rknpu_devfreq_target(struct device *dev, unsigned long *freq, u32 flags)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	unsigned long target_freq;
	int ret;

	if (!rknpu_dev)
		return -EINVAL;

	/* Robustly round to supported OPP bracket (200, 400, 600, 800 MHz) */
	target_freq = rknpu_round_opp_freq(*freq);
	*freq = target_freq;

	if (target_freq == rknpu_dev->current_freq)
		return 0;

	/*
	 * If hardware domain is not currently powered, record intended frequency
	 * and return 0. It will be enacted when runtime-resume powers the domain.
	 */
	if (!rknpu_dev->cores[0].power_held) {
		rknpu_dev->ondemand_freq = target_freq;
		return 0;
	}

	ret = rknpu_opp_set_freq(rknpu_dev, target_freq);
	if (!ret) {
		rknpu_dev->ondemand_freq = target_freq;
		if (rknpu_dev->devfreq)
			rknpu_dev->devfreq->last_status.current_frequency = target_freq;
	}
	return ret;
}

static int rknpu_devfreq_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *stat)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	if (!rknpu_dev)
		return -EINVAL;

	stat->current_frequency = rknpu_dev->current_freq ?
				  rknpu_dev->current_freq : RKNPU_FREQ_MIN;
	stat->total_time = 100;
	stat->busy_time = (rknpu_dev->cores[0].power_held &&
			   atomic_read(&rknpu_dev->power_refcount) > 0) ? 100 : 0;

	return 0;
}

static int rknpu_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	struct clk *npu_clk;

	if (!rknpu_dev)
		return -EINVAL;

	/* If powered on, read actual hardware clock; otherwise return cached */
	if (rknpu_dev->cores[0].power_held) {
		npu_clk = rknpu_get_npu_clk(rknpu_dev);
		if (npu_clk) {
			*freq = clk_get_rate(npu_clk);
			return 0;
		}
	}

	*freq = rknpu_dev->current_freq ? rknpu_dev->current_freq : RKNPU_FREQ_MIN;
	return 0;
}

static struct devfreq_dev_profile rknpu_devfreq_profile = {
	.polling_ms = 100,
	.target = rknpu_devfreq_target,
	.get_cur_freq = rknpu_devfreq_get_cur_freq,
	.get_dev_status = rknpu_devfreq_get_dev_status,
};

int rknpu_devfreq_init(struct rknpu_device *rknpu_dev)
{
	struct device *dev;
	struct clk *npu_clk = rknpu_get_npu_clk(rknpu_dev);
	struct regulator *vdd = rknpu_get_npu_regulator(rknpu_dev);
	unsigned long target_hz;
	int ret, i;
	static const unsigned long opp_table[] = {
		200000000UL,
		400000000UL,
		600000000UL,
		800000000UL,
	};

	if (!rknpu_dev || !rknpu_dev->cores[0].dev || !npu_clk)
		return -ENODEV;

	dev = rknpu_dev->cores[0].dev;

	rknpu_dev->current_freq = clk_get_rate(npu_clk);
	if (vdd) {
		int v = regulator_get_voltage(vdd);
		rknpu_dev->current_volt = (v > 0) ? v : RKNPU_BASELINE_VOLT;
	} else {
		rknpu_dev->current_volt = RKNPU_BASELINE_VOLT;
	}

	target_hz = (unsigned long)rknpu_target_freq_mhz * 1000000UL;
	if (target_hz < RKNPU_FREQ_MIN)
		target_hz = RKNPU_FREQ_MIN;
	if (target_hz > RKNPU_FREQ_MAX)
		target_hz = RKNPU_FREQ_MAX;

	rknpu_dev->ondemand_freq = target_hz;

	/* Dynamically populate OPP table for core 0 device */
	for (i = 0; i < ARRAY_SIZE(opp_table); i++) {
		ret = dev_pm_opp_add(dev, opp_table[i], RKNPU_BASELINE_VOLT);
		if (ret && ret != -EEXIST) {
			LOG_DEV_WARN(dev, "failed to add OPP %lu Hz: %d\n",
				     opp_table[i], ret);
		}
	}

	rknpu_devfreq_profile.initial_freq = target_hz;

	/*
	 * Register standard devfreq device with performance governor as initial default.
	 * Governors simple_ondemand, performance, and userspace are fully available in sysfs.
	 */
	rknpu_dev->devfreq = devfreq_add_device(dev, &rknpu_devfreq_profile,
						DEVFREQ_GOV_PERFORMANCE,
						NULL);
	if (IS_ERR(rknpu_dev->devfreq)) {
		LOG_DEV_WARN(dev, "failed to add devfreq device: %ld\n",
			     PTR_ERR(rknpu_dev->devfreq));
		rknpu_dev->devfreq = NULL;
	} else {
		/*
		 * Device starts unpowered at probe time. Suspend devfreq monitor immediately
		 * so it only activates during active workload execution.
		 */
		devfreq_suspend_device(rknpu_dev->devfreq);
		LOG_DEV_INFO(dev, "devfreq device registered, governor: %s (initially suspended)\n",
			     DEVFREQ_GOV_PERFORMANCE);
	}

	LOG_INFO("devfreq init: boot clk %lu Hz, volt %lu uV, target %lu MHz\n",
		 rknpu_dev->current_freq, rknpu_dev->current_volt,
		 target_hz / 1000000UL);

	return 0;
}

void rknpu_devfreq_remove(struct rknpu_device *rknpu_dev)
{
	struct device *dev = rknpu_dev->cores[0].dev;

	if (rknpu_dev->devfreq) {
		devfreq_remove_device(rknpu_dev->devfreq);
		rknpu_dev->devfreq = NULL;
	}

	if (dev)
		dev_pm_opp_remove_all_dynamic(dev);

	/* Gracefully restore clk to 200 MHz and voltage to 800 mV if powered */
	if (rknpu_dev->cores[0].power_held)
		rknpu_opp_set_freq(rknpu_dev, RKNPU_FREQ_MIN);
}

int rknpu_devfreq_runtime_suspend(struct device *dev)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	if (!rknpu_dev)
		return 0;

	if (rknpu_dev->devfreq)
		devfreq_suspend_device(rknpu_dev->devfreq);

	/*
	 * When suspending, restore clock to 200 MHz while power domain is STILL active
	 * to avoid high frequency stress and satisfy BL31 ATF sleep requirements.
	 */
	if (rknpu_dev->current_freq > RKNPU_FREQ_MIN)
		rknpu_opp_set_freq(rknpu_dev, RKNPU_FREQ_MIN);

	return 0;
}

int rknpu_devfreq_runtime_resume(struct device *dev)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	unsigned long target_hz;

	if (!rknpu_dev)
		return 0;

	target_hz = rknpu_dev->ondemand_freq;
	if (!target_hz)
		target_hz = (unsigned long)rknpu_target_freq_mhz * 1000000UL;

	/* Domain is now powered on, ramp clock to target rate */
	if (target_hz > 0 && target_hz != rknpu_dev->current_freq)
		rknpu_opp_set_freq(rknpu_dev, target_hz);

	if (rknpu_dev->devfreq)
		devfreq_resume_device(rknpu_dev->devfreq);

	return 0;
}

void rknpu_devfreq_lock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev && rknpu_dev->devfreq)
		mutex_lock(&rknpu_dev->devfreq->lock);
}

void rknpu_devfreq_unlock(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev && rknpu_dev->devfreq)
		mutex_unlock(&rknpu_dev->devfreq->lock);
}
