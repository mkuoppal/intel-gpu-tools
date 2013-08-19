/*
 * Copyright (c) 2013 Intel Corporation
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
 *  Mika Kuoppala <mika.kuoppala@intel.com>
 *
 */

#include <stdio.h>
#include "rendercopy.h"

static int gem_exec(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int ret;

	ret = ioctl(fd,
		    DRM_IOCTL_I915_GEM_EXECBUFFER2,
		    execbuf);

	if (ret < 0)
		return -errno;

	return 0;
}

#define BUFSIZE (4 * 1024)
#define ITEMS   (BUFSIZE >> 2)

static int inject_hang(int fd, int ctx)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec;
	uint64_t gtt_off;
	uint32_t *buf;
	int roff, i;

	buf = malloc(BUFSIZE);
	assert(buf != NULL);

	buf[0] = MI_BATCH_BUFFER_END;
	buf[1] = MI_NOOP;

	exec.handle = gem_create(fd, BUFSIZE);
	gem_write(fd, exec.handle, 0, buf, BUFSIZE);
	exec.relocation_count = 0;
	exec.relocs_ptr = 0;
	exec.alignment = 0;
	exec.offset = 0;
	exec.flags = 0;
	exec.rsvd1 = 0;
	exec.rsvd2 = 0;

	execbuf.buffers_ptr = (uintptr_t)&exec;
	execbuf.buffer_count = 1;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = BUFSIZE;
	execbuf.cliprects_ptr = 0;
	execbuf.num_cliprects = 0;
	execbuf.DR1 = 0;
	execbuf.DR4 = 0;
	execbuf.flags = 0;
	i915_execbuffer2_set_context_id(execbuf, ctx);
	execbuf.rsvd2 = 0;

	assert(gem_exec(fd, &execbuf) == 0);
	gem_sync(fd, exec.handle);

	gtt_off = exec.offset;

	for (i = 0; i < ITEMS; i++)
		buf[i] = MI_NOOP;

	/* We can't do a real hang but make a tight loop */
	roff = 0;

	buf[roff] = MI_BATCH_BUFFER_START;
	buf[roff + 1] = gtt_off;

	printf("hang injected at 0x%lx (offset 0x%x, bo_start 0x%lx, bo_end 0x%lx) \n",
	       (long unsigned int)((roff<<2) + gtt_off),
	       roff<<2, (long unsigned int)gtt_off,
	       (long unsigned int)(gtt_off + BUFSIZE - 1));

	gem_write(fd, exec.handle, 0, buf, BUFSIZE);

	exec.relocation_count = 0;
	exec.relocs_ptr = 0;
	exec.alignment = 0;
	exec.offset = 0;
	exec.flags = 0;
	exec.rsvd1 = 0;
	exec.rsvd2 = 0;

	execbuf.buffers_ptr = (uintptr_t)&exec;
	execbuf.buffer_count = 1;
	execbuf.batch_start_offset = 0;
	execbuf.batch_len = BUFSIZE;
	execbuf.cliprects_ptr = 0;
	execbuf.num_cliprects = 0;
	execbuf.DR1 = 0;
	execbuf.DR4 = 0;
	execbuf.flags = 0;
	i915_execbuffer2_set_context_id(execbuf, ctx);
	execbuf.rsvd2 = 0;

	assert(gem_exec(fd, &execbuf) == 0);

	// printf("batch started at address 0x%x\n", buf[1]);
	assert(gtt_off == exec.offset);

	free(buf);

	return exec.handle;
}

static void test_hang(void)
{
	int fd;
	int h;

	fd = drm_open_any();
	assert(fd);

	h = inject_hang(fd, 0);
	gem_sync(fd, h);

	gem_close(fd, h);
	close(fd);
}

int main(int argc, char **argv)
{
	uint32_t devid;
	int fd;

	fd = drm_open_any();
	devid = intel_get_drm_devid(fd);
	if (intel_gen(devid) < 4)
		igt_skip("too old gen\n");

	close(fd);

	test_hang();

	igt_success();
}
