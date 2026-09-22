/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __LINUX_RKNPU_RESET_H
#define __LINUX_RKNPU_RESET_H

#include <linux/reset.h>

#include "rknpu_drv.h"

int rknpu_reset_get(struct rknpu_device *rknpu_dev);

int rknpu_reset_begin(struct rknpu_device *rknpu_dev);

int rknpu_reset_execute(struct rknpu_device *rknpu_dev);

void rknpu_reset_end(struct rknpu_device *rknpu_dev);

void rknpu_reset_fail(struct rknpu_device *rknpu_dev);

int rknpu_soft_reset(struct rknpu_device *rknpu_dev);

#endif
