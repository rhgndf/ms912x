// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>

#include "ms912x.h"

static int ms912x_usb_suspend(struct usb_interface *interface,
			      pm_message_t message)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	return drm_mode_config_helper_suspend(&ms912x->drm);
}

static int ms912x_usb_resume(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);

	return drm_mode_config_helper_resume(&ms912x->drm);
}

static struct drm_gem_object *
ms912x_driver_gem_prime_import(struct drm_device *dev, struct dma_buf *dma_buf)
{
	struct ms912x_device *ms912x = to_ms912x(dev);

	if (!ms912x->dmadev)
		return ERR_PTR(-ENODEV);

	return drm_gem_prime_import_dev(dev, dma_buf, ms912x->dmadev);
}

DEFINE_DRM_GEM_FOPS(ms912x_driver_fops);

static struct drm_driver driver = {
	.driver_features = DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,

	/* GEM hooks */
	.fops = &ms912x_driver_fops,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_prime_import = ms912x_driver_gem_prime_import,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
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
	MS912X_MODE(800,  600, 60, 0x4200, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1024,  768, 60, 0x4700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1152,  864, 60, 0x4c00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280,  720, 60, 0x4f00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280,  800, 60, 0x5700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280,  960, 60, 0x5b00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 1024, 60, 0x6000, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1366,  768, 60, 0x6600, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1400, 1050, 60, 0x6700, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1440,  900, 60, 0x6b00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1680, 1050, 60, 0x7800, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1920, 1080, 60, 0x8100, MS912X_PIXFMT_UYVY),

	/* Dumped from the device */
	MS912X_MODE(720,  480, 60, 0x0200, MS912X_PIXFMT_UYVY),
	MS912X_MODE(720,  576, 60, 0x1100, MS912X_PIXFMT_UYVY),
	MS912X_MODE(640,  480, 60, 0x4000, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1024,  768, 60, 0x4900, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280,  600, 60, 0x4e00, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280,  768, 60, 0x5400, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1280, 1024, 60, 0x6100, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1360,  768, 60, 0x6400, MS912X_PIXFMT_UYVY),
	MS912X_MODE(1600, 1200, 60, 0x7300, MS912X_PIXFMT_UYVY),
	/* TODO: more mode numbers? */
};

static const struct ms912x_mode *
ms912x_get_mode(const struct drm_display_mode *mode)
{
	unsigned int i;
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
	return NULL;
}

static void ms912x_crtc_atomic_enable(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_device *dev = crtc->dev;
	struct ms912x_device *ms912x = to_ms912x(dev);
	const struct ms912x_mode *ms_mode;
	int ret;

	ret = ms912x_power_on(ms912x);
	if (ret) {
		drm_err(dev, "failed to power on display: %d\n", ret);
		return;
	}

	ms_mode = ms912x_get_mode(&crtc_state->mode);
	if (!ms_mode) {
		drm_err(dev, "unsupported mode passed to CRTC enable\n");
		return;
	}

	ret = ms912x_set_resolution(ms912x, ms_mode);
	if (ret)
		drm_err(dev, "failed to set display mode: %d\n", ret);
}

static void ms912x_crtc_atomic_disable(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct drm_device *dev = crtc->dev;
	struct ms912x_device *ms912x = to_ms912x(dev);
	int ret;

	ret = ms912x_power_off(ms912x);
	if (ret)
		drm_err(dev, "failed to power off display: %d\n", ret);
}

static enum drm_mode_status
ms912x_crtc_mode_valid(struct drm_crtc *crtc,
		       const struct drm_display_mode *mode)
{
	const struct ms912x_mode *ms_mode = ms912x_get_mode(mode);

	if (!ms_mode)
		return MODE_BAD;

	return MODE_OK;
}

static int ms912x_plane_atomic_check(struct drm_plane *plane,
				     struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *crtc_state = NULL;

	new_plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (new_plane_state->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state,
							   new_plane_state->crtc);

	return drm_atomic_helper_check_plane_state(new_plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, false);
}

static void ms912x_merge_rects(struct drm_rect *dest, struct drm_rect *r1,
			       struct drm_rect *r2)
{
	dest->x1 = min(r1->x1, r2->x1);
	dest->y1 = min(r1->y1, r2->y1);
	dest->x2 = max(r1->x2, r2->x2);
	dest->y2 = max(r1->y2, r2->y2);
}

static void ms912x_plane_atomic_update(struct drm_plane *plane,
				       struct drm_atomic_state *state)
{
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *new_crtc_state;
	struct drm_shadow_plane_state *shadow_plane_state;
	struct ms912x_device *ms912x = to_ms912x(plane->dev);
	struct drm_rect current_rect, rect;

	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	new_plane_state = drm_atomic_get_new_plane_state(state, plane);

	if (!new_plane_state->fb)
		return;
	new_crtc_state = drm_atomic_get_new_crtc_state(state,
						       new_plane_state->crtc);
	shadow_plane_state = to_drm_shadow_plane_state(new_plane_state);

	if (old_plane_state->fb != new_plane_state->fb ||
	    new_crtc_state->mode_changed)
		drm_rect_init(&ms912x->update_rect, 0, 0, 0, 0);

	if (drm_atomic_helper_damage_merged(old_plane_state, new_plane_state,
					    &current_rect)) {
		/*
		 * The device double buffers, so we need to send the update
		 * rects of the last two frames.
		 */
		ms912x_merge_rects(&rect, &current_rect, &ms912x->update_rect);
		if (ms912x_fb_send_rect(new_plane_state->fb,
					&shadow_plane_state->data[0], &rect)) {
			/* In case of error, merge the rects to update later */
			ms912x_merge_rects(&ms912x->update_rect,
					   &ms912x->update_rect, &rect);
		} else {
			ms912x->update_rect = current_rect;
		}
	}
}

static const struct drm_crtc_helper_funcs ms912x_crtc_helper_funcs = {
	.mode_valid = ms912x_crtc_mode_valid,
	.atomic_enable = ms912x_crtc_atomic_enable,
	.atomic_disable = ms912x_crtc_atomic_disable,
};

static const struct drm_crtc_funcs ms912x_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs ms912x_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_plane_helper_funcs ms912x_plane_helper_funcs = {
	DRM_GEM_SHADOW_PLANE_HELPER_FUNCS,
	.atomic_check = ms912x_plane_atomic_check,
	.atomic_update = ms912x_plane_atomic_update,
};

static const struct drm_plane_funcs ms912x_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	DRM_GEM_SHADOW_PLANE_FUNCS,
};

static const u32 ms912x_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static int ms912x_usb_probe(struct usb_interface *interface,
			    const struct usb_device_id *id)
{
	int ret;
	struct ms912x_device *ms912x;
	struct drm_device *dev;
	struct usb_endpoint_descriptor *bulk_out;

	ms912x = devm_drm_dev_alloc(&interface->dev, &driver,
				    struct ms912x_device, drm);
	if (IS_ERR(ms912x))
		return PTR_ERR(ms912x);

	ms912x->intf = interface;

	ret = usb_find_bulk_out_endpoint(interface->cur_altsetting, &bulk_out);
	if (ret)
		return ret;
	ms912x->bulk_pipe =
		usb_sndbulkpipe(interface_to_usbdev(interface),
				usb_endpoint_num(bulk_out));
	dev = &ms912x->drm;

	ret = drmm_mode_config_init(dev);

	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.max_width = 2048;
	dev->mode_config.min_height = 0;
	dev->mode_config.max_height = 2048;
	dev->mode_config.funcs = &ms912x_mode_config_funcs;

	/* This stops weird behavior in the device */
	ret = ms912x_set_resolution(ms912x, &ms912x_mode_list[0]);
	if (ret)
		return ret;

	ret = ms912x_init_request(ms912x, &ms912x->requests[0],
				  2048 * 2048 * 2 + MS912X_FRAME_OVERHEAD);
	if (ret)
		return ret;

	ret = ms912x_init_request(ms912x, &ms912x->requests[1],
				  2048 * 2048 * 2 + MS912X_FRAME_OVERHEAD);
	if (ret)
		goto err_free_request_0;
	complete(&ms912x->requests[1].done);

	ret = drm_universal_plane_init(dev, &ms912x->plane, 0,
				       &ms912x_plane_funcs,
				       ms912x_plane_formats,
				       ARRAY_SIZE(ms912x_plane_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		goto err_free_request_1;

	drm_plane_helper_add(&ms912x->plane, &ms912x_plane_helper_funcs);
	drm_plane_enable_fb_damage_clips(&ms912x->plane);

	ret = drm_crtc_init_with_planes(dev, &ms912x->crtc, &ms912x->plane,
					NULL, &ms912x_crtc_funcs, NULL);
	if (ret)
		goto err_free_request_1;

	drm_crtc_helper_add(&ms912x->crtc, &ms912x_crtc_helper_funcs);

	ret = drm_encoder_init(dev, &ms912x->encoder, &ms912x_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		goto err_free_request_1;
	ms912x->encoder.possible_crtcs = drm_crtc_mask(&ms912x->crtc);

	ret = ms912x_connector_init(ms912x);
	if (ret)
		goto err_free_request_1;

	drm_mode_config_reset(dev);

	usb_set_intfdata(interface, ms912x);

	drm_kms_helper_poll_init(dev);

	ms912x->dmadev = usb_intf_get_dma_device(interface);
	if (!ms912x->dmadev)
		drm_warn(dev,
			 "buffer sharing not supported"); /* not an error */

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_put_device;

	drm_fbdev_generic_setup(dev, 0);

	return 0;

err_put_device:
	if (ms912x->dmadev) {
		put_device(ms912x->dmadev);
		ms912x->dmadev = NULL;
	}
	drm_kms_helper_poll_fini(dev);
err_free_request_1:
	ms912x_free_request(&ms912x->requests[1]);
err_free_request_0:
	ms912x_free_request(&ms912x->requests[0]);
	return ret;
}

static void ms912x_usb_disconnect(struct usb_interface *interface)
{
	struct ms912x_device *ms912x = usb_get_intfdata(interface);
	struct drm_device *dev = &ms912x->drm;

	drm_kms_helper_poll_fini(dev);
	drm_dev_unplug(dev);
	drm_atomic_helper_shutdown(dev);
	cancel_work_sync(&ms912x->requests[0].work);
	cancel_work_sync(&ms912x->requests[1].work);
	ms912x_free_request(&ms912x->requests[0]);
	ms912x_free_request(&ms912x->requests[1]);
	if (ms912x->dmadev) {
		put_device(ms912x->dmadev);
		ms912x->dmadev = NULL;
	}
}

static const struct usb_device_id id_table[] = {
	/* USB 2 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x534d, 0x6021, 0xff, 0x00, 0x00) },
	/* USB 2 Sometimes this PID will pop up*/
	{ USB_DEVICE_AND_INTERFACE_INFO(0x534d, 0x0821, 0xff, 0x00, 0x00) },
	/* USB 3 */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x345f, 0x9132, 0xff, 0x00, 0x00) },
	{},
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver ms912x_driver = {
	.name = DRIVER_NAME,
	.probe = ms912x_usb_probe,
	.disconnect = ms912x_usb_disconnect,
	.suspend = ms912x_usb_suspend,
	.resume = ms912x_usb_resume,
	.id_table = id_table,
};
module_usb_driver(ms912x_driver);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
