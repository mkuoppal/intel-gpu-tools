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

static void prepare_pipe(igt_display_t *display, enum pipe pipe, igt_output_t *output, struct igt_fb *fb)
{
	drmModeModeInfo *mode = igt_output_get_mode(output);

	igt_create_pattern_fb(display->drm_fd, mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, LOCAL_DRM_FORMAT_MOD_NONE, fb);

	igt_output_set_pipe(output, pipe);

	igt_plane_set_fb(igt_output_get_plane(output, IGT_PLANE_PRIMARY), fb);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);
}

static void cleanup_pipe(igt_display_t *display, enum pipe pipe, igt_output_t *output, struct igt_fb *fb)
{
	igt_plane_t *plane;

	for_each_plane_on_pipe(display, pipe, plane)
		igt_plane_set_fb(plane, NULL);

	igt_output_set_pipe(output, PIPE_NONE);

	igt_display_commit2(display, display->is_atomic ? COMMIT_ATOMIC : COMMIT_LEGACY);

	igt_remove_fb(display->drm_fd, fb);
}

static bool ignore_crtc_property(const char *name, bool atomic)
{
	if (!strcmp(name, "GAMMA_LUT_SIZE"))
		return true;
	if (!strcmp(name, "DEGAMMA_LUT_SIZE"))
		return true;

	return false;
}

static bool ignore_connector_property(const char *name, bool atomic)
{
	if (!strcmp(name, "EDID") ||
	    !strcmp(name, "PATH") ||
	    !strcmp(name, "TILE"))
		return true;

	if (atomic && !strcmp(name, "DPMS"))
		return true;

	return false;
}

static bool ignore_plane_property(const char *name, bool atomic)
{
	if (!strcmp(name, "type"))
		return true;

	return false;
}

static bool ignore_property(uint32_t type, const char *name, bool atomic)
{
	switch (type) {
	case DRM_MODE_OBJECT_CRTC:
		return ignore_crtc_property(name, atomic);
	case DRM_MODE_OBJECT_CONNECTOR:
		return ignore_connector_property(name, atomic);
	case DRM_MODE_OBJECT_PLANE:
		return ignore_plane_property(name, atomic);
	default:
		igt_assert(0);
		return false;
	}
}

static void test_properties(int fd, uint32_t type, uint32_t id, bool atomic)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(fd, id, type);
	int i, ret;
	drmModeAtomicReqPtr req = NULL;

	igt_assert(props);

	if (atomic)
		req = drmModeAtomicAlloc();

	for (i = 0; i < props->count_props; i++) {
		uint32_t prop_id = props->props[i];
		uint64_t prop_value = props->prop_values[i];
		drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);

		igt_assert(prop);

		if (ignore_property(type, prop->name, atomic)) {
			igt_debug("Ignoring property \"%s\"\n", prop->name);

			continue;
		}

		igt_debug("Testing property \"%s\"\n", prop->name);

		if (!atomic) {
			ret = drmModeObjectSetProperty(fd, id, type, prop_id, prop_value);

			igt_assert_eq(ret, 0);
		} else {
			ret = drmModeAtomicAddProperty(req, id, prop_id, prop_value);
			igt_assert(ret >= 0);

			ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
			igt_assert_eq(ret, 0);
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);

	if (atomic) {
		ret = drmModeAtomicCommit(fd, req, 0, NULL);
		igt_assert_eq(ret, 0);

		drmModeAtomicFree(req);
	}
}

static void run_plane_property_tests(igt_display_t *display, enum pipe pipe, igt_output_t *output, bool atomic)
{
	struct igt_fb fb;
	igt_plane_t *plane;

	prepare_pipe(display, pipe, output, &fb);

	for_each_plane_on_pipe(display, pipe, plane) {
		igt_info("Testing plane properties on %s.%s (output: %s)\n",
			 kmstest_pipe_name(pipe), kmstest_plane_name(plane->index), output->name);

		test_properties(display->drm_fd, DRM_MODE_OBJECT_PLANE, plane->drm_plane->plane_id, atomic);
	}

	cleanup_pipe(display, pipe, output, &fb);
}

static void run_crtc_property_tests(igt_display_t *display, enum pipe pipe, igt_output_t *output, bool atomic)
{
	struct igt_fb fb;

	prepare_pipe(display, pipe, output, &fb);

	igt_info("Testing crtc properties on %s (output: %s)\n", kmstest_pipe_name(pipe), output->name);

	test_properties(display->drm_fd, DRM_MODE_OBJECT_CRTC, display->pipes[pipe].crtc_id, atomic);

	cleanup_pipe(display, pipe, output, &fb);
}

static void run_connector_property_tests(igt_display_t *display, enum pipe pipe, igt_output_t *output, bool atomic)
{
	struct igt_fb fb;

	if (pipe != PIPE_NONE)
		prepare_pipe(display, pipe, output, &fb);

	igt_info("Testing connector properties on output %s (pipe: %s)\n", output->name, kmstest_pipe_name(pipe));

	test_properties(display->drm_fd, DRM_MODE_OBJECT_CONNECTOR, output->id, atomic);

	if (pipe != PIPE_NONE)
		cleanup_pipe(display, pipe, output, &fb);
}

static void plane_properties(igt_display_t *display, bool atomic)
{
	bool found_any = false, found;
	igt_output_t *output;
	enum pipe pipe;

	if (atomic)
		igt_skip_on(!display->is_atomic);

	for_each_pipe(display, pipe) {
		found = false;

		for_each_valid_output_on_pipe(display, pipe, output) {
			found_any = found = true;

			run_plane_property_tests(display, pipe, output, atomic);
			break;
		}
	}

	igt_skip_on(!found_any);
}

static void crtc_properties(igt_display_t *display, bool atomic)
{
	bool found_any_valid_pipe = false, found;
	enum pipe pipe;
	igt_output_t *output;

	if (atomic)
		igt_skip_on(!display->is_atomic);

	for_each_pipe(display, pipe) {
		found = false;

		for_each_valid_output_on_pipe(display, pipe, output) {
			found_any_valid_pipe = found = true;

			run_crtc_property_tests(display, pipe, output, atomic);
			break;
		}
	}

	igt_skip_on(!found_any_valid_pipe);
}

static void connector_properties(igt_display_t *display, bool atomic)
{
	int i;
	enum pipe pipe;
	igt_output_t *output;

	if (atomic)
		igt_skip_on(!display->is_atomic);

	for_each_connected_output(display, output) {
		bool found = false;

		for_each_pipe(display, pipe) {
			if (!igt_pipe_connector_valid(pipe, output))
				continue;

			found = true;
			run_connector_property_tests(display, pipe, output, atomic);
			break;
		}

		igt_assert_f(found, "Connected output should have at least 1 valid crtc\n");
	}

	for (i = 0; i < display->n_outputs; i++)
		if (!igt_output_is_connected(&display->outputs[i]))
			run_connector_property_tests(display, PIPE_NONE, &display->outputs[i], atomic);
}

static void test_invalid_properties(int fd,
				    uint32_t id1,
				    uint32_t type1,
				    uint32_t id2,
				    uint32_t type2,
				    bool atomic)
{
	drmModeObjectPropertiesPtr props1 =
		drmModeObjectGetProperties(fd, id1, type1);
	drmModeObjectPropertiesPtr props2 =
		drmModeObjectGetProperties(fd, id2, type2);

	int i, j, ret;
	drmModeAtomicReqPtr req;

	igt_assert(props1 && props2);

	for (i = 0; i < props2->count_props; i++) {
		uint32_t prop_id = props2->props[i];
		uint64_t prop_value = props2->prop_values[i];
		drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
		bool found = false;

		igt_assert(prop);

		for (j = 0; j < props1->count_props; j++)
			if (props1->props[j] == prop_id) {
				found = true;
				break;
			}

		if (found)
			continue;

		igt_debug("Testing property \"%s\" on [%x:%u]\n", prop->name, type1, id1);

		if (!atomic) {
			ret = drmModeObjectSetProperty(fd, id1, type1, prop_id, prop_value);

			igt_assert_eq(ret, -EINVAL);
		} else {
			req = drmModeAtomicAlloc();
			igt_assert(req);

			ret = drmModeAtomicAddProperty(req, id1, prop_id, prop_value);
			igt_assert(ret >= 0);

			ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			igt_assert_eq(ret, -EINVAL);

			drmModeAtomicFree(req);
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props1);
	drmModeFreeObjectProperties(props2);
}
static void test_object_invalid_properties(igt_display_t *display,
					   uint32_t id, uint32_t type, bool atomic)
{
	igt_output_t *output;
	igt_plane_t *plane;
	enum pipe pipe;
	int i;

	for_each_pipe(display, pipe)
		test_invalid_properties(display->drm_fd, id, type, display->pipes[pipe].crtc_id, DRM_MODE_OBJECT_CRTC, atomic);

	for_each_pipe(display, pipe)
		for_each_plane_on_pipe(display, pipe, plane)
			test_invalid_properties(display->drm_fd, id, type, plane->drm_plane->plane_id, DRM_MODE_OBJECT_PLANE, atomic);

	for (i = 0, output = &display->outputs[0]; i < display->n_outputs; output = &display->outputs[++i])
		test_invalid_properties(display->drm_fd, id, type, output->id, DRM_MODE_OBJECT_CONNECTOR, atomic);
}

static void invalid_properties(igt_display_t *display, bool atomic)
{
	igt_output_t *output;
	igt_plane_t *plane;
	enum pipe pipe;
	int i;

	for_each_pipe(display, pipe)
		test_object_invalid_properties(display, display->pipes[pipe].crtc_id, DRM_MODE_OBJECT_CRTC, atomic);

	for_each_pipe(display, pipe)
		for_each_plane_on_pipe(display, pipe, plane)
			test_object_invalid_properties(display, plane->drm_plane->plane_id, DRM_MODE_OBJECT_PLANE, atomic);

	for (i = 0, output = &display->outputs[0]; i < display->n_outputs; output = &display->outputs[++i])
		test_object_invalid_properties(display, output->id, DRM_MODE_OBJECT_CONNECTOR, atomic);
}

igt_main
{
	igt_display_t display;

	igt_skip_on_simulation();

	igt_fixture {
		display.drm_fd = drm_open_driver_master(DRIVER_ANY);

		kmstest_set_vt_graphics_mode();

		igt_display_init(&display, display.drm_fd);
	}

	igt_subtest("plane-properties-legacy")
		plane_properties(&display, false);

	igt_subtest("plane-properties-atomic")
		plane_properties(&display, true);

	igt_subtest("crtc-properties-legacy")
		crtc_properties(&display, false);

	igt_subtest("crtc-properties-atomic")
		crtc_properties(&display, true);

	igt_subtest("connector-properties-legacy")
		connector_properties(&display, false);

	igt_subtest("connector-properties-atomic")
		connector_properties(&display, true);

	igt_subtest("invalid-properties-legacy")
		invalid_properties(&display, false);

	igt_subtest("invalid-properties-atomic")
		invalid_properties(&display, true);

	igt_fixture {
		igt_display_fini(&display);
	}
}
