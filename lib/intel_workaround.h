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

#ifndef INTEL_WORKAROUNDS
#define INTEL_WORKAROUNDS

int __wa_devid = 0;

struct wa {
	const char * const name;
	int (*check)(const int devid);
};

#define WA_INREG(x) intel_register_read(x)

#define WA(_name) /* _name */ static int _name##_fun(const int devid); \
static const struct wa _name =					\
{ #_name, _name##_fun };					\
								\
static int _name##_fun(const int devid)

#define wa_assert_m(reg, val, mask) { if ((WA_INREG((reg)) & (mask)) != (val)) { return 1; } }
#define wa_assert(reg, val) wa_assert_m(reg, val, val)

#define WA_RM(_name, reg, val, mask)			\
	WA(_name) {					\
		wa_assert_m(reg, val, mask);		\
		return 0;				\
	}

#define WA_R(_name, reg, val) WA_RM(_name, reg, val, val)

WA(WaDisablePSDDualDispatchEnable) {
	if (IS_IVB_GT1(devid))
		wa_assert(0xe100, 1 << 3);

	wa_assert(0xf100, 1 << 3);
	return 0;
}

/* snb workarounds */

/* ivb workarounds */
WA_R(WaFbcAsynchFlipDisableFbcQueue, 0x42000, 1 << 22);
WA_R(WaDisableEarlyCull, 0x2090, 1 << 10);
WA_R(WaDisableBackToBackFlipFix, 0x4200c, 1 << 2 | 1 << 5);
WA_R(WaDisableRHWOptimizationForRenderHang, 0x7010, 1 << 10);

WA(WaApplyL3ControlAndL3ChickenMode) {
	if (IS_IVB_GT1(devid))
		wa_assert(0xe4f3, 1 << 0);

	wa_assert(0xf4f4, 1 << 0);

	wa_assert_m(0xb01c, 0x3C4FFF8C, 0xFFFFFFFF);
	wa_assert_m(0xb030, 0x20000000, 0xFFFFFFFF);
	return 0;
}

WA_RM(WaForceL3Serialization, 0xb034, 0, 1 << 27);
WA_R(WaDisableRCZUnitClockGating_ivb, 0x9404, (1 << 11) | (1 << 13));
WA_R(WaCatErrorRejectionIssue, 0x9030, (1 << 11));
WA_RM(WaVSRefCountFullforceMissDisable, 0x20a0, 0, (1 << 16) | (1 << 12) | (1 << 4));
WA_R(WaDisable4x2SubspanOptimization, 0x7004, (1 << 6));

/* hsw workarounds */

/* helpers */

static inline void wa_init(const int devid)
{
	__wa_devid = devid;
	intel_register_access_init(intel_get_pci_device(), 0);
}

static inline void wa_fini(void)
{
	__wa_devid = 0;
	intel_register_access_fini();
}

static int wa_check(const struct wa *wa)
{
	igt_assert(__wa_devid != 0);
	igt_assert(wa->name);
	igt_assert(wa->check);
	return wa->check(__wa_devid);
}

#define WA_CHECK(x) { \
	int __wchk_fd, __wchk_devid; \
	__wchk_fd = drm_open_any(); \
	__wchk_devid = intel_get_drm_devid(__wchk_fd); \
	wa_init(__wchk_devid); \
	close(__wchk_fd); \
	igt_assert(wa_check(&x) == 0); \
	wa_fini(); \
	}

#endif
