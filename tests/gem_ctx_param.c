/*
 * Copyright © 2015 Intel Corporation
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
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#include "igt.h"

IGT_TEST_DESCRIPTION("Basic test for context set/get param input validation.");

igt_main
{
	struct local_i915_gem_context_param arg;
	int fd;
	uint32_t ctx;

	memset(&arg, 0, sizeof(arg));

	igt_fixture {
		fd = drm_open_driver_render(DRIVER_INTEL);
		ctx = gem_context_create(fd);
	}

	arg.param = LOCAL_CONTEXT_PARAM_BAN_PERIOD;

	/* XXX start to enforce ban period returning -EINVAL when
	 * transition has been done */
	if (__gem_context_get_param(fd, &arg) == -EINVAL)
		arg.param = LOCAL_CONTEXT_PARAM_BANNABLE;

	igt_subtest("basic") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		gem_context_set_param(fd, &arg);
	}

	igt_subtest("basic-default") {
		arg.context = 0;
		gem_context_get_param(fd, &arg);
		gem_context_set_param(fd, &arg);
	}

	igt_subtest("invalid-ctx-get") {
		arg.context = 2;
		igt_assert_eq(__gem_context_get_param(fd, &arg), -ENOENT);
	}

	igt_subtest("invalid-ctx-set") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		arg.context = 2;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -ENOENT);
	}

	igt_subtest("invalid-size-get") {
		arg.context = ctx;
		arg.size = 8;
		gem_context_get_param(fd, &arg);
		igt_assert(arg.size == 0);
	}

	igt_subtest("invalid-size-set") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		arg.size = 8;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -EINVAL);
		arg.size = 0;
	}

	igt_subtest("non-root-set") {
		igt_fork(child, 1) {
			igt_drop_root();

			arg.context = ctx;
			gem_context_get_param(fd, &arg);
			arg.value--;
			igt_assert_eq(__gem_context_set_param(fd, &arg), -EPERM);
		}

		igt_waitchildren();
	}

	igt_subtest("root-set") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		arg.value--;
		gem_context_set_param(fd, &arg);
	}

	arg.param = LOCAL_CONTEXT_PARAM_NO_ZEROMAP;

	igt_subtest("non-root-set-no-zeromap") {
		igt_fork(child, 1) {
			igt_drop_root();

			arg.context = ctx;
			gem_context_get_param(fd, &arg);
			arg.value--;
			gem_context_set_param(fd, &arg);
		}

		igt_waitchildren();
	}

	igt_subtest("root-set-no-zeromap-enabled") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		arg.value = 1;
		gem_context_set_param(fd, &arg);
	}

	igt_subtest("root-set-no-zeromap-disabled") {
		arg.context = ctx;
		gem_context_get_param(fd, &arg);
		arg.value = 0;
		gem_context_set_param(fd, &arg);
	}

	/* NOTE: This testcase intentionally tests for the next free parameter
	 * to catch ABI extensions. Don't "fix" this testcase without adding all
	 * the tests for the new param first.
	 */
	arg.param = LOCAL_CONTEXT_PARAM_BANNABLE + 1;

	igt_subtest("invalid-param-get") {
		arg.context = ctx;
		igt_assert_eq(__gem_context_get_param(fd, &arg), -EINVAL);
	}

	igt_subtest("invalid-param-set") {
		arg.context = ctx;
		igt_assert_eq(__gem_context_set_param(fd, &arg), -EINVAL);
	}

	igt_fixture
		close(fd);
}
