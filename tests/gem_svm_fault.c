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
 * Try to generate a page fault from the GPU by allocating memory and storing
 * to it from the CPU without having done so on the GPU yet.
 */

static int fd;
static uint32_t ctx_id;
static uint32_t *target_buffer;
static uint32_t batch_buffer[8];

static void
store_dword(int devid)
{
	struct local_drm_i915_exec_mm exec;
	uint32_t addr_hi, addr_lo, *bb = batch_buffer, *buf;
	int ret, cmd, val = 0xdead0000;

	target_buffer += 512*1024;

	printf("using GPU to write 0x%08x to %p\n", val, target_buffer);

#if defined(__x86_64__)
	addr_hi = ((uintptr_t)target_buffer) >> 32;
#else
	addr_hi = 0;
#endif

	cmd = MI_STORE_DWORD_IMM;

	*bb++ = cmd;
	*bb++ = addr_lo;
	*bb++ = addr_hi;
	*bb++ = val;
	*bb++ = MI_NOOP;
	*bb++ = MI_NOOP;
	*bb++ = MI_NOOP | MI_NOOP_WRITE_ID | (0x0f00);
	*bb++ = MI_BATCH_BUFFER_END;

	exec.batch_ptr = (uintptr_t)batch_buffer;
	exec.ctx_id = ctx_id;
	exec.flags = 0;

	ret = drmIoctl(fd, LOCAL_DRM_IOCTL_I915_EXEC_MM, &exec);
	if (ret)
		fprintf(stderr, "ioctl failed: %d (%s)\n", ret,
			strerror(errno));
	sleep(3); /* wait for GPU to finish */
	buf = target_buffer;
	if (buf[0] != val) {
		fprintf(stderr,
			"value mismatch: read 0x%08x, expected 0x%08x\n",
			buf[0], val);
		exit(-1);
	}
	printf("success: read 0x%08x, expected 0x%08x\n", buf[0], val);
}

int main(int argc, char **argv)
{
	int devid;

	if (argc != 1) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		exit(-1);
	}

	fd = drm_open_driver(DRIVER_INTEL);
	devid = intel_get_drm_devid(fd);

	if (intel_gen(devid) < 8) {
		fprintf(stderr, "SVM only available on BDW+\n");
		exit(-1);
	}

	ctx_id = gem_context_create2(fd, I915_GEM_CONTEXT_ENABLE_SVM);

	target_buffer = malloc(4096*1024);
	if (!target_buffer) {
		fprintf(stderr, "failed to alloc target buffer\n");
		exit(-1);
	}

	store_dword(devid);

	close(fd);

	return 0;
}
