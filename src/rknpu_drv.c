// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/overflow.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/spinlock.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/sizes.h>

#include "rknpu_ioctl.h"
#include "rknpu_reset.h"
#include "rknpu_fence.h"
#include "rknpu_drv.h"
#include "rknpu_core.h"
#include "rknpu_gem.h"
#include "rknpu_devfreq.h"
#include "rknpu_iommu.h"
#include "rknpu_job.h"

#include <drm/drm_device.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>
#include "rknpu_gem.h"


#define POWER_DOWN_FREQ 200000000


static int bypass_soft_reset;
module_param(bypass_soft_reset, int, 0644);
MODULE_PARM_DESC(bypass_soft_reset,
		 "bypass RKNPU soft reset if set it to 1, disabled by default");

bool rknpu_debug_log;
module_param(rknpu_debug_log, bool, 0644);
MODULE_PARM_DESC(rknpu_debug_log,
		 "enable verbose per-submit/per-IRQ/per-buffer debug logging, disabled by default");

static bool rknpu_per_fd_domain = true;
module_param_named(per_fd_domain, rknpu_per_fd_domain, bool, 0644);
MODULE_PARM_DESC(per_fd_domain,
		 "enable per-FD IOMMU domain isolation for client processes (default: true)");

static DEFINE_MUTEX(rknpu_global_lock);

struct rknpu_facade {
	struct rknpu_device rknpu_dev;
	struct rknpu_core cores_storage[RKNPU_MAX_CORES];
	struct kref refcount;
};

static struct rknpu_facade *rknpu_global;

static void rknpu_core_mask_to_string(unsigned int mask, char *buf,
				      size_t size)
{
	int i;
	int off = 0;

	buf[0] = '\0';
	for (i = 0; i < RKNPU_MAX_CORES; i++) {
		if (!(mask & BIT(i)))
			continue;
		off += snprintf(buf + off, size - off, "%s%d",
				off ? "," : "", i);
		if (off >= size - 1)
			break;
	}
}

static const struct rknpu_irqs_data rknpu_irqs[] = {
	{ "npu_irq", rknpu_core0_irq_handler }
};

static const struct rknpu_irqs_data rk3576_npu_irqs[] = {
	{ "npu0_irq", rknpu_core0_irq_handler },
	{ "npu1_irq", rknpu_core1_irq_handler }
};

static const struct rknpu_irqs_data rk3588_npu_irqs[] = {
	{ "npu0_irq", rknpu_core0_irq_handler },
	{ "npu1_irq", rknpu_core1_irq_handler },
	{ "npu2_irq", rknpu_core2_irq_handler }
};

static const struct rknpu_amount_data rknpu_old_top_amount = {
	.offset_clr_all = 0x8010,
	.offset_dt_wr = 0x8034,
	.offset_dt_rd = 0x8038,
	.offset_wt_rd = 0x803c,
};

static const struct rknpu_amount_data rknpu_top_amount = {
	.offset_clr_all = 0x2210,
	.offset_dt_wr = 0x2234,
	.offset_dt_rd = 0x2238,
	.offset_wt_rd = 0x223c
};

static const struct rknpu_amount_data rknpu_core_amount = {
	.offset_clr_all = 0x2410,
	.offset_dt_wr = 0x2434,
	.offset_dt_rd = 0x2438,
	.offset_wt_rd = 0x243c,
};

static void rk3576_state_init(struct rknpu_device *rknpu_dev)
{
	void __iomem *rknpu_core_base = rknpu_dev->base[0];

	writel(0x1, rknpu_core_base + 0x10);
	writel(0, rknpu_core_base + 0x1004);
	writel(0x80000000, rknpu_core_base + 0x1024);
	writel(1, rknpu_core_base + 0x1004);
	writel(0x80000000, rknpu_core_base + 0x1024);
	writel(0x1e, rknpu_core_base + 0x1004);
}

static int rk3576_cache_sgt_init(struct rknpu_device *rknpu_dev)
{
	struct sg_table *sgt = NULL;
	struct scatterlist *sgl = NULL;
	uint64_t block_size_kb[4] = { 448, 64, 448, 64 };
	uint64_t block_offset_kb[4] = { 0, 896, 448, 960 };
	int core_num = rknpu_dev->config->num_irqs;
	int ret = 0, i = 0, j = 0;

	for (i = 0; i < core_num; i++) {
		sgt = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (!sgt)
			goto out_free_table;
		ret = sg_alloc_table(sgt, core_num, GFP_KERNEL);
		if (ret) {
			kfree(sgt);
			goto out_free_table;
		}
		rknpu_dev->cache_sgt[i] = sgt;
		for_each_sgtable_sg(sgt, sgl, j) {
			sg_set_page(sgl, NULL,
				    block_size_kb[i * core_num + j] * 1024,
				    block_offset_kb[i * core_num + j] * 1024);
		}
	}
	return 0;

out_free_table:
	for (i = 0; i < core_num; i++) {
		if (rknpu_dev->cache_sgt[i]) {
			sg_free_table(rknpu_dev->cache_sgt[i]);
			kfree(rknpu_dev->cache_sgt[i]);
			rknpu_dev->cache_sgt[i] = NULL;
		}
	}

	return ret;
}

static const struct rknpu_config rk356x_rknpu_config = {
	.bw_priority_addr = 0xfe180008,
	.bw_priority_length = 0x10,
	.dma_mask = DMA_BIT_MASK(32),
	.pc_data_amount_scale = 1,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.pc_dma_ctrl = 0,
	.irqs = rknpu_irqs,
	.num_irqs = ARRAY_SIZE(rknpu_irqs),
	.nbuf_phyaddr = 0,
	.nbuf_size = 0,
	.max_submit_number = (1 << 12) - 1,
	.core_mask = 0x1,
	.amount_top = &rknpu_old_top_amount,
	.amount_core = NULL,
	.state_init = NULL,
	.cache_sgt_init = NULL,
};

static const struct rknpu_config rk3588_rknpu_config = {
	.bw_priority_addr = 0x0,
	.bw_priority_length = 0x0,
	.dma_mask = DMA_BIT_MASK(40),
	.pc_data_amount_scale = 2,
	.pc_task_number_bits = 12,
	.pc_task_number_mask = 0xfff,
	.pc_task_status_offset = 0x3c,
	.pc_dma_ctrl = 0,
	.irqs = rk3588_npu_irqs,
	.num_irqs = ARRAY_SIZE(rk3588_npu_irqs),
	.nbuf_phyaddr = 0,
	.nbuf_size = 0,
	.max_submit_number = (1 << 12) - 1,
	.core_mask = 0x7,
	.amount_top = NULL,
	.amount_core = NULL,
	.state_init = NULL,
	.cache_sgt_init = NULL,
};

static const struct rknpu_config rk3576_rknpu_config = {
	.bw_priority_addr = 0x0,
	.bw_priority_length = 0x0,
	.dma_mask = DMA_BIT_MASK(40),
	.pc_data_amount_scale = 2,
	.pc_task_number_bits = 16,
	.pc_task_number_mask = 0xffff,
	.pc_task_status_offset = 0x48,
	.pc_dma_ctrl = 1,
	.irqs = rk3576_npu_irqs,
	.num_irqs = ARRAY_SIZE(rk3576_npu_irqs),
	.nbuf_phyaddr = 0x3fe80000,
	.nbuf_size = 1024 * 1024,
	.max_submit_number = (1 << 16) - 1,
	.core_mask = 0x3,
	.amount_top = &rknpu_top_amount,
	.amount_core = &rknpu_core_amount,
	.state_init = rk3576_state_init,
	.cache_sgt_init = rk3576_cache_sgt_init,
};

/* driver probe and init */
static const struct of_device_id rknpu_of_match[] = {
	{
		.compatible = "rockchip,rk3568-rknn-core",
		.data = &rk356x_rknpu_config,
	},
	{
		.compatible = "rockchip,rk3576-rknn-core",
		.data = &rk3576_rknpu_config,
	},
	{
		.compatible = "rockchip,rk3588-rknn-core",
		.data = &rk3588_rknpu_config,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rknpu_of_match);

static int rknpu_get_drv_version(uint32_t *version)
{
	*version = RKNPU_GET_DRV_VERSION_CODE(DRIVER_MAJOR, DRIVER_MINOR,
					      DRIVER_PATCHLEVEL);
	return 0;
}



static int rknpu_validate_domain_id(struct device *dev, int domain_id)
{
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM) {
		dev_warn_ratelimited(
			dev,
			"RKNPU: IOMMU domain %d is out of range (0..%d)\n",
			domain_id, RKNPU_MAX_IOMMU_DOMAIN_NUM - 1);
		return -EINVAL;
	}

	/*
	 * Accepted and ignored, exactly like rknpu_iommu_domain_get_and_switch():
	 * this driver has one shared IOVA allocator rather than one address
	 * space per domain, so the id carries no addressing meaning here.
	 */
	return 0;
}

static int rknpu_action(struct rknpu_device *rknpu_dev,
			struct rknpu_action *args,
			struct drm_file *file_priv)
{
	int ret = -EINVAL;

	/* Dispatch action table */
	switch (args->flags) {
	case RKNPU_GET_HW_VERSION:
		ret = rknpu_get_hw_version(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_DRV_VERSION:
		ret = rknpu_get_drv_version(&args->value);
		break;
	case RKNPU_GET_FREQ:
		{
			struct clk *npu_clk = rknpu_get_npu_clk(rknpu_dev);
			if (!npu_clk) {
				ret = -ENODEV;
				break;
			}
			args->value = clk_get_rate(npu_clk);
			ret = 0;
		}
		break;
	case RKNPU_SET_FREQ:
		{
			ret = rknpu_opp_set_freq(rknpu_dev, args->value);
			if (!ret)
				rknpu_dev->ondemand_freq = rknpu_dev->current_freq;
		}
		break;
	case RKNPU_GET_VOLT:
		ret = rknpu_get_volt(rknpu_dev, &args->value);
		if (ret)
			break;
		ret = 0;
		break;
	case RKNPU_SET_VOLT:
		break;
	case RKNPU_ACT_RESET:
		ret = rknpu_soft_reset(rknpu_dev);
		break;
	case RKNPU_GET_BW_PRIORITY:
		ret = rknpu_get_bw_priority(rknpu_dev, &args->value, NULL,
					    NULL);
		break;
	case RKNPU_SET_BW_PRIORITY:
		ret = rknpu_set_bw_priority(rknpu_dev, args->value, 0, 0);
		break;
	case RKNPU_GET_BW_EXPECT:
		ret = rknpu_get_bw_priority(rknpu_dev, NULL, &args->value,
					    NULL);
		break;
	case RKNPU_SET_BW_EXPECT:
		ret = rknpu_set_bw_priority(rknpu_dev, 0, args->value, 0);
		break;
	case RKNPU_GET_BW_TW:
		ret = rknpu_get_bw_priority(rknpu_dev, NULL, NULL,
					    &args->value);
		break;
	case RKNPU_SET_BW_TW:
		ret = rknpu_set_bw_priority(rknpu_dev, 0, 0, args->value);
		break;
	case RKNPU_ACT_CLR_TOTAL_RW_AMOUNT:
		ret = rknpu_clear_rw_amount(rknpu_dev);
		break;
	case RKNPU_GET_DT_WR_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, &args->value, NULL, NULL);
		break;
	case RKNPU_GET_DT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, &args->value, NULL);
		break;
	case RKNPU_GET_WT_RD_AMOUNT:
		ret = rknpu_get_rw_amount(rknpu_dev, NULL, NULL, &args->value);
		break;
	case RKNPU_GET_TOTAL_RW_AMOUNT:
		ret = rknpu_get_total_rw_amount(rknpu_dev, &args->value);
		break;
	case RKNPU_GET_IOMMU_EN:
		args->value = rknpu_dev->iommu_en;
		ret = 0;
		break;
	case RKNPU_SET_PROC_NICE:
		set_user_nice(current, *(int32_t *)&args->value);
		ret = 0;
		break;
	case RKNPU_GET_TOTAL_SRAM_SIZE:
		if (rknpu_dev->sram_mm)
			args->value = rknpu_dev->sram_mm->total_chunks *
				      rknpu_dev->sram_mm->chunk_size;
		else
			args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_FREE_SRAM_SIZE:
		if (rknpu_dev->sram_mm)
			args->value = rknpu_dev->sram_mm->free_chunks *
				      rknpu_dev->sram_mm->chunk_size;
		else
			args->value = 0;
		ret = 0;
		break;
	case RKNPU_GET_IOMMU_DOMAIN_ID:
		if (file_priv && file_priv->driver_priv)
			args->value = ((struct rknpu_file_priv *)file_priv->driver_priv)->domain_id;
		else
			args->value = 0;
		ret = 0;
		break;
	case RKNPU_SET_IOMMU_DOMAIN_ID:
		ret = rknpu_validate_domain_id(rknpu_dev->dev,
					       *(int32_t *)&args->value);
		if (!ret && file_priv && file_priv->driver_priv)
			((struct rknpu_file_priv *)file_priv->driver_priv)->domain_id = *(int32_t *)&args->value;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int rknpu_action_ioctl(struct drm_device *dev, void *data,
			      struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;

	return rknpu_action(rknpu_dev, (struct rknpu_action *)data, file_priv);
}

#define RKNPU_IOCTL(func)                                       \
	static int __##func(struct drm_device *dev, void *data,     \
			    struct drm_file *file_priv)                     \
	{                                                           \
		struct rknpu_device *rknpu_dev = dev->dev_private;      \
		int idx;                                                \
		int ret;                                                \
		if (!drm_dev_enter(dev, &idx))                          \
			return -ENODEV;                                     \
		if (READ_ONCE(rknpu_dev->shutting_down)) {              \
			drm_dev_exit(idx);                                  \
			return -ENODEV;                                     \
		}                                                       \
		ret = rknpu_power_get(rknpu_dev);                       \
		if (!ret) {                                             \
			ret = func(dev, data, file_priv);                   \
			rknpu_power_put_delay(rknpu_dev);                   \
		}                                                       \
		drm_dev_exit(idx);                                      \
		return ret;                                             \
	}

RKNPU_IOCTL(rknpu_action_ioctl);
RKNPU_IOCTL(rknpu_submit_ioctl);
RKNPU_IOCTL(rknpu_gem_create_ioctl);
RKNPU_IOCTL(rknpu_gem_map_ioctl);
RKNPU_IOCTL(rknpu_gem_destroy_ioctl);
RKNPU_IOCTL(rknpu_gem_sync_ioctl);

static const struct drm_ioctl_desc rknpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(RKNPU_ACTION, __rknpu_action_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT, __rknpu_submit_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_CREATE, __rknpu_gem_create_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_MAP, __rknpu_gem_map_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_DESTROY, __rknpu_gem_destroy_ioctl,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_SYNC, __rknpu_gem_sync_ioctl,
			  DRM_RENDER_ALLOW),
};

static int rknpu_open(struct drm_device *dev, struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;
	struct rknpu_file_priv *fpriv;
	int domain_id = 0;

	fpriv = kzalloc(sizeof(*fpriv), GFP_KERNEL);
	if (!fpriv)
		return -ENOMEM;

	fpriv->file_priv = file_priv;

	if (rknpu_per_fd_domain && rknpu_dev->iommu_en) {
		domain_id = ida_alloc_range(&rknpu_dev->domain_ida,
					    RKNPU_PER_FD_DOMAIN_START,
					    RKNPU_MAX_IOMMU_DOMAIN_NUM - 1,
					    GFP_KERNEL);
		if (domain_id < 0) {
			LOG_DEV_DBG(dev->dev,
				    "dynamic IOMMU domains exhausted, falling back to domain 0\n");
			domain_id = 0;
		} else {
			LOG_DEV_DBG(dev->dev,
				    "client opened, assigned isolated domain %d\n",
				    domain_id);
		}
	}

	fpriv->domain_id = domain_id;
	file_priv->driver_priv = fpriv;

	return 0;
}

static void rknpu_postclose(struct drm_device *dev, struct drm_file *file_priv)
{
	struct rknpu_device *rknpu_dev = dev->dev_private;
	struct rknpu_file_priv *fpriv = file_priv->driver_priv;

	if (fpriv) {
		if (fpriv->domain_id >= RKNPU_PER_FD_DOMAIN_START) {
			ida_free(&rknpu_dev->domain_ida, fpriv->domain_id);
			LOG_DEV_DBG(dev->dev,
				    "client closed, released domain %d\n",
				    fpriv->domain_id);
		}
		kfree(fpriv);
		file_priv->driver_priv = NULL;
	}
}

DEFINE_DRM_GEM_FOPS(rknpu_drm_driver_fops);

static struct drm_driver rknpu_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.open = rknpu_open,
	.postclose = rknpu_postclose,
	.dumb_create = rknpu_gem_dumb_create,
	.dumb_map_offset = drm_gem_dumb_map_offset,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = rknpu_gem_prime_import,
	.gem_prime_import_sg_table = rknpu_gem_prime_import_sg_table,
#if KERNEL_VERSION(6, 6, 0) > LINUX_VERSION_CODE
	.gem_prime_mmap = drm_gem_prime_mmap,
#endif
	.ioctls = rknpu_ioctls,
	.num_ioctls = ARRAY_SIZE(rknpu_ioctls),
	.fops = &rknpu_drm_driver_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static enum hrtimer_restart hrtimer_handler(struct hrtimer *timer)
{
	struct rknpu_device *rknpu_dev =
		container_of(timer, struct rknpu_device, timer);
	struct rknpu_subcore_data *subcore_data = NULL;
	struct rknpu_job *job = NULL;
	ktime_t now;
	unsigned long flags;
	int i;

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		subcore_data = &rknpu_dev->subcore_datas[i];

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

		job = subcore_data->job;
		if (job) {
			now = ktime_get();
			subcore_data->timer.busy_time +=
				ktime_sub(now, job->hw_recoder_time[i]);
			job->hw_recoder_time[i] = now;
		}

		subcore_data->timer.total_busy_time =
			subcore_data->timer.busy_time;
		subcore_data->timer.busy_time = 0;

		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
	}

	hrtimer_forward_now(timer, rknpu_dev->kt);
	return HRTIMER_RESTART;
}

static void rknpu_init_timer(struct rknpu_device *rknpu_dev)
{
	rknpu_dev->kt = ktime_set(0, RKNPU_LOAD_INTERVAL);
	hrtimer_setup(&rknpu_dev->timer, hrtimer_handler, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrtimer_start(&rknpu_dev->timer, rknpu_dev->kt, HRTIMER_MODE_REL);
}

static void rknpu_cancel_timer(struct rknpu_device *rknpu_dev)
{
	hrtimer_cancel(&rknpu_dev->timer);
}

static int rknpu_root_device_register(struct rknpu_device *rknpu_dev)
{
	struct device *root;
	int ret;

	if (rknpu_dev->dev)
		return 0;

	root = root_device_register("rknpu");
	if (IS_ERR(root))
		return PTR_ERR(root);

	ret = dma_coerce_mask_and_coherent(root,
					   rknpu_dev->config->dma_mask);
	if (ret) {
		root_device_unregister(root);
		return ret;
	}

	rknpu_dev->dev = root;
	return 0;
}

static void rknpu_root_device_unregister(struct rknpu_device *rknpu_dev)
{
	struct device *root = rknpu_dev->dev;

	if (!root)
		return;

	rknpu_dev->dev = NULL;
	root_device_unregister(root);
}

static int rknpu_drm_probe(struct rknpu_device *rknpu_dev)
{
	struct drm_device *drm_dev = NULL;
	int ret;

	ret = rknpu_root_device_register(rknpu_dev);
	if (ret)
		return ret;

	drm_dev = drm_dev_alloc(&rknpu_drm_driver, rknpu_dev->dev);
	if (IS_ERR(drm_dev)) {
		ret = PTR_ERR(drm_dev);
		goto err_unregister_root;
	}

	drm_dev->dev_private = rknpu_dev;
	rknpu_dev->drm_dev = drm_dev;

	/* Expose the render node only after every other probe stage succeeded. */
	ret = drm_dev_register(drm_dev, 0);
	if (ret < 0)
		goto err_put_drm;

	return 0;

err_put_drm:
	rknpu_dev->drm_dev = NULL;
	drm_dev_put(drm_dev);
err_unregister_root:
	rknpu_root_device_unregister(rknpu_dev);
	return ret;
}


static void rknpu_drm_unplug(struct rknpu_device *rknpu_dev)
{
	struct drm_device *drm_dev = rknpu_dev->drm_dev;

	if (drm_dev)
		drm_dev_unplug(drm_dev);
}

static void rknpu_drm_remove(struct rknpu_device *rknpu_dev)
{
	struct drm_device *drm_dev = rknpu_dev->drm_dev;

	if (!drm_dev)
		return;

	rknpu_dev->drm_dev = NULL;
	rknpu_root_device_unregister(rknpu_dev);

	drm_dev_put(drm_dev);
}



static void rknpu_power_off(struct rknpu_device *rknpu_dev);
static void rknpu_core_release_power(struct rknpu_device *rknpu_dev,
				     unsigned int index);

static void rknpu_power_off_delay_work(struct work_struct *work)
{
	struct rknpu_device *rknpu_dev =
		container_of(to_delayed_work(work), struct rknpu_device,
			     power_off_work);

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0)
		rknpu_power_off(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);
}

int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
	struct device *core_dev;
	unsigned int i;
	int ret = 0;

	mutex_lock(&rknpu_dev->power_lock);

	if (atomic_inc_return(&rknpu_dev->power_refcount) != 1) {
		mutex_unlock(&rknpu_dev->power_lock);
		return 0;
	}

	for (i = 0; i < RKNPU_MAX_CORES; i++) {
		core_dev = rknpu_dev->cores[i].dev;
		if (!core_dev)
			continue;

		ret = regulator_bulk_enable(rknpu_dev->cores[i].num_regulators,
					    rknpu_dev->cores[i].regulators);
		if (ret)
			goto err_put_cores;

		ret = pm_runtime_resume_and_get(core_dev);
		if (ret) {
			/*
			 * This core's supplies were enabled one line above but
			 * the loop below only rolls back cores < i, so undo
			 * them here. (The BSP code left this enable behind.)
			 */
			regulator_bulk_disable(rknpu_dev->cores[i].num_regulators,
					       rknpu_dev->cores[i].regulators);
			goto err_put_cores;
		}

		/*
		 * Record the ownership on the core itself, so the matching put
		 * can still be performed later even after other cores have
		 * already been torn down and cleared (see
		 * rknpu_core_release_power()).
		 */
		rknpu_dev->cores[i].power_held = true;
	}

	rknpu_devfreq_runtime_resume(rknpu_dev->cores[0].dev);

	mutex_unlock(&rknpu_dev->power_lock);
	return 0;

err_put_cores:
	while (i--) {
		/*
		 * Roll back through the same primitive the teardown uses, so
		 * the autosuspend handling and the power_held ownership check
		 * cannot drift apart between the two paths.
		 */
		rknpu_core_release_power(rknpu_dev, i);
	}

	atomic_dec(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);
	return ret;
}

int rknpu_power_put(struct rknpu_device *rknpu_dev)
{
	/*
	 * Preserve the BSP 0.9.8 ownership semantics: an unbalanced put must
	 * never power the NPU down while another user still holds it. Powering
	 * every core down unconditionally underflows runtime-PM/regulator
	 * references as soon as debugfs readers get/put a second time.
	 */
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0)
		rknpu_power_off(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);

	return 0;
}

int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
	if (READ_ONCE(rknpu_dev->shutting_down))
		return 0;

	if (rknpu_dev->power_put_delay == 0)
		return rknpu_power_put(rknpu_dev);

	mutex_lock(&rknpu_dev->power_lock);
	if (READ_ONCE(rknpu_dev->shutting_down)) {
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
		mutex_unlock(&rknpu_dev->power_lock);
		return 0;
	}
	if (atomic_read(&rknpu_dev->power_refcount) == 1)
		queue_delayed_work(rknpu_dev->power_off_wq,
				   &rknpu_dev->power_off_work,
				   msecs_to_jiffies(rknpu_dev->power_put_delay));
	else
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);

	return 0;
}

/*
 * Release runtime-PM and supplies for one core during device removal.
 */
static void rknpu_core_release_power(struct rknpu_device *rknpu_dev,
				     unsigned int index)
{
	struct rknpu_core *core = &rknpu_dev->cores[index];

	if (!core->dev)
		return;

	/* Only release power if previously acquired for this core. */
	if (core->power_held) {
		/* Disable autosuspend so runtime suspend happens synchronously. */
		pm_runtime_dont_use_autosuspend(core->dev);
		pm_runtime_put_sync(core->dev);

		/* Balance regulator enable count. */
		regulator_bulk_disable(core->num_regulators, core->regulators);
		core->power_held = false;
	}
}

static void rknpu_power_off(struct rknpu_device *rknpu_dev)
{
	unsigned int i;

	rknpu_devfreq_runtime_suspend(rknpu_dev->cores[0].dev);

	for (i = 0; i < RKNPU_MAX_CORES; i++)
		rknpu_core_release_power(rknpu_dev, i);
}

static void rknpu_facade_release(struct kref *ref)
{
	struct rknpu_facade *facade =
		container_of(ref, struct rknpu_facade, refcount);

	WARN_ON(facade->rknpu_dev.available_cores);
	ida_destroy(&facade->rknpu_dev.domain_ida);
	rknpu_global = NULL;
	kfree(facade);
}

static int rknpu_probe(struct platform_device *pdev)
{
	struct rknpu_device *rknpu_dev = NULL;
	struct device *dev = &pdev->dev;
	const struct rknpu_config *config = NULL;
	struct rknpu_facade *facade = NULL;
	unsigned int index;
	unsigned int prev_available;
	bool created = false;
	bool drm_registered = false;
	int ret;

	if (!pdev->dev.of_node) {
		dev_err(dev, "rknpu device-tree data is missing!\n");
		return -ENODEV;
	}

	config = of_device_get_match_data(dev);
	if (!config)
		return -EINVAL;

	ret = rknpu_core_index_by_addr(pdev, &index);
	if (ret)
		return ret;

	mutex_lock(&rknpu_global_lock);

	if (!rknpu_global) {
		facade = kzalloc(sizeof(*facade), GFP_KERNEL);
		if (!facade) {
			mutex_unlock(&rknpu_global_lock);
			return -ENOMEM;
		}

		kref_init(&facade->refcount);
		rknpu_dev = &facade->rknpu_dev;
		rknpu_dev->cores = facade->cores_storage;

		xa_init(&rknpu_dev->gem_dma_xa);
		for (int i = 0; i < RKNPU_MEM_STAT_COUNT; i++)
			atomic64_set(&rknpu_dev->mem_stats[i], 0);
		ida_init(&rknpu_dev->domain_ida);

		rknpu_dev->config = config;
		/* The facade owner is created lazily as a module root device. */
		rknpu_dev->bypass_soft_reset = bypass_soft_reset;

		spin_lock_init(&rknpu_dev->lock);
		spin_lock_init(&rknpu_dev->irq_lock);
		mutex_init(&rknpu_dev->power_lock);
		mutex_init(&rknpu_dev->reset_lock);
		INIT_LIST_HEAD(&rknpu_dev->jobs);
		init_waitqueue_head(&rknpu_dev->commit_wq);
		for (int i = 0; i < RKNPU_MAX_CORES; i++) {
			INIT_LIST_HEAD(&rknpu_dev->subcore_datas[i].todo_list);
			init_waitqueue_head(&rknpu_dev->subcore_datas[i].job_done_wq);
			rknpu_dev->subcore_datas[i].task_num = 0;
		}

		/* Per-core regulators are handled by each core's platform device. */
		rknpu_global = facade;
		created = true;
	} else {
		rknpu_dev = &rknpu_global->rknpu_dev;
	}

	prev_available = rknpu_dev->available_cores;


	ret = rknpu_core_init(rknpu_dev, pdev, index);
	if (ret) {
		/*
		 * rknpu_core_init() may have failed after taking the per-core
		 * regulator handles (they are the only non-devres acquisition
		 * it makes). ->registered is still false, so undo exactly those
		 * and nothing else - a probe that fails half-way must not leak
		 * (cores * supplies) handles that no later path can release.
		 */
		rknpu_core_release_regulators(&rknpu_dev->cores[index]);
		if (created)
			kfree(rknpu_global);
		rknpu_global = NULL;
		mutex_unlock(&rknpu_global_lock);
		return ret;
	}

	kref_get(&rknpu_global->refcount);
	rknpu_dev->available_cores |= BIT(index);

	if (rknpu_dev->available_cores == (BIT(RKNPU_MAX_CORES) - 1)) {
		ret = rknpu_iommu_init_domain(rknpu_dev);
		if (ret) {
			rknpu_core_fini(rknpu_dev, index);
			rknpu_dev->available_cores = prev_available;
			kref_put(&rknpu_global->refcount,
				 rknpu_facade_release);
			if (created) {
				rknpu_global = NULL;
				kfree(facade);
			}
			mutex_unlock(&rknpu_global_lock);
			return ret;
		}
	}

	dev_set_drvdata(dev, rknpu_dev);

	{
		char core_list[16];
		rknpu_core_mask_to_string(rknpu_dev->available_cores,
					  core_list, sizeof(core_list));
		dev_info(dev, "RKNPU core %u registered, available cores: [%s]\n",
			 index, core_list);
	}

	/* Register facade/DRM only after all three cores are available. */
	if (rknpu_dev->available_cores == (BIT(RKNPU_MAX_CORES) - 1) &&
	    !rknpu_dev->drm_dev) {
		ret = rknpu_drm_probe(rknpu_dev);
		if (ret) {
			dev_err(dev, "failed to register RKNPU DRM device\n");
			goto err_core_fini;
		}
		drm_registered = true;

		ret = rknpu_fence_context_alloc(rknpu_dev);
		if (ret) {
			dev_err(dev, "failed to allocate fence context\n");
			goto err_drm_remove;
		}

		atomic_set(&rknpu_dev->power_refcount, 0);
		atomic_set(&rknpu_dev->cmdline_power_refcount, 0);
		rknpu_dev->power_put_delay = 3000;
		rknpu_dev->power_off_wq =
			create_freezable_workqueue("rknpu_power_off_wq");
		if (!rknpu_dev->power_off_wq) {
			ret = -ENOMEM;
			goto err_drm_remove;
		}
		INIT_DEFERRABLE_WORK(&rknpu_dev->power_off_work,
				     rknpu_power_off_delay_work);

		rknpu_dev->job_wq = alloc_ordered_workqueue("rknpu_job_wq", 0);
		if (!rknpu_dev->job_wq) {
			ret = -ENOMEM;
			goto err_remove_power_wq;
		}

		ret = rknpu_debugger_init(rknpu_dev);
		if (ret)
			goto err_remove_job_wq;

		rknpu_init_timer(rknpu_dev);
		rknpu_devfreq_init(rknpu_dev);
		WRITE_ONCE(rknpu_dev->facade_ready, true);
	}

	mutex_unlock(&rknpu_global_lock);
	return 0;

err_remove_job_wq:
	destroy_workqueue(rknpu_dev->job_wq);
err_remove_power_wq:
	destroy_workqueue(rknpu_dev->power_off_wq);
	rknpu_fence_context_free(rknpu_dev);
err_drm_remove:
	if (drm_registered)
		rknpu_drm_remove(rknpu_dev);
err_core_fini:
	rknpu_core_fini(rknpu_dev, index);
	rknpu_dev->available_cores = prev_available;
	if (created)
		rknpu_global = NULL;
	mutex_unlock(&rknpu_global_lock);
	return ret;
}

static void rknpu_remove(struct platform_device *pdev)
{
	struct rknpu_device *rknpu_dev = platform_get_drvdata(pdev);
	struct rknpu_facade *facade;
	unsigned int index;
	int ret;

	ret = rknpu_core_index_by_addr(pdev, &index);
	if (ret)
		return;

	mutex_lock(&rknpu_global_lock);

	facade = rknpu_global;
	rknpu_dev = facade ? &facade->rknpu_dev : NULL;
	if (!rknpu_dev || !facade || !rknpu_dev->cores[index].registered) {
		mutex_unlock(&rknpu_global_lock);
		return;
	}

	rknpu_dev->available_cores &= ~BIT(index);

	if (rknpu_dev->available_cores == 0) {
		int i;

		WRITE_ONCE(rknpu_dev->shutting_down, true);
		for (i = 0; i < RKNPU_MAX_CORES; i++)
			wake_up_all(&rknpu_dev->iommu_active_wq[i]);

		if (READ_ONCE(rknpu_dev->facade_ready)) {
			rknpu_drm_unplug(rknpu_dev);

			cancel_delayed_work_sync(&rknpu_dev->power_off_work);
			ret = rknpu_job_shutdown(rknpu_dev);
			if (ret)
				LOG_DEV_ERROR(rknpu_dev->dev,
					      "failed to drain jobs during remove: %d\n",
					      ret);
			/* Tear down DRM device before freeing IOVA allocators and domains. */
			rknpu_drm_remove(rknpu_dev);
			/* gem_dma_xa only exists once the multicore lookup is built. */
			xa_destroy(&rknpu_dev->gem_dma_xa);
			rknpu_fence_context_free(rknpu_dev);

			destroy_workqueue(rknpu_dev->job_wq);
			cancel_delayed_work_sync(&rknpu_dev->power_off_work);
			destroy_workqueue(rknpu_dev->power_off_wq);
			rknpu_debugger_remove(rknpu_dev);
			rknpu_cancel_timer(rknpu_dev);
			rknpu_devfreq_remove(rknpu_dev);
		} else {
			/* Clean up partial initialization if probe failed before facade ready. */
			xa_destroy(&rknpu_dev->gem_dma_xa);
			rknpu_root_device_unregister(rknpu_dev);
		}

		/* Free IOMMU domains and IOVA allocators on final core removal. */
		rknpu_iommu_free_domains(rknpu_dev);

	}

	/* Drop per-core runtime-PM and regulator references before devm cleanup. */
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) > 0) {
		atomic_set(&rknpu_dev->power_refcount, 0);
		atomic_set(&rknpu_dev->cmdline_power_refcount, 0);
	}
	rknpu_core_release_power(rknpu_dev, index);
	mutex_unlock(&rknpu_dev->power_lock);
	rknpu_core_fini(rknpu_dev, index);

	{
		char core_list[16];
		rknpu_core_mask_to_string(rknpu_dev->available_cores,
					  core_list, sizeof(core_list));
		dev_info(&pdev->dev, "RKNPU core %u removed, available cores: [%s]\n",
			 index, core_list);
	}

	kref_put(&facade->refcount, rknpu_facade_release);

	mutex_unlock(&rknpu_global_lock);
}

#ifdef CONFIG_PM_SLEEP
static int rknpu_suspend(struct device *dev)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&rknpu_dev->power_off_work);
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) > 0) {
		mutex_unlock(&rknpu_dev->power_lock);
		return -EBUSY;
	}
	mutex_unlock(&rknpu_dev->power_lock);

	return pm_runtime_force_suspend(dev);
}

static int rknpu_resume(struct device *dev)
{
	return pm_runtime_force_resume(dev);
}
#endif

static int rknpu_runtime_suspend(struct device *dev)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	unsigned int index;

	if (!rknpu_dev)
		return -ENODEV;

	for (index = 0; index < RKNPU_MAX_CORES; index++) {
		if (rknpu_dev->cores[index].dev != dev)
			continue;
		clk_bulk_disable_unprepare(rknpu_dev->cores[index].num_clks,
					  rknpu_dev->cores[index].clks);
		return 0;
	}

	return -ENODEV;
}

static int rknpu_runtime_resume(struct device *dev)
{
	struct rknpu_device *rknpu_dev = dev_get_drvdata(dev);
	unsigned int index;
	struct rknpu_core *core;

	if (!rknpu_dev)
		return -ENODEV;

	for (index = 0; index < RKNPU_MAX_CORES; index++) {
		if (rknpu_dev->cores[index].dev != dev)
			continue;
		core = &rknpu_dev->cores[index];
		return clk_bulk_prepare_enable(core->num_clks, core->clks);
	}

	return -ENODEV;
}

static const struct dev_pm_ops rknpu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rknpu_suspend, rknpu_resume) SET_RUNTIME_PM_OPS(
		rknpu_runtime_suspend, rknpu_runtime_resume, NULL)
};

static struct platform_driver rknpu_driver = {
	.probe = rknpu_probe,
	.remove = rknpu_remove,
	/*
	 * The RKNPU driver owns IOMMU domains explicitly (per GEM mapping
	 * and per job). Do not let the DMA API attach the platform default
	 * domain behind this driver's back.
	 */
	.driver_managed_dma = true,
	.driver = {
		.owner = THIS_MODULE,
		.name = "RKNPU",
		.pm = &rknpu_pm_ops,
		.of_match_table = of_match_ptr(rknpu_of_match),
	},
};

static int rknpu_init(void)
{
	int ret;

	ret = rknpu_job_cache_init();
	if (ret)
		return ret;

	ret = platform_driver_register(&rknpu_driver);
	if (ret)
		rknpu_job_cache_fini();

	return ret;
}

static void rknpu_exit(void)
{
	platform_driver_unregister(&rknpu_driver);
	rknpu_job_cache_fini();
}

late_initcall(rknpu_init);
module_exit(rknpu_exit);

MODULE_DESCRIPTION("RKNPU driver");
MODULE_AUTHOR("lurenJBD <31967654+lurenJBD@users.noreply.github.com>");
MODULE_ALIAS("rockchip-rknpu");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(RKNPU_GET_DRV_VERSION_STRING(DRIVER_MAJOR, DRIVER_MINOR,
					    DRIVER_PATCHLEVEL));
