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

static void
wm_setup_plane(igt_display_t *display, enum pipe pipe, uint32_t mask,
	       struct igt_fb *fb, struct igt_fb *argb_fb,
	       uint64_t cursor_width, uint64_t cursor_height)
{
	igt_plane_t *plane;

	/*
	* Make sure these buffers are suited for display use
	* because most of the modeset operations must be fast
	* later on.
	*/
	for_each_plane_on_pipe(display, pipe, plane) {
		if (!((1 << plane->index) & mask)) {
			igt_plane_set_fb(plane, NULL);
			continue;
		}

		if (plane->is_primary)
			igt_plane_set_fb(plane, fb);
		else
			igt_plane_set_fb(plane, argb_fb);

		if (plane->is_cursor) {
			igt_fb_set_size(argb_fb, plane, cursor_width, cursor_height);
			igt_plane_set_size(plane, cursor_width, cursor_height);
		}
	}
}

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
run_transition_test(igt_display_t *display, enum pipe pipe, igt_output_t *output, bool modeset)
{
	struct igt_fb fb, argb_fb;
	drmModeModeInfo *mode;
	igt_plane_t *plane;
	uint64_t cursor_width, cursor_height;
	uint32_t iter_max = 1 << display->pipes[pipe].n_planes, i, j;

	mode = igt_output_get_mode(output);

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

	if (modeset) {
		igt_output_set_pipe(output, PIPE_NONE);

		wm_setup_plane(display, pipe, 0, NULL, NULL,
			      cursor_width, cursor_height);

		igt_display_commit2(display, COMMIT_ATOMIC);
	}

	for (i = 0; i < iter_max; i++) {
		igt_output_set_pipe(output, pipe);

		wm_setup_plane(display, pipe, i, &fb, &argb_fb,
			       cursor_width, cursor_height);

		igt_display_commit2(display, COMMIT_ATOMIC);

		if (modeset) {
			igt_output_set_pipe(output, PIPE_NONE);

			wm_setup_plane(display, pipe, 0, NULL, NULL,
				       cursor_width, cursor_height);

			igt_display_commit2(display, COMMIT_ATOMIC);
		} else {
			/* i -> i+1 will be done when i increases, can be skipped here */
			for (j = iter_max - 1; j > i + 1; j--) {
				wm_setup_plane(display, pipe, j, &fb, &argb_fb,
					      cursor_width, cursor_height);

				igt_display_commit2(display, COMMIT_ATOMIC);

				wm_setup_plane(display, pipe, i, &fb, &argb_fb,
					      cursor_width, cursor_height);
				igt_display_commit2(display, COMMIT_ATOMIC);
			}
		}
	}

	igt_output_set_pipe(output, PIPE_NONE);

	for_each_plane_on_pipe(display, pipe, plane)
		igt_plane_set_fb(plane, NULL);

	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_remove_fb(display->drm_fd, &fb);
	igt_remove_fb(display->drm_fd, &argb_fb);
}

igt_main
{
	igt_display_t display;
	igt_output_t *output;
	enum pipe pipe;

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

	igt_subtest_f("plane-all-transition")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, false);

	igt_subtest_f("plane-modeset-transition")
		for_each_pipe_with_valid_output(&display, pipe, output)
			run_transition_test(&display, pipe, output, true);

	igt_fixture {
		igt_display_fini(&display);
	}
}
