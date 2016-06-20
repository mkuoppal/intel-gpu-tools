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

#include "igt.h"
#include "igt_vgem.h"

#include <sys/poll.h>

IGT_TEST_DESCRIPTION("Basic check of polling for prime/vgem fences.");

static void test_read(int vgem, int i915)
{
	struct vgem_bo scratch;
	uint32_t handle;
	uint32_t *ptr;
	int dmabuf, i;

	scratch.width = 1024;
	scratch.height = 1024;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);

	dmabuf = prime_handle_to_fd(vgem, scratch.handle);
	handle = prime_fd_to_handle(i915, dmabuf);
	close(dmabuf);

	ptr = vgem_mmap(vgem, &scratch, PROT_WRITE);
	for (i = 0; i < 1024; i++)
		ptr[1024*i] = i;
	munmap(ptr, scratch.size);
	gem_close(vgem, scratch.handle);

	for (i = 0; i < 1024; i++) {
		uint32_t tmp;
		gem_read(i915, handle, 4096*i, &tmp, sizeof(tmp));
		igt_assert_eq(tmp, i);
	}
	gem_close(i915, handle);
}

static void test_write(int vgem, int i915)
{
	struct vgem_bo scratch;
	uint32_t handle;
	uint32_t *ptr;
	int dmabuf, i;

	scratch.width = 1024;
	scratch.height = 1024;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);

	dmabuf = prime_handle_to_fd(vgem, scratch.handle);
	handle = prime_fd_to_handle(i915, dmabuf);
	close(dmabuf);

	ptr = vgem_mmap(vgem, &scratch, PROT_READ);
	gem_close(vgem, scratch.handle);

	for (i = 0; i < 1024; i++)
		gem_write(i915, handle, 4096*i, &i, sizeof(i));
	gem_close(i915, handle);

	for (i = 0; i < 1024; i++)
		igt_assert_eq(ptr[1024*i], i);
	munmap(ptr, scratch.size);
}

static void test_gtt(int vgem, int i915)
{
	struct vgem_bo scratch;
	uint32_t handle;
	uint32_t *ptr, *gtt;
	int dmabuf, i;

	scratch.width = 1024;
	scratch.height = 1024;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);

	dmabuf = prime_handle_to_fd(vgem, scratch.handle);
	handle = prime_fd_to_handle(i915, dmabuf);
	close(dmabuf);

	ptr = gem_mmap__gtt(i915, handle, scratch.size, PROT_WRITE);
	for (i = 0; i < 1024; i++)
		ptr[1024*i] = i;
	munmap(ptr, scratch.size);

	ptr = vgem_mmap(vgem, &scratch, PROT_READ | PROT_WRITE);
	for (i = 0; i < 1024; i++) {
		igt_assert_eq(ptr[1024*i], i);
		ptr[1024*i] = ~i;
	}
	munmap(ptr, scratch.size);

	ptr = gem_mmap__gtt(i915, handle, scratch.size, PROT_READ);
	for (i = 0; i < 1024; i++)
		igt_assert_eq(ptr[1024*i], ~i);
	munmap(ptr, scratch.size);


	ptr = vgem_mmap(vgem, &scratch, PROT_WRITE);
	gtt = gem_mmap__gtt(i915, handle, scratch.size, PROT_WRITE);
	for (i = 0; i < 1024; i++) {
		gtt[1024*i] = i;
		igt_assert_eq(ptr[1024*i], i);
		ptr[1024*i] = ~i;
		igt_assert_eq(gtt[1024*i], ~i);
	}
	munmap(gtt, scratch.size);
	munmap(ptr, scratch.size);

	gem_close(i915, handle);
	gem_close(vgem, scratch.handle);
}

static bool prime_busy(int fd, bool excl)
{
	struct pollfd pfd = { .fd = fd, .events = excl ? POLLOUT : POLLIN };
	return poll(&pfd, 1, 0) == 0;
}

static void work(int i915, int dmabuf, unsigned ring, uint32_t flags)
{
	const int SCRATCH = 0;
	const int BATCH = 1;
	const int gen = intel_gen(intel_get_drm_devid(i915));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry store[1024+1];
	struct drm_i915_gem_execbuffer2 execbuf;
	unsigned size = ALIGN(ARRAY_SIZE(store)*16 + 4, 4096);
	uint32_t *batch, *bbe;
	int i, count;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;
	execbuf.flags = ring | flags;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = prime_fd_to_handle(i915, dmabuf);

	obj[BATCH].handle = gem_create(i915, size);
	obj[BATCH].relocs_ptr = (uintptr_t)store;
	obj[BATCH].relocation_count = ARRAY_SIZE(store);
	memset(store, 0, sizeof(store));

	batch = gem_mmap__wc(i915, obj[BATCH].handle, 0, size, PROT_WRITE);
	gem_set_domain(i915, obj[BATCH].handle,
		       I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	i = 0;
	for (count = 0; count < 1024; count++) {
		store[count].target_handle = obj[SCRATCH].handle;
		store[count].presumed_offset = -1;
		store[count].offset = sizeof(uint32_t) * (i + 1);
		store[count].delta = sizeof(uint32_t) * count;
		store[count].read_domains = I915_GEM_DOMAIN_INSTRUCTION;
		store[count].write_domain = I915_GEM_DOMAIN_INSTRUCTION;
		batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
		if (gen >= 8) {
			batch[++i] = 0;
			batch[++i] = 0;
		} else if (gen >= 4) {
			batch[++i] = 0;
			batch[++i] = 0;
			store[count].offset += sizeof(uint32_t);
		} else {
			batch[i]--;
			batch[++i] = 0;
		}
		batch[++i] = count;
		i++;
	}

	bbe = &batch[i];
	store[count].target_handle = obj[BATCH].handle; /* recurse */
	store[count].presumed_offset = 0;
	store[count].offset = sizeof(uint32_t) * (i + 1);
	store[count].delta = 0;
	store[count].read_domains = I915_GEM_DOMAIN_COMMAND;
	store[count].write_domain = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			store[count].delta = 1;
		}
	}
	i++;
	igt_assert(i < size/sizeof(*batch));
	igt_require(__gem_execbuf(i915, &execbuf) == 0);
	gem_close(i915, obj[BATCH].handle);
	gem_close(i915, obj[SCRATCH].handle);

	igt_assert(prime_busy(dmabuf, true));
	igt_assert(prime_busy(dmabuf, false));

	*bbe = MI_BATCH_BUFFER_END;
	__sync_synchronize();
	munmap(batch, size);
}

static void test_busy(int i915, int vgem, unsigned ring, uint32_t flags)
{
	struct vgem_bo scratch;
	struct timespec tv;
	uint32_t *ptr;
	int dmabuf;
	int i;

	scratch.width = 1024;
	scratch.height = 1;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);
	dmabuf = prime_handle_to_fd(vgem, scratch.handle);

	work(i915, dmabuf, ring, flags);

	/* Calling busy in a loop should be enough to flush the rendering */
	memset(&tv, 0, sizeof(tv));
	while (prime_busy(dmabuf, false))
		igt_assert(igt_seconds_elapsed(&tv) < 10);

	ptr = vgem_mmap(vgem, &scratch, PROT_READ);
	for (i = 0; i < 1024; i++)
		igt_assert_eq_u32(ptr[i], i);
	munmap(ptr, 4096);

	gem_close(vgem, scratch.handle);
	close(dmabuf);
}

static void test_wait(int i915, int vgem, unsigned ring, uint32_t flags)
{
	struct vgem_bo scratch;
	struct pollfd pfd;
	uint32_t *ptr;
	int i;

	scratch.width = 1024;
	scratch.height = 1;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);
	pfd.fd = prime_handle_to_fd(vgem, scratch.handle);

	work(i915, pfd.fd, ring, flags);

	pfd.events = POLLIN;
	igt_assert_eq(poll(&pfd, 1, 10000), 1);

	ptr = vgem_mmap(vgem, &scratch, PROT_READ);
	for (i = 0; i < 1024; i++)
		igt_assert_eq_u32(ptr[i], i);
	munmap(ptr, 4096);

	gem_close(vgem, scratch.handle);
	close(pfd.fd);
}

static void test_sync(int i915, int vgem, unsigned ring, uint32_t flags)
{
	struct vgem_bo scratch;
	uint32_t *ptr;
	int dmabuf;
	int i;

	scratch.width = 1024;
	scratch.height = 1;
	scratch.bpp = 32;
	vgem_create(vgem, &scratch);
	dmabuf = prime_handle_to_fd(vgem, scratch.handle);

	work(i915, dmabuf, ring, flags);

	prime_sync_start(dmabuf, false);

	ptr = vgem_mmap(vgem, &scratch, PROT_READ);
	for (i = 0; i < 1024; i++)
		igt_assert_eq_u32(ptr[i], i);
	munmap(ptr, 4096);

	prime_sync_end(dmabuf, false);

	gem_close(vgem, scratch.handle);
	close(dmabuf);
}

static bool has_prime_export(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_EXPORT;
}

static bool has_prime_import(int fd)
{
	uint64_t value;

	if (drmGetCap(fd, DRM_CAP_PRIME, &value))
		return false;

	return value & DRM_PRIME_CAP_IMPORT;
}

igt_main
{
	const struct intel_execution_engine *e;
	int i915 = -1;
	int vgem = -1;
	int gen = 0;

	igt_skip_on_simulation();

	igt_fixture {
		vgem = drm_open_driver(DRIVER_VGEM);
		igt_require(has_prime_export(vgem));

		i915 = drm_open_driver_master(DRIVER_INTEL);
		igt_require(has_prime_import(i915));
		gem_require_mmap_wc(i915);
		gen = intel_gen(intel_get_drm_devid(i915));
	}

	igt_subtest("basic-read")
		test_read(vgem, i915);

	igt_subtest("basic-write")
		test_write(vgem, i915);

	igt_subtest("basic-gtt")
		test_gtt(vgem, i915);

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%ssync-%s",
			      e->exec_id == 0 ? "basic-" : "",
			      e->name) {
			gem_require_ring(i915, e->exec_id | e->flags);
			igt_skip_on_f(gen == 6 &&
				      e->exec_id == I915_EXEC_BSD,
				      "MI_STORE_DATA broken on gen6 bsd\n");
			gem_quiescent_gpu(i915);
			test_sync(i915, vgem, e->exec_id, e->flags);
		}
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%sbusy-%s",
			      e->exec_id == 0 ? "basic-" : "",
			      e->name) {
			gem_require_ring(i915, e->exec_id | e->flags);
			igt_skip_on_f(gen == 6 &&
				      e->exec_id == I915_EXEC_BSD,
				      "MI_STORE_DATA broken on gen6 bsd\n");
			gem_quiescent_gpu(i915);
			test_busy(i915, vgem, e->exec_id, e->flags);
		}
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%swait-%s",
			      e->exec_id == 0 ? "basic-" : "",
			      e->name) {
			gem_require_ring(i915, e->exec_id | e->flags);
			igt_skip_on_f(gen == 6 &&
				      e->exec_id == I915_EXEC_BSD,
				      "MI_STORE_DATA broken on gen6 bsd\n");
			gem_quiescent_gpu(i915);
			test_wait(i915, vgem, e->exec_id, e->flags);
		}
	}

	igt_fixture {
		close(i915);
		close(vgem);
	}
}
