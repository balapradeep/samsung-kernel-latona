/*
 * drivers/gpu/ion/omap_tiler_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/spinlock.h>

#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/io.h>
#include <linux/ion.h>
#include <linux/mm.h>
#include <linux/omap_ion.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <mach/tiler.h>
#include <asm/mach/map.h>
#include <asm/page.h>

#include "../ion_priv.h"

static int omap_tiler_heap_allocate(struct ion_heap *heap,
				    struct ion_buffer *buffer,
				    unsigned long size, unsigned long align,
				    unsigned long flags)
{
	if (size == 0)
		return 0;

	pr_err("%s: This should never be called directly -- use the "
	       "OMAP_ION_TILER_ALLOC flag to the ION_IOC_CUSTOM "
	       "instead\n", __func__);
	return -EINVAL;
}

struct omap_tiler_info {
	tiler_blk_handle tiler_handle;	/* handle of the allocation intiler */
	u32 n_phys_pages;		/* number of physical pages */
	u32 *phys_addrs;		/* array addrs of pages */
	u32 n_tiler_pages;		/* number of tiler pages */
	u32 *tiler_addrs;		/* array of addrs of tiler pages */
	u32 tiler_start;		/* start addr in tiler -- if not page
					   aligned this may not equal the
					   first entry onf tiler_addrs */
};

int omap_tiler_alloc(struct ion_heap *heap,
		     struct ion_client *client,
		     struct omap_ion_tiler_alloc_data *data)
{
	struct ion_handle *handle;
	struct ion_buffer *buffer;
	struct omap_tiler_info *info;
	int i, ret;

	if (data->fmt == TILER_PIXEL_FMT_PAGE && data->h != 1) {
		pr_err("%s: Page mode (1D) allocations must have a height "
		       "of one\n", __func__);
		return -EINVAL;
	}

	info = kzalloc(sizeof(struct omap_tiler_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	ret = tiler_memsize(data->fmt, data->w, data->h, &info->n_phys_pages,
			    &info->n_tiler_pages);

	if (ret) {
		pr_err("%s: invalid tiler request w %u h %u fmt %u\n", __func__,
		       data->w, data->h, data->fmt);
		kfree(info);
		return ret;
	}

	BUG_ON(!info->n_phys_pages || !info->n_tiler_pages);

	info->phys_addrs = kzalloc(sizeof(u32) * info->n_phys_pages,
				   GFP_KERNEL);
	if (!info->phys_addrs) {
		ret = -ENOMEM;
		goto err_alloc_addrs;
	}

	info->tiler_addrs = kzalloc(sizeof(u32) * info->n_tiler_pages,
				   GFP_KERNEL);
	if (!info->tiler_addrs) {
		ret = -ENOMEM;
		goto err_tiler_addrs;
	}

	info->tiler_handle = tiler_alloc_block_area(data->fmt, data->w, data->h,
						    &info->tiler_start,
						    info->tiler_addrs);
	if (IS_ERR_OR_NULL(info->tiler_handle)) {
		ret = PTR_ERR(info->tiler_handle);
		pr_err("%s: failure to allocate address space from tiler\n",
		       __func__);
		goto err_tiler_block_alloc;
	}

	for (i = 0; i < info->n_phys_pages; i++) {
		ion_phys_addr_t addr = ion_carveout_allocate(heap, PAGE_SIZE,
							     0);

		if (addr == ION_CARVEOUT_ALLOCATE_FAIL) {
			ret = -ENOMEM;
			goto err_alloc;
			pr_err("%s: failure to pages to back tiler "
			       "address space\n", __func__);
		}
		info->phys_addrs[i] = addr;
	}

	ret = tiler_pin_block(info->tiler_handle, info->phys_addrs,
			      info->n_phys_pages);
	if (ret) {
		pr_err("%s: failure to pin pages to tiler\n", __func__);
		goto err_alloc;
	}

	data->stride = tiler_block_vstride(info->tiler_handle);

	/* create an ion handle  for the allocation */
	handle = ion_alloc(client, 0, 0, 1 << OMAP_ION_HEAP_TILER);
	if (IS_ERR_OR_NULL(handle)) {
		ret = PTR_ERR(handle);
		pr_err("%s: failure to allocate handle to manage tiler"
		       " allocation\n", __func__);
		goto err;
	}

	buffer = ion_handle_buffer(handle);
	buffer->size = info->n_tiler_pages * PAGE_SIZE;
	buffer->priv_virt = info;
	data->handle = handle;
	return 0;

err:
	tiler_unpin_block(info->tiler_handle);
err_alloc:
	tiler_free_block_area(info->tiler_handle);
	for (i -= 1; i >= 0; i--)
		ion_carveout_free(heap, info->phys_addrs[i], PAGE_SIZE);
err_tiler_block_alloc:
	kfree(info->tiler_addrs);
err_tiler_addrs:
	kfree(info->phys_addrs);
err_alloc_addrs:
	kfree(info);
	return ret;
}

void omap_tiler_heap_free(struct ion_buffer *buffer)
{
	struct omap_tiler_info *info = buffer->priv_virt;
	int i;

	tiler_unpin_block(info->tiler_handle);
	tiler_free_block_area(info->tiler_handle);

	for (i = 0; i < info->n_phys_pages; i--)
		ion_carveout_free(buffer->heap, info->phys_addrs[i], PAGE_SIZE);

	kfree(info->tiler_addrs);
	kfree(info->phys_addrs);
	kfree(info);
}

static int omap_tiler_phys(struct ion_heap *heap,
			   struct ion_buffer *buffer,
			   ion_phys_addr_t *addr, size_t *len)
{
	struct omap_tiler_info *info = buffer->priv_virt;

	*addr = info->tiler_start;
	*len = buffer->size;
	return 0;
}

int omap_tiler_pages(struct ion_client *client, struct ion_handle *handle,
		     int *n, u32 **tiler_addrs)
{
	ion_phys_addr_t addr;
	size_t len;
	int ret;
	struct omap_tiler_info *info = ion_handle_buffer(handle)->priv_virt;

	/* validate that the handle exists in this client */
	ret = ion_phys(client, handle, &addr, &len);
	if (ret)
		return ret;

	*n = info->n_tiler_pages;
	*tiler_addrs = info->tiler_addrs;
	return 0;
}

int omap_tiler_heap_map_user(struct ion_heap *heap, struct ion_buffer *buffer,
			     struct vm_area_struct *vma)
{
	struct omap_tiler_info *info = buffer->priv_virt;
	unsigned long addr = vma->vm_start;
	u32 vma_pages = (vma->vm_end - vma->vm_start) / PAGE_SIZE;
	int n_pages = min(vma_pages, info->n_tiler_pages);
	int i, ret;

	for (i = vma->vm_pgoff; i < n_pages; i++, addr += PAGE_SIZE) {
		ret = remap_pfn_range(vma, addr,
				      __phys_to_pfn(info->tiler_addrs[i]),
				      PAGE_SIZE,
				      pgprot_noncached(vma->vm_page_prot));
		if (ret)
			return ret;
	}
	return 0;
}

static struct ion_heap_ops omap_tiler_ops = {
	.allocate = omap_tiler_heap_allocate,
	.free = omap_tiler_heap_free,
	.phys = omap_tiler_phys,
	.map_user = omap_tiler_heap_map_user,
};

struct ion_heap *omap_tiler_heap_create(struct ion_platform_heap *data)
{
	struct ion_heap *heap;

	heap = ion_carveout_heap_create(data);
	if (!heap)
		return ERR_PTR(-ENOMEM);
	heap->ops = &omap_tiler_ops;
	heap->type = OMAP_ION_HEAP_TYPE_TILER;
	heap->name = data->name;
	heap->id = data->id;
	return heap;
}

void omap_tiler_heap_destroy(struct ion_heap *heap)
{
	kfree(heap);
}
