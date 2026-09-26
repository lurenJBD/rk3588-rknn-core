/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#ifndef __LINUX_RKNPU_GEM_H
#define __LINUX_RKNPU_GEM_H

#include <linux/errno.h>
#include <linux/mm_types.h>
#include <linux/version.h>

#include <drm/drm_device.h>
#include <drm/drm_mm.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_gem.h>
#include <drm/drm_mode.h>



#include "rknpu_ioctl.h"
#include "rknpu_mm.h"

#define to_rknpu_obj(x) container_of(x, struct rknpu_gem_object, base)

#define RKNPU_GEM_MAX_CORE_MAPS 3

struct seq_file;
struct rknpu_device;
int rknpu_gem_mem_stats_show(struct seq_file *m, struct rknpu_device *rknpu_dev);

struct rknpu_gem_core_map {
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	size_t mapped_size;
	bool mapped;
};

/*
 * rknpu drm buffer structure.
 *
 * @base: a gem object.
 *	- a new handle to this gem object would be created
 *	by drm_gem_handle_create().
 * @flags: indicate memory type to allocated buffer and cache attribute.
 * @size: size requested from user, in bytes and this size is aligned
 *	in page unit.
 * @cookie: cookie returned by dma_alloc_attrs
 * @kv_addr: kernel virtual address to allocated memory region.
 * @dma_addr: bus address(accessed by dma) to allocated memory region.
 *	- this address could be physical address without IOMMU and
 *	device address with IOMMU.
 * @pages: Array of backing pages.
 * @sgt: Imported sg_table.
 *
 * P.S. this object would be transferred to user as kms_bo.handle so
 *	user can access the buffer through kms_bo.handle.
 */
struct rknpu_gem_object {
	struct drm_gem_object base;
	unsigned int flags;
	unsigned long size;
	void *cookie;
	void __iomem *kv_addr;
	dma_addr_t dma_addr;
	unsigned long dma_attrs;
	unsigned long num_pages;
	struct page **pages;
	struct sg_table *sgt;
	unsigned int dma_nents;
	bool dma_allocated;
	bool dma_mapped;
	bool pages_backed;
	/*
	 * Mainline three-node topology: each RKNN core has a separate IOMMU
	 * domain. Keep one mapping per requested core and expose the core-0
	 * mapping as the runtime-facing canonical dma_addr.
	 */
	unsigned int core_mask;
	struct rknpu_gem_core_map core_maps[RKNPU_GEM_MAX_CORE_MAPS];
	/*
	 * One IOVA reservation for the whole object, shared by every core's
	 * mapping. librknnrt keeps a single dma_addr per buffer and reuses it
	 * whichever core it later submits to, so all cores must translate the
	 * same address (see the comment in struct rknpu_device).
	 */
	struct drm_mm_node iova_node;
	dma_addr_t iova_base;
	/* The IOMMU domain ID requested at MEM_CREATE */
	int iommu_domain_id;
};

static inline int rknpu_gem_validate_flags(unsigned int flags)
{
	if (flags & ~RKNPU_MEM_MASK)
		return -EINVAL;

	return 0;
}

static inline bool rknpu_gem_token_is_valid(u64 token)
{
	return token && token <= U32_MAX;
}

static inline bool rknpu_gem_sync_is_valid(u32 flags, u64 offset, u64 size,
					    size_t object_size)
{
	if (!flags || (flags & ~RKNPU_MEM_SYNC_MASK) || !size)
		return false;

	return offset <= object_size && size <= object_size - offset;
}

/* create a new buffer with gem object */
struct rknpu_gem_object *
rknpu_gem_object_create(struct drm_device *dev, unsigned int flags,
			unsigned long size, unsigned long sram_size,
			int iommu_domain_id, unsigned int core_mask);

/* destroy a buffer with gem object */
void rknpu_gem_object_destroy(struct rknpu_gem_object *rknpu_obj);

/* request gem object creation and buffer allocation as the size */
int rknpu_gem_create_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file_priv);

/* get fake-offset of gem object that can be used with mmap. */
int rknpu_gem_map_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);

int rknpu_gem_destroy_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file_priv);

/*
 * get rknpu drm object,
 * gem object reference count would be increased.
 */
static inline void rknpu_gem_object_get(struct drm_gem_object *obj)
{
	drm_gem_object_get(obj);
}

/*
 * put rknpu drm object acquired from rknpu_gem_object_find() or rknpu_gem_object_get(),
 * gem object reference count would be decreased.
 */
static inline void rknpu_gem_object_put(struct drm_gem_object *obj)
{
	drm_gem_object_put(obj);
}

/*
 * Get an RKNPU GEM object from a handle. The returned object retains the
 * lookup reference and must be released with rknpu_gem_object_put().
 */
struct rknpu_gem_object *
rknpu_gem_object_find(struct drm_file *filp, unsigned int handle);

struct rknpu_gem_object *
rknpu_gem_object_find_by_dma(struct drm_device *drm, struct drm_file *file_priv,
			     dma_addr_t dma_addr);
bool rknpu_gem_object_is_file_handle(struct drm_file *file_priv,
				      struct rknpu_gem_object *rknpu_obj);

struct rknpu_gem_object *
rknpu_gem_object_find_token(struct drm_device *drm, struct drm_file *file_priv,
			    u64 token);

int rknpu_gem_core_mapping(struct rknpu_gem_object *rknpu_obj,
			   unsigned int core, dma_addr_t *dma_addr);

/* get buffer information to memory region allocated by gem. */
int rknpu_gem_get_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);

/* free gem object. */
void rknpu_gem_free_object(struct drm_gem_object *obj);

/* create memory region for drm framebuffer. */
int rknpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev,
			  struct drm_mode_create_dumb *args);

/* page fault handler and mmap fault address(virtual) to physical memory. */
vm_fault_t rknpu_gem_fault(struct vm_fault *vmf);

int rknpu_gem_mmap_obj(struct drm_gem_object *obj, struct vm_area_struct *vma);

/* set vm_flags and we can change the vm attribute to other one at here. */
int rknpu_gem_mmap(struct file *filp, struct vm_area_struct *vma);

/* low-level interface prime helpers */
struct drm_gem_object *rknpu_gem_prime_import(struct drm_device *dev,
					      struct dma_buf *dma_buf);
struct sg_table *rknpu_gem_prime_get_sg_table(struct drm_gem_object *obj);
struct drm_gem_object *
rknpu_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt);
int rknpu_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map);
void rknpu_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map);
int rknpu_gem_prime_mmap(struct drm_gem_object *obj,
			 struct vm_area_struct *vma);

int rknpu_gem_sync_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv);

static inline void *rknpu_gem_alloc_page(size_t nr_pages)
{
	return kvmalloc_array(nr_pages, sizeof(struct page *),
			      GFP_KERNEL | __GFP_ZERO);
}

static inline void rknpu_gem_free_page(void *pages)
{
	kvfree(pages);
}

#endif
