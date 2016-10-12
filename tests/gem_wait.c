/*
 * Copyright © 2012 Intel Corporation
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
 *    Ben Widawsky <ben@bwidawsk.net>
 *
 */

#include "igt.h"

#include <signal.h>
#include <time.h>
#include <sys/syscall.h>

#define gettid() syscall(__NR_gettid)
#define sigev_notify_thread_id _sigev_un._tid

#define LOCAL_I915_EXEC_BSD_SHIFT      (13)
#define LOCAL_I915_EXEC_BSD_MASK       (3 << LOCAL_I915_EXEC_BSD_SHIFT)

#define ENGINE_MASK  (I915_EXEC_RING_MASK | LOCAL_I915_EXEC_BSD_MASK)

static int __gem_wait(int fd, struct drm_i915_gem_wait *w)
{
	int err;

	err = 0;
	if (igt_ioctl(fd, DRM_IOCTL_I915_GEM_WAIT, w))
		err = -errno;

	return err;
}

static void invalid_flags(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = gem_create(fd, 4096);
	wait.timeout_ns = 1;
	/* NOTE: This test intentionally tests for just the next available flag.
	 * Don't "fix" this testcase without the ABI testcases for new flags
	 * first. */
	wait.flags = 1;

	igt_assert_eq(__gem_wait(fd, &wait), -EINVAL);

	gem_close(fd, wait.bo_handle);
}

static void invalid_buf(int fd)
{
	struct drm_i915_gem_wait wait;

	memset(&wait, 0, sizeof(wait));
	igt_assert_eq(__gem_wait(fd, &wait), -ENOENT);
}

static uint32_t *batch;

static void sigiter(int sig, siginfo_t *info, void *arg)
{
	*batch = MI_BATCH_BUFFER_END;
	__sync_synchronize();
}

#define MSEC_PER_SEC (1000)
#define USEC_PER_SEC (1000 * MSEC_PER_SEC)
#define NSEC_PER_SEC (1000 * USEC_PER_SEC)

#define BUSY 1
#define HANG 2
static void basic(int fd, unsigned engine, unsigned flags)
{
	const int gen = intel_gen(intel_get_drm_devid(fd));
	struct drm_i915_gem_exec_object2 obj;
	struct drm_i915_gem_relocation_entry reloc;
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_wait wait;
	unsigned engines[16];
	unsigned nengine;
	int i, timeout;

	nengine = 0;
	if (engine == -1) {
		for_each_engine(fd, engine)
			if (engine) engines[nengine++] = engine;
	} else {
		igt_require(gem_has_ring(fd, engine));
		engines[nengine++] = engine;
	}
	igt_require(nengine);

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&obj;
	execbuf.buffer_count = 1;

	memset(&obj, 0, sizeof(obj));
	obj.handle = gem_create(fd, 4096);

	obj.relocs_ptr = (uintptr_t)&reloc;
	obj.relocation_count = 1;
	memset(&reloc, 0, sizeof(reloc));

	batch = gem_mmap__gtt(fd, obj.handle, 4096, PROT_WRITE);
	gem_set_domain(fd, obj.handle,
			I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);

	reloc.target_handle = obj.handle; /* recurse */
	reloc.presumed_offset = 0;
	reloc.offset = sizeof(uint32_t);
	reloc.delta = 0;
	reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
	reloc.write_domain = 0;

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
			reloc.delta = 1;
		}
	}

	for (i = 0; i < nengine; i++) {
		execbuf.flags &= ~ENGINE_MASK;
		execbuf.flags |= engines[i];
		gem_execbuf(fd, &execbuf);
	}

	memset(&wait, 0, sizeof(wait));
	wait.bo_handle = obj.handle;
	igt_assert_eq(__gem_wait(fd, &wait), -ETIME);

	if (flags & BUSY) {
		struct timespec tv;

		timeout = 120;
		if ((flags & HANG) == 0) {
			*batch = MI_BATCH_BUFFER_END;
			__sync_synchronize();
			timeout = 1;
		}
		munmap(batch, 4096);

		memset(&tv, 0, sizeof(tv));
		while (__gem_wait(fd, &wait) == -ETIME)
			igt_assert(igt_seconds_elapsed(&tv) < timeout);
	} else {
		timer_t timer;

		if ((flags & HANG) == 0) {
			struct sigevent sev;
			struct sigaction act;
			struct itimerspec its;

			memset(&sev, 0, sizeof(sev));
			sev.sigev_notify = SIGEV_SIGNAL | SIGEV_THREAD_ID;
			sev.sigev_notify_thread_id = gettid();
			sev.sigev_signo = SIGRTMIN + 1;
			igt_assert(timer_create(CLOCK_MONOTONIC, &sev, &timer) == 0);

			memset(&act, 0, sizeof(act));
			act.sa_sigaction = sigiter;
			act.sa_flags = SA_SIGINFO;
			igt_assert(sigaction(SIGRTMIN + 1, &act, NULL) == 0);

			memset(&its, 0, sizeof(its));
			its.it_value.tv_nsec = 0;
			its.it_value.tv_sec = 1;
			igt_assert(timer_settime(timer, 0, &its, NULL) == 0);
		}

		wait.timeout_ns = NSEC_PER_SEC / 2; /* 0.5s */
		igt_assert_eq(__gem_wait(fd, &wait), -ETIME);
		igt_assert_eq_s64(wait.timeout_ns, 0);

		if ((flags & HANG) == 0) {
			wait.timeout_ns = NSEC_PER_SEC; /* 1.0s */
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns > 0);
		} else {
			wait.timeout_ns = -1;
			igt_assert_eq(__gem_wait(fd, &wait), 0);
			igt_assert(wait.timeout_ns == -1);
		}

		wait.timeout_ns = 0;
		igt_assert_eq(__gem_wait(fd, &wait), 0);
		igt_assert(wait.timeout_ns == 0);

		if ((flags & HANG) == 0)
			timer_delete(timer);
	}

	gem_close(fd, obj.handle);
}

igt_main
{
	const struct intel_execution_engine *e;
	int fd = -1;

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver_master(DRIVER_INTEL);
	}

	igt_subtest("invalid-flags")
		invalid_flags(fd);

	igt_subtest("invalid-buf")
		invalid_buf(fd);

	igt_subtest_group {
		igt_fixture {
			igt_fork_hang_detector(fd);
			igt_fork_signal_helper();
		}

		igt_subtest("basic-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY);
		}
		igt_subtest("basic-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, 0);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_group {
				igt_subtest_f("busy-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, BUSY);
				}
				igt_subtest_f("wait-%s", e->name) {
					gem_quiescent_gpu(fd);
					basic(fd, e->exec_id | e->flags, 0);
				}
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_stop_hang_detector();
		}
	}

	igt_subtest_group {
		igt_hang_t hang;

		igt_fixture {
			hang = igt_allow_hang(fd, 0, 0);
			igt_fork_signal_helper();
		}

		igt_subtest("hang-busy-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, BUSY | HANG);
		}
		igt_subtest("hang-wait-all") {
			gem_quiescent_gpu(fd);
			basic(fd, -1, HANG);
		}

		for (e = intel_execution_engines; e->name; e++) {
			igt_subtest_f("hang-busy-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG | BUSY);
			}
			igt_subtest_f("hang-wait-%s", e->name) {
				gem_quiescent_gpu(fd);
				basic(fd, e->exec_id | e->flags, HANG);
			}
		}

		igt_fixture {
			igt_stop_signal_helper();
			igt_disallow_hang(fd, hang);
		}
	}

	igt_fixture {
		close(fd);
	}
}
