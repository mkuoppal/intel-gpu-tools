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

#include "igt.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>

#include <drm.h>

#ifndef I915_PARAM_CMD_PARSER_VERSION
#define I915_PARAM_CMD_PARSER_VERSION       28
#endif

#define DERRMR 0x44050
#define OACONTROL 0x2360
#define SO_WRITE_OFFSET_0 0x5280

#define HSW_CS_GPR(n) (0x2600 + 8*(n))
#define HSW_CS_GPR0 HSW_CS_GPR(0)
#define HSW_CS_GPR1 HSW_CS_GPR(1)

#define MI_LOAD_REGISTER_REG (0x2a << 23)
#define MI_STORE_REGISTER_MEM (0x24 << 23)
#define MI_ARB_ON_OFF (0x8 << 23)
#define MI_DISPLAY_FLIP ((0x14 << 23) | 1)

#define GFX_OP_PIPE_CONTROL	((0x3<<29)|(0x3<<27)|(0x2<<24)|2)
#define   PIPE_CONTROL_QW_WRITE	(1<<14)
#define   PIPE_CONTROL_LRI_POST_OP (1<<23)

static int parser_version;

static int command_parser_version(int fd)
{
	int version = -1;
	drm_i915_getparam_t gp;

	gp.param = I915_PARAM_CMD_PARSER_VERSION;
	gp.value = &version;

	if (drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp) == 0)
		return version;

	return -1;
}

static uint64_t __exec_batch_patched(int fd, uint32_t cmd_bo, uint32_t *cmds,
				     int size, int patch_offset)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[1];

	uint32_t target_bo = gem_create(fd, 4096);
	uint64_t actual_value = 0;

	gem_write(fd, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_bo;
	obj[1].handle = cmd_bo;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = patch_offset;
	reloc[0].target_handle = obj[0].handle;
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = I915_GEM_DOMAIN_COMMAND;
	obj[1].relocs_ptr = (uintptr_t)reloc;
	obj[1].relocation_count = 1;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 2;
	execbuf.batch_len = size;
	execbuf.flags = I915_EXEC_RENDER;

	gem_execbuf(fd, &execbuf);
	gem_sync(fd, cmd_bo);

	gem_read(fd,target_bo, 0, &actual_value, sizeof(actual_value));

	gem_close(fd, target_bo);

	return actual_value;
}

static void exec_batch_patched(int fd, uint32_t cmd_bo, uint32_t *cmds,
			       int size, int patch_offset,
			       uint64_t expected_value)
{
	igt_assert_eq(__exec_batch_patched(fd, cmd_bo, cmds,
					   size, patch_offset),
		      expected_value);
}

static int __exec_batch(int fd, uint32_t cmd_bo, uint32_t *cmds,
			int size, int ring)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[1];

	gem_write(fd, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cmd_bo;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 1;
	execbuf.batch_len = size;
	execbuf.flags = ring;

	return __gem_execbuf(fd, &execbuf);
}
#define exec_batch(fd, bo, cmds, sz, ring, expected) \
	igt_assert_eq(__exec_batch(fd, bo, cmds, sz, ring), expected)

static void exec_split_batch(int fd, uint32_t *cmds,
			     int size, int ring, int expected_ret)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[1];
	uint32_t cmd_bo;
	uint32_t noop[1024] = { 0 };
	const int alloc_size = 4096 * 2;
	const int actual_start_offset = 4096-sizeof(uint32_t);

	/* Allocate and fill a 2-page batch with noops */
	cmd_bo = gem_create(fd, alloc_size);
	gem_write(fd, cmd_bo, 0, noop, sizeof(noop));
	gem_write(fd, cmd_bo, 4096, noop, sizeof(noop));

	/* Write the provided commands such that the first dword
	 * of the command buffer is the last dword of the first
	 * page (i.e. the command is split across the two pages).
	 */
	gem_write(fd, cmd_bo, actual_start_offset, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cmd_bo;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 1;
	/* NB: We want batch_start_offset and batch_len to point to the block
	 * of the actual commands (i.e. at the last dword of the first page),
	 * but have to adjust both the start offset and length to meet the
	 * kernel driver's requirements on the alignment of those fields.
	 */
	execbuf.batch_start_offset = actual_start_offset & ~0x7;
	execbuf.batch_len =
		ALIGN(size + actual_start_offset - execbuf.batch_start_offset,
		      0x8);
	execbuf.flags = ring;

	igt_assert_eq(__gem_execbuf(fd, &execbuf), expected_ret);

	gem_sync(fd, cmd_bo);
	gem_close(fd, cmd_bo);
}

static void exec_batch_chained(int fd, uint32_t cmd_bo, uint32_t *cmds,
			       int size, int patch_offset,
			       uint64_t expected_value)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc[1];
	struct drm_i915_gem_relocation_entry first_level_reloc;

	uint32_t target_bo = gem_create(fd, 4096);
	uint32_t first_level_bo = gem_create(fd, 4096);
	uint64_t actual_value = 0;

	static uint32_t first_level_cmds[] = {
		MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965,
		0,
		MI_BATCH_BUFFER_END,
		0,
	};

	if (IS_HASWELL(intel_get_drm_devid(fd)))
		first_level_cmds[0] |= MI_BATCH_NON_SECURE_HSW;

	gem_write(fd, first_level_bo, 0,
		  first_level_cmds, sizeof(first_level_cmds));
	gem_write(fd, cmd_bo, 0, cmds, size);

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target_bo;
	obj[1].handle = cmd_bo;
	obj[2].handle = first_level_bo;

	memset(reloc, 0, sizeof(reloc));
	reloc[0].offset = patch_offset;
	reloc[0].delta = 0;
	reloc[0].target_handle = target_bo;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = I915_GEM_DOMAIN_COMMAND;
	obj[1].relocation_count = 1;
	obj[1].relocs_ptr = (uintptr_t)&reloc;

	memset(&first_level_reloc, 0, sizeof(first_level_reloc));
	first_level_reloc.offset = 4;
	first_level_reloc.delta = 0;
	first_level_reloc.target_handle = cmd_bo;
	first_level_reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	first_level_reloc.write_domain = 0;
	obj[2].relocation_count = 1;
	obj[2].relocs_ptr = (uintptr_t)&first_level_reloc;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)obj;
	execbuf.buffer_count = 3;
	execbuf.batch_len = sizeof(first_level_cmds);
	execbuf.flags = I915_EXEC_RENDER;

	gem_execbuf(fd, &execbuf);
	gem_sync(fd, cmd_bo);

	gem_read(fd,target_bo, 0, &actual_value, sizeof(actual_value));
	igt_assert_eq(expected_value, actual_value);

	gem_close(fd, first_level_bo);
	gem_close(fd, target_bo);
}

static void stray_lri(int fd, uint32_t handle)
{
	/* Ideally this would test all once whitelisted registers */
	uint32_t lri[] = {
		MI_LOAD_REGISTER_IMM,
		OACONTROL,
		0x31337000,
		MI_BATCH_BUFFER_END,
	};
	int err;

	igt_assert_eq_u32(intel_register_read(OACONTROL), 0xdeadbeef);

	err = __exec_batch(fd, handle, lri, sizeof(lri), I915_EXEC_RENDER);
	if (err == -EINVAL)
		return;

	igt_assert_eq(err, 0);
	gem_sync(fd, handle);

	igt_assert_eq_u32(intel_register_read(OACONTROL), 0xdeadbeef);
}

static void test_allocations(int fd)
{
	uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 obj[17];
	int i, j;

	intel_require_memory(2, 1ull<<(12 + ARRAY_SIZE(obj)), CHECK_RAM);

	memset(obj, 0, sizeof(obj));
	for (i = 0; i < ARRAY_SIZE(obj); i++) {
		obj[i].handle = gem_create(fd, 1ull<<(12 + i));
		gem_write(fd, obj[i].handle, (1ull<<(12+i)) - 8 - sizeof(bbe),
			  &bbe, sizeof(bbe));
	}

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffer_count = 1;
	for (j = 0; j < 16384; j++) {
		igt_progress("allocations ", j, 16384);
		i = rand() % ARRAY_SIZE(obj);
		execbuf.buffers_ptr = (uintptr_t)&obj[i];
		execbuf.batch_start_offset = (rand() % (1ull<<i)) << 12;
		execbuf.batch_len = (1ull<<(12+i)) - 8 - execbuf.batch_start_offset;
		gem_execbuf(fd, &execbuf);
	}

	for (i = 0; i < ARRAY_SIZE(obj); i++) {
		gem_sync(fd, obj[i].handle);
		gem_close(fd, obj[i].handle);
	}
}

static void hsw_load_register_reg(void)
{
	uint32_t init_gpr0[16] = {
		MI_LOAD_REGISTER_IMM | (3 - 2),
		HSW_CS_GPR0,
		0xabcdabc0, /* leave [1:0] zero */
		MI_BATCH_BUFFER_END,
	};
	uint32_t store_gpr0[16] = {
		MI_STORE_REGISTER_MEM | (3 - 2),
		HSW_CS_GPR0,
		0, /* reloc*/
		MI_BATCH_BUFFER_END,
	};
	uint32_t do_lrr[16] = {
		MI_LOAD_REGISTER_REG | (3 - 2),
		0, /* [1] = src */
		HSW_CS_GPR0, /* dst */
		MI_BATCH_BUFFER_END,
	};
	uint32_t allowed_regs[] = {
		HSW_CS_GPR1,
		SO_WRITE_OFFSET_0,
	};
	uint32_t disallowed_regs[] = {
		0,
		OACONTROL, /* filtered */
		DERRMR, /* master only */
		0x2038, /* RING_START: invalid */
	};
	int fd;
	uint32_t handle;

	/* Open again to get a non-master file descriptor */
	fd = drm_open_driver(DRIVER_INTEL);

	igt_require(IS_HASWELL(intel_get_drm_devid(fd)));
	igt_require(parser_version >= 7);

	handle = gem_create(fd, 4096);

	for (int i = 0 ; i < ARRAY_SIZE(allowed_regs); i++) {
		uint32_t var;

		exec_batch(fd, handle, init_gpr0, sizeof(init_gpr0),
			   I915_EXEC_RENDER,
			   0);
		exec_batch_patched(fd, handle,
				   store_gpr0, sizeof(store_gpr0),
				   2 * sizeof(uint32_t), /* reloc */
				   0xabcdabc0);
		do_lrr[1] = allowed_regs[i];
		exec_batch(fd, handle, do_lrr, sizeof(do_lrr),
			   I915_EXEC_RENDER,
			   0);
		var = __exec_batch_patched(fd, handle,
					   store_gpr0, sizeof(store_gpr0),
					   2 * sizeof(uint32_t)); /* reloc */
		igt_assert_neq(var, 0xabcdabc0);
	}

	for (int i = 0 ; i < ARRAY_SIZE(disallowed_regs); i++) {
		do_lrr[1] = disallowed_regs[i];
		exec_batch(fd, handle, do_lrr, sizeof(do_lrr),
			   I915_EXEC_RENDER,
			   -EINVAL);
	}

	close(fd);
}

igt_main
{
	uint32_t handle;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);

		parser_version = command_parser_version(fd);
		igt_require(parser_version != -1);

		igt_require(gem_uses_ppgtt(fd));

		handle = gem_create(fd, 4096);

		/* ATM cmd parser only exists on gen7. */
		igt_require(intel_gen(intel_get_drm_devid(fd)) == 7);
	}

	igt_subtest("basic-allowed") {
		uint32_t pc[] = {
			GFX_OP_PIPE_CONTROL,
			PIPE_CONTROL_QW_WRITE,
			0, /* To be patched */
			0x12000000,
			0,
			MI_BATCH_BUFFER_END,
		};
		exec_batch_patched(fd, handle,
				   pc, sizeof(pc),
				   8, /* patch offset, */
				   0x12000000);
	}

	igt_subtest("basic-rejected") {
		uint32_t arb_on_off[] = {
			MI_ARB_ON_OFF,
			MI_BATCH_BUFFER_END,
		};
		uint32_t display_flip[] = {
			MI_DISPLAY_FLIP,
			0, 0, 0,
			MI_BATCH_BUFFER_END,
			0
		};
		exec_batch(fd, handle,
			   arb_on_off, sizeof(arb_on_off),
			   I915_EXEC_RENDER,
			   -EINVAL);
		exec_batch(fd, handle,
			   arb_on_off, sizeof(arb_on_off),
			   I915_EXEC_BSD,
			   -EINVAL);
		if (gem_has_vebox(fd)) {
			exec_batch(fd, handle,
				   arb_on_off, sizeof(arb_on_off),
				   I915_EXEC_VEBOX,
				   -EINVAL);
		}
		exec_batch(fd, handle,
			   display_flip, sizeof(display_flip),
			   I915_EXEC_BLT,
			   -EINVAL);
	}

	igt_subtest_group {
		igt_fixture {
			intel_register_access_init(intel_get_pci_device(), 0);

			intel_register_write(OACONTROL, 0xdeadbeef);
			igt_assert_eq_u32(intel_register_read(OACONTROL), 0xdeadbeef);
		}

		igt_subtest("basic-stray-lri")
			stray_lri(fd, handle);

		igt_fixture {
			intel_register_write(OACONTROL, 0);
			intel_register_access_fini();
		}
	}

	igt_subtest("basic-allocation") {
		test_allocations(fd);
	}

	igt_subtest("registers") {
		uint32_t lri_bad[] = {
			MI_LOAD_REGISTER_IMM,
			0, /* disallowed register address */
			0x12000000,
			MI_BATCH_BUFFER_END,
		};
		uint32_t lri_ok[] = {
			MI_LOAD_REGISTER_IMM,
			0x5280, /* allowed register address (SO_WRITE_OFFSET[0]) */
			0x1,
			MI_BATCH_BUFFER_END,
		};
		exec_batch(fd, handle,
			   lri_bad, sizeof(lri_bad),
			   I915_EXEC_RENDER,
			   -EINVAL);
		exec_batch(fd, handle,
			   lri_ok, sizeof(lri_ok),
			   I915_EXEC_RENDER,
			   0);
	}

	igt_subtest("bitmasks") {
		uint32_t pc[] = {
			GFX_OP_PIPE_CONTROL,
			(PIPE_CONTROL_QW_WRITE |
			 PIPE_CONTROL_LRI_POST_OP),
			0, /* To be patched */
			0x12000000,
			0,
			MI_BATCH_BUFFER_END,
		};
		exec_batch(fd, handle,
			   pc, sizeof(pc),
			   I915_EXEC_RENDER,
			   -EINVAL);
	}

	igt_subtest("batch-without-end") {
		uint32_t noop[1024] = { 0 };
		exec_batch(fd, handle,
			   noop, sizeof(noop),
			   I915_EXEC_RENDER,
			   -EINVAL);
	}

	igt_subtest("cmd-crossing-page") {
		uint32_t lri_ok[] = {
			MI_LOAD_REGISTER_IMM,
			0x5280, /* allowed register address (SO_WRITE_OFFSET[0]) */
			0x1,
			MI_BATCH_BUFFER_END,
		};
		exec_split_batch(fd,
				 lri_ok, sizeof(lri_ok),
				 I915_EXEC_RENDER,
				 0);
	}

	igt_subtest("oacontrol-tracking") {
		uint32_t lri_ok[] = {
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x31337000,
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x0,
			MI_BATCH_BUFFER_END,
			0
		};
		uint32_t lri_bad[] = {
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x31337000,
			MI_BATCH_BUFFER_END,
		};
		uint32_t lri_extra_bad[] = {
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x31337000,
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x0,
			MI_LOAD_REGISTER_IMM,
			OACONTROL,
			0x31337000,
			MI_BATCH_BUFFER_END,
		};

		igt_require(parser_version < 9);

		exec_batch(fd, handle,
			   lri_ok, sizeof(lri_ok),
			   I915_EXEC_RENDER,
			   0);
		exec_batch(fd, handle,
			   lri_bad, sizeof(lri_bad),
			   I915_EXEC_RENDER,
			   -EINVAL);
		exec_batch(fd, handle,
			   lri_extra_bad, sizeof(lri_extra_bad),
			   I915_EXEC_RENDER,
			   -EINVAL);
	}

	igt_subtest("chained-batch") {
		uint32_t pc[] = {
			GFX_OP_PIPE_CONTROL,
			PIPE_CONTROL_QW_WRITE,
			0, /* To be patched */
			0x12000000,
			0,
			MI_BATCH_BUFFER_END,
		};
		exec_batch_chained(fd, handle,
				   pc, sizeof(pc),
				   8, /* patch offset, */
				   0x12000000);
	}

	igt_subtest("load-register-reg")
		hsw_load_register_reg();

	igt_fixture {
		gem_close(fd, handle);

		close(fd);
	}
}
