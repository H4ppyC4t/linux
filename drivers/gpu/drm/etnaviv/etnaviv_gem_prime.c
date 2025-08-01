// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014-2018 Etnaviv Project
 */

#include <drm/drm_prime.h>
#include <linux/dma-buf.h>
#include <linux/module.h>

#include "etnaviv_drv.h"
#include "etnaviv_gem.h"

MODULE_IMPORT_NS("DMA_BUF");

static struct lock_class_key etnaviv_prime_lock_class;

struct sg_table *etnaviv_gem_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);
	unsigned int npages = obj->size >> PAGE_SHIFT;

	if (WARN_ON(!etnaviv_obj->pages))  /* should have already pinned! */
		return ERR_PTR(-EINVAL);

	return drm_prime_pages_to_sg(obj->dev, etnaviv_obj->pages, npages);
}

int etnaviv_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	void *vaddr;

	vaddr = etnaviv_gem_vmap(obj);
	if (!vaddr)
		return -ENOMEM;
	iosys_map_set_vaddr(map, vaddr);

	return 0;
}

int etnaviv_gem_prime_pin(struct drm_gem_object *obj)
{
	if (!drm_gem_is_imported(obj)) {
		struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

		mutex_lock(&etnaviv_obj->lock);
		etnaviv_gem_get_pages(etnaviv_obj);
		mutex_unlock(&etnaviv_obj->lock);
	}
	return 0;
}

void etnaviv_gem_prime_unpin(struct drm_gem_object *obj)
{
	if (!drm_gem_is_imported(obj)) {
		struct etnaviv_gem_object *etnaviv_obj = to_etnaviv_bo(obj);

		mutex_lock(&etnaviv_obj->lock);
		etnaviv_gem_put_pages(to_etnaviv_bo(obj));
		mutex_unlock(&etnaviv_obj->lock);
	}
}

static void etnaviv_gem_prime_release(struct etnaviv_gem_object *etnaviv_obj)
{
	struct iosys_map map = IOSYS_MAP_INIT_VADDR(etnaviv_obj->vaddr);

	if (etnaviv_obj->vaddr)
		dma_buf_vunmap_unlocked(etnaviv_obj->base.import_attach->dmabuf, &map);

	/* Don't drop the pages for imported dmabuf, as they are not
	 * ours, just free the array we allocated:
	 */
	kvfree(etnaviv_obj->pages);

	drm_prime_gem_destroy(&etnaviv_obj->base, etnaviv_obj->sgt);
}

static void *etnaviv_gem_prime_vmap_impl(struct etnaviv_gem_object *etnaviv_obj)
{
	struct iosys_map map;
	int ret;

	lockdep_assert_held(&etnaviv_obj->lock);

	ret = dma_buf_vmap(etnaviv_obj->base.import_attach->dmabuf, &map);
	if (ret)
		return NULL;
	return map.vaddr;
}

static int etnaviv_gem_prime_mmap_obj(struct etnaviv_gem_object *etnaviv_obj,
		struct vm_area_struct *vma)
{
	int ret;

	ret = dma_buf_mmap(etnaviv_obj->base.dma_buf, vma, 0);
	if (!ret) {
		/* Drop the reference acquired by drm_gem_mmap_obj(). */
		drm_gem_object_put(&etnaviv_obj->base);
	}

	return ret;
}

static const struct etnaviv_gem_ops etnaviv_gem_prime_ops = {
	/* .get_pages should never be called */
	.release = etnaviv_gem_prime_release,
	.vmap = etnaviv_gem_prime_vmap_impl,
	.mmap = etnaviv_gem_prime_mmap_obj,
};

struct drm_gem_object *etnaviv_gem_prime_import_sg_table(struct drm_device *dev,
	struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct etnaviv_gem_object *etnaviv_obj;
	size_t size = PAGE_ALIGN(attach->dmabuf->size);
	int ret, npages;

	ret = etnaviv_gem_new_private(dev, size, ETNA_BO_WC,
				      &etnaviv_gem_prime_ops, &etnaviv_obj);
	if (ret < 0)
		return ERR_PTR(ret);

	lockdep_set_class(&etnaviv_obj->lock, &etnaviv_prime_lock_class);

	npages = size / PAGE_SIZE;

	etnaviv_obj->sgt = sgt;
	etnaviv_obj->pages = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	if (!etnaviv_obj->pages) {
		ret = -ENOMEM;
		goto fail;
	}

	ret = drm_prime_sg_to_page_array(sgt, etnaviv_obj->pages, npages);
	if (ret)
		goto fail;

	etnaviv_gem_obj_add(dev, &etnaviv_obj->base);

	return &etnaviv_obj->base;

fail:
	drm_gem_object_put(&etnaviv_obj->base);

	return ERR_PTR(ret);
}
