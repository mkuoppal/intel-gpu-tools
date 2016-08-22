/*
 * Copyright © 2016 Intel Corporation
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

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef DRM_CAP_CURSOR_WIDTH
#define DRM_CAP_CURSOR_WIDTH 0x8
#endif
#ifndef DRM_CAP_CURSOR_HEIGHT
#define DRM_CAP_CURSOR_HEIGHT 0x9
#endif

struct plane_parms {
	struct igt_fb *fb;
	uint32_t width, height;
};

#define hweight32 __builtin_popcount

static void
wm_setup_plane(igt_display_t *display, enum pipe pipe,
	       uint32_t mask, struct plane_parms *parms)
{
	igt_plane_t *plane;

	/*
	* Make sure these buffers are suited for display use
	* because most of the modeset operations must be fast
	* later on.
	*/
	for_each_plane_on_pipe(display, pipe, plane) {
		int i = plane->index;

		if (!((1 << plane->index) & mask)) {
			igt_plane_set_fb(plane, NULL);
			continue;
		}

		igt_plane_set_fb(plane, parms[i].fb);
		igt_fb_set_size(parms[i].fb, plane, parms[i].width, parms[i].height);
		igt_plane_set_size(plane, parms[i].width, parms[i].height);
	}
}

static bool skip_on_unsupported_nonblocking_modeset(igt_display_t *display)
{
	enum pipe pipe;
	int ret;

	/*
	 * Make sure we only skip when the suggested configuration is
	 * unsupported by committing it first with TEST_ONLY, if it's
	 * unsupported -EINVAL is returned. If the second commit returns
	 * -EINVAL, it's from not being able to support nonblocking modeset.
	 */
	igt_display_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK, NULL);

	if (ret == -EINVAL)
		return true;

	igt_assert_eq(ret, 0);

	/* Force the next state to update all crtc's, to synchronize with the nonblocking modeset. */
	for_each_pipe(display, pipe)
		display->pipes[pipe].mode_changed = true;

	return false;
}

static void ev_page_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *user_data)
{
	igt_debug("Retrieved vblank seq: %u on unk\n", seq);
}

static drmEventContext drm_events = {
	.version = DRM_EVENT_CONTEXT_VERSION,
	.page_flip_handler = ev_page_flip
};

enum transition_type {
	TRANSITION_PLANES,
	TRANSITION_MODESET,
	TRANSITION_MODESET_DISABLE,
};

/*
 * 1. Set primary plane to a known fb.
 * 2. Make sure getcrtc returns the correct fb id.
 * 3. Call rmfb on the fb.
 * 4. Make sure getcrtc returns 0 fb id.
 *
 * RMFB is supposed to free the framebuffers from any and all planes,
 * so test this and make sure it works.
 */
static void
run_transition_test(igt_display_t *display, enum pipe pipe, igt_output_t *output, enum transition_type type, bool nonblocking)
{
	struct igt_fb fb, argb_fb;
	drmModeModeInfo *mode, override_mode;
	igt_plane_t *plane;
	uint64_t cursor_width, cursor_height;
	uint32_t n_planes = display->pipes[pipe].n_planes;
	uint32_t iter_max = 1 << n_planes, i;
	struct plane_parms parms[IGT_MAX_PLANES];
	bool skip_test = false;
	unsigned flags = DRM_MODE_PAGE_FLIP_EVENT;

	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	if (type >= TRANSITION_MODESET)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	mode = igt_output_get_mode(output);
	override_mode = *mode;
	override_mode.flags |= DRM_MODE_FLAG_HSKEW;

	igt_create_fb(display->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &fb);

	igt_create_fb(display->drm_fd, mode->hdisplay, mode->vdisplay,
		      DRM_FORMAT_ARGB8888, LOCAL_DRM_FORMAT_MOD_NONE, &argb_fb);

	do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_width));
	if (cursor_width > mode->hdisplay)
		cursor_width = mode->hdisplay;

	do_or_die(drmGetCap(display->drm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_height));
	if (cursor_height > mode->vdisplay)
		cursor_height = mode->vdisplay;

	igt_output_set_pipe(output, pipe);

	wm_setup_plane(display, pipe, 0, NULL);

	if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) {
		skip_test = nonblocking && skip_on_unsupported_nonblocking_modeset(display);
		if (skip_test)
			goto cleanup;

		igt_output_set_pipe(output, PIPE_NONE);

		igt_display_commit2(display, COMMIT_ATOMIC);

		igt_output_set_pipe(output, pipe);
	}

	for_each_plane_on_pipe(display, pipe, plane) {
		i = plane->index;

		if (plane->is_primary)
			parms[i].fb = &fb;
		else
			parms[i].fb = &argb_fb;

		if (plane->is_cursor) {
			parms[i].width = cursor_width;
			parms[i].height = cursor_height;
		} else {
			parms[i].width = mode->hdisplay;
			parms[i].height = mode->vdisplay;
		}
	}

	igt_display_commit2(display, COMMIT_ATOMIC);

	/*
	 * In some configurations the tests may not run to completion with all
	 * sprite planes lit up at 4k resolution, try decreasing width/size of secondary
	 * planes to fix this
	 */
	while (1) {
		int ret;

		wm_setup_plane(display, pipe, iter_max - 1, parms);
		ret = igt_display_try_commit_atomic(display, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		if (ret != -EINVAL || n_planes < 3)
			break;

		ret = 0;
		for_each_plane_on_pipe(display, pipe, plane) {
			i = plane->index;

			if (plane->is_primary || plane->is_cursor)
				continue;

			if (parms[i].width <= 512)
				continue;

			parms[i].width /= 2;
			ret = 1;
			igt_info("Reducing sprite %i to %ux%u\n", i - 1, parms[i].width, parms[i].height);
			break;
		}

		if (!ret)
			igt_skip("Cannot run tests without proper size sprite planes\n");
	}

	for (i = 0; i < iter_max; i++) {
		igt_output_set_pipe(output, pipe);

		wm_setup_plane(display, pipe, i, parms);

		igt_display_commit_atomic(display, flags, (void *)(unsigned long)i);
		drmHandleEvent(display->drm_fd, &drm_events);

		if (type == TRANSITION_MODESET_DISABLE) {
			igt_output_set_pipe(output, PIPE_NONE);

			wm_setup_plane(display, pipe, 0, parms);

			igt_display_commit_atomic(display, flags, (void *)0UL);

			drmHandleEvent(display->drm_fd, &drm_events);
		} else {
			uint32_t j;

			/* i -> i+1 will be done when i increases, can be skipped here */
			for (j = iter_max - 1; j > i + 1; j--) {
				wm_setup_plane(display, pipe, j, parms);

				if (type == TRANSITION_MODESET)
					igt_output_override_mode(output, &override_mode);

				igt_display_commit_atomic(display, flags, (void *)(unsigned long)j);
				drmHandleEvent(display->drm_fd, &drm_events);

				wm_setup_plane(display, pipe, i, parms);
				if (type == TRANSITION_MODESET)
					igt_output_override_mode(output, NULL);

				igt_display_commit_atomic(display, flags, (void *)(unsigned long)i);
				drmHandleEvent(display->drm_fd, &drm_events);
			}
		}
	}

cleanup:
	igt_output_set_pipe(output, PIPE_NONE);

	for_each_plane_on_pipe(display, pipe, plane)
		igt_plane_set_fb(plane, NULL);

	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_remove_fb(display->drm_fd, &fb);
	igt_remove_fb(display->drm_fd, &argb_fb);
	if (skip_test)
		igt_skip("Atomic nonblocking modesets are not supported.\n");
}

static void commit_display(igt_display_t *display, unsigned event_mask, bool nonblocking)
{
	unsigned flags;
	int num_events = hweight32(event_mask);
	ssize_t ret;

	flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
	if (nonblocking)
		flags |= DRM_MODE_ATOMIC_NONBLOCK;

	igt_display_commit_atomic(display, flags, NULL);

	while (num_events) {
		char buf[32];
		struct drm_event *e = (void *)buf;
		struct drm_event_vblank *vblank = (void *)buf;
		uint32_t crtc_id, pipe = I915_MAX_PIPES;

		ret = read(display->drm_fd, buf, sizeof(buf));
		if (ret < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		igt_assert(ret >= 0);
		igt_assert_eq(e->type, DRM_EVENT_FLIP_COMPLETE);

		crtc_id = vblank->reserved;
		if (crtc_id) {
			for_each_pipe(display, pipe)
				if (display->pipes[pipe].crtc_id == crtc_id)
					break;

			igt_assert_lt(pipe, display->n_pipes);

			igt_debug("Retrieved vblank seq: %u on %u/%u\n", vblank->sequence, vblank->reserved, pipe);
		} else
			igt_debug("Retrieved vblank seq: %u on unk/unk\n", vblank->sequence);

		num_events--;
	}
}

static unsigned set_combinations(igt_display_t *display, unsigned mask, struct igt_fb *fb)
{
	igt_output_t *output;
	enum pipe pipe;
	unsigned event_mask = 0;

	for_each_connected_output(display, output)
		igt_output_set_pipe(output, PIPE_NONE);

	for_each_pipe(display, pipe) {
		igt_plane_t *plane = &display->pipes[pipe].planes[IGT_PLANE_PRIMARY];
		drmModeModeInfo *mode = NULL;

		if (!(mask & (1 << pipe))) {
			if (display->pipes[pipe].mode_blob) {
				event_mask |= 1 << pipe;
				igt_plane_set_fb(plane, NULL);
			}

			continue;
		}

		event_mask |= 1 << pipe;

		for_each_valid_output_on_pipe(display, pipe, output) {
			if (output->pending_crtc_idx_mask)
				continue;

			mode = igt_output_get_mode(output);
			break;
		}

		if (!mode)
			return 0;

		igt_output_set_pipe(output, pipe);
		igt_plane_set_fb(plane, fb);
		igt_fb_set_size(fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
	}

	return event_mask;
}

static void refresh_primaries(igt_display_t *display)
{
	enum pipe pipe;
	igt_plane_t *plane;

	for_each_pipe(display, pipe)
		for_each_plane_on_pipe(display, pipe, plane)
			if (plane->is_primary && plane->fb)
				plane->fb_changed = true;
}

static void collect_crcs_mask(igt_pipe_crc_t **pipe_crcs, unsigned mask, igt_crc_t *crcs)
{
	int i;

	for (i = 0; i < I915_MAX_PIPES; i++) {
		if (!((1 << i) & mask))
			continue;

		if (!pipe_crcs[i])
			continue;

		igt_pipe_crc_collect_crc(pipe_crcs[i], &crcs[i]);
	}
}

static void run_modeset_tests(igt_display_t *display, int howmany, bool nonblocking)
{
	struct igt_fb fbs[2];
	int i, j;
	unsigned iter_max = 1 << I915_MAX_PIPES;
	igt_pipe_crc_t *pipe_crcs[I915_MAX_PIPES];
	igt_output_t *output;
	unsigned width = 0, height = 0;
	bool skip_test = false;

	for_each_connected_output(display, output) {
		drmModeModeInfo *mode = igt_output_get_mode(output);

		igt_output_set_pipe(output, PIPE_NONE);

		width = max(width, mode->hdisplay);
		height = max(height, mode->vdisplay);
	}

	igt_create_pattern_fb(display->drm_fd, width, height,
				   DRM_FORMAT_XRGB8888, 0, &fbs[0]);
	igt_create_color_pattern_fb(display->drm_fd, width, height,
				    DRM_FORMAT_XRGB8888, 0, .5, .5, .5, &fbs[1]);

	for_each_pipe(display, i) {
		igt_plane_t *plane = &display->pipes[i].planes[IGT_PLANE_PRIMARY];
		drmModeModeInfo *mode = NULL;

		if (is_i915_device(display->drm_fd))
			pipe_crcs[i] = igt_pipe_crc_new(i, INTEL_PIPE_CRC_SOURCE_AUTO);

		for_each_valid_output_on_pipe(display, i, output) {
			if (output->pending_crtc_idx_mask)
				continue;

			igt_output_set_pipe(output, i);
			mode = igt_output_get_mode(output);
			break;
		}

		if (mode) {
			igt_plane_set_fb(plane, &fbs[1]);
			igt_fb_set_size(&fbs[1], plane, mode->hdisplay, mode->vdisplay);
			igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);
		} else
			igt_plane_set_fb(plane, NULL);
	}

	/*
	 * When i915 supports nonblocking modeset, this if branch can be removed.
	 * It's only purpose is to ensure nonblocking modeset works.
	 */
	if (nonblocking && (skip_test = skip_on_unsupported_nonblocking_modeset(display)))
		goto cleanup;

	igt_display_commit2(display, COMMIT_ATOMIC);

	for (i = 0; i < iter_max; i++) {
		igt_crc_t crcs[5][I915_MAX_PIPES];
		unsigned event_mask;

		if (hweight32(i) > howmany)
			continue;

		event_mask = set_combinations(display, i, &fbs[0]);
		if (!event_mask && i)
			continue;

		commit_display(display, event_mask, nonblocking);

		collect_crcs_mask(pipe_crcs, i, crcs[0]);

		for (j = iter_max - 1; j > i + 1; j--) {
			if (hweight32(j) > howmany)
				continue;

			if (hweight32(i) < howmany && hweight32(j) < howmany)
				continue;

			event_mask = set_combinations(display, j, &fbs[1]);
			if (!event_mask)
				continue;

			commit_display(display, event_mask, nonblocking);

			collect_crcs_mask(pipe_crcs, j, crcs[1]);

			refresh_primaries(display);
			commit_display(display, j, nonblocking);
			collect_crcs_mask(pipe_crcs, j, crcs[2]);

			event_mask = set_combinations(display, i, &fbs[0]);
			if (!event_mask)
				continue;

			commit_display(display, event_mask, nonblocking);
			collect_crcs_mask(pipe_crcs, i, crcs[3]);

			refresh_primaries(display);
			commit_display(display, i, nonblocking);
			collect_crcs_mask(pipe_crcs, i, crcs[4]);

			if (!is_i915_device(display->drm_fd))
				continue;

			for (int k = 0; k < I915_MAX_PIPES; k++) {
				if (i & (1 << k)) {
					igt_assert_crc_equal(&crcs[0][k], &crcs[3][k]);
					igt_assert_crc_equal(&crcs[0][k], &crcs[4][k]);
				}

				if (j & (1 << k))
					igt_assert_crc_equal(&crcs[1][k], &crcs[2][k]);
			}
		}
	}

cleanup:
	set_combinations(display, 0, NULL);
	igt_display_commit2(display, COMMIT_ATOMIC);

	if (is_i915_device(display->drm_fd))
		for_each_pipe(display, i)
			igt_pipe_crc_free(pipe_crcs[i]);

	igt_remove_fb(display->drm_fd, &fbs[1]);
	igt_remove_fb(display->drm_fd, &fbs[0]);

	if (skip_test)
		igt_skip("Atomic nonblocking modesets are not supported.\n");

}

static void run_modeset_transition(igt_display_t *display, int requested_outputs, bool nonblocking)
{
	igt_output_t *outputs[I915_MAX_PIPES] = {};
	int num_outputs = 0;
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		igt_output_t *output;

		for_each_valid_output_on_pipe(display, pipe, output) {
			int i;

			for (i = pipe - 1; i >= 0; i--)
				if (outputs[i] == output)
					break;

			if (i < 0) {
				outputs[pipe] = output;
				num_outputs++;
				break;
			}
		}
	}

	igt_require_f(num_outputs >= requested_outputs,
		      "Should have at least %i outputs, found %i\n",
		      requested_outputs, num_outputs);

	run_modeset_tests(display, requested_outputs, nonblocking);
}

igt_main
{
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;
	int i;

	igt_skip_on_simulation();

	igt_fixture {
		int valid_outputs = 0;

		display.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_init(&display, display.drm_fd);

		igt_require(display.is_atomic);

		for_each_pipe_with_valid_output(&display, pipe, output)
			valid_outputs++;

		igt_require_f(valid_outputs, "no valid crtc/connector combinations found\n");
	}

	igt_subtest("plane-all-transition")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, TRANSITION_PLANES, false);

	igt_subtest("plane-all-transition-nonblocking")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, TRANSITION_PLANES, true);

	igt_subtest("plane-all-modeset-transition")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, TRANSITION_MODESET, false);

	igt_subtest("plane-toggle-modeset-transition")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, TRANSITION_MODESET_DISABLE, false);

	for (i = 1; i <= I915_MAX_PIPES; i++) {
		igt_subtest_f("%ix-modeset-transitions", i)
			run_modeset_transition(&display, i, false);

		igt_subtest_f("%ix-modeset-transitions-nonblocking", i)
			run_modeset_transition(&display, i, true);
	}

	igt_fixture {
		igt_display_fini(&display);
	}
}
