/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __RKNPU_CORE_H__
#define __RKNPU_CORE_H__

#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/iommu.h>
#include <linux/platform_device.h>
#include <linux/types.h>

#include "rknpu_drv.h"

#define RKNPU_CORE_MAX_CLOCKS 4
#define RKNPU_CORE_MAX_RESETS 2

/* Size of one NPU core's aggregated register window */
#define RKNPU_CORE_WINDOW_SIZE 0x10000

/*
 * Window offsets of the block bases, as mesa's registers.xml lays them out in
 * the aggregated 64 KiB window.  Only the bases needed by the span check in
 * rknpu_core_read_window() are named here.
 */
#define RKNPU_CORE_WINDOW_PC_BASE	0x0000
#define RKNPU_CORE_WINDOW_CNA_BASE	0x1000
#define RKNPU_CORE_WINDOW_CORE_BASE	0x3000
#define RKNPU_CORE_WINDOW_DPU_BASE	0x4000
#define RKNPU_CORE_WINDOW_GLOBAL_BASE	0xf000

/**
 * struct rknpu_core - Per-core hardware resources
 *
 * @pdev: The platform device for this core (one of three rknn-core nodes)
 * @dev: &pdev->dev
 * @index: Core index, determined by reg[0].start (not probe order)
 * @pc: PC registers ("pc" reg-name, e.g. fdab0000)
 * @cna: CNA registers ("cna" reg-name, e.g. fdab1000)
 * @core_reg: CORE registers ("core" reg-name, e.g. fdab3000)
 * @clks: Per-core clocks (aclk, hclk, npu, pclk)
 * @num_clks: Number of clocks obtained
 * @regulators: Optional per-core regulators parsed from all *-supply entries
 * @num_regulators: Number of regulators in @regulators
 * @srsts: Per-core reset controls (srst_a, srst_h)
 * @num_srsts: Number of resets obtained
 * @irq: Platform IRQ for this core
 * @group: IOMMU group for this core's device, if any
 * @registered: True once this core has completed probe
 */
struct rknpu_core {
	struct platform_device *pdev;
	struct device *dev;
	unsigned int index;
	void __iomem *pc;
	void __iomem *cna;
	void __iomem *core_reg;
	/*
	 * Per-core 64 KiB register window mapping for block-level access.
	 * NULL if mapping failed.
	 */
	void __iomem *win;
	struct clk_bulk_data clks[RKNPU_CORE_MAX_CLOCKS];
	int num_clks;
	struct regulator_bulk_data *regulators;
	int num_regulators;
	struct reset_control *srsts[RKNPU_CORE_MAX_RESETS];
	int num_srsts;
	int irq;
	struct iommu_group *group;
	u32 version;
	u32 version_num;
	/*
	 * Set while this core holds one rknpu_power_get() reference (one
	 * pm_runtime_resume_and_get() plus one regulator_bulk_enable()).
	 * rknpu_power_get() takes exactly one per core, so the teardown side
	 * must put back exactly one per core - and each core's put has to
	 * happen while its own struct rknpu_core is still populated.
	 */
	bool power_held;
	bool registered;
};

int rknpu_core_index_by_addr(struct platform_device *pdev, unsigned int *index);
int rknpu_core_init(struct rknpu_device *rknpu_dev,
		    struct platform_device *pdev, unsigned int index);
void rknpu_core_fini(struct rknpu_device *rknpu_dev, unsigned int index);
void rknpu_core_release_regulators(struct rknpu_core *core);

/*
 * Block-level ("sub-core") registers.
 * Offsets are relative to each block's base.
 */
enum rknpu_core_block_reg {
	RKNPU_CORE_BLOCK_CNA_S_STATUS,
	RKNPU_CORE_BLOCK_CNA_S_POINTER,
	RKNPU_CORE_BLOCK_CNA_OP_EN,
	RKNPU_CORE_BLOCK_CORE_S_STATUS,
	RKNPU_CORE_BLOCK_CORE_S_POINTER,
	RKNPU_CORE_BLOCK_CORE_OP_EN,
	/*
	 * The downstream blocks and the op_en broadcast target.
	 */
	RKNPU_CORE_BLOCK_DPU_S_STATUS,
	RKNPU_CORE_BLOCK_DPU_S_POINTER,
	RKNPU_CORE_BLOCK_DPU_OP_EN,
	RKNPU_CORE_BLOCK_DPU_DATA_FORMAT,
	RKNPU_CORE_BLOCK_DPU_FEATURE_MODE,
	RKNPU_CORE_BLOCK_DPU_DST_BASE,
	RKNPU_CORE_BLOCK_DPU_OUT_SCALE,
	RKNPU_CORE_BLOCK_DPU_RDMA_S_STATUS,
	RKNPU_CORE_BLOCK_DPU_RDMA_S_POINTER,
	RKNPU_CORE_BLOCK_DPU_RDMA_OP_EN,
	RKNPU_CORE_BLOCK_PPU_S_STATUS,
	RKNPU_CORE_BLOCK_PPU_S_POINTER,
	RKNPU_CORE_BLOCK_PPU_OP_EN,
	RKNPU_CORE_BLOCK_PPU_RDMA_S_STATUS,
	RKNPU_CORE_BLOCK_PPU_RDMA_S_POINTER,
	RKNPU_CORE_BLOCK_PPU_RDMA_OP_EN,
	RKNPU_CORE_BLOCK_GLOBAL_OP_EN,
	RKNPU_CORE_BLOCK_REG_COUNT,
};

u32 rknpu_core_read_block(struct rknpu_device *rknpu_dev, unsigned int core,
			  enum rknpu_core_block_reg reg);

/* Accessors for compatibility with the existing job/MMIO abstraction */
static inline void __iomem *rknpu_core_pc(struct rknpu_device *rknpu_dev,
					   unsigned int core)
{
	return rknpu_dev->base[core];
}

static inline void __iomem *
rknpu_core_multicore(struct rknpu_device *rknpu_dev, unsigned int core,
		     unsigned int multicore_index)
{
	return rknpu_dev->multicore_base[core][multicore_index];
}

#endif /* __RKNPU_CORE_H__ */
