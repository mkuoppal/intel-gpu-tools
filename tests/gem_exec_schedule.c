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

#include <sys/poll.h>

#include "igt.h"
#include "igt_vgem.h"

#define LOCAL_PARAM_HAS_SCHEDULER 42
#define LOCAL_CONTEXT_PARAM_PRIORITY 5

#define LO 0
#define HI 1
#define NOISE 2

#define MAX_PRIO 1023

#define BUSY_QLEN 8

IGT_TEST_DESCRIPTION("Check that we can control the order of execution");

static void ctx_set_priority(int fd, uint32_t ctx, int prio)
{
	struct local_i915_gem_context_param param;

	memset(&param, 0, sizeof(param));
	param.context = ctx;
	param.size = 0;
	param.param = LOCAL_CONTEXT_PARAM_PRIORITY;
	param.value = prio;

	gem_context_set_param(fd, &param);
}

static void store_dword(int fd, uint32_t ctx, unsigned ring,
			uint32_t target, uint32_t offset, uint32_t value,
			uint32_t cork, unsigned write_domain)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[3];
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t batch[16];
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)(obj + !cork);
	execbuf.buffer_count = 2 + !!cork;
	execbuf.flags = ring;
	if (gen < 6)
		execbuf.flags |= I915_EXEC_SECURE;
	execbuf.rsvd1 = ctx;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = cork;
	obj[1].handle = target;
	obj[2].handle = gem_create(fd, 4096);

	memset(&reloc, 0, sizeof(reloc));
	reloc.target_handle = obj[1].handle;
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = offset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = write_domain;
	obj[2].relocs_ptr = (uintptr_t)&reloc;
	obj[2].relocation_count = 1;

	i = 0;
	batch[i] = MI_STORE_DWORD_IMM | (gen < 6 ? 1 << 22 : 0);
	if (gen >= 8) {
		batch[++i] = offset;
		batch[++i] = 0;
	} else if (gen >= 4) {
		batch[++i] = 0;
		batch[++i] = offset;
		reloc.offset += sizeof(uint32_t);
	} else {
		batch[i]--;
		batch[++i] = offset;
	}
	batch[++i] = value;
	batch[++i] = MI_BATCH_BUFFER_END;
	gem_write(fd, obj[2].handle, 0, batch, sizeof(batch));
	gem_execbuf(fd, &execbuf);
	gem_close(fd, obj[2].handle);
}

static uint32_t *make_busy(int fd, uint32_t target, unsigned ring)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj[2];
	struct drm_i915_gem_relocation_entry reloc[2];
	struct drm_i915_gem_execbuffer2 execbuf;
	uint32_t *batch;
	int i;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)(obj + !target);
	execbuf.buffer_count = 1 + !!target;

	memset(obj, 0, sizeof(obj));
	obj[0].handle = target;
	obj[1].handle = gem_create(fd, 4096);
	batch = gem_mmap__wc(fd, obj[1].handle, 0, 4096, PROT_WRITE);
	gem_set_domain(fd, obj[1].handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	obj[1].relocs_ptr = (uintptr_t)reloc;
	obj[1].relocation_count = 1 + !!target;
	memset(reloc, 0, sizeof(reloc));

	reloc[0].target_handle = obj[1].handle; /* recurse */
	reloc[0].presumed_offset = 0;
	reloc[0].offset = sizeof(uint32_t);
	reloc[0].delta = 0;
	reloc[0].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[0].write_domain = 0;

	reloc[1].target_handle = target;
	reloc[1].presumed_offset = 0;
	reloc[1].offset = 1024;
	reloc[1].delta = 0;
	reloc[1].read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc[1].write_domain = 0;

	i = 0;
	batch[i] = MI_BATCH_BUFFER_START;
	if (gen >= 8) {
		batch[i] |= 1 << 8 | 1;
		batch[++i] = 0;
		batch[++i] = 0;
	} else if (gen >= 6) {
		batch[i] |= 1 << 8;
		batch[++i] = 0;
	} else {
		batch[i] |= 2 << 6;
		batch[++i] = 0;
		if (gen < 4) {
			batch[i] |= 1;
			reloc[0].delta = 1;
		}
	}
	i++;

	if (ring != -1) {
		execbuf.flags = ring;
		for (int n = 0; n < BUSY_QLEN; n++)
			gem_execbuf(fd, &execbuf);
	} else {
		for_each_engine(fd, ring) {
			if (ring == 0)
				continue;

			execbuf.flags = ring;
			for (int n = 0; n < BUSY_QLEN; n++)
				gem_execbuf(fd, &execbuf);
			igt_assert(execbuf.flags == ring);
		}
	}

	if (target) {
		execbuf.flags = 0;
		reloc[1].write_domain = I915_GEM_DOMAIN_COMMAND;
		gem_execbuf(fd, &execbuf);
	}

	gem_close(fd, obj[1].handle);

	return batch;
}

static void finish_busy(uint32_t *busy)
{
	*busy = MI_BATCH_BUFFER_END;
	munmap(busy, 4096);
}

struct cork {
	int device;
	uint32_t handle;
	uint32_t fence;
};

static void plug(int fd, struct cork *c)
{
	struct vgem_bo bo;
	int dmabuf;

	c->device = drm_open_driver(DRIVER_VGEM);

	bo.width = bo.height = 1;
	bo.bpp = 4;
	vgem_create(c->device, &bo);
	c->fence = vgem_fence_attach(c->device, &bo, VGEM_FENCE_WRITE);

	dmabuf = prime_handle_to_fd(c->device, bo.handle);
	c->handle = prime_fd_to_handle(fd, dmabuf);
	close(dmabuf);
}

static void unplug(struct cork *c)
{
	vgem_fence_signal(c->device, c->fence);
	close(c->device);
}

static void fifo(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t *busy;
	uint32_t scratch;
	uint32_t *ptr;

	scratch = gem_create(fd, 4096);

	busy = make_busy(fd, scratch, ring);
	plug(fd, &cork);

	/* Same priority, same timeline, final result will be the second eb */
	store_dword(fd, 0, ring, scratch, 0, 1, cork.handle, 0);
	store_dword(fd, 0, ring, scratch, 0, 2, cork.handle, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	igt_assert_eq_u32(ptr[0], 2);
	munmap(ptr, 4096);
}

static void reorder(int fd, unsigned ring, unsigned flags)
#define EQUAL 1
{
	struct cork cork;
	uint32_t scratch;
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t ctx[2];

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], flags & EQUAL ? -MAX_PRIO : 0);

	scratch = gem_create(fd, 4096);

	busy = make_busy(fd, scratch, ring);
	plug(fd, &cork);

	/* We expect the high priority context to be executed first, and
	 * so the final result will be value from the low priority context.
	 */
	store_dword(fd, ctx[LO], ring, scratch, 0, ctx[LO], cork.handle, 0);
	store_dword(fd, ctx[HI], ring, scratch, 0, ctx[HI], cork.handle, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, scratch, 4096, PROT_READ);
	gem_set_domain(fd, scratch, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, scratch);

	if (flags & EQUAL) /* equal priority, result will be fifo */
		igt_assert_eq_u32(ptr[0], ctx[HI]);
	else
		igt_assert_eq_u32(ptr[0], ctx[LO]);
	munmap(ptr, 4096);
}

static void promotion(int fd, unsigned ring)
{
	struct cork cork;
	uint32_t result, dep;
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t ctx[3];

	ctx[LO] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[LO], -MAX_PRIO);

	ctx[HI] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[HI], 0);

	ctx[NOISE] = gem_context_create(fd);
	ctx_set_priority(fd, ctx[NOISE], -MAX_PRIO/2);

	result = gem_create(fd, 4096);
	dep = gem_create(fd, 4096);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	/* Expect that HI promotes LO, so the order will be LO, HI, NOISE.
	 *
	 * fifo would be NOISE, LO, HI.
	 * strict priority would be  HI, NOISE, LO
	 */
	store_dword(fd, ctx[NOISE], ring, result, 0, ctx[NOISE], cork.handle, 0);
	store_dword(fd, ctx[LO], ring, result, 0, ctx[LO], cork.handle, 0);

	/* link LO <-> HI via a dependency on another buffer */
	store_dword(fd, ctx[LO], ring, dep, 0, ctx[LO], 0, I915_GEM_DOMAIN_INSTRUCTION);
	store_dword(fd, ctx[HI], ring, dep, 0, ctx[HI], 0, 0);

	store_dword(fd, ctx[HI], ring, result, 0, ctx[HI], 0, 0);

	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	gem_context_destroy(fd, ctx[NOISE]);
	gem_context_destroy(fd, ctx[LO]);
	gem_context_destroy(fd, ctx[HI]);

	ptr = gem_mmap__gtt(fd, dep, 4096, PROT_READ);
	gem_set_domain(fd, dep, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, dep);

	igt_assert_eq_u32(ptr[0], ctx[HI]);
	munmap(ptr, 4096);

	ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	igt_assert_eq_u32(ptr[0], ctx[NOISE]);
	munmap(ptr, 4096);
}

static void deep(int fd, unsigned ring)
{
#define XS 8
	struct cork cork;
	uint32_t result, dep[XS];
	uint32_t *busy;
	uint32_t *ptr;
	uint32_t *ctx;

	ctx = malloc(sizeof(*ctx)*(MAX_PRIO + 1));
	for (int n = 0; n <= MAX_PRIO; n++) {
		ctx[n] = gem_context_create(fd);
		ctx_set_priority(fd, ctx[n], n);
	}

	result = gem_create(fd, 4096);
	for (int m = 0; m < XS; m ++)
		dep[m] = gem_create(fd, 4096);

	busy = make_busy(fd, result, ring);
	plug(fd, &cork);

	/* Create a deep dependency chain, with a few branches */
	for (int n = 0; n <= MAX_PRIO; n++)
		for (int m = 0; m < XS; m++)
			store_dword(fd, ctx[n], ring, dep[m], 4*n, ctx[n], cork.handle, I915_GEM_DOMAIN_INSTRUCTION);

	for (int n = 0; n <= MAX_PRIO; n++) {
		for (int m = 0; m < XS; m++) {
			store_dword(fd, ctx[n], ring, result, 4*n, ctx[n], dep[m], 0);
			store_dword(fd, ctx[n], ring, result, 4*m, ctx[n], 0, I915_GEM_DOMAIN_INSTRUCTION);
		}
	}

	igt_assert(gem_bo_busy(fd, result));
	unplug(&cork); /* only now submit our batches */
	igt_debugfs_dump(fd, "i915_engine_info");
	finish_busy(busy);

	for (int n = 0; n <= MAX_PRIO; n++)
		gem_context_destroy(fd, ctx[n]);

	for (int m = 0; m < XS; m++) {
		ptr = gem_mmap__gtt(fd, dep[m], 4096, PROT_READ);
		gem_set_domain(fd, dep[m], /* no write hazard lies! */
				I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
		gem_close(fd, dep[m]);

		for (int n = 0; n <= MAX_PRIO; n++)
			igt_assert_eq_u32(ptr[n], ctx[n]);
		munmap(ptr, 4096);
	}

	ptr = gem_mmap__gtt(fd, result, 4096, PROT_READ);
	gem_set_domain(fd, result, /* no write hazard lies! */
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
	gem_close(fd, result);

	for (int m = 0; m < XS; m++)
		igt_assert_eq_u32(ptr[m], ctx[MAX_PRIO]);
	munmap(ptr, 4096);

	free(ctx);
}

static bool has_scheduler(int fd)
{
	drm_i915_getparam_t gp;
	int has = -1;

	gp.param = LOCAL_PARAM_HAS_SCHEDULER;
	gp.value = &has;
	drmIoctl(fd, DRM_IOCTL_I915_GETPARAM, &gp);

	return has > 0;
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
		gem_require_mmap_wc(fd);
		igt_fork_hang_detector(fd);
	}

	igt_subtest_group {
		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_f("fifo-%s", e->name) {
				gem_require_ring(fd, e->exec_id | e->flags);
				fifo(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_subtest_group {
		igt_fixture {
			igt_require(has_scheduler(fd));
			ctx_set_priority(fd, 0, MAX_PRIO);
		}

		for (e = intel_execution_engines; e->name; e++) {
			/* default exec-id is purely symbolic */
			if (e->exec_id == 0)
				continue;

			igt_subtest_group {
				igt_fixture
					gem_require_ring(fd, e->exec_id | e->flags);

				igt_subtest_f("in-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, EQUAL);

				igt_subtest_f("out-order-%s", e->name)
					reorder(fd, e->exec_id | e->flags, 0);

				igt_subtest_f("promotion-%s", e->name)
					promotion(fd, e->exec_id | e->flags);

				igt_subtest_f("deep-%s", e->name)
					deep(fd, e->exec_id | e->flags);
			}
		}
	}

	igt_fixture {
		igt_stop_hang_detector();
		close(fd);
	}
}
