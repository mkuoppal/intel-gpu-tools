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
 */

/** @file kms_vblank.c
 *
 * This is a test of performance of drmWaitVblank.
 */

#include "igt.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <drm.h>

#include "intel_bufmgr.h"

IGT_TEST_DESCRIPTION("Test speed of WaitVblank.");

typedef struct {
	igt_display_t display;
	struct igt_fb primary_fb;
	igt_output_t *output;
	enum pipe pipe;
	uint8_t mode_busy:1;
} data_t;

static double elapsed(const struct timespec *start,
		      const struct timespec *end,
		      int loop)
{
	return (1e6*(end->tv_sec - start->tv_sec) + (end->tv_nsec - start->tv_nsec)/1000)/loop;
}

static bool prepare_crtc(data_t *data, int fd, igt_output_t *output)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, data->pipe);
	igt_display_commit(display);

	if (!output->valid) {
		igt_output_set_pipe(output, PIPE_ANY);
		igt_display_commit(display);
		return false;
	}

	/* create and set the primary plane fb */
	mode = igt_output_get_mode(output);
	igt_create_color_fb(fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    LOCAL_DRM_FORMAT_MOD_NONE,
			    0.0, 0.0, 0.0,
			    &data->primary_fb);

	primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
	igt_plane_set_fb(primary, &data->primary_fb);

	igt_display_commit(display);

	igt_wait_for_vblank(fd, data->pipe);

	return true;
}

static void cleanup_crtc(data_t *data, int fd, igt_output_t *output)
{
	igt_display_t *display = &data->display;
	igt_plane_t *primary;

	igt_remove_fb(fd, &data->primary_fb);

	primary = igt_output_get_plane(output, IGT_PLANE_PRIMARY);
	igt_plane_set_fb(primary, NULL);

	igt_output_set_pipe(output, PIPE_ANY);
	igt_display_commit(display);
}

static void run_test(data_t *data, int fd, void (*testfunc)(data_t *, int))
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe p;
	unsigned int valid_tests = 0;

	for_each_connected_output(display, output) {
		for_each_pipe(display, p) {
			data->pipe = p;

			if (!prepare_crtc(data, fd, output))
				continue;

			valid_tests++;

			igt_info("Beginning %s on pipe %s, connector %s\n",
				 igt_subtest_name(),
				 kmstest_pipe_name(data->pipe),
				 igt_output_name(output));

			testfunc(data, fd);

			igt_info("\n%s on pipe %s, connector %s: PASSED\n\n",
				 igt_subtest_name(),
				 kmstest_pipe_name(data->pipe),
				 igt_output_name(output));

			/* cleanup what prepare_crtc() has done */
			cleanup_crtc(data, fd, output);
		}
	}

	igt_require_f(valid_tests, "no valid crtc/connector combinations found\n");
}

static void accuracy(data_t *data, int fd)
{
	union drm_wait_vblank vbl;
	unsigned long target;
	uint32_t pipe_id_flag;
	int n;

	memset(&vbl, 0, sizeof(vbl));
	pipe_id_flag = kmstest_get_vbl_flag(data->pipe);

	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.type |= pipe_id_flag;
	vbl.request.sequence = 1;
	do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);

	target = vbl.reply.sequence + 60;
	for (n = 0; n < 60; n++) {
		vbl.request.type = DRM_VBLANK_RELATIVE;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = 1;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);

		vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = target;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
	}
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.type |= pipe_id_flag;
	vbl.request.sequence = 0;
	do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
	igt_assert_eq(vbl.reply.sequence, target);

	for (n = 0; n < 60; n++) {
		struct drm_event_vblank ev;
		igt_assert_eq(read(fd, &ev, sizeof(ev)), sizeof(ev));
		igt_assert_eq(ev.sequence, target);
	}
}

static void vblank_query(data_t *data, int fd)
{
	union drm_wait_vblank vbl;
	struct timespec start, end;
	unsigned long sq, count = 0;
	struct drm_event_vblank buf;
	uint32_t pipe_id_flag;

	memset(&vbl, 0, sizeof(vbl));
	pipe_id_flag = kmstest_get_vbl_flag(data->pipe);

	if (data->mode_busy) {
		vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = 72;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
	}

	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.type |= pipe_id_flag;
	vbl.request.sequence = 0;
	do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);

	sq = vbl.reply.sequence;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		vbl.request.type = DRM_VBLANK_RELATIVE;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = 0;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
		count++;
	} while ((vbl.reply.sequence - sq) <= 60);
	clock_gettime(CLOCK_MONOTONIC, &end);

	igt_info("Time to query current counter (%s):		%7.3fµs\n",
		 data->mode_busy ? "busy" : "idle", elapsed(&start, &end, count));

	if (data->mode_busy)
		igt_assert_eq(read(fd, &buf, sizeof(buf)), sizeof(buf));
}

static void vblank_wait(data_t *data, int fd)
{
	union drm_wait_vblank vbl;
	struct timespec start, end;
	unsigned long sq, count = 0;
	struct drm_event_vblank buf;
	uint32_t pipe_id_flag;

	memset(&vbl, 0, sizeof(vbl));
	pipe_id_flag = kmstest_get_vbl_flag(data->pipe);

	if (data->mode_busy) {
		vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = 72;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
	}

	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.type |= pipe_id_flag;
	vbl.request.sequence = 0;
	do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);

	sq = vbl.reply.sequence;

	clock_gettime(CLOCK_MONOTONIC, &start);
	do {
		vbl.request.type = DRM_VBLANK_RELATIVE;
		vbl.request.type |= pipe_id_flag;
		vbl.request.sequence = 1;
		do_ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &vbl);
		count++;
	} while ((vbl.reply.sequence - sq) <= 60);
	clock_gettime(CLOCK_MONOTONIC, &end);

	igt_info("Time to wait for %ld/%d vblanks (%s):		%7.3fµs\n",
		 count, (int)(vbl.reply.sequence - sq),
		 data->mode_busy ? "busy" : "idle",
		 elapsed(&start, &end, count));

	if (data->mode_busy)
		igt_assert_eq(read(fd, &buf, sizeof(buf)), sizeof(buf));
}

igt_main
{
	int fd;
	data_t data;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_display_init(&data.display, fd);
	}

	igt_subtest("accuracy") {
		data.mode_busy = 0;
		run_test(&data, fd, accuracy);
	}

	igt_subtest("query-idle") {
		data.mode_busy = 0;
		run_test(&data, fd, vblank_query);
	}

	igt_subtest("query-busy") {
		data.mode_busy = 1;
		run_test(&data, fd, vblank_query);
	}

	igt_subtest("wait-idle") {
		data.mode_busy = 0;
		run_test(&data, fd, vblank_wait);
	}

	igt_subtest("wait-busy") {
		data.mode_busy = 1;
		run_test(&data, fd, vblank_wait);
	}
}
