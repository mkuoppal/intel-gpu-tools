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
 * Authors:
 *    Mika Kuoppala <mika.kuoppala@intel.com>
 *
 */

#include <string.h>
#include "drmtest.h"
#include "intel_workaround.h"

static const struct wa *snb_workarounds[] = {
	NULL,
};

static const struct wa *ivb_workarounds[] = {
	&WaDisableEarlyCull,
	&WaDisableBackToBackFlipFix,
	&WaDisablePSDDualDispatchEnable,
	&WaDisableRHWOptimizationForRenderHang,
	&WaApplyL3ControlAndL3ChickenMode,
	&WaForceL3Serialization,
	&WaDisableRCZUnitClockGating_ivb,
	&WaCatErrorRejectionIssue,
	&WaVSRefCountFullforceMissDisable,
	&WaDisable4x2SubspanOptimization,
	NULL,
};

static const struct wa *hsw_workarounds[] = {
	NULL,
};

static void strip_arch(const char* testname, const char *arch, char *tmp, int l)
{
	char *p;
	int bytes;

	p = strstr(testname, arch);
	if (p)
		bytes = p - testname - 1;
	else
		bytes = strlen(testname);

	if (bytes < 0)
		bytes = 0;

	if (bytes >= (l-1))
		bytes = l-1;

	strncpy(tmp, testname, bytes);
	tmp[bytes] = '\0';
}

static int check_workarounds(const struct wa **workarounds, const char *arch)
{
	const struct wa **p;
	char tmpname[256];
	int fail_count = 0, wa_count = 0;
	int ret;
	p  = workarounds;

	while(*p) {
		wa_count++;
		ret = wa_check(*p);
		strip_arch((*p)->name, arch, tmpname, sizeof(tmpname));
		printf("%-8s %s:%s\n", ret ? "FAIL" : "OK", tmpname, arch);
		if (ret)
			fail_count++;
		p++;
	}

	if (fail_count) {
		printf("%d workarounds tested, %d passed, %d failed\n",
		      wa_count, wa_count - fail_count, fail_count);
	}

	return fail_count;
}

#define do_check(was, arch) igt_assert(check_workarounds(was, arch) == 0)

int main(int argc, char *argv[])
{
	int fd, devid;

	fd = drm_open_any();
	devid = intel_get_drm_devid(fd);
	close(fd);

	igt_skip_on(!IS_INTEL(devid));

	wa_init(devid);

	if (IS_GEN6(devid))
		do_check(&snb_workarounds[0], "snb");

	if (IS_IVYBRIDGE(devid))
		do_check(&ivb_workarounds[0], "ivb");

	if (IS_HASWELL(devid))
		do_check(&hsw_workarounds[0], "hsw");

	wa_fini();

	igt_success();
}
