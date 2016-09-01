/*
 * Copyright © 2012 Intel Corporation
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
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Test that context cannot be submitted to any ring");

static int exec(int fd, unsigned ring)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj;

	memset(&obj, 0, sizeof(obj));
	memset(&execbuf, 0, sizeof(execbuf));

	execbuf.buffers_ptr = (uintptr_t)&obj;
	execbuf.buffer_count = 1;
	i915_execbuffer2_set_context_id(execbuf, 1);

	return __gem_execbuf(fd, &execbuf);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture
		fd = drm_open_driver_render(DRIVER_INTEL);

	for (e = intel_execution_engines; e->name; e++) {
		igt_subtest_f("%s", e->name) {
			gem_require_ring(fd, e->exec_id | e->flags);
			igt_assert_eq(exec(fd, e->exec_id | e->flags), -ENOENT);
		}
	}
}
