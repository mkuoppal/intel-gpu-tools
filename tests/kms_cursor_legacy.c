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
 */

#define _GNU_SOURCE
#include <sched.h>

#include "igt.h"

IGT_TEST_DESCRIPTION("Stress legacy cursor ioctl");

struct data {
	int fd;
};

static uint32_t state = 0x12345678;

static uint32_t
hars_petruska_f54_1_random (void)
{
#define rol(x,k) ((x << k) | (x >> (32-k)))
    return state = (state ^ rol (state, 5) ^ rol (state, 24)) + 0x37798849;
#undef rol
}

static void stress(struct data *data,
		   int num_children, unsigned mode, int timeout)
{
	drmModeRes *r;
	struct drm_mode_cursor arg;
	int n;

	r = drmModeGetResources(data->fd);
	igt_assert(r);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = 0;
	arg.width = 64;
	arg.height = 64;
	arg.handle = gem_create(data->fd, 4*64*64);

	for (n = 0; n < r->count_crtcs; n++)
		drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);

	arg.flags = mode;
	igt_fork(child, num_children) {
		struct sched_param rt = {.sched_priority = 99 };
		cpu_set_t allowed;
		unsigned long count = 0;

		sched_setscheduler(getpid(), SCHED_RR, &rt);

		CPU_ZERO(&allowed);
		CPU_SET(child, &allowed);
		sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed);

		state ^= child;
		igt_until_timeout(timeout) {
			arg.crtc_id = r->crtcs[hars_petruska_f54_1_random() % r->count_crtcs];
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_info("[%d] count=%lu\n", child, count);
	}
	igt_waitchildren();

	gem_close(data->fd, arg.handle);
	drmModeFreeResources(r);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct data data = { .fd = -1 };

	igt_skip_on_simulation();

	igt_fixture {
		data.fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();
	}

	igt_subtest("single-bo")
		stress(&data, 1, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("single-move")
		stress(&data, 1, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("forked-bo")
		stress(&data, ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("forked-move")
		stress(&data, ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_fixture {
	}
}
