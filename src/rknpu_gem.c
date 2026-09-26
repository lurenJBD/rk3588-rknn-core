// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <drm/drm_device.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_prime.h>
#include <drm/drm_file.h>
#include <drm/drm_drv.h>

#include <linux/delay.h>
#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <linux/iommu.h>
#include <linux/version.h>
#include <asm/cacheflush.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>

#include <linux/dma-map-ops.h>

#include "rknpu_drv.h"
#include "rknpu_core.h"
#include "rknpu_ioctl.h"
#include "rknpu_gem.h"
#include "rknpu_iommu.h"

#define RKNPU_GEM_ALLOC_FROM_PAGES 1

static bool mem_profile;
module_param(mem_profile, bool, 0444);
MODULE_PARM_DESC(mem_profile,
		 "collect memory-path counters/timings (default off; read debugfs mem_stats)");

static inline void rknpu_mem_stat_add(struct rknpu_device *dev,
				    enum rknpu_mem_stat stat, u64 value)
{
	if (unlikely(mem_profile))
		atomic64_add(value, &dev->mem_stats[stat]);
}

int rknpu_gem_mem_stats_show(struct seq_file *m, struct rknpu_device *dev)
{
	static const char * const names[RKNPU_MEM_STAT_COUNT] = {
		"handle_hit", "dma_fallback", "dma_probes", "owner_entries",
		"lookup_ns", "sync_calls", "sync_errors", "sync_request_bytes",
		"sync_pages_calls", "sync_partial_calls", "sync_pages_request_bytes",
		"sync_whole_bytes",
		"sync_import_calls", "sync_other_calls", "sync_ns",
		"sync_range_calls", "sync_effective_bytes",
	};
	int i;

	seq_printf(m, "enabled %u\n", mem_profile);
	for (i = 0; i < RKNPU_MEM_STAT_COUNT; i++)
		seq_printf(m, "%s %lld\n", names[i],
			   (long long)atomic64_read(&dev->mem_stats[i]));
	return 0;
}

#if RKNPU_GEM_ALLOC_FROM_PAGES
static struct device *
rknpu_gem_core_dev(struct rknpu_gem_object *rknpu_obj, unsigned int core)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct rknpu_device *rknpu_dev = drm->dev_private;

	if (core >= RKNPU_MAX_CORES || !rknpu_dev->cores[core].dev)
		return NULL;

	return rknpu_dev->cores[core].dev;
}

static unsigned int
rknpu_gem_canonical_core(struct rknpu_gem_object *rknpu_obj)
{
	return rknpu_obj->core_mask ? ffs(rknpu_obj->core_mask) - 1 : 0;
}

static struct device *
rknpu_gem_target_dev(struct rknpu_gem_object *rknpu_obj)
{
	return rknpu_gem_core_dev(rknpu_obj,
				  rknpu_gem_canonical_core(rknpu_obj));
}

/*
 * Map object into every available core's domain with shared IOVA,
 * so that buffers created on one core can be safely submitted to any core.
 */
static unsigned int rknpu_gem_map_core_mask(struct rknpu_gem_object *rknpu_obj)
{
	struct rknpu_device *rknpu_dev = rknpu_obj->base.dev->dev_private;
	unsigned int mask = 0;
	int core;

	if (!rknpu_dev->iommu_en)
		return rknpu_obj->core_mask;

	for (core = 0; core < RKNPU_MAX_CORES; core++)
		if (rknpu_dev->cores[core].registered &&
		    rknpu_dev->cores[core].dev)
			mask |= BIT(core);

	return mask ? mask : rknpu_obj->core_mask;
}

static bool rknpu_gem_core_is_mapped(struct rknpu_gem_object *rknpu_obj,
				     unsigned int core)
{
	return rknpu_gem_map_core_mask(rknpu_obj) & BIT(core);
}

static struct sg_table *
rknpu_gem_map_core_sg(struct rknpu_gem_object *rknpu_obj, unsigned int core,
		      unsigned int *dma_nents)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct device *map_dev = rknpu_gem_core_dev(rknpu_obj, core);
	struct sg_table *sgt;

	if (!map_dev)
		return ERR_PTR(-EINVAL);

	sgt = drm_prime_pages_to_sg(drm, rknpu_obj->pages,
				    rknpu_obj->num_pages);
	if (IS_ERR(sgt)) {
		LOG_DEV_ERROR(drm->dev,
			      "failed to allocate sgt for core %u: %ld\n",
			      core, PTR_ERR(sgt));
		return sgt;
	}

	/*
	 * This sg_table is not passed through dma_map_sg(); the IOMMU mapping
	 * is installed explicitly by rknpu_iommu_map_core_sg(). Cache-sync
	 * helpers consume sg_dma_address(), so populate it with page physical
	 * addresses to keep CPU-cache maintenance valid.
	 */
	{
		struct scatterlist *sg;
		int i;

		for_each_sgtable_sg(sgt, sg, i)
			sg_dma_address(sg) = sg_phys(sg);
	}

	if (dma_nents)
		*dma_nents = sgt->nents;

	return sgt;
}

static void rknpu_gem_unmap_core_sg(struct rknpu_gem_object *rknpu_obj,
				    unsigned int core)
{
	struct sg_table *sgt = rknpu_obj->core_maps[core].sgt;

	if (!rknpu_obj->core_maps[core].mapped || !sgt)
		return;

	rknpu_iommu_unmap_core_sg(rknpu_obj->base.dev->dev_private, core,
				  rknpu_obj->iommu_domain_id,
				  &rknpu_obj->core_maps[core]);

	sg_free_table(sgt);
	kfree(sgt);
	rknpu_obj->core_maps[core].sgt = NULL;
	rknpu_obj->core_maps[core].dma_addr = 0;
	rknpu_obj->core_maps[core].mapped = false;
}

/*
 * Release the object's single IOVA reservation. It is taken once per object
 * and shared by all of that object's per-core mappings, so the last mapping
 * torn down is the one that frees it.
 */
static void rknpu_gem_release_iova(struct rknpu_gem_object *rknpu_obj)
{
	struct rknpu_device *rknpu_dev = rknpu_obj->base.dev->dev_private;

	if (!rknpu_dev->iommu_en || !drm_mm_node_allocated(&rknpu_obj->iova_node))
		return;

	rknpu_iommu_release_iova(rknpu_dev, rknpu_obj->iommu_domain_id,
				 &rknpu_obj->iova_node);
	rknpu_obj->iova_base = 0;
}

static void rknpu_gem_clear_core_maps(struct rknpu_gem_object *rknpu_obj)
{
	memset(rknpu_obj->core_maps, 0, sizeof(rknpu_obj->core_maps));
}

/*
 * In non-IOMMU mode, publish the single contiguous allocation under every
 * registered core so that per-core submit validation succeeds.
 */
static void rknpu_gem_publish_contiguous_map(struct rknpu_gem_object *rknpu_obj)
{
	struct rknpu_device *rknpu_dev = rknpu_obj->base.dev->dev_private;
	unsigned int core;

	if (!rknpu_obj->sgt)
		return;

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		if (!rknpu_dev->cores[core].registered ||
		    !rknpu_dev->cores[core].dev)
			continue;

		rknpu_obj->core_maps[core].sgt = rknpu_obj->sgt;
		rknpu_obj->core_maps[core].dma_addr = rknpu_obj->dma_addr;
		rknpu_obj->core_maps[core].mapped_size = rknpu_obj->size;
		rknpu_obj->core_maps[core].mapped = true;
	}
}

static int rknpu_gem_get_pages(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct rknpu_device *rknpu_dev = drm->dev_private;
	unsigned int canonical_core = rknpu_gem_canonical_core(rknpu_obj);
	unsigned int mapped;
	int core;
	int ret;

	rknpu_obj->pages = drm_gem_get_pages(&rknpu_obj->base);
	if (IS_ERR(rknpu_obj->pages)) {
		ret = PTR_ERR(rknpu_obj->pages);
		rknpu_obj->pages = NULL;
		LOG_ERROR("failed to get pages: %d\n", ret);
		return ret;
	}

	rknpu_obj->num_pages = rknpu_obj->size >> PAGE_SHIFT;

	if (rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING) {
		rknpu_obj->cookie = vmap(rknpu_obj->pages,
					 rknpu_obj->num_pages, VM_MAP,
					 PAGE_KERNEL);
		if (!rknpu_obj->cookie) {
			ret = -ENOMEM;
			LOG_ERROR("failed to vmap: %d\n", ret);
			goto put_pages;
		}
		rknpu_obj->kv_addr = rknpu_obj->cookie;
	}

	mapped = 0;

	/* Reserve a single IOVA for the object and map all target cores to it. */
	if (rknpu_dev->iommu_en) {
		ret = rknpu_iommu_reserve_iova(rknpu_dev,
					       rknpu_obj->iommu_domain_id,
					       rknpu_obj->size,
					       &rknpu_obj->iova_node,
					       &rknpu_obj->iova_base);
		if (ret) {
			LOG_DEV_ERROR(drm->dev,
				      "failed to reserve IOVA for size %lu: %d\n",
				      rknpu_obj->size, ret);
			goto put_pages;
		}
	}

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		struct sg_table *sgt;
		dma_addr_t map_end;
		unsigned int nents = 0;

		if (!rknpu_gem_core_is_mapped(rknpu_obj, core))
			continue;

		if (!rknpu_dev->cores[core].dev) {
			ret = -EINVAL;
			LOG_DEV_ERROR(drm->dev, "invalid GEM core %u\n",
				      core);
			goto unmap_cores;
		}

		sgt = rknpu_gem_map_core_sg(rknpu_obj, core, &nents);
		if (IS_ERR(sgt)) {
			ret = PTR_ERR(sgt);
			goto unmap_cores;
		}

		ret = rknpu_iommu_map_core_sg(rknpu_dev, core,
					       rknpu_obj->iommu_domain_id, sgt,
					       &rknpu_obj->core_maps[core],
					       rknpu_obj->iova_base);
		if (ret) {
			sg_free_table(sgt);
			kfree(sgt);
			goto unmap_cores;
		}

		rknpu_obj->core_maps[core].sgt = sgt;
		rknpu_obj->core_maps[core].mapped = true;
		mapped++;

		if (core == canonical_core) {
			rknpu_obj->sgt = sgt;
			rknpu_obj->dma_nents = nents;
			rknpu_obj->dma_mapped = true;
			rknpu_obj->dma_addr = rknpu_obj->core_maps[core].dma_addr;
		}

		map_end = rknpu_obj->core_maps[core].dma_addr + rknpu_obj->size;
		LOG_DEV_DBG(drm->dev,
			     "mapped GEM buffer on core %u: dma_addr=%pad size=%lu end=%pad nents=%u flags=%#x\n",
			     core, &rknpu_obj->core_maps[core].dma_addr,
			     rknpu_obj->size, &map_end, nents,
			     rknpu_obj->flags);
	}

	if (!mapped) {
		ret = -EINVAL;
		LOG_DEV_ERROR(drm->dev, "no valid core in GEM core mask %#x\n",
			      rknpu_obj->core_mask);
		goto unmap_cores;
	}

	rknpu_obj->pages_backed = true;

	return 0;

unmap_cores:
	while (core-- > 0) {
		if (rknpu_gem_core_is_mapped(rknpu_obj, core))
			rknpu_gem_unmap_core_sg(rknpu_obj, core);
		if (core == canonical_core) {
			rknpu_obj->sgt = NULL;
			rknpu_obj->dma_nents = 0;
			rknpu_obj->dma_mapped = false;
			rknpu_obj->dma_addr = 0;
		}
	}
	rknpu_gem_release_iova(rknpu_obj);
put_pages:
	if (rknpu_obj->kv_addr) {
		vunmap(rknpu_obj->kv_addr);
		rknpu_obj->kv_addr = NULL;
		rknpu_obj->cookie = NULL;
	}
	drm_gem_put_pages(&rknpu_obj->base, rknpu_obj->pages, false, false);
	rknpu_obj->pages = NULL;

	return ret;
}

static void rknpu_gem_put_pages(struct rknpu_gem_object *rknpu_obj)
{
	unsigned int canonical_core = rknpu_gem_canonical_core(rknpu_obj);
	int core;

	if (rknpu_obj->kv_addr) {
		vunmap(rknpu_obj->kv_addr);
		rknpu_obj->kv_addr = NULL;
		rknpu_obj->cookie = NULL;
	}

	for (core = RKNPU_MAX_CORES - 1; core >= 0; core--) {
		if (!rknpu_gem_core_is_mapped(rknpu_obj, core))
			continue;
		rknpu_gem_unmap_core_sg(rknpu_obj, core);
		if (core == canonical_core) {
			rknpu_obj->sgt = NULL;
			rknpu_obj->dma_nents = 0;
			rknpu_obj->dma_mapped = false;
			rknpu_obj->dma_addr = 0;
		}
	}
	rknpu_gem_release_iova(rknpu_obj);

	if (rknpu_obj->pages) {
		drm_gem_put_pages(&rknpu_obj->base, rknpu_obj->pages, true,
				  true);
		rknpu_obj->pages = NULL;
	}

	rknpu_obj->pages_backed = false;
}
#endif

static int rknpu_gem_alloc_buf(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct device *map_dev;
	unsigned int nr_pages = 0;
	struct sg_table *sgt = NULL;
	struct scatterlist *s = NULL;
	gfp_t gfp_mask = GFP_KERNEL;
	int ret = -EINVAL, i = 0;

	map_dev = rknpu_gem_target_dev(rknpu_obj);
	if (!map_dev) {
		LOG_DEV_ERROR(drm->dev, "invalid GEM target core\n");
		return -EINVAL;
	}

	if (rknpu_obj->dma_allocated) {
		LOG_DEBUG("buffer already allocated.\n");
		return 0;
	}

	rknpu_obj->dma_attrs = 0;

	/*
	 * if RKNPU_MEM_CONTIGUOUS, fully physically contiguous memory
	 * region will be allocated else physically contiguous
	 * as possible.
	 */
	if (!(rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS))
		rknpu_obj->dma_attrs |= DMA_ATTR_FORCE_CONTIGUOUS;

	// cacheable mapping or writecombine mapping
	if (rknpu_obj->flags & RKNPU_MEM_CACHEABLE) {
#ifdef DMA_ATTR_NON_CONSISTENT
		rknpu_obj->dma_attrs |= DMA_ATTR_NON_CONSISTENT;
#endif
#ifdef DMA_ATTR_SYS_CACHE_ONLY
		rknpu_obj->dma_attrs |= DMA_ATTR_SYS_CACHE_ONLY;
#endif
	} else if (rknpu_obj->flags & RKNPU_MEM_WRITE_COMBINE) {
		rknpu_obj->dma_attrs |= DMA_ATTR_WRITE_COMBINE;
	}

	if (!(rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING))
		rknpu_obj->dma_attrs |= DMA_ATTR_NO_KERNEL_MAPPING;

#ifdef DMA_ATTR_SKIP_ZEROING
	if (!(rknpu_obj->flags & RKNPU_MEM_ZEROING))
		rknpu_obj->dma_attrs |= DMA_ATTR_SKIP_ZEROING;
#endif

#if RKNPU_GEM_ALLOC_FROM_PAGES
	/*
	 * With an IOMMU, always use one set of pages plus per-core device
	 * mappings. Otherwise a "contiguous" request would silently allocate
	 * on core0 only and multi-core masks would not be usable.
	 */
	if (rknpu_dev->iommu_en)
		return rknpu_gem_get_pages(rknpu_obj);
#endif

	if (rknpu_obj->flags & RKNPU_MEM_ZEROING)
		gfp_mask |= __GFP_ZERO;

	if (!rknpu_dev->iommu_en ||
	    rknpu_dev->config->dma_mask <= DMA_BIT_MASK(32) ||
	    (rknpu_obj->flags & RKNPU_MEM_DMA32)) {
		gfp_mask &= ~__GFP_HIGHMEM;
		gfp_mask |= __GFP_DMA32;
	}

	nr_pages = rknpu_obj->size >> PAGE_SHIFT;

	rknpu_obj->pages = rknpu_gem_alloc_page(nr_pages);
	if (!rknpu_obj->pages) {
		LOG_ERROR("failed to allocate pages.\n");
		return -ENOMEM;
	}

	rknpu_obj->cookie = dma_alloc_attrs(map_dev, rknpu_obj->size,
					    &rknpu_obj->dma_addr, gfp_mask,
					    rknpu_obj->dma_attrs);
	if (!rknpu_obj->cookie) {
		/*
		 * when RKNPU_MEM_CONTIGUOUS and IOMMU is available
		 * try to fallback to allocate non-contiguous buffer
		 */
		if (!(rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS) &&
		    rknpu_dev->iommu_en) {
			LOG_DEV_WARN(
				drm->dev,
				"try to fallback to allocate non-contiguous %lu buffer.\n",
				rknpu_obj->size);
			rknpu_obj->dma_attrs &= ~DMA_ATTR_FORCE_CONTIGUOUS;
			rknpu_obj->flags |= RKNPU_MEM_NON_CONTIGUOUS;
			rknpu_obj->cookie = dma_alloc_attrs(
				map_dev, rknpu_obj->size, &rknpu_obj->dma_addr,
				gfp_mask, rknpu_obj->dma_attrs);
			if (!rknpu_obj->cookie) {
				LOG_DEV_ERROR(
					drm->dev,
					"failed to allocate non-contiguous %lu buffer.\n",
					rknpu_obj->size);
				goto err_free;
			}
		} else {
			LOG_DEV_ERROR(drm->dev,
				      "failed to allocate %lu buffer.\n",
				      rknpu_obj->size);
			goto err_free;
		}
	}
	rknpu_obj->dma_allocated = true;

	if (rknpu_obj->flags & RKNPU_MEM_KERNEL_MAPPING)
		rknpu_obj->kv_addr = rknpu_obj->cookie;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt) {
		ret = -ENOMEM;
		goto err_free_dma;
	}

	ret = dma_get_sgtable_attrs(map_dev, sgt, rknpu_obj->cookie,
				    rknpu_obj->dma_addr, rknpu_obj->size,
				    rknpu_obj->dma_attrs);
	if (ret < 0) {
		LOG_DEV_ERROR(drm->dev, "failed to get sgtable.\n");
		goto err_free_sgt;
	}

	for_each_sg(sgt->sgl, s, sgt->nents, i) {
		sg_dma_address(s) = sg_phys(s);
		LOG_DEBUG("dma alloc sgt[%d], phys_address: %pad, length: %u\n",
			  i, &s->dma_address, s->length);
	}

	ret = drm_prime_sg_to_page_array(sgt, rknpu_obj->pages, nr_pages);

	if (ret < 0) {
		LOG_DEV_ERROR(drm->dev, "invalid sgtable, ret: %d\n", ret);
		goto err_free_sg_table;
	}

	rknpu_obj->sgt = sgt;

	/*
	 * One physical allocation serves every core when there is no IOMMU,
	 * so publish it in the per-core table the submit path reads. See
	 * rknpu_gem_publish_contiguous_map().
	 */
	if (!rknpu_dev->iommu_en)
		rknpu_gem_publish_contiguous_map(rknpu_obj);

	return ret;

err_free_sg_table:
	sg_free_table(sgt);
err_free_sgt:
	kfree(sgt);
err_free_dma:
	dma_free_attrs(map_dev ? map_dev : drm->dev, rknpu_obj->size, rknpu_obj->cookie,
		       rknpu_obj->dma_addr, rknpu_obj->dma_attrs);
err_free:
	rknpu_gem_free_page(rknpu_obj->pages);

	return ret;
}

static void rknpu_gem_free_buf(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct device *map_dev = rknpu_gem_target_dev(rknpu_obj);

#if RKNPU_GEM_ALLOC_FROM_PAGES
	if (rknpu_obj->pages_backed) {
		rknpu_gem_put_pages(rknpu_obj);
		return;
	}
#endif

	if (!rknpu_obj->dma_allocated) {
		LOG_DEBUG("DMA buffer is not allocated.\n");
		return;
	}

	sg_free_table(rknpu_obj->sgt);
	kfree(rknpu_obj->sgt);

	if (!map_dev) {
		LOG_DEV_ERROR(drm->dev,
			      "target core disappeared during DMA free\n");
		return;
	}

	dma_free_attrs(map_dev, rknpu_obj->size, rknpu_obj->cookie,
		       rknpu_obj->dma_addr, rknpu_obj->dma_attrs);

	rknpu_gem_free_page(rknpu_obj->pages);

	rknpu_obj->dma_addr = 0;
	rknpu_obj->dma_allocated = false;
}

static int rknpu_gem_handle_create(struct drm_gem_object *obj,
				   struct drm_file *file_priv,
				   unsigned int *handle)
{
	int ret;

	/* Allocate an id in the per-file object table. */
	ret = drm_gem_handle_create(file_priv, obj, handle);
	if (ret)
		return ret;

	LOG_DEBUG("gem handle: %#x\n", *handle);

	return 0;
}

static int rknpu_gem_handle_destroy(struct drm_file *file_priv,
				    unsigned int handle)
{
	return drm_gem_handle_delete(file_priv, handle);
}

static const struct vm_operations_struct vm_ops = {
	.fault = rknpu_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs rknpu_gem_object_funcs = {
	.free = rknpu_gem_free_object,
	.export = drm_gem_prime_export,
	.get_sg_table = rknpu_gem_prime_get_sg_table,
	.vmap = rknpu_gem_prime_vmap,
	.vunmap = rknpu_gem_prime_vunmap,
	.mmap = rknpu_gem_mmap_obj,
	.vm_ops = &vm_ops,
};

struct rknpu_gem_object *
rknpu_gem_object_find(struct drm_file *file_priv, unsigned int handle)
{
	struct drm_gem_object *obj;

	obj = drm_gem_object_lookup(file_priv, handle);
	if (!obj)
		return NULL;

	if (obj->funcs != &rknpu_gem_object_funcs) {
		rknpu_gem_object_put(obj);
		return NULL;
	}

	return to_rknpu_obj(obj);
}

static struct rknpu_gem_object *rknpu_gem_init(struct drm_device *drm,
					       unsigned long size,
					       unsigned int flags)
{
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct rknpu_gem_object *rknpu_obj = NULL;
	struct drm_gem_object *obj = NULL;
	gfp_t gfp_mask;
	int ret = -EINVAL;

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return ERR_PTR(-ENOMEM);

	rknpu_obj->flags = flags;
	obj = &rknpu_obj->base;
	obj->funcs = &rknpu_gem_object_funcs;

	ret = drm_gem_object_init(drm, obj, size);
	if (ret < 0) {
		LOG_DEV_ERROR(drm->dev, "failed to initialize gem object\n");
		kfree(rknpu_obj);
		return ERR_PTR(ret);
	}

	rknpu_obj->size = rknpu_obj->base.size;

	gfp_mask = mapping_gfp_mask(obj->filp->f_mapping);

	if (rknpu_obj->flags & RKNPU_MEM_ZEROING)
		gfp_mask |= __GFP_ZERO;

	if (!rknpu_dev->iommu_en ||
	    rknpu_dev->config->dma_mask <= DMA_BIT_MASK(32) ||
	    (rknpu_obj->flags & RKNPU_MEM_DMA32)) {
		gfp_mask &= ~__GFP_HIGHMEM;
		gfp_mask |= __GFP_DMA32;
	}

	mapping_set_gfp_mask(obj->filp->f_mapping, gfp_mask);

	return rknpu_obj;
}

static inline unsigned long
rknpu_gem_dma_token_key(int domain_id, dma_addr_t addr)
{
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;
	return ((unsigned long)domain_id << 32) | (unsigned long)(addr & 0xffffffff);
}

static int rknpu_gem_dma_token_insert(struct rknpu_device *rknpu_dev,
				      struct rknpu_gem_object *rknpu_obj)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	struct xarray *xa = &rknpu_dev->gem_dma_xa;
	unsigned long keys[RKNPU_GEM_MAX_CORE_MAPS + 1];
	int count = 0;
	int core, i, ret;

	/* Canonical token plus unique per-core aliases, including all cores. */
	keys[count++] = rknpu_gem_dma_token_key(rknpu_obj->iommu_domain_id,
						rknpu_obj->dma_addr);
	for (core = 0; core < RKNPU_GEM_MAX_CORE_MAPS; core++) {
		dma_addr_t addr = rknpu_obj->core_maps[core].dma_addr;
		unsigned long key;

		if (!addr || !rknpu_obj->core_maps[core].mapped)
			continue;
		key = rknpu_gem_dma_token_key(rknpu_obj->iommu_domain_id, addr);
		for (i = 0; i < count; i++)
			if (keys[i] == key)
				break;
		if (i == count)
			keys[count++] = key;
	}

	for (core = 0; core < count; core++) {
		/* Never replace another object's live token on a collision. */
		ret = xa_insert(xa, keys[core], rknpu_obj, GFP_KERNEL);
		if (!ret)
			continue;
		if (ret == -EBUSY) {
			LOG_DEV_ERROR(drm->dev,
				      "duplicate GEM dma token %#lx (domain %d)\n",
				      keys[core], rknpu_obj->iommu_domain_id);
			ret = -EEXIST;
		}
		/* Roll back only entries this invocation actually inserted. */
		xa_lock(xa);
		while (core-- > 0)
			if (xa_load(xa, keys[core]) == rknpu_obj)
				__xa_erase(xa, keys[core]);
		xa_unlock(xa);
		return ret;
	}

	return 0;
}

static void rknpu_gem_dma_token_remove(struct rknpu_device *rknpu_dev,
				       struct rknpu_gem_object *rknpu_obj)
{
	struct xarray *xa = &rknpu_dev->gem_dma_xa;
	unsigned long key;
	int core;

	if (xa_empty(xa))
		return;

	xa_lock(xa);
	key = rknpu_gem_dma_token_key(rknpu_obj->iommu_domain_id,
				      rknpu_obj->dma_addr);
	if (xa_load(xa, key) == rknpu_obj)
		__xa_erase(xa, key);

	for (core = 0; core < RKNPU_GEM_MAX_CORE_MAPS; core++) {
		dma_addr_t addr = rknpu_obj->core_maps[core].dma_addr;

		if (!addr)
			continue;
		key = rknpu_gem_dma_token_key(rknpu_obj->iommu_domain_id,
					      addr);
		if (xa_load(xa, key) == rknpu_obj)
			__xa_erase(xa, key);
	}
	xa_unlock(xa);
}

/*
 * Token removal in the final GEM release uses the same xa_lock. Acquire a
 * nonzero reference before releasing that lock: xa_load's internal RCU only
 * protects the index nodes, not the lifetime of the stored GEM object.
 */
static struct rknpu_gem_object *
rknpu_gem_dma_token_get(struct drm_device *drm, struct drm_file *file_priv,
			unsigned long key)
{
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct xarray *xa = &rknpu_dev->gem_dma_xa;
	struct rknpu_gem_object *obj;

	rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_DMA_PROBES, 1);
	xa_lock(xa);
	obj = xa_load(xa, key);
	if (obj && !kref_get_unless_zero(&obj->base.refcount))
		obj = NULL;
	xa_unlock(xa);

	if (!obj)
		return NULL;
	/* Never nest the file table lock or final put under xa_lock. */
	if (obj->base.dev == drm &&
	    rknpu_gem_object_is_file_handle(file_priv, obj))
		return obj;
	rknpu_gem_object_put(&obj->base);
	return NULL;
}

struct rknpu_gem_object *
rknpu_gem_object_find_by_dma(struct drm_device *drm, struct drm_file *file_priv,
			     dma_addr_t dma_addr)
{
	struct rknpu_gem_object *obj;
	struct rknpu_file_priv *fpriv;
	int preferred = 0;
	int d;

	if (!dma_addr || !file_priv)
		return NULL;

	/* Preserve direct-key precedence, but only for caller-owned objects. */
	obj = rknpu_gem_dma_token_get(drm, file_priv, (unsigned long)dma_addr);
	if (obj || (dma_addr >> 32))
		return obj;

	fpriv = file_priv->driver_priv;
	if (fpriv && fpriv->domain_id > 0 &&
	    fpriv->domain_id < RKNPU_MAX_IOMMU_DOMAIN_NUM) {
		preferred = fpriv->domain_id;
		obj = rknpu_gem_dma_token_get(drm, file_priv,
			rknpu_gem_dma_token_key(preferred, dma_addr));
		if (obj)
			return obj;
	}

	/* A foreign same-IOVA candidate must not hide a later owned object. */
	for (d = 1; d < RKNPU_MAX_IOMMU_DOMAIN_NUM; d++) {
		if (d == preferred)
			continue;
		obj = rknpu_gem_dma_token_get(drm, file_priv,
			rknpu_gem_dma_token_key(d, dma_addr));
		if (obj)
			return obj;
	}

	return NULL;
}


/*
 * The DMA token is a runtime compatibility aid, not an authorization token.
 * Requiring that the caller also owns at least one GEM handle for the object
 * keeps the security model equivalent to the original handle-only ioctl.
 */
bool rknpu_gem_object_is_file_handle(struct drm_file *file_priv,
				      struct rknpu_gem_object *rknpu_obj)
{
	struct drm_gem_object *obj;
	int id;
	u64 visited = 0;
	bool found = false;

	if (!file_priv || !rknpu_obj)
		return false;

	spin_lock(&file_priv->table_lock);
	idr_for_each_entry(&file_priv->object_idr, obj, id) {
		visited++;
		if (obj == &rknpu_obj->base) {
			found = true;
			break;
		}
	}
	spin_unlock(&file_priv->table_lock);
	rknpu_mem_stat_add(rknpu_obj->base.dev->dev_private,
			   RKNPU_MEM_STAT_OWNER_ENTRIES, visited);

	return found;
}

struct rknpu_gem_object *
rknpu_gem_object_find_token(struct drm_device *drm,
			    struct drm_file *file_priv, u64 token)
{
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct rknpu_gem_object *obj;
	u64 start = unlikely(mem_profile) ? ktime_get_ns() : 0;

	if (token && token <= U32_MAX) {
		obj = rknpu_gem_object_find(file_priv, (u32)token);
		if (obj) {
			rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_HANDLE_HIT, 1);
			goto out;
		}
	}

	rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_DMA_FALLBACK, 1);
	/* The DMA helper returns a referenced, caller-owned object. */
	obj = rknpu_gem_object_find_by_dma(drm, file_priv, token);
out:
	if (unlikely(mem_profile))
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_LOOKUP_NS,
				   ktime_get_ns() - start);
	return obj;
}

static void rknpu_gem_release(struct rknpu_gem_object *rknpu_obj)
{
	/* release file pointer to gem object. */
	drm_gem_object_release(&rknpu_obj->base);
	kfree(rknpu_obj);
}

struct rknpu_gem_object *
rknpu_gem_object_create(struct drm_device *drm, unsigned int flags,
			unsigned long size, unsigned long sram_size,
			int iommu_domain_id, unsigned int core_mask)
{
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct rknpu_gem_object *rknpu_obj;
	unsigned long aligned_size;
	int ret;

	(void)sram_size;

	if (core_mask == RKNPU_CORE_AUTO_MASK)
		core_mask = RKNPU_CORE0_MASK;

	if (!core_mask || (core_mask & ~rknpu_dev->config->core_mask)) {
		LOG_DEV_ERROR(drm->dev,
			      "invalid GEM core mask: %#x\n", core_mask);
		return ERR_PTR(-EINVAL);
	}

	if (hweight32(core_mask) != 1 && !rknpu_dev->iommu_en) {
		LOG_DEV_ERROR(drm->dev,
			      "multi-core GEM requires an IOMMU, core_mask=%#x\n",
			      core_mask);
		return ERR_PTR(-EOPNOTSUPP);
	}

	ret = rknpu_gem_validate_flags(flags);
	if (ret)
		return ERR_PTR(ret);

	if (!rknpu_dev->iommu_en && (flags & RKNPU_MEM_IOMMU)) {
		LOG_DEV_ERROR(drm->dev,
			      "IOMMU memory requested without an attached IOMMU\n");
		return ERR_PTR(-EINVAL);
	}

	if (flags & (RKNPU_MEM_TRY_ALLOC_SRAM | RKNPU_MEM_TRY_ALLOC_NBUF)) {
		dev_warn_ratelimited(
			drm->dev,
			"RKNPU: SRAM and NBUF allocation is unavailable in this build\n");
		return ERR_PTR(-EOPNOTSUPP);
	}

	if (!size) {
		LOG_DEV_ERROR(drm->dev, "invalid buffer size: %lu\n", size);
		return ERR_PTR(-EINVAL);
	}

	if (size > ULONG_MAX - (PAGE_SIZE - 1))
		return ERR_PTR(-E2BIG);

	ret = rknpu_iommu_domain_get_and_switch(rknpu_dev, iommu_domain_id);
	if (ret)
		return ERR_PTR(ret);

	aligned_size = round_up(size, PAGE_SIZE);
	rknpu_obj = rknpu_gem_init(drm, aligned_size, flags);
	if (IS_ERR(rknpu_obj))
		goto out_domain;

	if (!rknpu_dev->iommu_en && (flags & RKNPU_MEM_NON_CONTIGUOUS)) {
		flags &= ~RKNPU_MEM_NON_CONTIGUOUS;
		LOG_WARN(
			"non-contiguous allocation is not supported without IOMMU, falling back to contiguous buffer\n");
	}

	rknpu_obj->flags = flags;
	rknpu_obj->core_mask = core_mask;
	rknpu_obj->iommu_domain_id = iommu_domain_id;
	rknpu_gem_clear_core_maps(rknpu_obj);
	ret = rknpu_gem_alloc_buf(rknpu_obj);
	if (ret < 0) {
		/* Set error pointer after releasing object on allocation failure */
		rknpu_gem_release(rknpu_obj);
		rknpu_obj = ERR_PTR(ret);
	}
	else {
		ret = rknpu_gem_dma_token_insert(rknpu_dev, rknpu_obj);
		if (ret) {
			/* A concurrent token probe may briefly hold a reference. */
			rknpu_gem_object_put(&rknpu_obj->base);
			rknpu_obj = ERR_PTR(ret);
		}
	}

	if (IS_ERR(rknpu_obj))
		goto out_domain;

	LOG_DEBUG(
		"created dma addr: %pad, cookie: %p, size: %lu, attrs: %#lx, flags: %#x\n",
		&rknpu_obj->dma_addr, rknpu_obj->cookie, rknpu_obj->size,
		rknpu_obj->dma_attrs, rknpu_obj->flags);

out_domain:
	rknpu_iommu_domain_put(rknpu_dev);
	return rknpu_obj;
}

void rknpu_gem_object_destroy(struct rknpu_gem_object *rknpu_obj)
{
	struct drm_gem_object *obj = &rknpu_obj->base;
	struct rknpu_device *rknpu_dev = obj->dev->dev_private;

	LOG_DEBUG(
		"destroy dma addr: %pad, cookie: %p, size: %lu, attrs: %#lx, flags: %#x, handle count: %d\n",
		&rknpu_obj->dma_addr, rknpu_obj->cookie, rknpu_obj->size,
		rknpu_obj->dma_attrs, rknpu_obj->flags, obj->handle_count);

	rknpu_gem_dma_token_remove(rknpu_dev, rknpu_obj);

	/* Imported memory remains owned by its dma-buf exporter. */
	if (obj->import_attach) {
		drm_prime_gem_destroy(obj, rknpu_obj->sgt);
		rknpu_gem_free_page(rknpu_obj->pages);
	} else {
		rknpu_gem_free_buf(rknpu_obj);
	}

	rknpu_gem_release(rknpu_obj);
}

int rknpu_gem_create_ioctl(struct drm_device *drm, void *data,
			   struct drm_file *file_priv)
{
	struct rknpu_mem_create *args = data;
	struct rknpu_gem_object *rknpu_obj = NULL;
	int domain_id;
	int ret = -EINVAL;

	rknpu_obj = rknpu_gem_object_find(file_priv, args->handle);
	if (rknpu_obj) {
		if (!rknpu_gem_token_is_valid(args->handle) ||
		    rknpu_obj->base.dev != drm) {
			rknpu_gem_object_put(&rknpu_obj->base);
			return -EINVAL;
		}

		args->size = rknpu_obj->size;
		args->sram_size = 0;
		args->obj_addr = args->handle;
		args->dma_addr = rknpu_obj->dma_addr;
		rknpu_gem_object_put(&rknpu_obj->base);
		return 0;
	}

	domain_id = args->iommu_domain_id;

	if (domain_id <= 0 && file_priv && file_priv->driver_priv) {
		struct rknpu_file_priv *fpriv = file_priv->driver_priv;

		if (fpriv->domain_id > 0)
			domain_id = fpriv->domain_id;
	}

	rknpu_obj = rknpu_gem_object_create(drm, args->flags, args->size,
					    args->sram_size,
					    domain_id,
					    args->core_mask);
	if (IS_ERR(rknpu_obj))
		return PTR_ERR(rknpu_obj);

	ret = rknpu_gem_handle_create(&rknpu_obj->base, file_priv,
				      &args->handle);
	if (ret) {
		/* Published DMA tokens may have transient lookup references. */
		rknpu_gem_object_put(&rknpu_obj->base);
		return ret;
	}

	args->size = rknpu_obj->size;
	args->sram_size = 0;
	args->obj_addr = args->handle;
	args->dma_addr = rknpu_obj->dma_addr;
	rknpu_gem_object_put(&rknpu_obj->base);

	return 0;
}

int rknpu_gem_map_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct rknpu_mem_map *args = data;

	return drm_gem_dumb_map_offset(file_priv, dev, args->handle,
				       &args->offset);
}

int rknpu_gem_destroy_ioctl(struct drm_device *drm, void *data,
				    struct drm_file *file_priv)
{
	struct rknpu_mem_destroy *args = data;

	(void)drm;
	/* Userspace identifies objects by handle; obj_addr is not required. */
	if (!rknpu_gem_token_is_valid(args->handle))
		return -EINVAL;

	return rknpu_gem_handle_destroy(file_priv, args->handle);
}

#if RKNPU_GEM_ALLOC_FROM_PAGES
/*
 * __vm_map_pages - maps range of kernel pages into user vma
 * @vma: user vma to map to
 * @pages: pointer to array of source kernel pages
 * @num: number of pages in page array
 * @offset: user's requested vm_pgoff
 *
 * This allows drivers to map range of kernel pages into a user vma.
 *
 * Return: 0 on success and error code otherwise.
 */
static int __vm_map_pages(struct vm_area_struct *vma, struct page **pages,
			  unsigned long num, unsigned long offset)
{
	unsigned long count = vma_pages(vma);
	unsigned long uaddr = vma->vm_start;
	int ret = -EINVAL, i = 0;

	/* Fail if the user requested offset is beyond the end of the object */
	if (offset >= num)
		return -ENXIO;

	/* Fail if the user requested size exceeds available object size */
	if (count > num - offset)
		return -ENXIO;

	for (i = 0; i < count; i++) {
		ret = vm_insert_page(vma, uaddr, pages[offset + i]);
		if (ret < 0)
			return ret;
		uaddr += PAGE_SIZE;
	}

	return 0;
}

static int rknpu_gem_mmap_pages(struct rknpu_gem_object *rknpu_obj,
				struct vm_area_struct *vma)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	int ret = -EINVAL;

	vm_flags_set(vma, VM_MIXEDMAP);

	ret = __vm_map_pages(vma, rknpu_obj->pages, rknpu_obj->num_pages,
			     vma->vm_pgoff);
	if (ret < 0)
		LOG_DEV_ERROR(drm->dev, "failed to map pages into vma: %d\n",
			      ret);

	return ret;
}
#endif

static int rknpu_gem_mmap_buffer(struct rknpu_gem_object *rknpu_obj,
				 struct vm_area_struct *vma)
{
	struct drm_device *drm = rknpu_obj->base.dev;
	unsigned long vm_size = 0;
	int ret = -EINVAL;

	/*
	 * clear the VM_PFNMAP flag that was set by drm_gem_mmap(), and set the
	 * vm_pgoff (used as a fake buffer offset by DRM) to 0 as we want to map
	 * the whole buffer.
	 */
	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP | VM_IO);
	vm_flags_clear(vma, VM_PFNMAP);
	vma->vm_pgoff = 0;

	vm_size = vma->vm_end - vma->vm_start;

	/* check if user-requested size is valid. */
	if (vm_size > rknpu_obj->size)
		return -EINVAL;

#if RKNPU_GEM_ALLOC_FROM_PAGES
	if (rknpu_obj->pages_backed)
		return rknpu_gem_mmap_pages(rknpu_obj, vma);
#endif

	ret = dma_mmap_attrs(drm->dev, vma, rknpu_obj->cookie,
			     rknpu_obj->dma_addr, rknpu_obj->size,
			     rknpu_obj->dma_attrs);
	if (ret < 0) {
		LOG_DEV_ERROR(drm->dev, "failed to mmap, ret: %d\n", ret);
		return ret;
	}

	return 0;
}

void rknpu_gem_free_object(struct drm_gem_object *obj)
{
	struct rknpu_device *rknpu_dev = obj->dev->dev_private;

	rknpu_power_get(rknpu_dev);
	rknpu_gem_object_destroy(to_rknpu_obj(obj));
	rknpu_power_put_delay(rknpu_dev);
}

int rknpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *drm,
			  struct drm_mode_create_dumb *args)
{
	struct rknpu_device *rknpu_dev = drm->dev_private;
	struct rknpu_gem_object *rknpu_obj = NULL;
	unsigned int flags = 0;
	int domain_id = 0;
	int ret = -EINVAL;

	/*
	 * allocate memory to be used for framebuffer.
	 * - this callback would be called by user application
	 *	with DRM_IOCTL_MODE_CREATE_DUMB command.
	 */
	args->pitch = args->width * ((args->bpp + 7) / 8);
	args->size = args->pitch * args->height;

	if (rknpu_dev->iommu_en)
		flags = RKNPU_MEM_NON_CONTIGUOUS | RKNPU_MEM_WRITE_COMBINE;
	else
		flags = RKNPU_MEM_CONTIGUOUS | RKNPU_MEM_WRITE_COMBINE;

	domain_id = 0;

	if (file_priv && file_priv->driver_priv) {
		struct rknpu_file_priv *fpriv = file_priv->driver_priv;

		if (fpriv->domain_id > 0)
			domain_id = fpriv->domain_id;
	}

	rknpu_obj = rknpu_gem_object_create(drm, flags, args->size, 0, domain_id, 0);
	if (IS_ERR(rknpu_obj)) {
		LOG_DEV_ERROR(drm->dev, "gem object allocate failed.\n");
		return PTR_ERR(rknpu_obj);
	}

	ret = rknpu_gem_handle_create(&rknpu_obj->base, file_priv,
				      &args->handle);
	if (ret) {
		/* Published DMA tokens may have transient lookup references. */
		rknpu_gem_object_put(&rknpu_obj->base);
		return ret;
	}
	rknpu_gem_object_put(&rknpu_obj->base);

	return 0;
}

vm_fault_t rknpu_gem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_object *obj = vma->vm_private_data;
	struct rknpu_gem_object *rknpu_obj = to_rknpu_obj(obj);
	struct drm_device *drm = rknpu_obj->base.dev;
	unsigned long pfn = 0;
	pgoff_t page_offset = 0;

	page_offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;

	if (page_offset >= (rknpu_obj->size >> PAGE_SHIFT)) {
		LOG_DEV_ERROR(drm->dev, "invalid page offset\n");
		return VM_FAULT_SIGBUS;
	}

	pfn = page_to_pfn(rknpu_obj->pages[page_offset]);
	return vmf_insert_pfn(vma, vmf->address, pfn);
}

int rknpu_gem_mmap_obj(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct rknpu_gem_object *rknpu_obj = to_rknpu_obj(obj);
	int ret = -EINVAL;

	LOG_DEBUG("flags: %#x\n", rknpu_obj->flags);

	/* non-cacheable as default. */
	if (rknpu_obj->flags & RKNPU_MEM_CACHEABLE) {
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	} else if (rknpu_obj->flags & RKNPU_MEM_WRITE_COMBINE) {
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	} else {
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));
	}

	ret = rknpu_gem_mmap_buffer(rknpu_obj, vma);
	if (ret)
		goto err_close_vm;

	return 0;

err_close_vm:
	drm_gem_vm_close(vma);

	return ret;
}

int rknpu_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_gem_object *obj = NULL;
	int ret = -EINVAL;

	/* set vm_area_struct. */
	ret = drm_gem_mmap(filp, vma);
	if (ret < 0) {
		LOG_ERROR("failed to mmap, ret: %d\n", ret);
		return ret;
	}

	obj = vma->vm_private_data;

	if (obj->import_attach)
		return dma_buf_mmap(obj->dma_buf, vma, 0);

	return rknpu_gem_mmap_obj(obj, vma);
}

/* low-level interface prime helpers */
struct drm_gem_object *rknpu_gem_prime_import(struct drm_device *dev,
					      struct dma_buf *dma_buf)
{
	return drm_gem_prime_import_dev(dev, dma_buf, dev->dev);
}

struct sg_table *rknpu_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct rknpu_gem_object *rknpu_obj = to_rknpu_obj(obj);
	int npages = 0;

	npages = rknpu_obj->size >> PAGE_SHIFT;

	return drm_prime_pages_to_sg(obj->dev, rknpu_obj->pages, npages);
}

struct drm_gem_object *
rknpu_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt)
{
	struct rknpu_gem_object *rknpu_obj = NULL;
	unsigned int mapped_nents;
	int npages = 0;
	int ret = -EINVAL;

	rknpu_obj = rknpu_gem_init(dev, PAGE_ALIGN(attach->dmabuf->size), 0);
	if (IS_ERR(rknpu_obj)) {
		ret = PTR_ERR(rknpu_obj);
		return ERR_PTR(ret);
	}

	if (!sgt || !sgt->sgl || !sgt->orig_nents || !sgt->nents ||
	    sgt->nents > sgt->orig_nents || sgt->nents > INT_MAX) {
		ret = -EINVAL;
		goto err;
	}

	mapped_nents = sgt->nents;
	if (!rknpu_dma_sg_is_contiguous(sgt->sgl, mapped_nents,
					 rknpu_obj->size)) {
		ret = -ERANGE;
		LOG_DEV_ERROR(dev->dev,
			      "%s: imported DMA buffer is not one contiguous device range\n",
			      __func__);
		goto err;
	}

	rknpu_obj->dma_addr = sg_dma_address(sgt->sgl);
	rknpu_obj->dma_nents = mapped_nents;

	if ((rknpu_obj->size >> PAGE_SHIFT) > INT_MAX) {
		ret = -E2BIG;
		goto err;
	}
	npages = rknpu_obj->size >> PAGE_SHIFT;
	rknpu_obj->pages = rknpu_gem_alloc_page(npages);
	if (!rknpu_obj->pages) {
		ret = -ENOMEM;
		goto err;
	}

	ret = drm_prime_sg_to_page_array(sgt, rknpu_obj->pages, npages);
	if (ret < 0)
		goto err_free_large;

	rknpu_obj->sgt = sgt;
	rknpu_obj->num_pages = npages;

	if (sgt->nents == 1) {
		/* always physically continuous memory if sgt->nents is 1. */
		rknpu_obj->flags |= RKNPU_MEM_CONTIGUOUS;
	} else {
		/* Multiple nents treated as non-contiguous */
		rknpu_obj->flags |= RKNPU_MEM_NON_CONTIGUOUS;
	}

	return &rknpu_obj->base;

err_free_large:
	rknpu_gem_free_page(rknpu_obj->pages);
err:
	rknpu_gem_release(rknpu_obj);
	return ERR_PTR(ret);
}

int rknpu_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct rknpu_gem_object *rknpu_obj = to_rknpu_obj(obj);
	void *vaddr = NULL;

	if (!rknpu_obj->pages)
		return -EINVAL;

	vaddr = vmap(rknpu_obj->pages, rknpu_obj->num_pages, VM_MAP,
		     PAGE_KERNEL);
	if (!vaddr)
		return -ENOMEM;

	iosys_map_set_vaddr(map, vaddr);

	return 0;
}

void rknpu_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct rknpu_gem_object *rknpu_obj = to_rknpu_obj(obj);

	if (rknpu_obj->pages) {
		vunmap(map->vaddr);
		map->vaddr = NULL;
	}
}

int rknpu_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	int ret = -EINVAL;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret < 0)
		return ret;

	return rknpu_gem_mmap_obj(obj, vma);
}

/**
 * rknpu_gem_core_mapping() - Query one core-specific mapping.
 *
 * The DRM ABI intentionally exposes only one canonical dma_addr. This
 * helper is for kernel-internal debugging and tests that must verify that
 * each selected core received its own IOMMU mapping.
 */
int rknpu_gem_core_mapping(struct rknpu_gem_object *rknpu_obj,
			   unsigned int core, dma_addr_t *dma_addr)
{
	if (!rknpu_obj || core >= RKNPU_GEM_MAX_CORE_MAPS || !dma_addr)
		return -EINVAL;

	if (!rknpu_obj->core_maps[core].mapped)
		return -ENOENT;

	*dma_addr = rknpu_obj->core_maps[core].dma_addr;
	return 0;
}
EXPORT_SYMBOL_GPL(rknpu_gem_core_mapping);

/*
 * These private-IOMMU SG tables describe physical pages, not dma_map_sg()
 * mappings. Keep the existing SG cache-maintenance API: passing their NPU
 * IOVAs to dma_sync_single_range_* would use the wrong DMA domain.
 * Clip into a small stack batch; never mutate the shared object's SG table.
 * The DMA backend handles cache-line edges and the completion barriers.
 */
static int rknpu_gem_sync_sg_range(struct device *dev, struct sg_table *sgt,
				  unsigned long offset, unsigned long size,
				  unsigned int flags)
{
	struct scatterlist range[16], *sg;
	unsigned int count = 0;
	int i;

	sg_init_table(range, ARRAY_SIZE(range));
	for_each_sgtable_sg(sgt, sg, i) {
		unsigned long start, length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		}
		length = min_t(unsigned long, size, sg->length - offset);
		start = sg->offset + offset;
		sg_set_page(&range[count],
			    pfn_to_page(page_to_pfn(sg_page(sg)) +
					(start >> PAGE_SHIFT)),
			    length, offset_in_page(start));
		sg_dma_address(&range[count]) = sg_phys(&range[count]);
		sg_dma_len(&range[count]) = length;
		count++;
		size -= length;
		offset = 0;
		if (count == ARRAY_SIZE(range) || !size) {
			sg_mark_end(&range[count - 1]);
			if (flags & RKNPU_MEM_SYNC_TO_DEVICE)
				dma_sync_sg_for_device(dev, range, count,
						       DMA_BIDIRECTIONAL);
			if (flags & RKNPU_MEM_SYNC_FROM_DEVICE)
				dma_sync_sg_for_cpu(dev, range, count,
						    DMA_BIDIRECTIONAL);
			if (!size)
				return 0;
			count = 0;
			sg_init_table(range, ARRAY_SIZE(range));
		}
	}
	/* The object's backing must cover every validated request. */
	return -EIO;
}

static int rknpu_gem_sync_core_maps(struct rknpu_gem_object *rknpu_obj,
			      unsigned int flags, unsigned long offset,
			      unsigned long size)
{
	struct rknpu_device *rknpu_dev = rknpu_obj->base.dev->dev_private;
	unsigned int directions = !!(flags & RKNPU_MEM_SYNC_TO_DEVICE) +
				  !!(flags & RKNPU_MEM_SYNC_FROM_DEVICE);
	int core;

	for (core = 0; core < RKNPU_GEM_MAX_CORE_MAPS; core++) {
		struct rknpu_gem_core_map *map = &rknpu_obj->core_maps[core];
		struct device *core_dev;
		int ret;

		if (!map->mapped || !map->sgt)
			continue;

		core_dev = rknpu_gem_core_dev(rknpu_obj, core);
		if (!core_dev)
			continue;

		/*
		 * All core mappings share the same physical pages; syncing once
		 * performs the cache maintenance globally across all cores.
		 */
		ret = rknpu_gem_sync_sg_range(core_dev, map->sgt, offset, size, flags);
		if (ret) {
			LOG_DEV_ERROR(core_dev,
				      "GEM sync backing does not cover offset=%lu size=%lu\n",
				      offset, size);
			return ret;
		}
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_RANGE_CALLS, 1);
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_EFFECTIVE_BYTES,
				   (u64)size * directions);
		return 0;
	}
	LOG_DEV_ERROR(rknpu_dev->dev, "GEM sync has no usable core mapping\n");
	return -ENXIO;
}

int rknpu_gem_sync_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv)
{
	struct rknpu_mem_sync *args = data;
	struct rknpu_device *rknpu_dev = dev->dev_private;
	struct rknpu_gem_object *rknpu_obj;
	u64 start = unlikely(mem_profile) ? ktime_get_ns() : 0;
	int ret = 0;

	rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_CALLS, 1);

	rknpu_obj = rknpu_gem_object_find_token(dev, file_priv,
						args->obj_addr);
	if (!rknpu_obj) {
		ret = -ENOENT;
		goto out_stats;
	}
	if (rknpu_obj->base.dev != dev) {
		ret = -EINVAL;
		goto out_put;
	}

	if (!rknpu_gem_sync_is_valid(args->flags, args->offset, args->size,
				     rknpu_obj->size)) {
		ret = -EINVAL;
		goto out_put;
	}

	if (!(rknpu_obj->flags & RKNPU_MEM_CACHEABLE) &&
	    !rknpu_obj->base.import_attach) {
		ret = -EINVAL;
		goto out_put;
	}

	/* Reject cache sync if object is not mapped */
	if (!rknpu_obj->base.import_attach && !rknpu_obj->dma_addr) {
		LOG_DEV_ERROR(dev->dev,
			      "rejecting sync for unmapped GEM object size=%lu flags=%#x\n",
			      rknpu_obj->size, rknpu_obj->flags);
		ret = -ENXIO;
		goto out_put;
	}

	rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_REQUEST_BYTES,
			   args->size);
	if (rknpu_obj->base.import_attach) {
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_IMPORT_CALLS, 1);
		/* Pair begin and end CPU access for the complete DMA-BUF. */
		if (args->offset || args->size != rknpu_obj->size) {
			ret = -EOPNOTSUPP;
			goto out_put;
		}

		ret = dma_buf_begin_cpu_access(rknpu_obj->base.dma_buf,
					       DMA_BIDIRECTIONAL);
		if (ret)
			goto out_put;
		ret = dma_buf_end_cpu_access(rknpu_obj->base.dma_buf,
					     DMA_BIDIRECTIONAL);
	} else if (rknpu_obj->pages_backed) {
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_PAGES_CALLS, 1);
		if (args->offset || args->size != rknpu_obj->size)
			rknpu_mem_stat_add(rknpu_dev,
					   RKNPU_MEM_STAT_SYNC_PARTIAL_CALLS, 1);
		/* Both byte counters use the same per-direction accounting. */
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_PAGES_REQUEST_BYTES,
			args->size *
			(!!(args->flags & RKNPU_MEM_SYNC_TO_DEVICE) +
			 !!(args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)));
		/* Logical whole-buffer bytes per requested direction, not bus traffic. */
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_WHOLE_BYTES,
			rknpu_obj->size *
			(!!(args->flags & RKNPU_MEM_SYNC_TO_DEVICE) +
			 !!(args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)));
		/* Sync the mapped sg_table for cache maintenance. */
		ret = rknpu_gem_sync_core_maps(rknpu_obj, args->flags,
					     args->offset, args->size);
	} else {
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_OTHER_CALLS, 1);
		if (args->flags & RKNPU_MEM_SYNC_TO_DEVICE)
			dma_sync_single_range_for_device(
				dev->dev, rknpu_obj->dma_addr, args->offset,
				args->size, DMA_TO_DEVICE);
		if (args->flags & RKNPU_MEM_SYNC_FROM_DEVICE)
			dma_sync_single_range_for_cpu(
				dev->dev, rknpu_obj->dma_addr, args->offset,
				args->size, DMA_FROM_DEVICE);
	}

out_put:
	rknpu_gem_object_put(&rknpu_obj->base);
out_stats:
	if (ret)
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_ERRORS, 1);
	if (unlikely(mem_profile))
		rknpu_mem_stat_add(rknpu_dev, RKNPU_MEM_STAT_SYNC_NS,
				   ktime_get_ns() - start);
	return ret;
}

MODULE_IMPORT_NS("DMA_BUF");
