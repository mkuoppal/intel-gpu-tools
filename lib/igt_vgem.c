/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#define _GNU_SOURCE

#include "igt.h"
#include "igt_vgem.h"

#include <sys/mman.h>

int __vgem_create(int fd, struct vgem_bo *bo)
{
	struct drm_mode_create_dumb arg;

	memset(&arg, 0, sizeof(arg));
	arg.width = bo->width;
	arg.height = bo->height;
	arg.bpp = bo->bpp;

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg))
		return -errno;

	bo->handle = arg.handle;
	bo->pitch = arg.pitch;
	bo->size = arg.size;

	return 0;
}

void vgem_create(int fd, struct vgem_bo *bo)
{
	igt_assert_eq(__vgem_create(fd, bo), 0);
}

void *__vgem_mmap(int fd, struct vgem_bo *bo, unsigned prot)
{
	struct drm_mode_map_dumb arg;
	void *ptr;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &arg))
		return NULL;

	ptr = mmap64(0, bo->size, prot, MAP_SHARED, fd, arg.offset);
	if (ptr == MAP_FAILED)
		return NULL;

	return ptr;
}

void *vgem_mmap(int fd, struct vgem_bo *bo, unsigned prot)
{
	void *ptr;

	igt_assert_f((ptr = __vgem_mmap(fd, bo, prot)),
		     "vgem_map(fd=%d, bo->handle=%d, prot=%x)\n",
		     fd, bo->handle, prot);

	return ptr;
}

