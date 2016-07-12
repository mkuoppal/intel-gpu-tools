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

struct data {
	int fd;
	drmModeRes *resources;
};

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

		hars_petruska_f54_1_random_perturb(child);
		igt_until_timeout(timeout) {
			arg.crtc_id = crtc_id[hars_petruska_f54_1_random_unsafe() % num_crtcs];
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

static bool set_fb_on_crtc(struct data *data, int pipe, struct igt_fb *fb_info)
{
	struct drm_mode_modeinfo *modes = malloc(4096*sizeof(*modes));
	uint32_t encoders[32];

	for (int o = 0; o < data->resources->count_connectors; o++) {
		struct drm_mode_get_connector conn;
		struct drm_mode_crtc set;
		int e, m;

		memset(&conn, 0, sizeof(conn));
		conn.connector_id = data->resources->connectors[o];
		conn.count_modes = 4096;
		conn.modes_ptr = (uintptr_t)modes;
		conn.count_encoders = 32;
		conn.encoders_ptr = (uintptr_t)encoders;

		drmIoctl(data->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);

		for (e = 0; e < conn.count_encoders; e++) {
			struct drm_mode_get_encoder enc;

			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = encoders[e];
			drmIoctl(data->fd, DRM_IOCTL_MODE_GETENCODER, &enc);
			if (enc.possible_crtcs & (1 << pipe))
				break;
		}
		if (e == conn.count_encoders)
			continue;

		for (m = 0; m < conn.count_modes; m++) {
			if (modes[m].hdisplay == fb_info->width &&
			    modes[m].vdisplay == fb_info->height)
				break;
		}
		if (m == conn.count_modes)
			continue;

		memset(&set, 0, sizeof(set));
		set.crtc_id = data->resources->crtcs[pipe];
		set.fb_id = fb_info->fb_id;
		set.set_connectors_ptr = (uintptr_t)&conn.connector_id;
		set.count_connectors = 1;
		set.mode = modes[m];
		set.mode_valid = 1;
		if (drmIoctl(data->fd, DRM_IOCTL_MODE_SETCRTC, &set) == 0)
			return true;
	}

	return false;
}

static void flip(struct data *data,
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
	arg.crtc_id = data->resources->crtcs[cursor_pipe];
	arg.width = 64;
	arg.height = 64;
	arg.handle = gem_create(data->fd, 4*64*64);

	drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);

	fb_id = igt_create_fb(data->fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(data, flip_pipe, &fb_info));

	arg.flags = DRM_MODE_CURSOR_MOVE;
	igt_fork(child, 1) {
		unsigned long count = 0;

		igt_until_timeout(timeout) {
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
			count++;
		}

		igt_debug("cursor count=%lu\n", count);
		results[0] = count;
	}
	igt_fork(child, 1) {
		unsigned long count = 0;
		unsigned crtc = data->resources->crtcs[flip_pipe];

		igt_until_timeout(timeout) {
			char buf[128];
			drmModePageFlip(data->fd, crtc, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT,
					NULL);
			while (read(data->fd, buf, sizeof(buf)) < 0 &&
			       (errno == EINTR || errno == EAGAIN))
				;
			count++;
		}

		igt_debug("flip count=%lu\n", count);
		results[1] = count;
	}
	igt_waitchildren();

	gem_close(data->fd, arg.handle);
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

static void basic_flip_vs_cursor(struct data *data, int nloops)
{
	struct drm_mode_cursor arg;
	struct drm_event_vblank vbl;
	struct igt_fb fb_info;
	unsigned vblank_start;
	int target;
	uint32_t fb_id;

	memset(&arg, 0, sizeof(arg));
	arg.flags = DRM_MODE_CURSOR_BO;
	arg.crtc_id = data->resources->crtcs[0];
	arg.width = 64;
	arg.height = 64;
	arg.handle = gem_create(data->fd, 4*64*64);

	drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
	arg.flags = DRM_MODE_CURSOR_MOVE;

	fb_id = igt_create_fb(data->fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(data, 0, &fb_info));

	target = 4096;
	do {
		vblank_start = get_vblank(data->fd, 0, DRM_VBLANK_NEXTONMISS);
		igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
		target /= 2;
		if (get_vblank(data->fd, 0, 0) == vblank_start)
			break;
	} while (target);
	igt_require(target > 1);

	igt_debug("Using a target of %d cursor updates per half-vblank\n",
		  target);

	vblank_start = get_vblank(data->fd, 0, DRM_VBLANK_NEXTONMISS);
	igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);
	for (int n = 0; n < target; n++)
		do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
	igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);

	while (nloops--) {
		/* Start with a synchronous query to align with the vblank */
		vblank_start = get_vblank(data->fd, 0, DRM_VBLANK_NEXTONMISS);
		do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);

		/* Schedule a nonblocking flip for the next vblank */
		do_or_die(drmModePageFlip(data->fd, arg.crtc_id, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &fb_id));

		igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
		igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);

		igt_set_timeout(1, "Stuck page flip");
		igt_ignore_warn(read(data->fd, &vbl, sizeof(vbl)));
		igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start + 1);
		igt_reset_timeout();
	}

	igt_remove_fb(data->fd, &fb_info);
	gem_close(data->fd, arg.handle);
}

static void basic_cursor_vs_flip(struct data *data, int nloops)
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
	arg.crtc_id = data->resources->crtcs[0];
	arg.width = 64;
	arg.height = 64;
	arg.handle = gem_create(data->fd, 4*64*64);

	drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
	arg.flags = DRM_MODE_CURSOR_MOVE;

	fb_id = igt_create_fb(data->fd, 1024, 768, DRM_FORMAT_XRGB8888,
			      I915_TILING_NONE, &fb_info);
	igt_require(set_fb_on_crtc(data, 0, &fb_info));

	target = 4096;
	do {
		vblank_start = get_vblank(data->fd, 0, DRM_VBLANK_NEXTONMISS);
		igt_assert_eq(get_vblank(data->fd, 0, 0), vblank_start);
		for (int n = 0; n < target; n++)
			do_ioctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
		target /= 2;
		if (get_vblank(data->fd, 0, 0) == vblank_start)
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
				drmIoctl(data->fd, DRM_IOCTL_MODE_CURSOR, &arg);
				count++;
			}
			igt_debug("child: %lu cursor updates\n", count);
			shared[0] = count;
		}
		do_or_die(drmModePageFlip(data->fd, arg.crtc_id, fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &fb_id));
		igt_assert_eq(read(data->fd, &vbl, sizeof(vbl)), sizeof(vbl));
		vblank_start = vblank_last = vbl.sequence;
		for (int n = 0; n < 60; n++) {
			do_or_die(drmModePageFlip(data->fd, arg.crtc_id, fb_id,
						DRM_MODE_PAGE_FLIP_EVENT, &fb_id));
			igt_assert_eq(read(data->fd, &vbl, sizeof(vbl)), sizeof(vbl));
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

	igt_remove_fb(data->fd, &fb_info);
	gem_close(data->fd, arg.handle);
	munmap((void *)shared, 4096);
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

		igt_require(data.resources->count_crtcs > 0);
	}

	igt_subtest_group {
		for (int n = 0; n < I915_MAX_PIPES; n++) {
			uint32_t *crtcs = NULL;

			errno = 0;
			igt_fixture {
				igt_skip_on(n >= data.resources->count_crtcs);
				crtcs = &data.resources->crtcs[n];
			}

			igt_subtest_f("pipe-%s-single-bo", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, 1, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-single-move", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, 1, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-forked-bo", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-forked-move", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, ncpus, DRM_MODE_CURSOR_MOVE, 20);

			igt_subtest_f("pipe-%s-torture-bo", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, -ncpus, DRM_MODE_CURSOR_BO, 20);
			igt_subtest_f("pipe-%s-torture-move", kmstest_pipe_name(n))
				stress(&data, crtcs, 1, -ncpus, DRM_MODE_CURSOR_MOVE, 20);
		}
	}

	igt_subtest("all-pipes-single-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       1, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-single-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       1, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-forked-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-forked-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("all-pipes-torture-bo")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       -ncpus, DRM_MODE_CURSOR_BO, 20);
	igt_subtest("all-pipes-torture-move")
		stress(&data,
		       data.resources->crtcs, data.resources->count_crtcs,
		       -ncpus, DRM_MODE_CURSOR_MOVE, 20);

	igt_subtest("basic-flip-vs-cursor")
		basic_flip_vs_cursor(&data, 1);
	igt_subtest("long-flip-vs-cursor")
		basic_flip_vs_cursor(&data, 150);
	igt_subtest("basic-cursor-vs-flip")
		basic_cursor_vs_flip(&data, 1);
	igt_subtest("long-cursor-vs-flip")
		basic_cursor_vs_flip(&data, 150);

	igt_subtest("cursorA-vs-flipA")
		flip(&data, 0, 0, 10);

	igt_subtest_group {
		igt_fixture
			igt_skip_on(data.resources->count_crtcs < 2);

		igt_subtest("cursorA-vs-flipB")
			flip(&data, 0, 1, 10);
		igt_subtest("cursorB-vs-flipA")
			flip(&data, 1, 0, 10);
		igt_subtest("cursorB-vs-flipB")
			flip(&data, 1, 1, 10);
	}

	igt_fixture {
		drmModeFreeResources(data.resources);
	}
}
