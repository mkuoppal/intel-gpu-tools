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
 *    Jeff McGee <jeff.mcgee@intel.com>
 *
 */

#define _GNU_SOURCE
#include "igt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

#include "intel_bufmgr.h"

static int drm_fd;

static const char sysfs_base_path[] = "/sys/class/drm/card%d/gt_%s_freq_mhz";
enum {
	CUR,
	MIN,
	MAX,
	RP0,
	RP1,
	RPn,
	NUMFREQ
};

static int origfreqs[NUMFREQ];

struct junk {
	const char *name;
	const char *mode;
	FILE *filp;
} stuff[] = {
	{ "cur", "r", NULL }, { "min", "rb+", NULL }, { "max", "rb+", NULL }, { "RP0", "r", NULL }, { "RP1", "r", NULL }, { "RPn", "r", NULL }, { NULL, NULL, NULL }
};

static int readval(FILE *filp)
{
	int val;
	int scanned;

	rewind(filp);
	scanned = fscanf(filp, "%d", &val);
	igt_assert_eq(scanned, 1);

	return val;
}

static void read_freqs(int *freqs)
{
	int i;

	for (i = 0; i < NUMFREQ; i++)
		freqs[i] = readval(stuff[i].filp);
}

static void nsleep(unsigned long ns)
{
	struct timespec ts;
	int ret;

	ts.tv_sec = 0;
	ts.tv_nsec = ns;
	do {
		struct timespec rem;

		ret = nanosleep(&ts, &rem);
		igt_assert(ret == 0 || errno == EINTR);
		ts = rem;
	} while (ret && errno == EINTR);
}

static void wait_freq_settle(void)
{
	int timeout = 10;

	while (1) {
		int freqs[NUMFREQ];

		read_freqs(freqs);
		if (freqs[CUR] >= freqs[MIN] && freqs[CUR] <= freqs[MAX])
			break;
		nsleep(1000000);
		if (!timeout--)
			break;
	}
}

static int do_writeval(FILE *filp, int val, int lerrno, bool readback_check)
{
	int ret, orig;

	orig = readval(filp);
	rewind(filp);
	ret = fprintf(filp, "%d", val);

	if (lerrno) {
		/* Expecting specific error */
		igt_assert(ret == EOF && errno == lerrno);
		if (readback_check)
			igt_assert_eq(readval(filp), orig);
	} else {
		/* Expecting no error */
		igt_assert_lt(0, ret);
		wait_freq_settle();
		if (readback_check)
			igt_assert_eq(readval(filp), val);
	}

	return ret;
}
#define writeval(filp, val) do_writeval(filp, val, 0, true)
#define writeval_inval(filp, val) do_writeval(filp, val, EINVAL, true)
#define writeval_nocheck(filp, val) do_writeval(filp, val, 0, false)

static void checkit(const int *freqs)
{
	igt_assert_lte(freqs[MIN], freqs[MAX]);
	igt_assert_lte(freqs[CUR], freqs[MAX]);
	igt_assert_lte(freqs[RPn], freqs[CUR]);
	igt_assert_lte(freqs[RPn], freqs[MIN]);
	igt_assert_lte(freqs[MAX], freqs[RP0]);
	igt_assert_lte(freqs[RP1], freqs[RP0]);
	igt_assert_lte(freqs[RPn], freqs[RP1]);
	igt_assert_neq(freqs[RP0], 0);
	igt_assert_neq(freqs[RP1], 0);
}

static void dump(const int *freqs)
{
	int i;

	igt_debug("gt freq (MHz):");
	for (i = 0; i < NUMFREQ; i++)
		igt_debug("  %s=%d", stuff[i].name, freqs[i]);

	igt_debug("\n");
}

enum load {
	LOW,
	HIGH
};

static struct load_helper {
	int devid;
	int has_ppgtt;
	drm_intel_bufmgr *bufmgr;
	struct intel_batchbuffer *batch;
	drm_intel_bo *target_buffer;
	enum load load;
	bool exit;
	struct igt_helper_process igt_proc;
	drm_intel_bo *src, *dst;
} lh;

static void load_helper_signal_handler(int sig)
{
	if (sig == SIGUSR2)
		lh.load = lh.load == LOW ? HIGH : LOW;
	else
		lh.exit = true;
}

static void emit_store_dword_imm(uint32_t val)
{
	int cmd;
	struct intel_batchbuffer *batch = lh.batch;

	cmd = MI_STORE_DWORD_IMM;
	if (!lh.has_ppgtt)
		cmd |= MI_MEM_VIRTUAL;

	BEGIN_BATCH(4, 0); /* just ignore the reloc we emit and count dwords */
	OUT_BATCH(cmd);
	if (batch->gen >= 8) {
		OUT_RELOC(lh.target_buffer, I915_GEM_DOMAIN_INSTRUCTION,
			  I915_GEM_DOMAIN_INSTRUCTION, 0);
	} else {
		OUT_BATCH(0); /* reserved */
		OUT_RELOC(lh.target_buffer, I915_GEM_DOMAIN_INSTRUCTION,
			  I915_GEM_DOMAIN_INSTRUCTION, 0);
	}
	OUT_BATCH(val);
	ADVANCE_BATCH();
}

#define LOAD_HELPER_PAUSE_USEC 500
#define LOAD_HELPER_BO_SIZE (16*1024*1024)
static void load_helper_set_load(enum load load)
{
	igt_assert(lh.igt_proc.running);

	if (lh.load == load)
		return;

	lh.load = load;
	kill(lh.igt_proc.pid, SIGUSR2);
}

static void load_helper_run(enum load load)
{
	/*
	 * FIXME fork helpers won't get cleaned up when started from within a
	 * subtest, so handle the case where it sticks around a bit too long.
	 */
	if (lh.igt_proc.running) {
		load_helper_set_load(load);
		return;
	}

	lh.load = load;

	igt_fork_helper(&lh.igt_proc) {
		const uint32_t bbe = MI_BATCH_BUFFER_END;
		struct drm_i915_gem_exec_object2 object;
		struct drm_i915_gem_execbuffer2 execbuf;
		uint32_t fences[3];
		uint32_t val = 0;

		signal(SIGUSR1, load_helper_signal_handler);
		signal(SIGUSR2, load_helper_signal_handler);

		fences[0] = gem_create(drm_fd, 4096);
		gem_write(drm_fd, fences[0], 0,  &bbe, sizeof(bbe));
		fences[1] = gem_create(drm_fd, 4096);
		gem_write(drm_fd, fences[1], 0,  &bbe, sizeof(bbe));
		fences[2] = gem_create(drm_fd, 4096);
		gem_write(drm_fd, fences[2], 0,  &bbe, sizeof(bbe));

		memset(&execbuf, 0, sizeof(execbuf));
		execbuf.buffers_ptr = (uintptr_t)&object;
		execbuf.buffer_count = 1;
		if (intel_gen(lh.devid) >= 6)
			execbuf.flags = I915_EXEC_BLT;

		while (!lh.exit) {
			memset(&object, 0, sizeof(object));
			object.handle = fences[val%3];

			while (gem_bo_busy(drm_fd, object.handle))
				usleep(100);

			if (lh.load == HIGH)
				intel_copy_bo(lh.batch, lh.dst, lh.src,
					      LOAD_HELPER_BO_SIZE);

			emit_store_dword_imm(val);
			intel_batchbuffer_flush_on_ring(lh.batch,
                                                        I915_EXEC_BLT);
			val++;

			gem_execbuf(drm_fd, &execbuf);

			/* Lower the load by pausing after every submitted
			 * write. */
			if (lh.load == LOW)
				usleep(LOAD_HELPER_PAUSE_USEC);
		}

		/* Wait for completion without boosting */
		usleep(1000);
		while (gem_bo_busy(drm_fd, lh.target_buffer->handle))
			usleep(1000);

		igt_debug("load helper sent %u dword writes\n", val);
		gem_close(drm_fd, fences[0]);
		gem_close(drm_fd, fences[1]);
		gem_close(drm_fd, fences[2]);
	}
}

static void load_helper_stop(void)
{
	kill(lh.igt_proc.pid, SIGUSR1);
	igt_assert(igt_wait_helper(&lh.igt_proc) == 0);
}

static void load_helper_init(void)
{
	lh.devid = intel_get_drm_devid(drm_fd);
	lh.has_ppgtt = gem_uses_ppgtt(drm_fd);

	/* MI_STORE_DATA can only use GTT address on gen4+/g33 and needs
	 * snoopable mem on pre-gen6. Hence load-helper only works on gen6+, but
	 * that's also all we care about for the rps testcase*/
	igt_assert(intel_gen(lh.devid) >= 6);
	lh.bufmgr = drm_intel_bufmgr_gem_init(drm_fd, 4096);
	igt_assert(lh.bufmgr);

	drm_intel_bufmgr_gem_enable_reuse(lh.bufmgr);

	lh.batch = intel_batchbuffer_alloc(lh.bufmgr, lh.devid);
	igt_assert(lh.batch);

	lh.target_buffer = drm_intel_bo_alloc(lh.bufmgr, "target bo",
					      4096, 4096);
	igt_assert(lh.target_buffer);

	lh.dst = drm_intel_bo_alloc(lh.bufmgr, "dst bo",
				    LOAD_HELPER_BO_SIZE, 4096);
	igt_assert(lh.dst);
	lh.src = drm_intel_bo_alloc(lh.bufmgr, "src bo",
				    LOAD_HELPER_BO_SIZE, 4096);
	igt_assert(lh.src);
}

static void load_helper_deinit(void)
{
	if (lh.igt_proc.running)
		load_helper_stop();

	if (lh.target_buffer)
		drm_intel_bo_unreference(lh.target_buffer);
	if (lh.src)
		drm_intel_bo_unreference(lh.src);
	if (lh.dst)
		drm_intel_bo_unreference(lh.dst);

	if (lh.batch)
		intel_batchbuffer_free(lh.batch);

	if (lh.bufmgr)
		drm_intel_bufmgr_destroy(lh.bufmgr);
}

static void do_load_gpu(void)
{
	load_helper_run(LOW);
	nsleep(10000000);
	load_helper_stop();
}

/* Return a frequency rounded by HW to the nearest supported value */
static int get_hw_rounded_freq(int target)
{
	int freqs[NUMFREQ];
	int old_freq;
	int idx;
	int ret;

	read_freqs(freqs);

	if (freqs[MIN] > target)
		idx = MIN;
	else
		idx = MAX;

	old_freq = freqs[idx];
	writeval_nocheck(stuff[idx].filp, target);
	read_freqs(freqs);
	ret = freqs[idx];
	writeval_nocheck(stuff[idx].filp, old_freq);

	return ret;
}

static void min_max_config(void (*check)(void), bool load_gpu)
{
	int fmid = (origfreqs[RPn] + origfreqs[RP0]) / 2;

	/*
	 * hw (and so kernel) rounds to the nearest value supported by
	 * the given platform.
	 */
	fmid = get_hw_rounded_freq(fmid);

	igt_debug("\nCheck original min and max...\n");
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nSet min=RPn and max=RP0...\n");
	writeval(stuff[MIN].filp, origfreqs[RPn]);
	writeval(stuff[MAX].filp, origfreqs[RP0]);
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nIncrease min to midpoint...\n");
	writeval(stuff[MIN].filp, fmid);
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nIncrease min to RP0...\n");
	writeval(stuff[MIN].filp, origfreqs[RP0]);
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nIncrease min above RP0 (invalid)...\n");
	writeval_inval(stuff[MIN].filp, origfreqs[RP0] + 1000);
	check();

	igt_debug("\nDecrease max to RPn (invalid)...\n");
	writeval_inval(stuff[MAX].filp, origfreqs[RPn]);
	check();

	igt_debug("\nDecrease min to midpoint...\n");
	writeval(stuff[MIN].filp, fmid);
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nDecrease min to RPn...\n");
	writeval(stuff[MIN].filp, origfreqs[RPn]);
	if (load_gpu)
		do_load_gpu();
	check();

	igt_debug("\nDecrease min below RPn (invalid)...\n");
	writeval_inval(stuff[MIN].filp, 0);
	check();

	igt_debug("\nDecrease max to midpoint...\n");
	writeval(stuff[MAX].filp, fmid);
	check();

	igt_debug("\nDecrease max to RPn...\n");
	writeval(stuff[MAX].filp, origfreqs[RPn]);
	check();

	igt_debug("\nDecrease max below RPn (invalid)...\n");
	writeval_inval(stuff[MAX].filp, 0);
	check();

	igt_debug("\nIncrease min to RP0 (invalid)...\n");
	writeval_inval(stuff[MIN].filp, origfreqs[RP0]);
	check();

	igt_debug("\nIncrease max to midpoint...\n");
	writeval(stuff[MAX].filp, fmid);
	check();

	igt_debug("\nIncrease max to RP0...\n");
	writeval(stuff[MAX].filp, origfreqs[RP0]);
	check();

	igt_debug("\nIncrease max above RP0 (invalid)...\n");
	writeval_inval(stuff[MAX].filp, origfreqs[RP0] + 1000);
	check();

	writeval(stuff[MIN].filp, origfreqs[MIN]);
	writeval(stuff[MAX].filp, origfreqs[MAX]);
}

static void basic_check(void)
{
	int freqs[NUMFREQ];

	read_freqs(freqs);
	dump(freqs);
	checkit(freqs);
}

#define IDLE_WAIT_TIMESTEP_MSEC 250
#define IDLE_WAIT_TIMEOUT_MSEC 2500
static void idle_check(void)
{
	int freqs[NUMFREQ];
	int wait = 0;

	/* Monitor frequencies until cur settles down to min, which should
	 * happen within the allotted time */
	do {
		read_freqs(freqs);
		dump(freqs);
		checkit(freqs);
		if (freqs[CUR] == freqs[RPn])
			break;
		usleep(1000 * IDLE_WAIT_TIMESTEP_MSEC);
		wait += IDLE_WAIT_TIMESTEP_MSEC;
	} while (wait < IDLE_WAIT_TIMEOUT_MSEC);

	igt_assert_eq(freqs[CUR], freqs[RPn]);
	igt_debug("Required %d msec to reach cur=idle\n", wait);
}

#define LOADED_WAIT_TIMESTEP_MSEC 100
#define LOADED_WAIT_TIMEOUT_MSEC 3000
static void loaded_check(void)
{
	int freqs[NUMFREQ];
	int wait = 0;

	/* Monitor frequencies until cur increases to max, which should
	 * happen within the allotted time */
	do {
		read_freqs(freqs);
		dump(freqs);
		checkit(freqs);
		if (freqs[CUR] == freqs[MAX])
			break;
		usleep(1000 * LOADED_WAIT_TIMESTEP_MSEC);
		wait += LOADED_WAIT_TIMESTEP_MSEC;
	} while (wait < LOADED_WAIT_TIMEOUT_MSEC);

	igt_assert_eq(freqs[CUR], freqs[MAX]);
	igt_debug("Required %d msec to reach cur=max\n", wait);
}

#define STABILIZE_WAIT_TIMESTEP_MSEC 250
#define STABILIZE_WAIT_TIMEOUT_MSEC 15000
static void stabilize_check(int *out)
{
	int freqs[NUMFREQ];
	int wait = 0;

	read_freqs(freqs);
	dump(freqs);
	usleep(1000 * STABILIZE_WAIT_TIMESTEP_MSEC);
	do {
		read_freqs(out);
		dump(out);

		if (memcmp(freqs, out, sizeof(freqs)) == 0)
			break;

		memcpy(freqs, out, sizeof(freqs));
		wait += STABILIZE_WAIT_TIMESTEP_MSEC;
	} while (wait < STABILIZE_WAIT_TIMEOUT_MSEC);

	igt_debug("Waited %d msec to stabilize cur\n", wait);
}

static void reset_gpu(void)
{
	int fd = drm_open_driver(DRIVER_INTEL);
	igt_post_hang_ring(fd, igt_hang_ring(fd, I915_EXEC_DEFAULT));
	close(fd);
}

static void waitboost(bool reset)
{
	const uint32_t bbe = MI_BATCH_BUFFER_END;
	struct drm_i915_gem_exec_object2 object;
	struct drm_i915_gem_execbuffer2 execbuf;
	int pre_freqs[NUMFREQ];
	int boost_freqs[NUMFREQ];
	int post_freqs[NUMFREQ];

	int fd = drm_open_driver(DRIVER_INTEL);

	/* When we wait upon the GPU, we want to temporarily boost it
	 * to maximum.
	 */

	load_helper_run(LOW);

	igt_debug("Apply low load...\n");
	sleep(1);
	stabilize_check(pre_freqs);

	if (reset) {
		igt_debug("Reset gpu...\n");
		reset_gpu();
		sleep(1);
	}

	igt_debug("Wait for gpu...\n");
	memset(&object, 0, sizeof(object));
	object.handle = gem_create(fd, 4096);
	gem_write(fd, object.handle, 0, &bbe, sizeof(bbe));
	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)&object;
	execbuf.buffer_count = 1;
	do {
		for (int i = 0; i < 64; i++)
			gem_execbuf(fd, &execbuf);
	} while (!gem_bo_busy(fd, object.handle));
	gem_sync(fd, object.handle);
	read_freqs(boost_freqs);
	dump(boost_freqs);
	gem_close(fd, object.handle);

	igt_debug("Apply low load again...\n");
	sleep(1);
	stabilize_check(post_freqs);

	igt_debug("Removing load...\n");
	load_helper_stop();
	idle_check();

	igt_assert_lt(pre_freqs[CUR], pre_freqs[MAX]);
	igt_assert_eq(boost_freqs[CUR], boost_freqs[MAX]);
	igt_assert_lt(post_freqs[CUR], post_freqs[MAX]);

	close(fd);
}

static void pm_rps_exit_handler(int sig)
{
	if (origfreqs[MIN] > readval(stuff[MAX].filp)) {
		writeval(stuff[MAX].filp, origfreqs[MAX]);
		writeval(stuff[MIN].filp, origfreqs[MIN]);
	} else {
		writeval(stuff[MIN].filp, origfreqs[MIN]);
		writeval(stuff[MAX].filp, origfreqs[MAX]);
	}

	load_helper_deinit();
	close(drm_fd);
}

igt_main
{
	igt_skip_on_simulation();

	igt_fixture {
		const int device = drm_get_card();
		struct junk *junk = stuff;
		int ret;

		/* Use drm_open_driver to verify device existence */
		drm_fd = drm_open_driver(DRIVER_INTEL);

		do {
			int val = -1;
			char *path;
			ret = asprintf(&path, sysfs_base_path, device, junk->name);
			igt_assert(ret != -1);
			junk->filp = fopen(path, junk->mode);
			igt_require(junk->filp);
			setbuf(junk->filp, NULL);

			val = readval(junk->filp);
			igt_assert(val >= 0);
			junk++;
		} while(junk->name != NULL);

		read_freqs(origfreqs);

		igt_install_exit_handler(pm_rps_exit_handler);

		load_helper_init();
	}

	igt_subtest("basic-api")
		min_max_config(basic_check, false);

	igt_subtest("min-max-config-idle")
		min_max_config(idle_check, true);

	igt_subtest("min-max-config-loaded") {
		load_helper_run(HIGH);
		min_max_config(loaded_check, false);
		load_helper_stop();
	}

	igt_subtest("waitboost")
		waitboost(false);

	igt_subtest("reset")
		waitboost(true);

}
