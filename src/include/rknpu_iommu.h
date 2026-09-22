/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __LINUX_RKNPU_IOMMU_H
#define __LINUX_RKNPU_IOMMU_H

#include <linux/scatterlist.h>

#include "rknpu_drv.h"

#include "rknpu_gem.h"

bool rknpu_dma_range_add(dma_addr_t addr, size_t len, bool first,
			 dma_addr_t *next, size_t *mapped);
bool rknpu_dma_sg_is_contiguous(struct scatterlist *sgl, int mapped_nents,
				 size_t size);

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev);
void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev);
int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id);
int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev);
int rknpu_iommu_domain_attach(struct rknpu_device *rknpu_dev,
			       unsigned int core_index, int domain_id,
			       struct rknpu_iommu_domain_ref *ref);
void rknpu_iommu_domain_detach(struct rknpu_device *rknpu_dev,
			       unsigned int core_index,
			       struct rknpu_iommu_domain_ref *ref);
void rknpu_iommu_detach_core(struct rknpu_device *rknpu_dev,
			     unsigned int core_index);
int rknpu_iommu_reserve_iova(struct rknpu_device *rknpu_dev, int domain_id,
			     size_t size, struct drm_mm_node *node,
			     dma_addr_t *iova);
void rknpu_iommu_release_iova(struct rknpu_device *rknpu_dev, int domain_id,
			      struct drm_mm_node *node);
int rknpu_iommu_map_core_sg(struct rknpu_device *rknpu_dev,
			    unsigned int core_index, int domain_id,
			    struct sg_table *sgt,
			    struct rknpu_gem_core_map *map, dma_addr_t iova);
void rknpu_iommu_unmap_core_sg(struct rknpu_device *rknpu_dev,
				unsigned int core_index, int domain_id,
				struct rknpu_gem_core_map *map);

#endif
