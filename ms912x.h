/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef MS912X_H
#define MS912X_H

#include <linux/mm_types.h>
#include <linux/scatterlist.h>
#include <linux/usb.h>

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_encoder.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem.h>
#include <drm/drm_plane.h>
#include <drm/drm_rect.h>

#define DRIVER_NAME "ms912x"
#define DRIVER_DESC "MacroSilicon USB to VGA/HDMI"

#define DRIVER_MAJOR 1
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 1

#define MS912X_CMD_RESOLUTION		0x01
#define MS912X_CMD_MODE			0x02
#define MS912X_CMD_UNKNOWN1		0x03
#define MS912X_CMD_UNKNOWN2		0x04
#define MS912X_CMD_OUTPUT_ENABLE	0x05
#define MS912X_CMD_POWER		0x07

struct ms912x_usb_request {
	void *transfer_buffer;
	struct ms912x_device *ms912x;
	size_t transfer_len;
	size_t alloc_len;
	struct sg_table transfer_sgt;
	struct usb_sg_request sgr;
	struct work_struct work;
	struct timer_list timer;
	struct completion done;
	struct drm_format_conv_state fmtcnv_state;
};

struct ms912x_device {
	struct drm_device drm;
	struct usb_interface *intf;
	unsigned int bulk_pipe;

	struct drm_connector connector;
	struct drm_encoder encoder;
	struct drm_crtc crtc;
	struct drm_plane plane;

	struct drm_rect update_rect;

	/* Double buffer to allow memcpy and transfer
	 * to happen in parallel
	 */
	int current_request;
	struct ms912x_usb_request requests[2];
};

struct ms912x_request {
	u8 type;
	__be16 addr;
	u8 data[5];
} __packed;

struct ms912x_write_request {
	u8 type;
	u8 addr;
	u8 data[6];
} __packed;

struct ms912x_resolution_request {
	__be16 width;
	__be16 height;
	__be16 pixel_format;
} __packed;

struct ms912x_mode_request {
	__be16 mode;
	__be16 width;
	__be16 height;
} __packed;

struct ms912x_frame_update_header {
	__be16 header; /* ff 00 */
	u8 x; /* left in multiple of 16 */
	__be16 y;
	u8 width; /* width in multiples of 16 */
	__be16 height;
} __packed;

struct ms912x_mode {
	int width;
	int height;
	int hz;
	int mode;
	int pix_fmt;
};

#define MS912X_PIXFMT_UYVY 0x2200
#define MS912X_PIXFMT_RGB 0x1100

#define MS912X_MODE(w, h, z, m, f)                                             \
	{                                                                      \
		.width = w, .height = h, .hz = z, .mode = m, .pix_fmt = f      \
	}

#define MS912X_MAX_TRANSFER_LENGTH 65536

#define to_ms912x(x) container_of(x, struct ms912x_device, drm)

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address);
int ms912x_connector_init(struct ms912x_device *ms912x);
int ms912x_set_resolution(struct ms912x_device *ms912x,
			  const struct ms912x_mode *mode);

int ms912x_power_on(struct ms912x_device *ms912x);
int ms912x_power_off(struct ms912x_device *ms912x);

int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_rect *rect);

void ms912x_free_request(struct ms912x_usb_request *request);
int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len);
#endif
