/*
 * Copyright © 2014 Renesas Electronics Corp.
 *
 * Based on pixman-renderer by:
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Vasily Khoruzhick <anarsoul@gmail.com>
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Authors:
 *	Takanari Hayama <taki@igel.co.jp>
 */

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include "v4l2-renderer.h"
#include "v4l2-renderer-device.h"

#include <xf86drm.h>
#include <libkms/libkms.h>

#include <wayland-kms.h>
#include <wayland-kms-server-protocol.h>

#include "media-ctl/mediactl.h"
#include "media-ctl/v4l2subdev.h"
#include "media-ctl/tools.h"

#include <linux/input.h>

/* Required for a short term workaround */
#include "v4l2-compat.h"

#if 0
#define DBG(...) weston_log(__VA_ARGS__)
#define DBGC(...) weston_log_continue(__VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#define DBGC(...) do {} while (0)
#endif

struct v4l2_output_state {
	struct v4l2_renderer_output *output;
	uint32_t stride;
	void *map;
};

struct v4l2_renderer {
	struct weston_renderer base;

	struct kms_driver *kms;
	struct wl_kms *wl_kms;

	struct media_device *media;
	char *device_name;
	int drm_fd;

	struct v4l2_renderer_device *device;

	int repaint_debug;
	struct weston_binding *debug_binding;

	struct wl_signal destroy_signal;
};

static struct v4l2_device_interface *device_interface = NULL;

static inline struct v4l2_output_state *
get_output_state(struct weston_output *output)
{
	return (struct v4l2_output_state *)output->renderer_state;
}

static int
v4l2_renderer_create_surface(struct weston_surface *surface);

static inline struct v4l2_surface_state *
get_surface_state(struct weston_surface *surface)
{
	if (!surface->renderer_state)
		v4l2_renderer_create_surface(surface);

	return (struct v4l2_surface_state *)surface->renderer_state;
}

static inline struct v4l2_renderer *
get_renderer(struct weston_compositor *ec)
{
	return (struct v4l2_renderer *)ec->renderer;
}

static int
v4l2_renderer_read_pixels(struct weston_output *output,
			 pixman_format_code_t format, void *pixels,
			 uint32_t x, uint32_t y,
			 uint32_t width, uint32_t height)
{
	struct v4l2_output_state *vo = get_output_state(output);
	uint32_t v, len = width * 4, stride = vo->stride * 4;
	void *src, *dst;

	switch(format) {
	case PIXMAN_a8r8g8b8:
		break;
	default:
		return -1;
	}

	if (x == 0 && y == 0 &&
	    width == (uint32_t)output->current_mode->width &&
	    height == (uint32_t)output->current_mode->height &&
	    vo->stride == len) {
		DBG("%s: copy entire buffer at once\n", __func__);
		// TODO: we may want to optimize this using underlying
		// V4L2 MC hardware if possible.
		memcpy(pixels, vo->map, vo->stride * height);
		return 0;
	}

	src = vo->map + x * 4 + y * stride;
	dst = pixels;
	for (v = y; v < height; v++) {
		memcpy(dst, src, len);
		src += stride;
		dst += len;
	}

	return 0;
}

static void
region_global_to_output(struct weston_output *output, pixman_region32_t *region)
{
	pixman_region32_translate(region, -output->x, -output->y);
	weston_transformed_region(output->width, output->height,
				  output->transform, output->current_scale,
				  region, region);
}

#define D2F(v) pixman_double_to_fixed((double)v)

static void
transform_apply_viewport(pixman_transform_t *transform,
			 struct weston_surface *surface)
{
	struct weston_buffer_viewport *vp = &surface->buffer_viewport;
	double src_width, src_height;
	double src_x, src_y;

	if (vp->buffer.src_width == wl_fixed_from_int(-1)) {
		if (vp->surface.width == -1)
			return;

		src_x = 0.0;
		src_y = 0.0;
		src_width = surface->width_from_buffer;
		src_height = surface->height_from_buffer;
	} else {
		src_x = wl_fixed_to_double(vp->buffer.src_x);
		src_y = wl_fixed_to_double(vp->buffer.src_y);
		src_width = wl_fixed_to_double(vp->buffer.src_width);
		src_height = wl_fixed_to_double(vp->buffer.src_height);
	}

	pixman_transform_scale(transform, NULL,
			       D2F(src_width / surface->width),
			       D2F(src_height / surface->height));
	pixman_transform_translate(transform, NULL, D2F(src_x), D2F(src_y));
}

static void
repaint_region(struct weston_view *ev, struct weston_output *output,
	       pixman_region32_t *region, pixman_region32_t *surf_region,
	       pixman_op_t pixman_op)	// XXX: this needs to be replaced.
{
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;
	struct v4l2_surface_state *vs = get_surface_state(ev->surface);
	struct weston_buffer_viewport *vp = &ev->surface->buffer_viewport;
	pixman_region32_t final_region;
	float view_x, view_y;
	pixman_transform_t transform;
	pixman_fixed_t fw, fh;
	pixman_box32_t *bbox;
	pixman_vector_t q1, q2;
	int src_x, src_y, src_width, src_height;
	int dst_x, dst_y, dst_width, dst_height;

	/* The final region to be painted is the intersection of
	 * 'region' and 'surf_region'. However, 'region' is in the global
	 * coordinates, and 'surf_region' is in the surface-local
	 * coordinates
	 */
	pixman_region32_init(&final_region);
	if (surf_region) {
		pixman_region32_copy(&final_region, surf_region);

		/* Convert from surface to global coordinates */
		if (!ev->transform.enabled) {
			pixman_region32_translate(&final_region, ev->geometry.x, ev->geometry.y);
		} else {
			weston_view_to_global_float(ev, 0, 0, &view_x, &view_y);
			pixman_region32_translate(&final_region, (int)view_x, (int)view_y);
		}

		/* We need to paint the intersection */
		pixman_region32_intersect(&final_region, &final_region, region);
	} else {
		/* If there is no surface region, just use the global region */
		pixman_region32_copy(&final_region, region);
	}

	/* Convert from global to output coord */
	bbox = pixman_region32_extents(&final_region);
	DBG("%s: final_region: global:(%d,%d)-(%d,%d)\n", __func__, bbox->x1, bbox->y1, bbox->x2, bbox->y2);

	region_global_to_output(output, &final_region);

	bbox = pixman_region32_extents(&final_region);
	DBG("%s: final_region: local:(%d,%d)-(%d,%d)\n", __func__, bbox->x1, bbox->y1, bbox->x2, bbox->y2);

	/*
	 * At this point, we should have the destination in final_region.
	 */

	/* Set up the source transformation based on the surface
	   position, the output position/transform/scale and the client
	   specified buffer transform/scale */
	pixman_transform_init_identity(&transform);
	pixman_transform_scale(&transform, NULL,
			       pixman_double_to_fixed ((double)1.0/output->current_scale),
			       pixman_double_to_fixed ((double)1.0/output->current_scale));

	fw = pixman_int_to_fixed(output->width);
	fh = pixman_int_to_fixed(output->height);
	switch (output->transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		pixman_transform_rotate(&transform, NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(&transform, NULL, 0, fh);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		pixman_transform_rotate(&transform, NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(&transform, NULL, fw, fh);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_rotate(&transform, NULL, 0, pixman_fixed_1);
		pixman_transform_translate(&transform, NULL, fw, 0);
		break;
	}

	switch (output->transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_scale(&transform, NULL,
				       pixman_int_to_fixed (-1),
				       pixman_int_to_fixed (1));
		pixman_transform_translate(&transform, NULL, fw, 0);
		break;
	}

        pixman_transform_translate(&transform, NULL,
				   pixman_double_to_fixed (output->x),
				   pixman_double_to_fixed (output->y));

	if (ev->transform.enabled) {
		/* Pixman supports only 2D transform matrix, but Weston uses 3D,
		 * so we're omitting Z coordinate here
		 */
		pixman_transform_t surface_transform = {{
				{ D2F(ev->transform.matrix.d[0]),
				  D2F(ev->transform.matrix.d[4]),
				  D2F(ev->transform.matrix.d[12]),
				},
				{ D2F(ev->transform.matrix.d[1]),
				  D2F(ev->transform.matrix.d[5]),
				  D2F(ev->transform.matrix.d[13]),
				},
				{ D2F(ev->transform.matrix.d[3]),
				  D2F(ev->transform.matrix.d[7]),
				  D2F(ev->transform.matrix.d[15]),
				}
			}};

		pixman_transform_invert(&surface_transform, &surface_transform);
		pixman_transform_multiply (&transform, &surface_transform, &transform);
	} else {
		pixman_transform_translate(&transform, NULL,
					   pixman_double_to_fixed ((double)-ev->geometry.x),
					   pixman_double_to_fixed ((double)-ev->geometry.y));
	}

	transform_apply_viewport(&transform, ev->surface);

	fw = pixman_int_to_fixed(ev->surface->width_from_buffer);
	fh = pixman_int_to_fixed(ev->surface->height_from_buffer);

	switch (vp->buffer.transform) {
	case WL_OUTPUT_TRANSFORM_FLIPPED:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_scale(&transform, NULL,
				       pixman_int_to_fixed (-1),
				       pixman_int_to_fixed (1));
		pixman_transform_translate(&transform, NULL, fw, 0);
		break;
	}

	switch (vp->buffer.transform) {
	default:
	case WL_OUTPUT_TRANSFORM_NORMAL:
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		break;
	case WL_OUTPUT_TRANSFORM_90:
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		pixman_transform_rotate(&transform, NULL, 0, pixman_fixed_1);
		pixman_transform_translate(&transform, NULL, fh, 0);
		break;
	case WL_OUTPUT_TRANSFORM_180:
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		pixman_transform_rotate(&transform, NULL, -pixman_fixed_1, 0);
		pixman_transform_translate(&transform, NULL, fw, fh);
		break;
	case WL_OUTPUT_TRANSFORM_270:
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		pixman_transform_rotate(&transform, NULL, 0, -pixman_fixed_1);
		pixman_transform_translate(&transform, NULL, 0, fw);
		break;
	}

	pixman_transform_scale(&transform, NULL,
			       pixman_double_to_fixed(vp->buffer.scale),
			       pixman_double_to_fixed(vp->buffer.scale));

	/*
	 * at this point, we should have a transformation for the source.
	 * However what we really need is not a transformation. We need
	 * is a source region.
	 */

	bbox = pixman_region32_extents(&final_region);
	q1.vector[0] = pixman_int_to_fixed(bbox->x1);
	q1.vector[1] = pixman_int_to_fixed(bbox->y1);
	q1.vector[2] = pixman_int_to_fixed(1);

	q2.vector[0] = pixman_int_to_fixed(bbox->x2);
	q2.vector[1] = pixman_int_to_fixed(bbox->y2);
	q2.vector[2] = pixman_int_to_fixed(1);

	DBG("bbox: (%d,%d)-(%d,%d)\n", bbox->x1, bbox->y1, bbox->x2, bbox->y2);
	DBG("q1: (%d,%d,%d)\n", pixman_fixed_to_int(q1.vector[0]), pixman_fixed_to_int(q1.vector[1]), pixman_fixed_to_int(q1.vector[2]));
	DBG("q2: (%d,%d,%d)\n", pixman_fixed_to_int(q2.vector[0]), pixman_fixed_to_int(q2.vector[1]), pixman_fixed_to_int(q2.vector[2]));

	DBG("transform: (%d,%d,%d)(%d,%d,%d)(%d,%d,%d)\n",
	    pixman_fixed_to_int(transform.matrix[0][0]), pixman_fixed_to_int(transform.matrix[1][0]), pixman_fixed_to_int(transform.matrix[2][0]),
	    pixman_fixed_to_int(transform.matrix[0][1]), pixman_fixed_to_int(transform.matrix[1][1]), pixman_fixed_to_int(transform.matrix[2][1]),
	    pixman_fixed_to_int(transform.matrix[0][2]), pixman_fixed_to_int(transform.matrix[1][2]), pixman_fixed_to_int(transform.matrix[2][2])
	);

	pixman_transform_point(&transform, &q1);
	pixman_transform_point(&transform, &q2);

	DBG("q1': (%d,%d,%d)\n", pixman_fixed_to_int(q1.vector[0]), pixman_fixed_to_int(q1.vector[1]), pixman_fixed_to_int(q1.vector[2]));
	DBG("q2': (%d,%d,%d)\n", pixman_fixed_to_int(q2.vector[0]), pixman_fixed_to_int(q2.vector[1]), pixman_fixed_to_int(q2.vector[2]));

	if (q1.vector[0] < q2.vector[0]) {
		src_x = pixman_fixed_to_int(q1.vector[0]);
		src_width = pixman_fixed_to_int(q2.vector[0] - q1.vector[0]);
	} else {
		src_x = pixman_fixed_to_int(q2.vector[0]);
		src_width = pixman_fixed_to_int(q1.vector[0] - q2.vector[0]);
	}

	if (q1.vector[1] < q2.vector[1]) {
		src_y = pixman_fixed_to_int(q1.vector[1]);
		src_height = pixman_fixed_to_int(q2.vector[1] - q1.vector[1]);
	} else {
		src_y = pixman_fixed_to_int(q2.vector[1]);
		src_height = pixman_fixed_to_int(q1.vector[1] - q2.vector[1]);
	}

	dst_x = bbox->x1;
	dst_y = bbox->y1;
	dst_width = bbox->x2 - bbox->x1;
	dst_height = bbox->y2 - bbox->y1;

	/* compose v4l2_surface_state */
	vs->src_rect.width = src_width;
	vs->src_rect.height = src_height;
	vs->src_rect.top = src_y;
	vs->src_rect.left = src_x;

	vs->dst_rect.width = dst_width;
	vs->dst_rect.height = dst_height;
	vs->dst_rect.top = dst_y;
	vs->dst_rect.left = dst_x;

	vs->alpha = ev->alpha;

	DBG("monitor: %dx%d@(%d,%d)\n", output->width, output->height, output->x, output->y);
	DBG("composing from %dx%d@(%d,%d) to %dx%d@(%d,%d)\n",
	    src_width, src_height, src_x, src_y,
	    dst_width, dst_height, dst_x, dst_y);

	device_interface->draw_view(renderer->device, vs);

	pixman_region32_fini(&final_region);
}

static void
draw_view(struct weston_view *ev, struct weston_output *output,
	  pixman_region32_t *damage) /* in global coordinates */
{
	struct v4l2_surface_state *vs = get_surface_state(ev->surface);
	/* repaint bounding region in global coordinates: */
	pixman_region32_t repaint;
	/* non-opaque region in surface coordinates: */
	pixman_region32_t surface_blend;
	pixman_box32_t *region;

	if (vs->planes[0].dmafd == 0)
		return;

	/*
	 * Check if the surface is still valid. OpenGL/ES apps may destroy
	 * buffers before they destroy a surface. This check works in the
	 * serialized world only.
	 */
	if (fcntl(vs->planes[0].dmafd, F_GETFD) < 0)
		return;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &ev->transform.boundingbox, damage);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	if (output->zoom.active) {
		weston_log("v4l2 renderer does not support zoom\n");
		goto out;
	}

#if 1
	{
		pixman_box32_t *b;

		DBG("%s: for dmafd=%d\n", __func__, vs->planes[0].dmafd);

		b = pixman_region32_extents(&repaint);
		DBG("%s: repaint: (%d,%d)-(%d,%d)\n", __func__, b->x1, b->y1, b->x2, b->y2);

		b = pixman_region32_extents(damage);
		DBG("%s: damage: (%d,%d)-(%d,%d)\n", __func__, b->x1, b->y1, b->x2, b->y2);

		b = pixman_region32_extents(&ev->clip);
		DBG("%s: clip: (%d,%d)-(%d,%d)\n", __func__, b->x1, b->y1, b->x2, b->y2);
	}
	repaint_region(ev, output, &repaint, NULL, PIXMAN_OP_OVER);
#else
	if (ev->alpha != 1.0 ||
	    (ev->transform.enabled &&
	     ev->transform.matrix.type != WESTON_MATRIX_TRANSFORM_TRANSLATE)) {
		repaint_region(ev, output, &repaint, NULL, PIXMAN_OP_OVER);
	} else {
		/* blended region is whole surface minus opaque region: */
		pixman_region32_init_rect(&surface_blend, 0, 0,
					  ev->surface->width, ev->surface->height);
		pixman_region32_subtract(&surface_blend, &surface_blend, &ev->surface->opaque);

		/*
		 * We assume renderer devices can compose only a single rectangular region.
		 * Therefore, we shall compose a none-opaque region first, including an opaque
		 * region, and then compose the opaque region on top of it.
		 */
		if (pixman_region32_not_empty(&surface_blend)) {
			repaint_region(ev, output, &repaint, &surface_blend, PIXMAN_OP_OVER);
		}

		/* now the opaque region */
		if (pixman_region32_not_empty(&ev->surface->opaque)) {
			repaint_region(ev, output, &repaint, &ev->surface->opaque, PIXMAN_OP_SRC);
		}
		pixman_region32_fini(&surface_blend);
	}
#endif

out:
	pixman_region32_fini(&repaint);
}

static void
repaint_surfaces(struct weston_output *output, pixman_region32_t *damage)
{
	struct v4l2_output_state *vo = get_output_state(output);
	struct weston_compositor *compositor = output->compositor;
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)compositor->renderer;
	struct weston_view *view;

	device_interface->begin_compose(renderer->device, vo->output);

	// for all views in the primary plane
	wl_list_for_each_reverse(view, &compositor->view_list, link)
		if (view->plane == &compositor->primary_plane)
			draw_view(view, output, &output->region);

	device_interface->finish_compose(renderer->device);
}

static void
v4l2_renderer_repaint_output(struct weston_output *output,
			    pixman_region32_t *output_damage)
{
	//struct v4l2_output_state *vo = get_output_state(output);
	DBG("%s\n", __func__);

	// render all views
	repaint_surfaces(output, output_damage);

	// remember the damaged area
	pixman_region32_copy(&output->previous_damage, output_damage);

	// emits signal
	wl_signal_emit(&output->frame_signal, output);

	/* Actual flip should be done by caller */
}

static inline void
v4l2_renderer_copy_buffer(struct v4l2_surface_state *vs, struct weston_buffer *buffer)
{
	void *src, *dst;
	int y, stride, bo_stride;

	src = wl_shm_buffer_get_data(buffer->shm_buffer);
	dst = vs->addr;

	stride = vs->planes[0].stride;
	bo_stride = vs->bo_stride;

	wl_shm_buffer_begin_access(buffer->shm_buffer);
	for (y = 0; y < buffer->height; y++) {
		memcpy(dst, src, buffer->width * vs->bpp);
		dst += bo_stride;
		src += stride;
	}
	wl_shm_buffer_end_access(buffer->shm_buffer);

}

static void
v4l2_renderer_flush_damage(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs = get_surface_state(surface);
	struct weston_buffer *buffer = vs->buffer_ref.buffer;

	DBG("%s: flushing damage..\n", __func__);

	v4l2_renderer_copy_buffer(vs, buffer);

	/*
	 * TODO: We may consider use of surface->damage to
	 * optimize updates.
	 */
}

static void
v4l2_release_kms_bo(struct v4l2_surface_state *vs)
{
	if (!vs)
		return;

	if (vs->bo) {
		if (kms_bo_unmap(vs->bo))
			weston_log("kms_bo_unmap failed.\n");

		kms_bo_destroy(&vs->bo);
		vs->bo = vs->addr = NULL;
	}
}

static void
buffer_state_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
			  buffer_destroy_listener);

	v4l2_release_kms_bo(vs);

	vs->buffer_destroy_listener.notify = NULL;
}

static int
v4l2_renderer_attach_shm(struct v4l2_surface_state *vs, struct weston_buffer *buffer,
			 struct wl_shm_buffer *shm_buffer)
{
	unsigned int pixel_format;
	int bpp;
	int fd = vs->renderer->drm_fd;
	unsigned attr[] = {
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_WIDTH, 0,
		KMS_HEIGHT, 0,
		KMS_TERMINATE_PROP_LIST
	};
	unsigned handle, stride, bo_stride;

	switch (wl_shm_buffer_get_format(shm_buffer)) {
	case WL_SHM_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bpp = 4;
		break;

	case WL_SHM_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bpp = 4;
		break;

	case WL_SHM_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bpp = 2;
		break;

	case WL_SHM_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bpp = 2;
		break;

	default:
		weston_log("Unsupported SHM buffer format\n");
		return -1;
	}

	buffer->shm_buffer = shm_buffer;
	buffer->width = wl_shm_buffer_get_width(shm_buffer);
	buffer->height = wl_shm_buffer_get_height(shm_buffer);
	stride = wl_shm_buffer_get_stride(shm_buffer);

	if (vs->width == buffer->width &&
	    vs->height == buffer->height &&
	    vs->planes[0].stride == stride && vs->bpp == bpp) {
	    // no need to recreate buffer
	    return 0;
	}

	// release if there's allocated buffer
	v4l2_release_kms_bo(vs);

	// create a reference to the shm_buffer.
	vs->width = buffer->width;
	vs->height = buffer->height;
	vs->pixel_format = pixel_format;
	vs->num_planes = 1;
	vs->planes[0].stride = stride;
	vs->bpp = bpp;

	if (device_interface->attach_buffer(vs) == -1)
		return -1;

	// create gbm_bo
	attr[3] = (buffer->width + 1) * bpp / 4;
	attr[5] = buffer->height;

	if (kms_bo_create(vs->renderer->kms, attr, &vs->bo)) {
		weston_log("kms_bo_create failed.\n");
		goto error;
	}

	if (kms_bo_map(vs->bo, &vs->addr)) {
		weston_log("kms_bo_map failed.\n");
		goto error;
	}

	if (kms_bo_get_prop(vs->bo, KMS_PITCH, &bo_stride)) {
		weston_log("kms_bo_get_prop failed.\n");
		goto error;
	}
	vs->bo_stride = stride;

	if (kms_bo_get_prop(vs->bo, KMS_HANDLE, &handle)) {
		weston_log("kms_bo_get_prop failed.\n");
		goto error;
	}
	drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &vs->planes[0].dmafd);

	v4l2_renderer_copy_buffer(vs, buffer);

	DBG("%s: %dx%d buffer attached (dmafd=%d).\n", __func__, buffer->width, buffer->height, vs->planes[0].dmafd);

	return 0;

error:
	if (vs->bo) {
		if (vs->addr)
			kms_bo_unmap(vs->bo);
		vs->addr = NULL;

		kms_bo_destroy(&vs->bo);
		vs->bo = NULL;
	}
	return -1;
}

static int
v4l2_renderer_attach_dmabuf(struct v4l2_surface_state *vs, struct weston_buffer *buffer)
{
	unsigned int pixel_format;
	struct wl_kms_buffer *kbuf;
	int bpp, i;

	buffer->legacy_buffer = (struct wl_buffer *)buffer->resource;

	kbuf = wayland_kms_buffer_get(buffer->resource);

	switch (kbuf->format) {
	case WL_KMS_FORMAT_XRGB8888:
		pixel_format = V4L2_PIX_FMT_XBGR32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_ARGB8888:
		pixel_format = V4L2_PIX_FMT_ABGR32;
		bpp = 4;
		break;

	case WL_KMS_FORMAT_RGB888:
		pixel_format = V4L2_PIX_FMT_RGB24;
		bpp = 3;
		break;

	case WL_KMS_FORMAT_RGB565:
		pixel_format = V4L2_PIX_FMT_RGB565;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_YUYV:
		pixel_format = V4L2_PIX_FMT_YUYV;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_UYVY:
		pixel_format = V4L2_PIX_FMT_UYVY;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV12:
		pixel_format = V4L2_PIX_FMT_NV12M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV16:
		pixel_format = V4L2_PIX_FMT_NV16M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV21:
		pixel_format = V4L2_PIX_FMT_NV21M;
		bpp = 2;
		break;

	case WL_KMS_FORMAT_NV61:
		pixel_format = V4L2_PIX_FMT_NV61M;
		bpp = 2;
		break;

	default:
		weston_log("Unsupported DMABUF buffer format\n");
		return -1;
	}

	vs->width = buffer->width = kbuf->width;
	vs->height = buffer->height = kbuf->height;
	vs->pixel_format = pixel_format;
	vs->bpp = bpp;
	vs->num_planes = kbuf->num_planes;
	for (i = 0; i < kbuf->num_planes; i++) {
		vs->planes[i].stride = kbuf->planes[i].stride;
		vs->planes[i].dmafd = kbuf->planes[i].fd;
	}

	if (device_interface->attach_buffer(vs) == -1)
		return -1;

	DBG("%s: %dx%d buffer attached (dmabuf=%d, stride=%d).\n", __func__, kbuf->width, kbuf->height, kbuf->fd, kbuf->stride);

	return 0;
}

static void
v4l2_renderer_attach(struct weston_surface *es, struct weston_buffer *buffer)
{
	struct v4l2_surface_state *vs = get_surface_state(es);
	struct wl_shm_buffer *shm_buffer;
	int ret;

	// refer the given weston_buffer. if there's an existing reference,
	// release it first if not the same. if the buffer is the new one,
	// increment the refrence counter. all done in weston_buffer_reference().
	weston_buffer_reference(&vs->buffer_ref, buffer);

	// clear the destroy listener if set.
	if (vs->buffer_destroy_listener.notify) {
		wl_list_remove(&vs->buffer_destroy_listener.link);
		vs->buffer_destroy_listener.notify = NULL;
	}

	// if no buffer given, quit now.
	if (!buffer)
		return;

	// for shm_buffer.
	shm_buffer = wl_shm_buffer_get(buffer->resource);

	if (shm_buffer) {
		ret = v4l2_renderer_attach_shm(vs, buffer, shm_buffer);
	} else {
		ret = v4l2_renderer_attach_dmabuf(vs, buffer);
	}

	if (ret == -1) {
		weston_buffer_reference(&vs->buffer_ref, NULL);
		return;
	}

	// listen to the buffer destroy event.
	vs->buffer_destroy_listener.notify =
		buffer_state_handle_buffer_destroy;
	wl_signal_add(&buffer->destroy_signal,
		      &vs->buffer_destroy_listener);
}

static void
v4l2_renderer_surface_state_destroy(struct v4l2_surface_state *vs)
{
	wl_list_remove(&vs->surface_destroy_listener.link);
	wl_list_remove(&vs->renderer_destroy_listener.link);
	if (vs->buffer_destroy_listener.notify) {
		wl_list_remove(&vs->buffer_destroy_listener.link);
		vs->buffer_destroy_listener.notify = NULL;
	}

	vs->surface->renderer_state = NULL;

	// TODO: Release any resources associated to the surface here.

	weston_buffer_reference(&vs->buffer_ref, NULL);
	free(vs);
}

static void
surface_state_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
				     surface_destroy_listener);

	v4l2_renderer_surface_state_destroy(vs);
}

static void
surface_state_handle_renderer_destroy(struct wl_listener *listener, void *data)
{
	struct v4l2_surface_state *vs;

	vs = container_of(listener, struct v4l2_surface_state,
				     renderer_destroy_listener);

	v4l2_renderer_surface_state_destroy(vs);
}

static int
v4l2_renderer_create_surface(struct weston_surface *surface)
{
	struct v4l2_surface_state *vs;
	struct v4l2_renderer *vr = get_renderer(surface->compositor);

	vs = device_interface->create_surface(vr->device);
	if (!vs)
		return -1;

	surface->renderer_state = vs;

	vs->surface = surface;
	vs->renderer = vr;

	vs->surface_destroy_listener.notify =
		surface_state_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &vs->surface_destroy_listener);

	vs->renderer_destroy_listener.notify =
		surface_state_handle_renderer_destroy;
	wl_signal_add(&vr->destroy_signal,
		      &vs->renderer_destroy_listener);

	return 0;
}

static void
v4l2_renderer_surface_set_color(struct weston_surface *es,
				float red, float green, float blue, float alpha)
{
	DBG("%s\n", __func__);

	// struct v4l2_surface_state *vs = get_surface_state(es);

	// TODO: set solid color to the surface
}

static void
v4l2_renderer_destroy(struct weston_compositor *ec)
{
	struct v4l2_renderer *vr = get_renderer(ec);

	DBG("%s\n", __func__);

	wl_signal_emit(&vr->destroy_signal, vr);
	weston_binding_destroy(vr->debug_binding);
	free(vr);

	ec->renderer = NULL;
}

static void
debug_media_ctl(void *ignore, char *fmt, ...)
{
	char buffer[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);

	weston_log(buffer);
}

static void
debug_binding(struct weston_seat *seat, uint32_t time, uint32_t key,
	      void *data)
{
	struct weston_compositor *ec = data;
	struct v4l2_renderer *vr = (struct v4l2_renderer *) ec->renderer;

	vr->repaint_debug ^= 1;

	if (vr->repaint_debug) {
		// TODO: enable repaint debug

                media_debug_set_handler(vr->media,
					(void (*)(void *, ...))debug_media_ctl, NULL);

	} else {
		// TODO: disable repaint debug

		weston_compositor_damage_all(ec);
	}
}

static void
v4l2_load_device_module(const char *device_name)
{
	char path[1024];

	if (!device_name)
		return;

	snprintf(path, sizeof(path), "v4l2-%s-device.so", device_name);
	device_interface =
		(struct v4l2_device_interface*)weston_load_module(path, "v4l2_device_interface");
}

static char*
v4l2_get_cname(const char *bus_info)
{
	char *p, *device_name;

	if (!bus_info)
		return NULL;

	if ((p = strchr(bus_info, ':')))
		device_name = strdup(p + 1);
	else
		device_name = strdup(bus_info);

	p = strchr(device_name, '.');
	*p = '\0';

	return device_name;
}

static int
v4l2_renderer_init(struct weston_compositor *ec, int drm_fd, char *drm_fn)
{
	struct v4l2_renderer *renderer;
	char *device;
	const char *device_name;
	const struct media_device_info *info;
	struct weston_config_section *section;

	if (!drm_fn)
		return -1;

	renderer = calloc(1, sizeof *renderer);
	if (renderer == NULL)
		return -1;

	renderer->wl_kms = wayland_kms_init(ec->wl_display, NULL, drm_fn, drm_fd);

	/* Get V4L2 media controller device to use */
	section = weston_config_get_section(ec->config,
					    "media-ctl", NULL, NULL);
	weston_config_section_get_string(section, "device", &device, "/dev/media0");

	/* Initialize V4L2 media controller */
	renderer->media = media_device_new(device);
	if (!renderer->media) {
		weston_log("Can't create a media controller.");
		goto error;
	}

	/* Enumerate entities, pads and links */
	if (media_device_enumerate(renderer->media)) {
		weston_log("Can't enumerate %s.", device);
		goto error;
	}

	/* Device info */
	info = media_get_info(renderer->media);
	weston_log("Media controller API version %u.%u.%u\n",
		   (info->media_version >> 16) & 0xff,
		   (info->media_version >>  8) & 0xff,
		   (info->media_version)       & 0xff);
	weston_log_continue("Media device information\n"
			    "------------------------\n"
			    "driver         %s\n"
			    "model          %s\n"
			    "serial         %s\n"
			    "bus info       %s\n"
			    "hw revision    0x%x\n"
			    "driver version %u.%u.%u\n",
			    info->driver, info->model,
			    info->serial, info->bus_info,
			    info->hw_revision,
			    (info->driver_version >> 16) & 0xff,
			    (info->driver_version >>  8) & 0xff,
			    (info->driver_version)       & 0xff);

	device_name = v4l2_get_cname(info->bus_info);
	v4l2_load_device_module(device_name);
	if (!device_interface)
		goto error;

	renderer->device = device_interface->init(renderer->media);
	if (!renderer->device)
		goto error;

	weston_log("V4L2 media controller device initialized.\n");

	kms_create(drm_fd, &renderer->kms);

	/* initialize renderer base */
	renderer->drm_fd = drm_fd;
	renderer->repaint_debug = 0;

	renderer->base.read_pixels = v4l2_renderer_read_pixels;
	renderer->base.repaint_output = v4l2_renderer_repaint_output;
	renderer->base.flush_damage = v4l2_renderer_flush_damage;
	renderer->base.attach = v4l2_renderer_attach;
	renderer->base.surface_set_color = v4l2_renderer_surface_set_color;
	renderer->base.destroy = v4l2_renderer_destroy;

	ec->renderer = &renderer->base;
	ec->capabilities |= device_interface->get_capabilities();

	ec->read_format = PIXMAN_a8r8g8b8;

	renderer->debug_binding =
		weston_compositor_add_debug_binding(ec, KEY_R,
						    debug_binding, ec);

	wl_display_add_shm_format(ec->wl_display, WL_SHM_FORMAT_RGB565);

	wl_signal_init(&renderer->destroy_signal);

	return 0;

error:
	free(renderer);
	weston_log("V4L2 renderer initialization failed.\n");
	return -1;
}

static void
v4l2_renderer_output_set_buffer(struct weston_output *output, struct v4l2_bo_state *bo)
{
	struct v4l2_output_state *vo = get_output_state(output);

	vo->stride = bo->stride;
	vo->map = bo->map;

	device_interface->set_output_buffer(vo->output, bo);

	return;
}

static int
v4l2_renderer_output_create(struct weston_output *output)
{
	struct v4l2_renderer *renderer = (struct v4l2_renderer*)output->compositor->renderer;
	struct v4l2_output_state *vo;
	struct v4l2_renderer_output *outdev;

	if (!renderer)
		return -1;

	outdev = device_interface->create_output(renderer->device,
						 output->current_mode->width,
						 output->current_mode->height);
	if (!outdev)
		return -1;

	if (!(vo = calloc(1, sizeof *vo)))
		return -1;

	vo->output = outdev;

	output->renderer_state = vo;

	return 0;
}

static void
v4l2_renderer_output_destroy(struct weston_output *output)
{
	struct v4l2_output_state *vo = get_output_state(output);
	free(vo);
}

WL_EXPORT struct v4l2_renderer_interface v4l2_renderer_interface = {
	.init = v4l2_renderer_init,
	.output_create = v4l2_renderer_output_create,
	.output_destroy = v4l2_renderer_output_destroy,
	.set_output_buffer = v4l2_renderer_output_set_buffer
};