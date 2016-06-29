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

#include <sys/ioctl.h>
#include <sys/poll.h>

IGT_TEST_DESCRIPTION("Check that execbuf waits for explicit fences");

#define LOCAL_PARAM_HAS_EXEC_FENCE 43
#define LOCAL_EXEC_FENCE_IN (1 << 16)
#define LOCAL_EXEC_FENCE_OUT (1 << 17)
#define LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR       DRM_IOWR(DRM_COMMAND_BASE + DRM_I915_GEM_EXECBUFFER2, struct drm_i915_gem_execbuffer2)

static bool can_mi_store_dword(int gen, unsigned engine)
{
	return !(gen == 6 && (engine & ~(3<<13)) == I915_EXEC_BSD);
}

static void store(int fd, unsigned ring, int fence, uint32_t target, unsigned offset_value)
{
	const int SCRATCH = 0;
	const int BATCH = 1;
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_IN;
	execbuf.rsvd2 = fence;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;

	memset(obj, 0, sizeof(obj));
	obj[SCRATCH].handle = target;

	obj[BATCH].handle = gem_create(fd, 4096);
	obj[BATCH].relocs_ptr = (uintptr_t)&reloc;
	obj[BATCH].relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	i = 0;
	reloc.target_handle = obj[SCRATCH].handle;
	reloc.presumed_offset = -1;
	reloc.offset = sizeof(uint32_t) * (i + 1);
	reloc.delta = sizeof(uint32_t) * offset_value;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = reloc.delta;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = reloc.delta;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = reloc.delta;
	}
	batch[++i] = offset_value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[BATCH].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[BATCH].handle);
}

static int __gem_execbuf_wr(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (igt_ioctl(fd, LOCAL_IOCTL_I915_GEM_EXECBUFFER2_WR, execbuf))
		err = -errno;
	errno = 0;
	return err;
}

static void gem_execbuf_wr(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	igt_assert_eq(__gem_execbuf_wr(fd, execbuf), 0);
}

static bool fence_busy(int fence)
{
	return poll(&(struct pollfd){fence, POLLIN}, 1, 0) == 0;
}

#define HANG 0x1
#define NONBLOCK 0x2
#define WAIT 0x4

static void test_fence_busy(int fd, unsigned ring, unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct timespec tv;
	uint32_t *batch;
	int fence, i, timeout;

	gem_quiescent_gpu(fd);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&obj;
	execbuf.buffer_count = 1;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_OUT;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);

	obj.relocs_ptr = (uintptr_t)&reloc;
	obj.relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

	i = 0;
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
			reloc.delta = 1;
		}
	}
	i++;

	execbuf.rsvd2 = -1;
	gem_execbuf_wr(fd, &execbuf);
	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence != -1);

	igt_assert(gem_bo_busy(fd, obj.handle));
	igt_assert(fence_busy(fence));

	timeout = 120;
	if ((flags & HANG) == 0) {
		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
		timeout = 1;
	}
	munmap(batch, 4096);

	if (flags & WAIT) {
		struct pollfd pfd = { .fd = fence, .events = POLLIN };
		igt_assert(poll(&pfd, 1, timeout*1000) == 1);
	} else {
		memset(&tv, 0, sizeof(tv));
		while (fence_busy(fence))
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	}

	igt_assert(!gem_bo_busy(fd, obj.handle));

	close(fence);
	gem_close(fd, obj.handle);

	gem_quiescent_gpu(fd);
}

static void test_fence_await(int fd, unsigned ring, unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t scratch = gem_create(fd, 4096);
	uint32_t *batch, *out;
	unsigned engine;
	int fence, i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&obj;
	execbuf.buffer_count = 1;
	execbuf.flags = ring | LOCAL_EXEC_FENCE_OUT;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);

	obj.relocs_ptr = (uintptr_t)&reloc;
	obj.relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	out = gem_mmap__wc(fd, scratch, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	batch = gem_mmap__wc(fd, obj.handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

	i = 0;
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
			reloc.delta = 1;
		}
	}
	i++;

	execbuf.rsvd2 = -1;
	gem_execbuf_wr(fd, &execbuf);
	gem_close(fd, obj.handle);
	fence = execbuf.rsvd2 >> 32;
	igt_assert(fence != -1);

	i = 0;
	for_each_engine(fd, engine) {
		if (!can_mi_store_dword(gen, engine))
			continue;

		if (flags & NONBLOCK) {
			store(fd, engine, fence, scratch, i);
		} else {
			igt_fork(child, 1)
				store(fd, engine, fence, scratch, i);
		}

		i++;
	}
	close(fence);

	sleep(1);

	/* Check for invalidly completing the task early */
	for (int n = 0; n < i; n++)
		igt_assert_eq_u32(out[n], 0);

	if ((flags & HANG) == 0) {
		*batch = MI_BATCH_BUFFER_END;
		__sync_synchronize();
	}
	munmap(batch, 4096);

	igt_waitchildren();

	gem_set_domain(fd, scratch, I915_GEM_DOMAIN_GTT, 0);
	while (i--)
		igt_assert_eq_u32(out[i], i);
	munmap(out, 4096);
	gem_close(fd, scratch);
}

static void test_fence_flip(int i915)
{
	igt_skip_on_f(1, "no fence-in for atomic flips\n");
}

static bool gem_has_exec_fence(int fd)
{
	struct drm_i915_getparam gp;
	int val = -1;

	memset(&gp, 0, sizeof(gp));
	gp.param = LOCAL_PARAM_HAS_EXEC_FENCE;
	gp.value = &val;

	ioctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return val > 0;
}

igt_main
{
	const struct intel_execution_engine *e;
	int i915 = -1;

	igt_skip_on_simulation();

	igt_fixture {
		i915 = drm_open_driver_master(DRIVER_INTEL);
		igt_require(gem_has_exec_fence(i915));
		gem_require_mmap_wc(i915);

		igt_allow_hang(i915, 0, 0);
	}

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_group {
			igt_fixture {
				igt_require(gem_has_ring(i915, e->exec_id | e->flags));
			}

			igt_subtest_f("%sbusy-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name)
				test_fence_busy(i915, e->exec_id | e->flags, 0);
			igt_subtest_f("%swait-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name)
				test_fence_busy(i915, e->exec_id | e->flags, WAIT);
			igt_subtest_f("%sawait-%s",
				      e->exec_id == 0 ? "basic-" : "",
				      e->name)
				test_fence_await(i915, e->exec_id | e->flags, 0);
			igt_subtest_f("nb-await-%s", e->name)
				test_fence_await(i915, e->exec_id | e->flags, NONBLOCK);
			igt_subtest_f("busy-hang-%s", e->name)
				test_fence_busy(i915, e->exec_id | e->flags, HANG);
			igt_subtest_f("wait-hang-%s", e->name)
				test_fence_busy(i915, e->exec_id | e->flags, HANG | WAIT);
			igt_subtest_f("await-hang-%s", e->name)
				test_fence_await(i915, e->exec_id | e->flags, HANG);
			igt_subtest_f("nb-await-hang-%s", e->name)
				test_fence_await(i915, e->exec_id | e->flags, NONBLOCK | HANG);
		}
	}

	igt_subtest("flip") {
		gem_quiescent_gpu(i915);
		test_fence_flip(i915);
	}

	igt_fixture {
		close(i915);
	}
}
