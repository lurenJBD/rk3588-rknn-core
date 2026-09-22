/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */
#ifndef __RKNPU_COMPAT_H_
#define __RKNPU_COMPAT_H_

#include <linux/of.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * Embedded by value in struct rknpu_device, but only ever touched by the
 * (disabled) devfreq translation unit.  Keep an opaque placeholder so the
 * struct definition compiles.
 */
struct rockchip_opp_info {
	int unused;
};

/* Referenced only through pointers in struct rknpu_device. */
struct monitor_dev_info;
struct rk_dma_heap;

/*
 * Vendor helper that reads the fused-off NPU core mask from an nvmem cell
 * on partial-good dies.  A full RK3588 exposes all three cores; if the DT
 * does not declare the "cores" nvmem cell the caller is never reached, so
 * reporting -ENOENT (mask stays 0 => all cores valid) is correct here.
 */
static inline int rockchip_nvmem_cell_read_u8(struct device_node *np,
					      const char *cell_id, u8 *val)
{
	return -ENOENT;
}

/*
 * OPP / System Monitor / IPA stubs for 0.9.8
 * The driver references these but they're only used in devfreq paths
 */
static inline int rockchip_opp_set_low_length(struct device *dev, int val)
{
	return 0;
}

static inline int rockchip_system_monitor_register(struct device *dev,
						   struct monitor_dev_info **info)
{
	return -ENODEV;
}

static inline void rockchip_system_monitor_unregister(struct monitor_dev_info *info)
{
}

static inline int rockchip_ipa_power_model_init(struct device *dev,
						struct monitor_dev_info *info)
{
	return -ENODEV;
}

#endif /* __RKNPU_COMPAT_H_ */
