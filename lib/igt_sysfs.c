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
 *
 */

#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <i915_drm.h>

#include "igt_sysfs.h"

static int readN(int fd, char *buf, int len)
{
	int total = 0;
	do {
		int ret = read(fd, buf + total, len - total);
		if (ret < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		if (ret <= 0)
			return total ?: ret;

		total += ret;
	} while (1);
}

static int writeN(int fd, const char *buf, int len)
{
	int total = 0;
	do {
		int ret = write(fd, buf + total, len - total);
		if (ret < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		if (ret <= 0)
			return total ?: ret;

		total += ret;
	} while (1);
}

/**
 * igt_sysfs_open:
 * @device: fd of the device (or -1 to default to Intel)
 *
 * This opens the sysfs directory corresponding to device for use
 * with igt_sysfs_set() and igt_sysfs_get().
 *
 * Returns:
 * The directory fd, or -1 on failure.
 */
int igt_sysfs_open(int fd, int *idx)
{
	char path[80];
	struct stat st;

	if (fd != -1 && (fstat(fd, &st) || !S_ISCHR(st.st_mode)))
		return -1;

	for (int n = 0; n < 16; n++) {
		int len = sprintf(path, "/sys/class/drm/card%d", n);
		if (fd != -1) {
			FILE *file;
			int ret, maj, min;

			sprintf(path + len, "/dev");
			file = fopen(path, "r");
			if (!file)
				continue;

			ret = fscanf(file, "%d:%d", &maj, &min);
			fclose(file);

			if (ret != 2 ||
			    major(st.st_rdev) != maj ||
			    minor(st.st_rdev) != min)
				continue;
		} else {
			/* Bleh. Search for intel */
			sprintf(path + len, "/error");
			if (stat(path, &st))
				continue;
		}

		path[len] = '\0';
		if (idx)
			*idx = n;
		return open(path, O_RDONLY);
	}

	return -1;
}

/**
 * igt_sysfs_set:
 * @dir: directory for the device from igt_sysfs_open()
 * @attr: name of the sysfs node to open
 * @value: the string to write
 *
 * This writes the value to the sysfs file.
 *
 * Returns:
 * True on success, false on failure.
 */
bool igt_sysfs_set(int dir, const char *attr, const char *value)
{
	int fd, len, ret;

	fd = openat(dir, attr, O_WRONLY);
	if (fd < 0)
		return false;

	len = strlen(value);
	ret = writeN(fd, value, len);
	close(fd);

	return len == ret;
}

/**
 * igt_sysfs_get:
 * @dir: directory for the device from igt_sysfs_open()
 * @attr: name of the sysfs node to open
 *
 * This reads the value from the sysfs file.
 *
 * Returns:
 * A nul-terminated string, must be freed by caller after use, or NULL
 * on failure.
 */
char *igt_sysfs_get(int dir, const char *attr)
{
	char *buf;
	int len, offset, rem;
	int ret, fd;

	fd = openat(dir, attr, O_RDONLY);
	if (fd < 0)
		return NULL;

	offset = 0;
	len = 64;
	rem = len - offset - 1;
	buf = malloc(len);
	if (!buf)
		goto out;

	while ((ret = readN(fd, buf + offset, rem)) == rem) {
		char *newbuf;

		newbuf = realloc(buf, 2*len);
		if (!newbuf)
			break;

		len *= 2;
		offset += ret;
		rem = len - offset - 1;
	}

	if (ret != -1)
		offset += ret;
	buf[offset] = '\0';
	while (offset > 0 && buf[offset-1] == '\n')
		buf[--offset] = '\0';

out:
	close(fd);
	return buf;
}
