// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>

#include "rknpu_core.h"

#include "rknpu_iommu.h"

bool rknpu_dma_range_add(dma_addr_t addr, size_t len, bool first,
			 dma_addr_t *next, size_t *mapped)
{
	dma_addr_t new_next;
	size_t new_mapped;

	if (!next || !mapped || !len || (!first && addr != *next) ||
	    check_add_overflow(addr, len, &new_next) ||
	    check_add_overflow(*mapped, len, &new_mapped))
		return false;

	*next = new_next;
	*mapped = new_mapped;
	return true;
}

EXPORT_SYMBOL_GPL(rknpu_dma_range_add);

bool rknpu_dma_sg_is_contiguous(struct scatterlist *sgl, int mapped_nents,
				 size_t size)
{
	struct scatterlist *sg;
	dma_addr_t next = 0;
	size_t mapped = 0;
	int i;

	if (!sgl || mapped_nents <= 0 || !size)
		return false;

	for_each_sg(sgl, sg, mapped_nents, i) {
		dma_addr_t addr = sg_dma_address(sg);
		size_t len = sg_dma_len(sg);

		if (!rknpu_dma_range_add(addr, len, i == 0, &next, &mapped))
			return false;
		if (mapped >= size)
			return true;
	}

	return false;
}

EXPORT_SYMBOL_GPL(rknpu_dma_sg_is_contiguous);

/*
 * Lazily allocate the IOMMU domain for one (core, domain_id).
 * Caller must hold iommu_domain_lock.
 */
static struct iommu_domain *
rknpu_iommu_ensure_domain(struct rknpu_device *rknpu_dev,
			  unsigned int core_index, int domain_id)
{
	struct rknpu_core *rk_core;
	struct iommu_domain *domain;

	if (core_index >= RKNPU_MAX_CORES || domain_id < 0 ||
	    domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		return ERR_PTR(-EINVAL);

	domain = rknpu_dev->iommu_domains[core_index][domain_id];
	if (domain)
		return domain;

	rk_core = &rknpu_dev->cores[core_index];
	if (!rk_core->registered || !rk_core->dev)
		return ERR_PTR(-ENODEV);

	domain = iommu_paging_domain_alloc(rk_core->dev);
	if (IS_ERR(domain))
		return domain;

	rknpu_dev->iommu_domains[core_index][domain_id] = domain;
	return domain;
}

/*
 * Lazily initialise the IOVA allocator for a domain id.
 * Caller must hold iommu_shared_mm_lock.
 */
static int rknpu_iommu_ensure_pool(struct rknpu_device *rknpu_dev, int domain_id)
{
	struct rknpu_iova_pool *pool;
	u64 pool_start, pool_size;

	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		return -EINVAL;

	pool = &rknpu_dev->iommu_pools[domain_id];
	if (pool->ready)
		return 0;

	/*
	 * Start each pool at PAGE_SIZE (not aperture_start + PAGE_SIZE):
	 * IOVA 0 is kept unmapped as a NULL guard; above that every domain
	 * id has its own independent 4 GiB address space so there is no
	 * competition across ids.
	 */
	pool_start = PAGE_SIZE;
	pool_size  = rknpu_dev->iommu_shared_mm_end - pool_start;
	drm_mm_init(&pool->mm, pool_start, pool_size);
	pool->begin = pool_start;
	pool->end   = rknpu_dev->iommu_shared_mm_end;
	pool->ready = true;
	return 0;
}

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev)
{
	u64 aperture_start = 0, aperture_end = 0;
	int core, ret;

	mutex_init(&rknpu_dev->iommu_domain_lock);
	mutex_init(&rknpu_dev->iommu_shared_mm_lock);

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		rknpu_dev->iommu_active_domain[core] = -1;
		rknpu_dev->iommu_active_refcount[core] = 0;
		init_waitqueue_head(&rknpu_dev->iommu_active_wq[core]);
	}

	if (!rknpu_dev->iommu_en)
		return 0;

	/*
	 * Allocate domain id 0 for every registered core up front. It doubles
	 * as the aperture probe (all ids share the same window) and as the base
	 * domain librknnrt uses for buffers it does not tag. Ids 2..15 are
	 * allocated lazily on first use.
	 */
	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		struct rknpu_core *rk_core = &rknpu_dev->cores[core];
		struct iommu_domain *domain;

		if (!rk_core->registered)
			continue;

		domain = iommu_paging_domain_alloc(rk_core->dev);
		if (IS_ERR(domain)) {
			ret = PTR_ERR(domain);
			rknpu_iommu_free_domains(rknpu_dev);
			return ret;
		}
		rknpu_dev->iommu_domains[core][0] = domain;

		aperture_start = domain->geometry.aperture_start;
		aperture_end = domain->geometry.aperture_end;
		/*
		 * A given IOVA must be translatable by every core that shares
		 * the address (librknnrt reuses one dma_addr across cores), so
		 * each id's window is the intersection of the apertures. Each
		 * core's mapping is then created at exactly the allocated
		 * address.
		 */
		if (core == 0 || aperture_start > rknpu_dev->iommu_shared_mm_begin)
			rknpu_dev->iommu_shared_mm_begin = aperture_start;
		if (core == 0 || aperture_end < rknpu_dev->iommu_shared_mm_end)
			rknpu_dev->iommu_shared_mm_end = aperture_end;
	}

	/*
	 * Keep page zero unmapped as a NULL-address guard, so an object can
	 * never be reachable through a dma_addr of 0.
	 */
	if (rknpu_dev->iommu_shared_mm_end <=
	    rknpu_dev->iommu_shared_mm_begin + PAGE_SIZE) {
		rknpu_iommu_free_domains(rknpu_dev);
		return -ERANGE;
	}

	mutex_lock(&rknpu_dev->iommu_shared_mm_lock);
	ret = rknpu_iommu_ensure_pool(rknpu_dev, 0);
	mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);
	if (ret) {
		rknpu_iommu_free_domains(rknpu_dev);
		return ret;
	}
	rknpu_dev->iommu_shared_mm_ready = true;

	rknpu_dev->iommu_domain_id = 0;
	return 0;
}

int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id)
{
	/*
	 * Validate domain ID. Per-domain allocator selection and domain switching
	 * are handled by rknpu_iommu_reserve_iova() and rknpu_iommu_domain_attach().
	 */
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM) {
		dev_warn_ratelimited(
			rknpu_dev->dev,
			"RKNPU: IOMMU domain %d is out of range (0..%d)\n",
			domain_id, RKNPU_MAX_IOMMU_DOMAIN_NUM - 1);
		return -EINVAL;
	}

	if (domain_id && !rknpu_dev->iommu_en)
		dev_warn_ratelimited(
			rknpu_dev->dev,
			"RKNPU: IOMMU domain %d accepted without an IOMMU; addresses stay physical\n",
			domain_id);

	return 0;
}

int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev)
{
	return 0;
}

void rknpu_iommu_detach_core(struct rknpu_device *rknpu_dev,
			     unsigned int core_index)
{
	struct rknpu_core *rk_core;
	int active;

	if (!rknpu_dev || core_index >= RKNPU_MAX_CORES || !rknpu_dev->iommu_en)
		return;

	rk_core = &rknpu_dev->cores[core_index];
	mutex_lock(&rknpu_dev->iommu_domain_lock);
	active = rknpu_dev->iommu_active_domain[core_index];
	if (active >= 0 && active < RKNPU_MAX_IOMMU_DOMAIN_NUM &&
	    rk_core->group && rknpu_dev->iommu_domains[core_index][active]) {
		iommu_detach_group(rknpu_dev->iommu_domains[core_index][active],
				   rk_core->group);
	}
	rknpu_dev->iommu_active_domain[core_index] = -1;
	rknpu_dev->iommu_active_refcount[core_index] = 0;
	mutex_unlock(&rknpu_dev->iommu_domain_lock);
}

void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev)
{
	int core, d;

	for (core = 0; core < RKNPU_MAX_CORES; core++)
		rknpu_iommu_detach_core(rknpu_dev, core);

	/*
	 * Defensive cleanup: if any GEM buffer leaked (e.g. from an error path
	 * or abnormal exit), sweep all active pools and unmap their remaining
	 * nodes from every core's domain BEFORE iommu_domain_free() destroys
	 * the domains and drm_mm_takedown() asserts clean allocators.
	 */
	if (rknpu_dev->iommu_shared_mm_ready) {
		for (d = 0; d < RKNPU_MAX_IOMMU_DOMAIN_NUM; d++) {
			struct drm_mm_node *node, *next;

			if (!rknpu_dev->iommu_pools[d].ready)
				continue;

			mutex_lock(&rknpu_dev->iommu_shared_mm_lock);
			drm_mm_for_each_node_safe(node, next,
						  &rknpu_dev->iommu_pools[d].mm) {
				LOG_DEV_WARN(
					rknpu_dev->dev,
					"stale IOVA node in domain %d: start=%#llx size=%#llx (cleaning up)\n",
					d, (unsigned long long)node->start,
					(unsigned long long)node->size);
				for (core = 0; core < RKNPU_MAX_CORES; core++) {
					if (rknpu_dev->iommu_domains[core][d])
						iommu_unmap(
							rknpu_dev->iommu_domains[core][d],
							node->start,
							node->size);
				}
				drm_mm_remove_node(node);
			}
			mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);
		}
	}

	for (core = 0; core < RKNPU_MAX_CORES; core++) {
		for (d = 0; d < RKNPU_MAX_IOMMU_DOMAIN_NUM; d++) {
			if (!rknpu_dev->iommu_domains[core][d])
				continue;
			iommu_domain_free(rknpu_dev->iommu_domains[core][d]);
			rknpu_dev->iommu_domains[core][d] = NULL;
		}
	}

	if (rknpu_dev->iommu_shared_mm_ready) {
		for (d = 0; d < RKNPU_MAX_IOMMU_DOMAIN_NUM; d++) {
			if (!rknpu_dev->iommu_pools[d].ready)
				continue;
			drm_mm_takedown(&rknpu_dev->iommu_pools[d].mm);
			rknpu_dev->iommu_pools[d].ready = false;
		}
		mutex_destroy(&rknpu_dev->iommu_shared_mm_lock);
		rknpu_dev->iommu_shared_mm_ready = false;
	}
}

/*
 * Log per-domain IOVA usage. Caller must hold iommu_shared_mm_lock.
 */
static void rknpu_iommu_log_domain_footprint(struct rknpu_device *rknpu_dev)
{
	char buf[256];
	int len = 0;
	int d;

	for (d = 0; d < RKNPU_MAX_IOMMU_DOMAIN_NUM; d++) {
		u64 peak = rknpu_dev->iommu_domain_peak_bytes[d];

		if (!peak)
			continue;
		len += scnprintf(buf + len, sizeof(buf) - len,
				 " d%d=%llu/%lluMiB", d,
				 rknpu_dev->iommu_domain_live_bytes[d] >> 20,
				 peak >> 20);
		if (len >= (int)sizeof(buf) - 24)
			break;
	}
	LOG_DEV_DBG(rknpu_dev->dev, "IOVA per-domain (live/peak):%s\n",
		     len ? buf : " (none)");
}

/**
 * rknpu_iommu_reserve_iova() - Reserve an IOVA in the object's domain pool.
 * @rknpu_dev: rknpu device
 * @domain_id: domain pool index
 * @size: allocation size in bytes
 * @node: returns drm_mm allocation node
 * @iova: returns reserved IOVA address
 */
int rknpu_iommu_reserve_iova(struct rknpu_device *rknpu_dev, int domain_id,
			     size_t size, struct drm_mm_node *node,
			     dma_addr_t *iova)
{
	struct rknpu_iova_pool *pool;
	int ret;

	if (!rknpu_dev || !node || !iova || !size)
		return -EINVAL;
	if (!rknpu_dev->iommu_en || !rknpu_dev->iommu_shared_mm_ready)
		return -ENODEV;
	/*
	 * Clamp out-of-range ids to 0. domain_get_and_switch already rejected
	 * out-of-range ids at MEM_CREATE, so a stored object id is always
	 * valid; this is defence in depth and keeps reserve/release/map/unmap
	 * selecting the same pool for a given object.
	 */
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;

	memset(node, 0, sizeof(*node));
	mutex_lock(&rknpu_dev->iommu_shared_mm_lock);
	ret = rknpu_iommu_ensure_pool(rknpu_dev, domain_id);
	if (ret) {
		mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);
		return ret;
	}
	pool = &rknpu_dev->iommu_pools[domain_id];
	ret = drm_mm_insert_node_generic(&pool->mm, node, size, PAGE_SIZE, 0, 0);
	if (ret) {
		u64 pool_window = pool->end - pool->begin;

		LOG_DEV_ERROR(
			rknpu_dev->dev,
			"IOVA reserve failed: size=%zu domain=%d ret=%d; domain window=%llu MiB [%#llx..%#llx], domain live=%llu MiB over %llu objs, peak=%llu MiB; device live=%llu MiB/%llu objs peak=%llu MiB/%llu objs reserves=%llu releases=%llu\n",
			size, domain_id, ret, pool_window >> 20,
			pool->begin, pool->end,
			rknpu_dev->iommu_domain_live_bytes[domain_id] >> 20,
			rknpu_dev->iommu_domain_live_objs[domain_id],
			rknpu_dev->iommu_domain_peak_bytes[domain_id] >> 20,
			rknpu_dev->iommu_live_bytes >> 20,
			rknpu_dev->iommu_live_objs,
			rknpu_dev->iommu_peak_bytes >> 20,
			rknpu_dev->iommu_peak_objs,
			rknpu_dev->iommu_total_reserves,
			rknpu_dev->iommu_total_releases);
		rknpu_iommu_log_domain_footprint(rknpu_dev);
		mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);
		return ret;
	}
	rknpu_dev->iommu_live_bytes += size;
	rknpu_dev->iommu_live_objs++;
	rknpu_dev->iommu_total_reserves++;
	if (rknpu_dev->iommu_live_bytes > rknpu_dev->iommu_peak_bytes)
		rknpu_dev->iommu_peak_bytes = rknpu_dev->iommu_live_bytes;
	if (rknpu_dev->iommu_live_objs > rknpu_dev->iommu_peak_objs)
		rknpu_dev->iommu_peak_objs = rknpu_dev->iommu_live_objs;
	rknpu_dev->iommu_domain_live_bytes[domain_id] += size;
	rknpu_dev->iommu_domain_live_objs[domain_id]++;
	if (rknpu_dev->iommu_domain_live_bytes[domain_id] >
	    rknpu_dev->iommu_domain_peak_bytes[domain_id])
		rknpu_dev->iommu_domain_peak_bytes[domain_id] =
			rknpu_dev->iommu_domain_live_bytes[domain_id];
	/* Periodic debug logging to monitor IOVA allocations without flooding kmsg. */
	if (rknpu_dev->iommu_total_reserves % 512 == 0) {
		LOG_DEV_DBG(
			rknpu_dev->dev,
			"IOVA acct: live=%llu MiB/%llu objs peak=%llu MiB/%llu objs reserves=%llu releases=%llu\n",
			rknpu_dev->iommu_live_bytes >> 20,
			rknpu_dev->iommu_live_objs,
			rknpu_dev->iommu_peak_bytes >> 20,
			rknpu_dev->iommu_peak_objs,
			rknpu_dev->iommu_total_reserves,
			rknpu_dev->iommu_total_releases);
		rknpu_iommu_log_domain_footprint(rknpu_dev);
	}
	mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);

	*iova = node->start;
	return 0;
}

void rknpu_iommu_release_iova(struct rknpu_device *rknpu_dev, int domain_id,
			      struct drm_mm_node *node)
{
	u64 size;

	if (!rknpu_dev || !node || !rknpu_dev->iommu_en ||
	    !rknpu_dev->iommu_shared_mm_ready)
		return;
	if (!drm_mm_node_allocated(node))
		return;
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;

	mutex_lock(&rknpu_dev->iommu_shared_mm_lock);
	size = node->size;
	drm_mm_remove_node(node);
	if (rknpu_dev->iommu_live_bytes >= size)
		rknpu_dev->iommu_live_bytes -= size;
	else
		rknpu_dev->iommu_live_bytes = 0;
	if (rknpu_dev->iommu_live_objs)
		rknpu_dev->iommu_live_objs--;
	if (rknpu_dev->iommu_domain_live_bytes[domain_id] >= size)
		rknpu_dev->iommu_domain_live_bytes[domain_id] -= size;
	else
		rknpu_dev->iommu_domain_live_bytes[domain_id] = 0;
	if (rknpu_dev->iommu_domain_live_objs[domain_id])
		rknpu_dev->iommu_domain_live_objs[domain_id]--;
	rknpu_dev->iommu_total_releases++;
	if (rknpu_dev->iommu_total_releases % 512 == 0)
		LOG_DEV_DBG(
			rknpu_dev->dev,
			"IOVA acct: live=%llu MiB/%llu objs peak=%llu MiB/%llu objs reserves=%llu releases=%llu\n",
			rknpu_dev->iommu_live_bytes >> 20,
			rknpu_dev->iommu_live_objs,
			rknpu_dev->iommu_peak_bytes >> 20,
			rknpu_dev->iommu_peak_objs,
			rknpu_dev->iommu_total_reserves,
			rknpu_dev->iommu_total_releases);
	mutex_unlock(&rknpu_dev->iommu_shared_mm_lock);

	memset(node, 0, sizeof(*node));
}

int rknpu_iommu_map_core_sg(struct rknpu_device *rknpu_dev,
			    unsigned int core_index, int domain_id,
			    struct sg_table *sgt,
			    struct rknpu_gem_core_map *map, dma_addr_t iova)
{
	struct iommu_domain *domain;
	struct scatterlist *sg;
	size_t size;
	ssize_t mapped;
	int i;

	if (!rknpu_dev || !sgt || !map || core_index >= RKNPU_MAX_CORES)
		return -EINVAL;
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;

	/*
	 * Map into this id's domain for this core (lazily allocated). The
	 * mapping is installed whether or not the domain is currently attached
	 * to the group: it edits that domain's page tables directly, and the
	 * refcount serialisation guarantees no job is translating through it on
	 * this core while it changes.
	 */
	mutex_lock(&rknpu_dev->iommu_domain_lock);
	domain = rknpu_iommu_ensure_domain(rknpu_dev, core_index, domain_id);
	mutex_unlock(&rknpu_dev->iommu_domain_lock);
	if (IS_ERR(domain))
		return PTR_ERR(domain);

	size = 0;
	for_each_sgtable_sg(sgt, sg, i)
		size += sg->length;
	if (!size)
		return -EINVAL;

	/*
	 * Map at the caller's address. It was reserved from this id's pool, so
	 * it is unique within the id and inside every core's aperture.
	 */
	mapped = iommu_map_sgtable(domain, iova, sgt,
				   IOMMU_READ | IOMMU_WRITE);
	if (mapped < 0 || mapped < size) {
		if (mapped > 0)
			iommu_unmap(domain, iova, mapped);
		return mapped < 0 ? (int)mapped : -ENOMEM;
	}

	map->dma_addr = iova;
	map->mapped_size = size;

	return 0;
}

void rknpu_iommu_unmap_core_sg(struct rknpu_device *rknpu_dev,
				unsigned int core_index, int domain_id,
				struct rknpu_gem_core_map *map)
{
	struct iommu_domain *domain;

	if (!rknpu_dev || !map || core_index >= RKNPU_MAX_CORES)
		return;

	if (!map->dma_addr)
		return;
	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;

	domain = rknpu_dev->iommu_domains[core_index][domain_id];
	if (domain)
		iommu_unmap(domain, map->dma_addr, map->mapped_size);

	map->dma_addr = 0;
	map->mapped_size = 0;
}

/**
 * rknpu_iommu_domain_attach() - Bind a core's IOMMU group to a job's domain id.
 *
 * Refcounts active domain usage per core. Switching domains waits for the
 * active refcount to reach zero before attaching the new domain.
 */
int rknpu_iommu_domain_attach(struct rknpu_device *rknpu_dev,
			       unsigned int core_index, int domain_id,
			       struct rknpu_iommu_domain_ref *ref)
{
	struct rknpu_core *core;
	struct iommu_domain *domain;
	int active;
	int ret;

	if (!rknpu_dev || !ref || core_index >= RKNPU_MAX_CORES)
		return -EINVAL;

	memset(ref, 0, sizeof(*ref));

	if (!rknpu_dev->iommu_en)
		return 0;

	if (domain_id < 0 || domain_id >= RKNPU_MAX_IOMMU_DOMAIN_NUM)
		domain_id = 0;

	core = &rknpu_dev->cores[core_index];
	if (!core->registered || !core->group)
		return -ENODEV;

	/* Mutex cannot be taken in hard IRQ */
	if (in_interrupt()) {
		dev_warn_ratelimited(
			rknpu_dev->dev,
			"RKNPU: IOMMU attach (core %u domain %d) refused in IRQ context\n",
			core_index, domain_id);
		return -EBUSY;
	}

	mutex_lock(&rknpu_dev->iommu_domain_lock);
	domain = rknpu_iommu_ensure_domain(rknpu_dev, core_index, domain_id);
	if (IS_ERR(domain)) {
		ret = PTR_ERR(domain);
		mutex_unlock(&rknpu_dev->iommu_domain_lock);
		return ret;
	}

	active = rknpu_dev->iommu_active_domain[core_index];
	while (active != domain_id && rknpu_dev->iommu_active_refcount[core_index] > 0) {
		int wait_ret;

		mutex_unlock(&rknpu_dev->iommu_domain_lock);
		wait_ret = wait_event_interruptible_timeout(
			rknpu_dev->iommu_active_wq[core_index],
			READ_ONCE(rknpu_dev->iommu_active_refcount[core_index]) == 0 ||
			READ_ONCE(rknpu_dev->shutting_down),
			msecs_to_jiffies(3000));
		if (wait_ret <= 0) {
			dev_warn_ratelimited(
				rknpu_dev->dev,
				"RKNPU: core %u timed out waiting for domain switch (%d -> %d)\n",
				core_index, active, domain_id);
			return wait_ret == 0 ? -ETIMEDOUT : wait_ret;
		}
		if (READ_ONCE(rknpu_dev->shutting_down))
			return -ESHUTDOWN;

		mutex_lock(&rknpu_dev->iommu_domain_lock);
		active = rknpu_dev->iommu_active_domain[core_index];
	}

	if (active == domain_id) {
		/* Fast path: the id is already attached to this core. */
		rknpu_dev->iommu_active_refcount[core_index]++;
	} else {
		/*
		 * Refcount is zero. Either nothing is attached (active == -1)
		 * or a previous id is still sticky-attached. Detach the stale
		 * one, then attach the requested id.
		 */
		if (active >= 0 && active < RKNPU_MAX_IOMMU_DOMAIN_NUM &&
		    rknpu_dev->iommu_domains[core_index][active])
			iommu_detach_group(
				rknpu_dev->iommu_domains[core_index][active],
				core->group);

		ret = iommu_attach_group(domain, core->group);
		if (ret) {
			rknpu_dev->iommu_active_domain[core_index] = -1;
			mutex_unlock(&rknpu_dev->iommu_domain_lock);
			return ret;
		}
		rknpu_dev->iommu_active_domain[core_index] = domain_id;
		rknpu_dev->iommu_active_refcount[core_index] = 1;
	}

	ref->domain = domain;
	ref->group = core->group;
	mutex_unlock(&rknpu_dev->iommu_domain_lock);

	return 0;
}

void rknpu_iommu_domain_detach(struct rknpu_device *rknpu_dev,
			       unsigned int core_index,
			       struct rknpu_iommu_domain_ref *ref)
{
	if (!rknpu_dev || !ref || core_index >= RKNPU_MAX_CORES)
		return;

	mutex_lock(&rknpu_dev->iommu_domain_lock);
	/*
	 * Drop domain refcount under lock. The group remains sticky-attached
	 * to allow fast reuse by the next job with the same domain ID.
	 */
	if (ref->domain) {
		if (rknpu_dev->iommu_active_refcount[core_index] > 0)
			rknpu_dev->iommu_active_refcount[core_index]--;
		ref->domain = NULL;
		ref->group = NULL;
		wake_up(&rknpu_dev->iommu_active_wq[core_index]);
	}
	mutex_unlock(&rknpu_dev->iommu_domain_lock);
}
