/*
 * Copyright © 2013 Intel Corporation
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
 *
 * Authors:
 *    Jesse Barnes <jbarnes@virtuousgeek.org>
 *
 */

#include "igt.h"

/*
 * Sanity check for SVM - just malloc batch and target buffers, store some
 * data from the GPU into the target, and check it for the right result
 * on the CPU.
 */

static
int gem_exec_mm(int fd, uint32_t ctx, uint64_t batch_ptr, uint32_t flags)
{
	struct local_drm_i915_exec_mm mm;

	memset(&mm, 0, sizeof(mm));
	mm.ctx_id = ctx;
	mm.batch_ptr = batch_ptr;
	mm.flags = flags;

	if (igt_ioctl(fd, LOCAL_DRM_IOCTL_I915_EXEC_MM, &mm)) {
		int err = -errno;
		igt_skip_on(err == -ENODEV || errno == -EINVAL);
		igt_assert_eq(err, 0);
	}

	errno = 0;

	return 0;
}


static void test_store_dword(const int fd, const uint32_t ctx_id)
{
	uint32_t batch_buffer[8];
	uint32_t addr_hi, addr_lo, *bb = batch_buffer, *buf;
	int cmd, val = 0xdead0000;
	uint32_t *target_buffer;

	target_buffer = malloc(4096);
	igt_assert(target_buffer);
	memset(target_buffer, 0, 4096);

	igt_debug("using GPU to write 0x%08x to %p\n", val, target_buffer);

#if defined(__x86_64__)
	addr_hi = ((uintptr_t)target_buffer) >> 32;
#else
	addr_hi = 0;
#endif

	addr_lo = ((uintptr_t)target_buffer) & 0xffffffff;

	cmd = MI_STORE_DWORD_IMM;

	*bb++ = cmd;
	*bb++ = addr_lo;
	*bb++ = addr_hi;
	*bb++ = val;
	*bb++ = MI_NOOP;
	*bb++ = MI_NOOP;
	*bb++ = MI_NOOP | MI_NOOP_WRITE_ID | (0x0f00);
	*bb++ = MI_BATCH_BUFFER_END;

	gem_exec_mm(fd, ctx_id, (uintptr_t)batch_buffer, 0);
	gem_quiescent_gpu(fd);

	buf = target_buffer;
	if (buf[0] != val) {
		fprintf(stderr,
			"value mismatch: read 0x%08x, expected 0x%08x\n",
			buf[0], val);
		exit(-1);
	}
	printf("success: read 0x%08x, expected 0x%08x\n", buf[0], val);
}

igt_main
{
	int fd, devid;
	uint32_t ctx_id;

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		devid = intel_get_drm_devid(fd);

		if (intel_gen(devid) < 8) {
			fprintf(stderr, "SVM only available on BDW+\n");
			exit(-1);
		}

		ctx_id = gem_context_create2(fd, I915_GEM_CONTEXT_ENABLE_SVM);
	}

	igt_subtest("sanity")
		test_store_dword(fd, ctx_id);

	igt_fixture
		close(fd);
}
