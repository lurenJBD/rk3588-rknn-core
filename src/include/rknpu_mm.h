/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __RKNPU_MM_H__
#define __RKNPU_MM_H__

#include <linux/types.h>

struct rknpu_mm {
	u32 total_chunks;
	u32 free_chunks;
	u32 chunk_size;
};

#endif /* __RKNPU_MM_H__ */
