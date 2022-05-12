// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 */

#include <linux/module.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_simple_kms_helper.h>

#include "ms912x.h"

static int ms912x_usb_suspend(struct usb_interface *interface,
			      pm_message_t message)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	return drm_mode_config_helper_suspend(dev);
}

static int ms912x_usb_resume(struct usb_interface *interface)
{
	struct drm_device *dev = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(dev);
}

/*
 * FIXME: Dma-buf sharing requires DMA support by the importing device.
 *        This function is a workaround to make USB devices work as well.
 *        See todo.rst for how to fix the issue in the dma-buf framework.
 */
static struct drm_gem_object *
ms912x_driver_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct ms912x_device *ms912x = to_ms912x(dev);

	if (!ms912x->dmadev)
		return ERR_PTR(-ENODEV);

	return drm_gem_prime_import_dev(dev, dma_buf, ms912x->dmadev);
}

DEFINE_DRM_GEM_FOPS(ms912x_driver_fops);

static const struct drm_driver driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &ms912x_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_prime_import = ms912x_driver_gem_prime_import,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static const struct drm_mode_config_funcs ms912x_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct ms912x_mode ms912x_mode_list[] = {
	/* Found in captures of the Windows driver */
	MS912X_MODE(800, 600, 60, 0x4200, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1024, 768, 60, 0x4700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1152, 864, 60, 0x4c00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 720, 60, 0x4f00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 800, 60, 0x5700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 960, 60, 0x5b00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 1024, 60, 0x6000, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1366, 768, 60, 0x6600, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1400, 1050, 60, 0x6700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1440, 900, 60, 0x6b00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1680, 1050, 60, 0x7800, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1920, 1080, 60, 0x8100, MS912X_PIXFMT_UYVY),
	/* TODO: more mode numbers? */
};

static const struct ms912x_mode *
ms912x_get_mode(const struct drm_display_mode *mode)
{
	int i;
	int width = mode->hdisplay;
	int height = mode->vdisplay;
	int hz = drm_mode_vrefresh(mode);
	for (i = 0; i < ARRAY_SIZE(ms912x_mode_list); i++) {
		if (ms912x_mode_list[i].width == width &&
		    ms912x_mode_list[i].height == height &&
		    ms912x_mode_list[i].hz == hz) {
			return &ms912x_mode_list[i];
		}
	}
	return ERR_PTR(-EINVAL);
}

static void ms912x_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
	struct ms912x_device *ms912x = to_ms912x(pipe->crtc.dev);
	struct drm_display_mode *mode = &crtc_state->mode;

	ms912x_power_on(ms912x);

	if (crtc_state->mode_changed)
		ms912x_set_resolution(ms912x, ms912x_get_mode(mode));
}

static void ms912x_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct ms912x_device *ms912x = to_ms912x(pipe->crtc.dev);

	ms912x_power_off(ms912x);
}

enum drm_mode_status
ms912x_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
		       const struct drm_display_mode *mode)
{
	const struct ms912x_mode *ret = ms912x_get_mode(mode);
	if (IS_ERR(ret)) {
		return MODE_BAD;
	}
	return MODE_OK;
}

int ms912x_pipe_check(struct drm_simple_display_pipe *pipe,
		      struct drm_plane_state *new_plane_state,
		      struct drm_crtc_state *new_crtc_state)
{
	return 0;
}

static inline int ms912x_rgb_to_y(int r, int g, int b)
{
	const int luma = (16 << 16) + 16763 * r + 32904 * g + 6391 * b;
	return luma >> 16;
}
static inline int ms912x_rgb_to_u(int r, int g, int b)
{
	const int u = (128 << 16) - 9676 * r - 18996 * g + 28672 * b;
	return u >> 16;
}
static inline int ms912x_rgb_to_v(int r, int g, int b)
{
	const int v = (128 << 16) + 28672 * r - 24009 * g - 4663 * b;
	return v >> 16;
}

static int ms912x_xrgb_to_yuv422_line(u8 *transfer_buffer, u32 *xrgb_buffer,
				      size_t len)
{
	int i, offset = 0;
	unsigned int pixel1, pixel2;
	int r1, g1, b1, r2, g2, b2;
	int v, y1, u, y2;
	for (i = 0; i < len; i += 2) {
		pixel1 = xrgb_buffer[i];
		pixel2 = xrgb_buffer[i + 1];

		r1 = (pixel1 >> 16) & 0xFF;
		g1 = (pixel1 >> 8) & 0xFF;
		b1 = pixel1 & 0xFF;
		r2 = (pixel2 >> 16) & 0xFF;
		g2 = (pixel2 >> 8) & 0xFF;
		b2 = pixel2 & 0xFF;

		y1 = ms912x_rgb_to_y(r1, g1, b1);
		y2 = ms912x_rgb_to_y(r2, g2, b2);

		v = (ms912x_rgb_to_v(r1, g1, b1) +
		     ms912x_rgb_to_v(r2, g2, b2)) /
		    2;
		u = (ms912x_rgb_to_u(r1, g1, b1) +
		     ms912x_rgb_to_u(r2, g2, b2)) /
		    2;

		transfer_buffer[offset++] = u;
		transfer_buffer[offset++] = y1;
		transfer_buffer[offset++] = v;
		transfer_buffer[offset++] = y2;
	}
	return offset;
}

static const unsigned char ms912x_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
						       0x00, 0x00, 0x00, 0x00 };
static void ms912x_urb_completion(struct urb *urb)
{
	struct ms912x_device *ms912x = urb->context;
	if (ms912x)
		complete(&ms912x->transfer_done);

	kfree(urb->transfer_buffer);
}
static void ms912x_fb_mark_dirty(struct drm_framebuffer *fb,
				 const struct dma_buf_map *map,
				 struct drm_rect *rect)
{
	int ret, i;
	void *vaddr = map->vaddr;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct usb_device *usb_dev = interface_to_usbdev(ms912x->intf);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_frame_update_header header;
	struct urb *urb;
	void *transfer_buffer, *usb_buffer;
	int total_length = 0;
	int transfer_blocks, transfer_length;
	/* Hardware can only update framebuffer in multiples of 16 horizontally */
	int x = ALIGN_DOWN(rect->x1, 16);
	int width = ALIGN(rect->x2, 16) - x;
	int y1 = rect->y1;
	int y2 = rect->y2;
	int idx;

	drm_dev_enter(drm, &idx);

	rect->x1 = x;
	rect->x2 = x + width;

	header.header = cpu_to_be16(0xff00);
	header.x = x / 16;
	header.y = cpu_to_be16(y1);
	header.width = width / 16;
	header.height = cpu_to_be16(drm_rect_height(rect));

	transfer_buffer =
		vmalloc(drm_rect_width(rect) * drm_rect_height(rect) * 2 + 16);
	if (!transfer_buffer)
		goto dev_exit;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto free_transfer_buffer;

	memcpy(transfer_buffer, &header, 8);
	total_length += 8;

	for (i = y1; i < y2; i++) {
		const int line_offset = fb->pitches[0] * i;
		const int byte_offset = line_offset + (x * 4);
		ms912x_xrgb_to_yuv422_line(transfer_buffer + total_length,
					   vaddr + byte_offset, width);
		total_length += width * 2;
	}

	memcpy(transfer_buffer + total_length, ms912x_end_of_buffer, 8);
	total_length += 8;

	transfer_blocks =
		DIV_ROUND_UP(total_length, MS912X_MAX_TRANSFER_LENGTH);
	for (i = 0; i < transfer_blocks; i++) {
		/* Last block may be shorter */
		urb = usb_alloc_urb(0, GFP_KERNEL);
		usb_buffer = kmalloc(MS912X_MAX_TRANSFER_LENGTH, GFP_KERNEL);
		if (!usb_buffer) {
			usb_free_urb(urb);
			goto end_cpu_access;
		}
		transfer_length = min((i + 1) * MS912X_MAX_TRANSFER_LENGTH,
				      total_length) -
				  i * MS912X_MAX_TRANSFER_LENGTH;
		memcpy(usb_buffer,
		       transfer_buffer + i * MS912X_MAX_TRANSFER_LENGTH,
		       transfer_length);

		/* Completion handler triggers completion after the last packet */
		usb_fill_bulk_urb(urb, usb_dev, usb_sndbulkpipe(usb_dev, 4),
				  usb_buffer, transfer_length,
				  ms912x_urb_completion,
				  (i == transfer_blocks - 1) ? ms912x : NULL);
		ret = usb_submit_urb(urb, GFP_KERNEL);

		usb_free_urb(urb);
		if (ret < 0) {
			goto end_cpu_access;
		}
	}

	wait_for_completion_interruptible_timeout(&ms912x->transfer_done,
						  msecs_to_jiffies(1000));

end_cpu_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

free_transfer_buffer:
	vfree(transfer_buffer);
dev_exit:
	drm_dev_exit(idx);
}
static void ms912x_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_shadow_plane_state *shadow_plane_state =
		to_drm_shadow_plane_state(state);
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		ms912x_fb_mark_dirty(state->fb, &shadow_plane_state->data[0],
				     &rect);
}

static const struct drm_simple_display_pipe_funcs ms912x_pipe_funcs = {
	.enable = ms912x_pipe_enable,
	.disable = ms912x_pipe_disable,
	.check = ms912x_pipe_check,
	.mode_valid = ms912x_pipe_mode_valid,
	.update = ms912x_pipe_update,
	DRM_GEM_SIMPLE_DISPLAY_PIPE_SHADOW_PLANE_FUNCS,
};

static const uint32_t ms912x_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static int ms912x_usb_probe(struct usb_interface *interface,
			    const struct usb_device_id *id)
{
	int ret;
	struct ms912x_device *ms912x;
	struct drm_device *dev;

	ms912x = devm_drm_dev_alloc(&interface->dev, &driver,
				    struct ms912x_device, drm);
	if (IS_ERR(ms912x))
		return PTR_ERR(ms912x);

	ms912x->intf = interface;
	dev = &ms912x->drm;

	init_completion(&ms912x->transfer_done);

	ms912x->dmadev = usb_intf_get_dma_device(interface);
	if (!ms912x->dmadev)
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */

	ret = drmm_mode_config_init(dev);
	if (ret)
		goto err_put_device;

	/* No idea */
	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = 10000;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = 10000;
	dev->mode_config.funcs = &ms912x_mode_config_funcs;

	ret = ms912x_connector_init(ms912x);
	if (ret)
		goto err_put_device;

	ret = drm_simple_display_pipe_init(&ms912x->drm, &ms912x->display_pipe,
					   &ms912x_pipe_funcs,
					   ms912x_pipe_formats,
					   ARRAY_SIZE(ms912x_pipe_formats),
					   NULL, &ms912x->connector);
	if (ret)
		goto err_put_device;

	drm_plane_enable_fb_damage_clips(&ms912x->display_pipe.plane);

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, ms912x);

	drm_kms_helper_poll_init(dev);

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_put_device;

	drm_fbdev_generic_setup(dev, 0);

	return 0;

err_put_device:
	put_device(ms912x->dmadev);
	return ret;
}

static void ms912x_usb_disconnect(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);
	struct drm_device *dev = &ms912x->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	put_device(ms912x->dmadev);
	ms912x->dmadev = NULL;
}

static const struct usb_device_id id_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(0x534d, 0x6021, 0xff, 0x00, 0x00) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver ms912x_driver = {
	.name = "ms912x",
	.probe = ms912x_usb_probe,
	.disconnect = ms912x_usb_disconnect,
	.suspend = ms912x_usb_suspend,
	.resume = ms912x_usb_resume,
	.id_table = id_table,
};
module_usb_driver(ms912x_driver);
MODULE_LICENSE("GPL");
