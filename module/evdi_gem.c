/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <drm/drmP.h>
#include "evdi_drv.h"
#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <linux/version.h>

struct evdi_gem_object *evdi_gem_alloc_object(struct drm_device *dev,
                                              size_t size)
{
  struct evdi_gem_object *obj;

  obj = kzalloc(sizeof(*obj), GFP_KERNEL);
  if (obj == NULL) {
    return NULL;
  }

  if (drm_gem_object_init(dev, &obj->base, size) != 0) {
    kfree(obj);
    return NULL;
  }

  return obj;
}

static int
evdi_gem_create(struct drm_file *file,
                struct drm_device *dev,
                uint64_t size,
                uint32_t *handle_p)
{
  struct evdi_gem_object *obj;
  int ret;
  u32 handle;

  size = roundup(size, PAGE_SIZE);

  obj = evdi_gem_alloc_object(dev, size);
  if (obj == NULL) {
    return -ENOMEM;
  }

  ret = drm_gem_handle_create(file, &obj->base, &handle);
  if (ret) {
    drm_gem_object_release(&obj->base);
    kfree(obj);
    return ret;
  }

  drm_gem_object_unreference(&obj->base);
  *handle_p = handle;
  return 0;
}

int evdi_dumb_create(struct drm_file *file,
                     struct drm_device *dev,
                     struct drm_mode_create_dumb *args)
{
  args->pitch = args->width * DIV_ROUND_UP(args->bpp, 8);
  args->size = args->pitch * args->height;
  return evdi_gem_create(file,
                         dev,
                         args->size,
                         &args->handle);
}

int evdi_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
  int ret;

  ret = drm_gem_mmap(filp, vma);
  if (ret) {
    return ret;
  }

  vma->vm_flags &= ~VM_PFNMAP;
  vma->vm_flags |= VM_MIXEDMAP;

  return ret;
}

int evdi_gem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
  struct evdi_gem_object *obj = to_evdi_bo(vma->vm_private_data);
  struct page *page;
  unsigned int page_offset;
  int ret = 0;

  page_offset = ((unsigned long)vmf->virtual_address - vma->vm_start) >>
    PAGE_SHIFT;

  if (!obj->pages) {
    return VM_FAULT_SIGBUS;
  }

  page = obj->pages[page_offset];
  ret = vm_insert_page(vma, (unsigned long)vmf->virtual_address, page);
  switch (ret) {
  case -EAGAIN:
  case 0:
  case -ERESTARTSYS:
    return VM_FAULT_NOPAGE;
  case -ENOMEM:
    return VM_FAULT_OOM;
  default:
    return VM_FAULT_SIGBUS;
  }
}

static int evdi_gem_get_pages(struct evdi_gem_object *obj, gfp_t gfpmask)
{
  struct page **pages;

  if (obj->pages) {
    return 0;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,17,0)
  pages = drm_gem_get_pages(&obj->base);
#else
  pages = drm_gem_get_pages(&obj->base, gfpmask);
#endif
  if (IS_ERR(pages)) {
    return PTR_ERR(pages);
  }

  obj->pages = pages;

  return 0;
}

static void evdi_gem_put_pages(struct evdi_gem_object *obj)
{
  if (obj->base.import_attach) {
    drm_free_large(obj->pages);
    obj->pages = NULL;
    return;
  }

  drm_gem_put_pages(&obj->base, obj->pages, false, false);
  obj->pages = NULL;
}

int evdi_gem_vmap(struct evdi_gem_object *obj)
{
  int page_count = obj->base.size / PAGE_SIZE;
  int ret;

  if (obj->base.import_attach) {
    obj->vmapping = dma_buf_vmap(obj->base.import_attach->dmabuf);
    if (!obj->vmapping) {
      return -ENOMEM;
    }
    return 0;
  }

  ret = evdi_gem_get_pages(obj, GFP_KERNEL);
  if (ret) {
    return ret;
  }

  obj->vmapping = vmap(obj->pages, page_count, 0, PAGE_KERNEL);
  if (!obj->vmapping) {
    return -ENOMEM;
  }
  return 0;
}

void evdi_gem_vunmap(struct evdi_gem_object *obj)
{
  if (obj->base.import_attach) {
    dma_buf_vunmap(obj->base.import_attach->dmabuf, obj->vmapping);
    obj->vmapping = NULL;
    return;
  }

  if (obj->vmapping) {
    vunmap(obj->vmapping);
    obj->vmapping = NULL;
  }

  evdi_gem_put_pages(obj);
}

void evdi_gem_free_object(struct drm_gem_object *gem_obj)
{
  struct evdi_gem_object *obj = to_evdi_bo(gem_obj);

  if (obj->vmapping) {
    evdi_gem_vunmap(obj);
  }

  if (gem_obj->import_attach) {
    drm_prime_gem_destroy(gem_obj, obj->sg);
    put_device(gem_obj->dev->dev);
  }

  if (obj->pages) {
    evdi_gem_put_pages(obj);
  }

  drm_gem_free_mmap_offset(gem_obj);
}

/* the dumb interface doesn't work with the GEM straight MMAP
   interface, it expects to do MMAP on the drm fd, like normal */
int evdi_gem_mmap(struct drm_file *file,
                  struct drm_device *dev,
                  uint32_t handle,
                  uint64_t *offset)
{
  struct evdi_gem_object *gobj;
  struct drm_gem_object *obj;
  int ret = 0;

  mutex_lock(&dev->struct_mutex);
  obj = drm_gem_object_lookup(dev, file, handle);
  if (obj == NULL) {
    ret = -ENOENT;
    goto unlock;
  }
  gobj = to_evdi_bo(obj);

  ret = evdi_gem_get_pages(gobj, GFP_KERNEL);
  if (ret) {
    goto out;
  }
  ret = drm_gem_create_mmap_offset(obj);
  if (ret) {
    goto out;
  }

  *offset = drm_vma_node_offset_addr(&gobj->base.vma_node);

out:
  drm_gem_object_unreference(&gobj->base);
unlock:
  mutex_unlock(&dev->struct_mutex);
  return ret;
}

static int evdi_prime_create(struct drm_device *dev,
                             size_t size,
                             struct sg_table *sg,
                             struct evdi_gem_object **obj_p)
{
  struct evdi_gem_object *obj;
  int npages;

  npages = size / PAGE_SIZE;

  *obj_p = NULL;
  obj = evdi_gem_alloc_object(dev, npages * PAGE_SIZE);
  if (!obj) {
    return -ENOMEM;
  }

  obj->sg = sg;
  obj->pages = drm_malloc_ab(npages, sizeof(struct page *));
  if (obj->pages == NULL) {
    DRM_ERROR("obj pages is NULL %d\n", npages);
    return -ENOMEM;
  }

  drm_prime_sg_to_page_addr_arrays(sg, obj->pages, NULL, npages);

  *obj_p = obj;
  return 0;
}

struct drm_gem_object *evdi_gem_prime_import(struct drm_device *dev,
                                             struct dma_buf *dma_buf)
{
  struct dma_buf_attachment *attach;
  struct sg_table *sg;
  struct evdi_gem_object *uobj;
  int ret;

  /* need to attach */
  get_device(dev->dev);
  attach = dma_buf_attach(dma_buf, dev->dev);
  if (IS_ERR(attach)) {
    put_device(dev->dev);
    return ERR_CAST(attach);
  }

  get_dma_buf(dma_buf);

  sg = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
  if (IS_ERR(sg)) {
    ret = PTR_ERR(sg);
    goto fail_detach;
  }

  ret = evdi_prime_create(dev, dma_buf->size, sg, &uobj);
  if (ret) {
    goto fail_unmap;
  }

  uobj->base.import_attach = attach;

  return &uobj->base;

fail_unmap:
  dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
fail_detach:
  dma_buf_detach(dma_buf, attach);
  dma_buf_put(dma_buf);
  put_device(dev->dev);
  return ERR_PTR(ret);
}

