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
#include "igt_stats.h"

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax()	__builtin_ia32_pause()
#else
#define cpu_relax()	asm volatile("": : :"memory")
#endif

IGT_TEST_DESCRIPTION("Stress legacy cursor ioctl");

struct data {
	int fd;
	drmModeRes *resources;
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
		   uint32_t *crtc_id, unsigned num_crtcs,
		   int num_children, unsigned mode,
		   int timeout)
{
	struct drm_mode_cursor arg;
	uint64_t *results;
	bool torture;
	int n;

	torture = false;
	if (num_children < 0) {
		torture = true;
		num_children = -num_children;
	}

	results = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = 0;
	arg.width = 64;
	arg.height = 64;
	arg.handle = gem_create(data->fd, 4*64*64);

	for (n = 0; n < num_crtcs; n++) {
		arg.crtc_id = crtc_id[n];
		drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
	}

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
			arg.crtc_id = crtc_id[hars_petruska_f54_1_random() % num_crtcs];
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_debug("[%d] count=%lu\n", child, count);
		results[child] = count;
	}
	if (torture) {
		igt_fork(child, num_children) {
			struct sched_param rt = {.sched_priority = 1 };
			cpu_set_t allowed;
			unsigned long long count = 0;

			sched_setscheduler(getpid(), SCHED_RR, &rt);

			CPU_ZERO(&allowed);
			CPU_SET(child, &allowed);
			sched_setaffinity(getpid(), sizeof(cpu_set_t), &allowed);
			igt_until_timeout(timeout) {
				count++;
				cpu_relax();
			}
			igt_debug("[hog:%d] count=%llu\n", child, count);
		}
	}
	igt_waitchildren();

	if (num_children > 1) {
		igt_stats_t stats;

		igt_stats_init_with_size(&stats, num_children);
		results[num_children] = 0;
		for (int child = 0; child < num_children; child++) {
			igt_stats_push(&stats, results[child]);
			results[num_children] += results[child];
		}
		igt_info("Total updates %llu (median of %d processes is %.2f)\n",
			 (long long)results[num_children],
			 num_children,
			 igt_stats_get_median(&stats));
		igt_stats_fini(&stats);
	} else {
		igt_info("Total updates %llu\n", (long long)results[0]);
	}

	gem_close(data->fd, arg.handle);
	munmap(results, 4096);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	struct data data = { .fd = -1 };

	igt_skip_on_simulation();

	igt_fixture {
		data.fd = drm_open_driver_master(DRIVER_INTEL);
		kmstest_set_vt_graphics_mode();

		data.resources = drmModeGetResources(data.fd);
		igt_assert(data.resources);
	}

	igt_subtest_group {
		for (int n = 0; n < 26; n++) {
			uint32_t *crtcs = NULL;

			errno = 0;
			igt_fixture {
				igt_skip_on(n >= data.resources->count_crtcs);
				crtcs = &data.resources->crtcs[n];
			}

			igt_subtest_f("single-%c-bo", 'A' + n)
				stress(&data, crtcs, 1, 1, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("single-%c-move", 'A' + n)
				stress(&data, crtcs, 1, 1, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("forked-%c-bo", 'A' + n)
				stress(&data, crtcs, 1, ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("forked-%c-move", 'A' + n)
				stress(&data, crtcs, 1, ncpus, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("torture-%c-bo", 'A' + n)
				stress(&data, crtcs, 1, -ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("torture-%c-move", 'A' + n)
				stress(&data, crtcs, 1, -ncpus, DRM_MODE_CURSOR_MOVE, 20);
		}
	}

	igt_subtest("single-all-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       1, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("single-all-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       1, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("forked-all-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("forked-all-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("torture-all-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       -ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("torture-all-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       -ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_fixture {
		drmModeFreeResources(data.resources);
	}
}
