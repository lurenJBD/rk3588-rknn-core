// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "rknpu_core.h"
#include "rknpu_drv.h"
#include "rknpu_job.h"
#include "rknpu_iommu.h"

static const u16 rknpu_core_mmio_offsets[] = {
	[RKNPU_CORE_REG_VERSION] = RKNPU_OFFSET_VERSION,
	[RKNPU_CORE_REG_VERSION_NUM] = RKNPU_OFFSET_VERSION_NUM,
	[RKNPU_CORE_REG_PC_OP_EN] = RKNPU_OFFSET_PC_OP_EN,
	[RKNPU_CORE_REG_PC_DATA_ADDR] = RKNPU_OFFSET_PC_DATA_ADDR,
	[RKNPU_CORE_REG_PC_DATA_AMOUNT] = RKNPU_OFFSET_PC_DATA_AMOUNT,
	[RKNPU_CORE_REG_INT_MASK] = RKNPU_OFFSET_INT_MASK,
	[RKNPU_CORE_REG_INT_CLEAR] = RKNPU_OFFSET_INT_CLEAR,
	[RKNPU_CORE_REG_INT_STATUS] = RKNPU_OFFSET_INT_STATUS,
	[RKNPU_CORE_REG_INT_RAW_STATUS] = RKNPU_OFFSET_INT_RAW_STATUS,
	[RKNPU_CORE_REG_PC_TASK_CONTROL] = RKNPU_OFFSET_PC_TASK_CONTROL,
	[RKNPU_CORE_REG_PC_DMA_BASE_ADDR] = RKNPU_OFFSET_PC_DMA_BASE_ADDR,
	[RKNPU_CORE_REG_PC_TASK_STATUS_RK3588] = 0x3c,
};

static int rknpu_multicore_reg_index(enum rknpu_core_mmio_reg reg)
{
	switch (reg) {
	case RKNPU_CORE_REG_MULTICORE_CFG:
		return 0;
	case RKNPU_CORE_REG_MULTICORE_CFG2:
		return 1;
	default:
		return -EINVAL;
	}
}

u32 rknpu_core_read(struct rknpu_device *rknpu_dev, unsigned int core,
		    enum rknpu_core_mmio_reg reg)
{
	int multicore_index;

	if (WARN_ON_ONCE(core >= RKNPU_MAX_CORES ||
			 reg >= RKNPU_CORE_REG_COUNT))
		return 0;

	multicore_index = rknpu_multicore_reg_index(reg);
	if (multicore_index >= 0) {
		if (WARN_ON_ONCE(!rknpu_dev->multicore_base[core][multicore_index]))
			return 0;
		return readl(rknpu_dev->multicore_base[core][multicore_index]);
	}

	return readl(rknpu_dev->base[core] + rknpu_core_mmio_offsets[reg]);
}

void rknpu_core_write(struct rknpu_device *rknpu_dev, unsigned int core,
		      enum rknpu_core_mmio_reg reg, u32 value)
{
	int multicore_index;

	if (WARN_ON_ONCE(core >= RKNPU_MAX_CORES ||
			 reg >= RKNPU_CORE_REG_COUNT))
		return;

	multicore_index = rknpu_multicore_reg_index(reg);
	if (multicore_index >= 0) {
		if (WARN_ON_ONCE(!rknpu_dev->multicore_base[core][multicore_index]))
			return;
		writel(value, rknpu_dev->multicore_base[core][multicore_index]);
		return;
	}

	writel(value, rknpu_dev->base[core] + rknpu_core_mmio_offsets[reg]);
}

static bool rknpu_core_dynamic_offset_is_allowed(
	const struct rknpu_device *rknpu_dev, resource_size_t offset)
{
	const struct rknpu_amount_data *amount = rknpu_dev->config->amount_top;

	if (offset == rknpu_dev->config->pc_task_status_offset)
		return true;

	if (amount &&
	    (offset == amount->offset_clr_all || offset == amount->offset_dt_wr ||
	     offset == amount->offset_dt_rd || offset == amount->offset_wt_rd))
		return true;

	amount = rknpu_dev->config->amount_core;
	return amount &&
	       (offset == amount->offset_clr_all || offset == amount->offset_dt_wr ||
		offset == amount->offset_dt_rd || offset == amount->offset_wt_rd);
}

u32 rknpu_core_read_dynamic(struct rknpu_device *rknpu_dev, unsigned int core,
			    resource_size_t offset)
{
	if (WARN_ON_ONCE(core >= RKNPU_MAX_CORES ||
			 !rknpu_core_dynamic_offset_is_allowed(rknpu_dev, offset)))
		return 0;

	return readl(rknpu_dev->base[core] + offset);
}

void rknpu_core_write_dynamic(struct rknpu_device *rknpu_dev,
			      unsigned int core, resource_size_t offset, u32 value)
{
	if (WARN_ON_ONCE(core >= RKNPU_MAX_CORES ||
			 !rknpu_core_dynamic_offset_is_allowed(rknpu_dev, offset)))
		return;

	writel(value, rknpu_dev->base[core] + offset);
}

/* Block-level register readback for hardware diagnosis. */
u32 rknpu_core_read_block(struct rknpu_device *rknpu_dev, unsigned int core,
			  enum rknpu_core_block_reg reg)
{
	/* Map block registers to their corresponding MMIO resources or window offsets. */
	static const struct {
		enum { BLK_CNA, BLK_CORE, BLK_WIN } which;
		resource_size_t off; /* offset inside the block, or into win */
	} blocks[RKNPU_CORE_BLOCK_REG_COUNT] = {
		{ BLK_CNA, 0x0 },		 /* CNA_S_STATUS     */
		{ BLK_CNA, 0x4 },		 /* CNA_S_POINTER    */
		{ BLK_CNA, 0x8 },		 /* CNA_OP_EN        */
		{ BLK_CORE, 0x0 },		 /* CORE_S_STATUS    */
		{ BLK_CORE, 0x4 },		 /* CORE_S_POINTER   */
		{ BLK_CORE, 0x8 },		 /* CORE_OP_EN       */
		{ BLK_WIN, 0x4000 + 0x0 },	 /* DPU_S_STATUS     */
		{ BLK_WIN, 0x4000 + 0x4 },	 /* DPU_S_POINTER    */
		{ BLK_WIN, 0x4000 + 0x8 },	 /* DPU_OP_EN        */
		{ BLK_WIN, 0x4000 + 0x10 },	 /* DPU_DATA_FORMAT  */
		{ BLK_WIN, 0x400c },		 /* DPU_FEATURE_MODE */
		{ BLK_WIN, 0x4020 },		 /* DPU_DST_BASE     */
		{ BLK_WIN, 0x4084 },		 /* DPU_OUT_SCALE    */
		{ BLK_WIN, 0x5000 + 0x0 },	 /* DPU_RDMA_S_STATUS  */
		{ BLK_WIN, 0x5000 + 0x4 },	 /* DPU_RDMA_S_POINTER */
		{ BLK_WIN, 0x5000 + 0x8 },	 /* DPU_RDMA_OP_EN     */
		{ BLK_WIN, 0x6000 + 0x0 },	 /* PPU_S_STATUS     */
		{ BLK_WIN, 0x6000 + 0x4 },	 /* PPU_S_POINTER    */
		{ BLK_WIN, 0x6000 + 0x8 },	 /* PPU_OP_EN        */
		{ BLK_WIN, 0x7000 + 0x0 },	 /* PPU_RDMA_S_STATUS  */
		{ BLK_WIN, 0x7000 + 0x4 },	 /* PPU_RDMA_S_POINTER */
		{ BLK_WIN, 0x7000 + 0x8 },	 /* PPU_RDMA_OP_EN     */
		{ BLK_WIN, 0xf008 },		 /* GLOBAL_OP_EN     */
	};
	void __iomem *base;
	resource_size_t off;

	if (WARN_ON_ONCE(core >= RKNPU_MAX_CORES ||
			 reg >= RKNPU_CORE_BLOCK_REG_COUNT))
		return 0;

	off = blocks[reg].off;

	switch (blocks[reg].which) {
	case BLK_CNA:
		base = rknpu_dev->cores[core].cna;
		break;
	case BLK_CORE:
		base = rknpu_dev->cores[core].core_reg;
		break;
	default:
		base = rknpu_dev->cores[core].win;
		break;
	}
	if (!base)
		return 0;

	return readl(base + off);
}

/* Must match include/rknpu_ioctl.h RKNPU_CORE0/1/2_MASK semantics. */
static const resource_size_t rknpu_core_addrs[RKNPU_MAX_CORES] = {
	0xfdab0000,
	0xfdac0000,
	0xfdad0000,
};

int rknpu_core_index_by_addr(struct platform_device *pdev,
			     unsigned int *index)
{
	struct resource *res;
	unsigned int i;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENXIO;

	for (i = 0; i < RKNPU_MAX_CORES; i++) {
		if (res->start == rknpu_core_addrs[i]) {
			*index = i;
			return 0;
		}
	}

	dev_err(&pdev->dev,
		"unrecognized RKNN core address %pa; expected one of fdab0000/fdac0000/fdad0000\n",
		&res->start);
	return -ENODEV;
}

/*
 * Drop stale runtime-PM reference if left behind by an earlier module reload.
 * pm_runtime_put_noidle() is a no-op if usage count is already zero.
 */
static void rknpu_core_drop_stale_pm_ref(struct device *dev)
{
	pm_runtime_put_noidle(dev);
}

int rknpu_core_init(struct rknpu_device *rknpu_dev,
		    struct platform_device *pdev, unsigned int index)
{
	struct rknpu_core *core = &rknpu_dev->cores[index];
	struct device *dev = &pdev->dev;
	static const char * const clk_names[RKNPU_CORE_MAX_CLOCKS] = {
		"aclk", "hclk", "npu", "pclk",
	};
	static const char * const rst_names[RKNPU_CORE_MAX_RESETS] = {
		"srst_a", "srst_h",
	};
	int ret;
	int i;

	if (core->registered)
		return -EBUSY;

	core->pdev = pdev;
	core->dev = dev;
	core->index = index;
	rknpu_core_drop_stale_pm_ref(dev);

	/* Named register resources: pc, cna, core */
	core->pc = devm_platform_ioremap_resource_byname(pdev, "pc");
	if (IS_ERR(core->pc))
		return dev_err_probe(dev, PTR_ERR(core->pc),
				     "failed to map PC registers\n");

	core->cna = devm_platform_ioremap_resource_byname(pdev, "cna");
	if (IS_ERR(core->cna))
		return dev_err_probe(dev, PTR_ERR(core->cna),
				     "failed to map CNA registers\n");

	core->core_reg = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(core->core_reg))
		return dev_err_probe(dev, PTR_ERR(core->core_reg),
				      "failed to map CORE registers\n");

	/*
	 * Map the 64 KiB per-core register window for block-level register access.
	 * If mapping fails, core->win remains NULL and reads return 0.
	 */
	{
		struct resource *pc_res =
			platform_get_resource(pdev, IORESOURCE_MEM, 0);

		if (pc_res)
			core->win = devm_ioremap(dev, pc_res->start,
						 RKNPU_CORE_WINDOW_SIZE);
		if (!pc_res || !core->win)
			dev_info(dev,
				 "core %u: 64 KiB window not mapped; DPU/PPU/GLOBAL reads will report 0\n",
				 index);
	}

	/* Populate the legacy MMIO arrays used by the job layer. */
	rknpu_dev->base[index] = core->pc;
	rknpu_dev->multicore_base[index][0] = core->cna + 0x4;
	rknpu_dev->multicore_base[index][1] = core->core_reg + 0x4;

	/* Per-core clocks by name. */
	for (i = 0; i < RKNPU_CORE_MAX_CLOCKS; i++)
		core->clks[i].id = clk_names[i];

	ret = devm_clk_bulk_get(dev, RKNPU_CORE_MAX_CLOCKS, core->clks);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to get clocks for core %u\n",
				     index);
	core->num_clks = RKNPU_CORE_MAX_CLOCKS;

	/*
	 * Mainline genpd controls the NPU domain and PM clocks. The board DTS
	 * may also put supplies on each core node (for example npu-supply and
	 * sram-supply). Keep them explicit rather than assuming they are
	 * always-on on every carrier board.
	 */
	core->num_regulators =
		of_regulator_bulk_get_all(dev, dev->of_node, &core->regulators);
	if (core->num_regulators < 0) {
		ret = core->num_regulators;
		core->num_regulators = 0;
		core->regulators = NULL;
		return dev_err_probe(dev, ret,
				     "failed to get regulators for core %u\n",
				     index);
	}
	for (i = 0; i < core->num_regulators; i++)
		core->regulators[i].supply = devm_kasprintf(
			dev, GFP_KERNEL, "%s", core->regulators[i].supply ?: "supply");

	/* Per-core resets by name. */
	for (i = 0; i < RKNPU_CORE_MAX_RESETS; i++) {
		core->srsts[i] = devm_reset_control_get_exclusive_by_index(dev, i);
		if (IS_ERR(core->srsts[i])) {
			return dev_err_probe(dev, PTR_ERR(core->srsts[i]),
					     "failed to get reset %s for core %u\n",
					     rst_names[i], index);
		}
	}
	core->num_srsts = RKNPU_CORE_MAX_RESETS;

	/* Per-core IRQ: handler index equals core index. */
	core->irq = platform_get_irq(pdev, 0);
	if (core->irq < 0)
		return core->irq;

	{
		irqreturn_t (*handler)(int, void *) = NULL;

		switch (index) {
		case 0:
			handler = rknpu_core0_irq_handler;
			break;
		case 1:
			handler = rknpu_core1_irq_handler;
			break;
		case 2:
			handler = rknpu_core2_irq_handler;
			break;
		default:
			return -EINVAL;
		}

		ret = devm_request_irq(dev, core->irq, handler, IRQF_SHARED,
				       dev_name(dev), rknpu_dev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request IRQ for core %u\n",
					     index);

		/* Direct IRQ affinity to high performance cores (CPU4..7) */
		if (num_possible_cpus() >= 8) {
			unsigned int target_cpu = 4 + (index % 4);

			if (cpu_online(target_cpu)) {
				ret = irq_set_affinity(core->irq, cpumask_of(target_cpu));
				if (!ret)
					dev_info(dev, "core %u IRQ %d bound to CPU%u (A76)\n",
						 index, core->irq, target_cpu);
			}
		}
	}

	/* IOMMU group (optional in principle, required for translated DMA). */
	core->group = iommu_group_get(dev);
	if (!core->group)
		dev_info(dev, "core %u has no IOMMU group\n", index);
	if (index == 0)
		rknpu_dev->iommu_en = !!core->group;
	else if (!!core->group != rknpu_dev->iommu_en)
		return dev_err_probe(dev, -EINVAL,
				     "inconsistent IOMMU state across cores\n");

	if (!dev->pm_domain)
		dev_warn(dev,
			 "core %u has no attached PM domain; hardware may be powered down\n",
			 index);

	/*
	 * Publish drvdata before touching runtime PM: pm_runtime_use_autosuspend()
	 * / pm_runtime_set_autosuspend_delay() can synchronously run
	 * rknpu_runtime_suspend() if runtime PM is already enabled, and that
	 * callback returns -ENODEV while drvdata is still NULL, latching
	 * dev->power.runtime_error. rknpu_probe() sets it again later (harmless).
	 */
	dev_set_drvdata(dev, rknpu_dev);

	/*
	 * Start from a known runtime-PM state. A previously bound driver can
	 * leave it dirty: in-tree rocket's rocket_remove() fails to find the last
	 * core after cores 0/1 were unbound (find_core_for_dev() scans only
	 * cores[0..num_cores-1]), so rocket_core_fini() is skipped and runtime
	 * PM stays enabled on fdad0000.npu ("Unbalanced pm_runtime_enable!").
	 * A latched runtime_error survives unbind/rebind and makes every
	 * pm_runtime_resume_and_get() - hence every RKNPU ioctl - fail -EINVAL.
	 */
	if (pm_runtime_enabled(dev)) {
		dev_warn(dev, "core %u: runtime PM left enabled by a previous driver, resetting\n",
			 index);
		pm_runtime_disable(dev);
	}
	if (dev->power.runtime_error)
		dev_warn(dev, "core %u: clearing stale runtime PM error %d\n",
			 index, dev->power.runtime_error);
	pm_runtime_set_suspended(dev);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 50);
	pm_runtime_enable(dev);

	core->registered = true;
	return 0;
}

/* Release regulator allocations acquired by of_regulator_bulk_get_all(). */
void rknpu_core_release_regulators(struct rknpu_core *core)
{
	if (!core->regulators)
		return;

	regulator_bulk_free(core->num_regulators, core->regulators);
	kfree(core->regulators);
	core->regulators = NULL;
	core->num_regulators = 0;
}

void rknpu_core_fini(struct rknpu_device *rknpu_dev, unsigned int index)
{
	struct rknpu_core *core = &rknpu_dev->cores[index];

	if (!core->registered)
		return;

	pm_runtime_dont_use_autosuspend(core->dev);
	pm_runtime_disable(core->dev);

	rknpu_iommu_detach_core(rknpu_dev, index);

	if (core->group) {
		iommu_group_put(core->group);
		core->group = NULL;
	}

	/* Before the memset() below wipes core->regulators. */
	rknpu_core_release_regulators(core);

	rknpu_dev->base[index] = NULL;
	rknpu_dev->multicore_base[index][0] = NULL;
	rknpu_dev->multicore_base[index][1] = NULL;

	memset(core, 0, sizeof(*core));
}
