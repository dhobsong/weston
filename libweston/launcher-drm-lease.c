/*
 * Copyright © 2012 Benjamin Franzke
 * Copyright © 2013 Intel Corporation
 * Copyright © 2021 Igel Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <libweston/libweston.h>

#include <dlmclient.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#include "launcher-impl.h"
#include "shared/string-helpers.h"

struct launcher_drm_lease {
	struct weston_launcher base;
	struct dlm_lease *drm_lease;
	int drm_fd;
};

static int
launcher_drm_lease_open(struct weston_launcher *launcher_base, const char *name, int flags)
{
	struct launcher_drm_lease *launcher = wl_container_of(launcher_base, launcher, base);
	struct stat s;
	int fd;
	const char *path;

	struct dlm_lease *drm_lease = dlm_get_lease(name);

	/* Fallback to open() for non DRM lease name/paths (e.g. input devices) */
	if (!drm_lease)
		return open(name, flags | O_CLOEXEC);

	fd = dlm_lease_fd(drm_lease);
	if (fd < 0) {
		weston_log("Invalid DRM lease: %s\n", name);
		dlm_release_lease(drm_lease);
		return -1;
	}

	launcher->drm_lease = drm_lease;
	launcher->drm_fd = fd;
	return fd;
}

static void
launcher_drm_lease_close(struct weston_launcher *launcher_base, int fd)
{
	struct launcher_drm_lease *launcher = wl_container_of(launcher_base, launcher, base);

	if (fd != launcher->drm_fd) {
		close(fd);
		return;
	}

	dlm_release_lease(launcher->drm_lease);
	launcher->drm_lease = NULL;
}

static void
launcher_drm_lease_restore(struct weston_launcher *launcher_base)
{
}

static int
launcher_drm_lease_activate_vt(struct weston_launcher *launcher_base, int vt)
{
	return -1;
}

static int
launcher_drm_lease_connect(struct weston_launcher **out, struct weston_compositor *compositor,
			int tty, const char *seat_id, bool sync_drm)
{
	struct launcher_drm_lease *launcher;

	char *s = getenv("WESTON_DRM_LEASE");
	int lease_enable = 0;

	if (!s || safe_strtoint(s, &lease_enable) < 0 || !lease_enable)
		return -EINVAL;

	if (geteuid() != 0)
		return -EINVAL;

	launcher = zalloc(sizeof(*launcher));
	if (launcher == NULL)
		return -ENOMEM;

	launcher->base.iface = &launcher_drm_lease_iface;

	* (struct launcher_drm_lease **) out = launcher;
	return 0;
}

static void
launcher_drm_lease_destroy(struct weston_launcher *launcher_base)
{
	struct launcher_drm_lease *launcher = wl_container_of(launcher_base, launcher, base);

	free(launcher);
}

static int
launcher_drm_lease_get_vt(struct weston_launcher *base)
{
	return -1;
}

const struct launcher_interface launcher_drm_lease_iface = {
	.connect = launcher_drm_lease_connect,
	.destroy = launcher_drm_lease_destroy,
	.open = launcher_drm_lease_open,
	.close = launcher_drm_lease_close,
	.activate_vt = launcher_drm_lease_activate_vt,
	.get_vt = launcher_drm_lease_get_vt,
};
