// SPDX-License-Identifier: GPL-2.0-only

#include <asm/byteorder.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/usb.h>

#include <drm/drm_drv.h>
#include <drm/drm_modes.h>

#include "ms912x.h"

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address)
{
	struct ms912x_request request;
	struct usb_device *usb_dev;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	mutex_lock(&ms912x->ctrl_lock);

	usb_dev = interface_to_usbdev(ms912x->intf);
	memset(&request, 0, sizeof(request));
	request.type = MS912X_REQ_TYPE_READ_BYTE;
	request.addr = cpu_to_be16(address);

	ret = usb_control_msg_send(usb_dev, 0, HID_REQ_SET_REPORT,
				   USB_DIR_OUT | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
	if (ret)
		goto ctrl_unlock;

	ret = usb_control_msg_recv(usb_dev, 0, HID_REQ_GET_REPORT,
				   USB_DIR_IN | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (!ret)
		ret = request.data[0];

ctrl_unlock:
	mutex_unlock(&ms912x->ctrl_lock);
	drm_dev_exit(idx);
	return ret;
}

static int ms912x_read_flash(struct ms912x_device *ms912x, u32 address,
			     void *data, size_t len)
{
	struct ms912x_flash_read_request request;
	struct usb_device *usb_dev;
	size_t offset = 0;
	u8 *dst = data;
	int idx, ret = 0;

	if (address > 0xffffff || len > 0x1000000 - address)
		return -ERANGE;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	mutex_lock(&ms912x->ctrl_lock);
	usb_dev = interface_to_usbdev(ms912x->intf);

	while (offset < len) {
		size_t chunk = min_t(size_t, len - offset, sizeof(request));
		u32 current_address = address + offset;

		memset(&request, 0, sizeof(request));
		request.type = MS912X_REQ_TYPE_READ_FLASH;
		request.addr[0] = current_address >> 16;
		request.addr[1] = current_address >> 8;
		request.addr[2] = current_address;

		ret = usb_control_msg_send(usb_dev, 0, HID_REQ_SET_REPORT,
					   USB_DIR_OUT | USB_TYPE_CLASS |
						   USB_RECIP_INTERFACE,
					   0x0300, 0, &request,
					   sizeof(request),
					   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
		if (ret)
			goto ctrl_unlock;

		ret = usb_control_msg_recv(usb_dev, 0, HID_REQ_GET_REPORT,
					   USB_DIR_IN | USB_TYPE_CLASS |
						   USB_RECIP_INTERFACE,
					   0x0300, 0, &request,
					   sizeof(request),
					   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
		if (ret)
			goto ctrl_unlock;

		memcpy(dst + offset, &request, chunk);
		offset += chunk;
	}

ctrl_unlock:
	mutex_unlock(&ms912x->ctrl_lock);
	drm_dev_exit(idx);
	return ret;
}

static int ms912x_read_chip_signature(struct ms912x_device *ms912x, u16 address,
				      u8 signature[3])
{
	unsigned int i;
	int ret;

	for (i = 0; i < 3; i++) {
		ret = ms912x_read_byte(ms912x, address + i);
		if (ret < 0)
			return ret;
		signature[i] = ret;
	}

	return 0;
}

static int ms912x_custom_timing_base(struct ms912x_device *ms912x, u32 *base)
{
	u8 signature[3];
	int ret;

	ret = ms912x_read_chip_signature(ms912x, MS913X_REG_CHIP_ID, signature);
	if (ret)
		return ret;

	if (signature[1] == MS913X_CHIP_ID_SIGNATURE_MSB &&
	    signature[2] == MS91XX_CHIP_ID_SIGNATURE_LSB) {
		*base = MS913X_CUSTOM_TIMING_BASE;
		return 1;
	}

	ret = ms912x_read_chip_signature(ms912x, MS912X_REG_CHIP_ID, signature);
	if (ret)
		return ret;

	if (signature[1] == MS912X_CHIP_ID_SIGNATURE_MSB &&
	    signature[2] == MS91XX_CHIP_ID_SIGNATURE_LSB) {
		*base = MS912X_CUSTOM_TIMING_BASE;
		return 1;
	}

	return 0;
}

static int
ms912x_decode_custom_timing(struct ms912x_custom_mode *custom_mode,
			    const struct ms912x_custom_timing_record *record)
{
	struct drm_display_mode *display_mode = &custom_mode->display_mode;
	u16 hsyncwidth = le16_to_cpu(record->hsyncwidth);
	u16 vsyncwidth = le16_to_cpu(record->vsyncwidth);
	u16 hoffset = le16_to_cpu(record->hoffset);
	u16 voffset = le16_to_cpu(record->voffset);
	u16 hactive = le16_to_cpu(record->hactive);
	u16 vactive = le16_to_cpu(record->vactive);
	u16 htotal = le16_to_cpu(record->htotal);
	u16 vtotal = le16_to_cpu(record->vtotal);
	u16 pixclk = le16_to_cpu(record->pixclk);
	u16 vfreq = le16_to_cpu(record->vfreq);
	unsigned int hblank, vblank;
	int refresh;

	if (!hactive || hactive > MS912X_MAX_WIDTH ||
	    !vactive || vactive > MS912X_MAX_HEIGHT ||
	    htotal <= hactive || vtotal <= vactive || !pixclk || !vfreq)
		return -EINVAL;

	hblank = htotal - hactive;
	vblank = vtotal - vactive;
	if (!hsyncwidth || hsyncwidth > hoffset || hoffset > hblank ||
	    !vsyncwidth || vsyncwidth > voffset || voffset > vblank)
		return -EINVAL;

	memset(display_mode, 0, sizeof(*display_mode));
	display_mode->clock = pixclk * 10;
	display_mode->hdisplay = hactive;
	display_mode->hsync_start = htotal - hoffset;
	display_mode->hsync_end = display_mode->hsync_start + hsyncwidth;
	display_mode->htotal = htotal;
	display_mode->vdisplay = vactive;
	display_mode->vsync_start = vtotal - voffset;
	display_mode->vsync_end = display_mode->vsync_start + vsyncwidth;
	display_mode->vtotal = vtotal;
	display_mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	display_mode->flags =
		record->polarity & MS912X_TIMING_POSITIVE_HSYNC ?
			DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC;
	display_mode->flags |=
		record->polarity & MS912X_TIMING_POSITIVE_VSYNC ?
			DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC;
	if (!(record->polarity & MS912X_TIMING_PROGRESSIVE))
		display_mode->flags |= DRM_MODE_FLAG_INTERLACE;

	refresh = DIV_ROUND_CLOSEST(vfreq, 100);
	if (!refresh || drm_mode_vrefresh(display_mode) != refresh)
		return -EINVAL;

	drm_mode_set_name(display_mode);
	custom_mode->mode.width = hactive;
	custom_mode->mode.height = vactive;
	custom_mode->mode.hz = refresh;
	custom_mode->mode.mode = record->vic;

	return 0;
}

int ms912x_read_custom_timing(struct ms912x_device *ms912x)
{
	struct ms912x_custom_timing_record record;
	unsigned int num_records, i;
	int error = 0;
	u8 marker[7];
	u32 base;
	int ret;

	ms912x->num_custom_modes = 0;

	ret = ms912x_custom_timing_base(ms912x, &base);
	if (ret <= 0)
		return ret;

	ret = ms912x_read_flash(ms912x, base, marker, sizeof(marker));
	if (ret)
		return ret;

	if (!memcmp(marker, "modify1", sizeof(marker)))
		num_records = 1;
	else if (!memcmp(marker, "modify2", sizeof(marker)))
		num_records = 2;
	else
		return 0;

	for (i = 0; i < num_records; i++) {
		ret = ms912x_read_flash(ms912x,
					base + MS912X_CUSTOM_TIMING_OFFSET +
						i * MS912X_CUSTOM_TIMING_STRIDE,
					&record, sizeof(record));
		if (ret) {
			error = ret;
			continue;
		}

		ret = ms912x_decode_custom_timing(
			&ms912x->custom_modes[ms912x->num_custom_modes],
			&record);
		if (ret) {
			error = ret;
			continue;
		}

		ms912x->num_custom_modes++;
	}

	return ms912x->num_custom_modes ? ms912x->num_custom_modes : error;
}

static int ms912x_write_6_bytes(struct ms912x_device *ms912x,
				u8 address, const void *data)
{
	struct ms912x_write_request request;
	struct usb_device *usb_dev;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	mutex_lock(&ms912x->ctrl_lock);

	usb_dev = interface_to_usbdev(ms912x->intf);
	request.type = MS912X_REQ_TYPE_WRITE_6_BYTES;
	request.addr = address;
	memcpy(request.data, data, sizeof(request.data));

	ret = usb_control_msg_send(usb_dev, 0, HID_REQ_SET_REPORT,
				   USB_DIR_OUT | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
	mutex_unlock(&ms912x->ctrl_lock);
	drm_dev_exit(idx);

	return ret;
}

int ms912x_power_on(struct ms912x_device *ms912x)
{
	u8 data[6];

	memset(data, 0, sizeof(data));
	data[0] = 0x01;
	data[1] = 0x02;

	return ms912x_write_6_bytes(ms912x, MS912X_CMD_POWER, data);
}

int ms912x_power_off(struct ms912x_device *ms912x)
{
	u8 data[6];

	memset(data, 0, sizeof(data));

	return ms912x_write_6_bytes(ms912x, MS912X_CMD_POWER, data);
}

int ms912x_set_resolution(struct ms912x_device *ms912x,
			  const struct ms912x_mode *mode)
{
	int ret;
	u8 data[6];
	struct ms912x_resolution_request resolution_request;
	struct ms912x_mode_request mode_request;

	int width = mode->width;
	int height = mode->height;
	int mode_num = mode->mode;

	/* Sequence obtained from Windows USB captures */
	memset(data, 0, sizeof(data));
	data[0] = 0;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_UNKNOWN2, data);
	if (ret < 0)
		return ret;

	ret = ms912x_read_byte(ms912x, MS912X_REG_MODE_SEQUENCE_0);
	if (ret < 0)
		return ret;
	ret = ms912x_read_byte(ms912x, MS912X_REG_MODE_SEQUENCE_1);
	if (ret < 0)
		return ret;
	ret = ms912x_read_byte(ms912x, MS912X_REG_MODE_SEQUENCE_2);
	if (ret < 0)
		return ret;

	memset(data, 0, sizeof(data));
	data[0] = 0x03;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_UNKNOWN1, data);
	if (ret < 0)
		return ret;

	/* Write resolution */
	resolution_request.width = cpu_to_be16(width);
	resolution_request.height = cpu_to_be16(height);
	resolution_request.pixel_format = MS912X_PIXFMT_UYVY;
	resolution_request.byte_select = MS912X_BYTE_SELECT_UYVY;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_RESOLUTION,
				   &resolution_request);
	if (ret < 0)
		return ret;

	/* Write mode */
	mode_request.mode = mode_num;
	mode_request.pixel_format = 0x01;
	mode_request.width = cpu_to_be16(width);
	mode_request.height = cpu_to_be16(height);
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_MODE, &mode_request);
	if (ret < 0)
		return ret;

	memset(data, 0, sizeof(data));
	data[0] = 1;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_UNKNOWN2, data);
	if (ret < 0)
		return ret;

	memset(data, 0, sizeof(data));
	data[0] = 1;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_OUTPUT_ENABLE, data);
	if (ret < 0)
		return ret;

	return 0;
}
