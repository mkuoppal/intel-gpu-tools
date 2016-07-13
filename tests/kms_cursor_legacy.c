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
#include "igt_rand.h"
#include "igt_stats.h"

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax()	__builtin_ia32_pause()
#else
#define cpu_relax()	asm volatile("": : :"memory")
#endif

IGT_TEST_DESCRIPTION("Stress legacy cursor ioctl");

static void stress(igt_display_t *display,
		   int pipe, int num_children, unsigned mode,
		   int timeout)
{
	struct drm_mode_cursor arg;
	uint64_t *results;
	bool torture;
	int n;
	unsigned crtc_id[I915_MAX_PIPES], num_crtcs;

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
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	if (pipe < 0) {
		num_crtcs = display->n_pipes;
		for_each_pipe(display, n) {
			arg.crtc_id = crtc_id[n] = display->pipes[n].crtc_id;
			drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		}
	} else {
		num_crtcs = 1;
		arg.crtc_id = crtc_id[0] = display->pipes[pipe].crtc_id;
		drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
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

		hars_petruska_f54_1_random_perturb(child);
		igt_until_timeout(timeout) {
			arg.crtc_id = crtc_id[hars_petruska_f54_1_random_unsafe() % num_crtcs];
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
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

	gem_close(display->drm_fd, arg.handle);
	munmap(results, 4096);
}

static bool set_fb_on_crtc(igt_display_t *display, int pipe, struct igt_fb *fb_info)
{
	drmModeModeInfoPtr modes;
	igt_output_t *output;

	for_each_valid_output_on_pipe(display, pipe, output) {
		struct drm_mode_crtc set;
		int m;

		modes = output->config.connector->modes;

		for (m = 0; m < output->config.connector->count_modes; m++) {
			if (modes[m].hdisplay == fb_info->width &&
			    modes[m].vdisplay == fb_info->height)
				break;
		}
		if (m == output->config.connector->count_modes)
			continue;

		memset(&set, 0, sizeof(set));
		set.crtc_id = display->pipes[pipe].crtc_id;
		set.fb_id = fb_info->fb_id;
		set.set_connectors_ptr = (uintptr_t)&output->id;
		set.count_connectors = 1;
		memcpy(&set.mode, &modes[m], sizeof(set.mode));
		set.mode_valid = 1;
		if (drmIoctl(display->drm_fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0)
			return true;
	}

	return false;
}

static void flip(igt_display_t *display,
		int cursor_pipe, int flip_pipe,
		int timeout)
{
	struct drm_mode_cursor arg;
	uint64_t *results;
	struct igt_fb fb_info;
	uint32_t fb_id;

	results = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(results != MAP_FAILED);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = display->pipes[cursor_pipe].crtc_id;
	arg.width = 64;
	arg.height = 64;
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);

	fb_id = igt_create_fb(display->drm_fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(display, flip_pipe, &fb_info));

	arg.flags = DRM_MODE_CURSOR_MOVE;
	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_debug("cursor count=%lu\n", count);
		results[0] = count;
	}
	igt_fork(child, 1) {
		unsigned long count = 0;
		unsigned crtc = display->pipes[flip_pipe].crtc_id;

		igt_until_timeout(timeout) {
			char buf[128];
			drmModePageFlip(display->drm_fd, crtc, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT,
					NULL);
			while (read(display->drm_fd, buf, sizeof(buf)) < 0 &&
			       (errno == EINTR || errno == EAGAIN))
				;
			count++;
		}

		igt_debug("flip count=%lu\n", count);
		results[1] = count;
	}
	igt_waitchildren();

	gem_close(display->drm_fd, arg.handle);
	munmap(results, 4096);
}

static inline uint32_t pipe_select(int pipe)
{
	if (pipe > 1)
		return pipe << DRM_VBLANK_HIGH_CRTC_SHIFT;
	else if (pipe > 0)
		return DRM_VBLANK_SECONDARY;
	else
		return 0;
}

static unsigned get_vblank(int fd, int pipe, unsigned flags)
{
	union drm_wait_vblank vbl;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE | pipe_select(pipe) | flags;
	if (drmIoctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl))
		return 0;

	return vbl.reply.sequence;
}

static void basic_flip_vs_cursor(igt_display_t *display, int nloops)
{
	struct drm_mode_cursor arg;
	struct drm_event_vblank vbl;
	struct igt_fb fb_info;
	unsigned vblank_start;
	int target;
	uint32_t fb_id;

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = display->pipes[0].crtc_id;
	arg.width = 64;
	arg.height = 64;
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
	arg.flags = DRM_MODE_CURSOR_MOVE;

	fb_id = igt_create_fb(display->drm_fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(display, 0, &fb_info));

	target = 4096;
	do {
		vblank_start = get_vblank(display->drm_fd, 0, DRM_VBLANK_NEXTONMISS);
		igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		target /= 2;
		if (get_vblank(display->drm_fd, 0, 0) == vblank_start)
			break;
	} while (target);
	igt_require(target > 1);

	igt_debug("Using a target of %d cursor updates per half-vblank\n",
		  target);

	vblank_start = get_vblank(display->drm_fd, 0, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);
	for (int n = 0; n < target; n++)
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
	igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);

	while (nloops--) {
		/* Start with a synchronous query to align with the vblank */
		vblank_start = get_vblank(display->drm_fd, 0, DRM_VBLANK_NEXTONMISS);
		do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);

		/* Schedule a nonblocking flip for the next vblank */
		do_or_die(drmModePageFlip(display->drm_fd, arg.crtc_id, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &fb_id));

		igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(display->drm_fd, &vbl, sizeof(vbl)));
		igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start + 1);
		igt_reset_timeout();
	}

	igt_remove_fb(display->drm_fd, &fb_info);
	gem_close(display->drm_fd, arg.handle);
}

static void basic_cursor_vs_flip(igt_display_t *display, int nloops)
{
	struct drm_mode_cursor arg;
	struct drm_event_vblank vbl;
	struct igt_fb fb_info;
	unsigned vblank_start, vblank_last;
	volatile unsigned long *shared;
	int target;
	uint32_t fb_id;

	shared = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	igt_assert(shared != MAP_FAILED);

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = display->pipes[0].crtc_id;
	arg.width = 64;
	arg.height = 64;
	arg.handle = kmstest_dumb_create(display->drm_fd, 64, 64, 32, NULL, NULL);

	drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
	arg.flags = DRM_MODE_CURSOR_MOVE;

	fb_id = igt_create_fb(display->drm_fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(display, 0, &fb_info));

	target = 4096;
	do {
		vblank_start = get_vblank(display->drm_fd, 0, DRM_VBLANK_NEXTONMISS);
		igt_assert_eq(get_vblank(display->drm_fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
		target /= 2;
		if (get_vblank(display->drm_fd, 0, 0) == vblank_start)
			break;
	} while (target);
	igt_require(target > 1);

	igt_debug("Using a target of %d cursor updates per half-vblank\n",
		  target);

	for (int i = 0; i < nloops; i++) {
		shared[0] = 0;
		igt_fork(child, 1) {
			unsigned long count = 0;
			while (!shared[0]) {
				drmIoctl(display->drm_fd, DRM_IOCTL_MODE_CURSOR, &arg);
				count++;
			}
			igt_debug("child: %lu cursor updates\n", count);
			shared[0] = count;
		}
		do_or_die(drmModePageFlip(display->drm_fd, arg.crtc_id, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &fb_id));
		igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
		vblank_start = vblank_last = vbl.sequence;
		for (int n = 0; n < 60; n++) {
			do_or_die(drmModePageFlip(display->drm_fd, arg.crtc_id, fb_id,
						DRM_MODE_PAGE_FLIP_EVENT, &fb_id));
			igt_assert_eq(read(display->drm_fd, &vbl, sizeof(vbl)), sizeof(vbl));
			if (vbl.sequence != vblank_last + 1) {
				igt_warn("page flip %d was delayed, missed %d frames\n",
					 n, vbl.sequence - vblank_last - 1);
			}
			vblank_last = vbl.sequence;
		}
		igt_assert_eq(vbl.sequence, vblank_start + 60);

		shared[0] = 1;
		igt_waitchildren();
		igt_assert_f(shared[0] > 60*target,
			     "completed %lu cursor updated in a period of 60 flips, "
			     "we expect to complete approximately %lu updateds, "
			     "with the threshold set at %lu\n",
			     shared[0], 2*60ul*target, 60ul*target);
	}

	igt_remove_fb(display->drm_fd, &fb_info);
	gem_close(display->drm_fd, arg.handle);
	munmap((void *)shared, 4096);
}

igt_main
{
	const int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
	igt_display_t display = { .drm_fd = -1 };

	igt_skip_on_simulation();

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();

		igt_display_init(&display, display.drm_fd);
		igt_require(display.n_pipes > 0);
	}

	igt_subtest_group {
		for (int n = 0; n < I915_MAX_PIPES; n++) {
			errno = 0;

			igt_fixture {
				igt_skip_on(n >= display.n_pipes);
			}

			igt_subtest_f("pipe-%s-single-bo", kmstest_pipe_name(n))
				stress(&display, n, 1, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-single-move", kmstest_pipe_name(n))
				stress(&display, n, 1, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-forked-bo", kmstest_pipe_name(n))
				stress(&display, n, ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-forked-move", kmstest_pipe_name(n))
				stress(&display, n, ncpus, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-torture-bo", kmstest_pipe_name(n))
				stress(&display, n, -ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-torture-move", kmstest_pipe_name(n))
				stress(&display, n, -ncpus, DRM_MODE_CURSOR_MOVE, 20);
		}
	}

	igt_subtest("all-pipes-single-bo")
		stress(&display, -1, 1, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-single-move")
		stress(&display, -1, 1, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-forked-bo")
		stress(&display, -1, ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-forked-move")
		stress(&display, -1, ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-torture-bo")
		stress(&display, -1, -ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-torture-move")
		stress(&display, -1, -ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("basic-flip-vs-cursor")
		basic_flip_vs_cursor(&display, 1);
	igt_subtest("long-flip-vs-cursor")
		basic_flip_vs_cursor(&display, 150);
	igt_subtest("basic-cursor-vs-flip")
		basic_cursor_vs_flip(&display, 1);
	igt_subtest("long-cursor-vs-flip")
		basic_cursor_vs_flip(&display, 150);

	igt_subtest("cursorA-vs-flipA")
		flip(&display, 0, 0, 10);

	igt_subtest_group {
		igt_fixture
			igt_skip_on(display.n_pipes < 2);

		igt_subtest("cursorA-vs-flipB")
			flip(&display, 0, 1, 10);
		igt_subtest("cursorB-vs-flipA")
			flip(&display, 1, 0, 10);
		igt_subtest("cursorB-vs-flipB")
			flip(&display, 1, 1, 10);
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
