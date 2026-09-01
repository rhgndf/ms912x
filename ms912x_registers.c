// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hid.h>

#include <drm/drm_drv.h>

#include "ms912x.h"

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address)
{
	struct ms912x_request request;
	struct usb_device *usb_dev;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	usb_dev = interface_to_usbdev(ms912x->intf);
	memset(&request, 0, sizeof(request));
	request.type = 0xb5;
	request.addr = cpu_to_be16(address);

	ret = usb_control_msg_send(usb_dev, 0, HID_REQ_SET_REPORT,
				   USB_DIR_OUT | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
	if (ret)
		goto dev_exit;

	ret = usb_control_msg_recv(usb_dev, 0, HID_REQ_GET_REPORT,
				   USB_DIR_IN | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_GET_TIMEOUT, GFP_KERNEL);
	if (!ret)
		ret = request.data[0];

dev_exit:
	drm_dev_exit(idx);
	return ret;
}

static inline int ms912x_write_6_bytes(struct ms912x_device *ms912x,
				       u8 address, const void *data)
{
	struct ms912x_write_request request;
	struct usb_device *usb_dev;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	usb_dev = interface_to_usbdev(ms912x->intf);
	request.type = 0xa6;
	request.addr = address;
	memcpy(request.data, data, sizeof(request.data));

	ret = usb_control_msg_send(usb_dev, 0, HID_REQ_SET_REPORT,
				   USB_DIR_OUT | USB_TYPE_CLASS |
					   USB_RECIP_INTERFACE,
				   0x0300, 0, &request, sizeof(request),
				   USB_CTRL_SET_TIMEOUT, GFP_KERNEL);
	drm_dev_exit(idx);

	return ret;
}

int ms912x_power_on(struct ms912x_device *ms912x)
{
	int ret;
	u8 data[6];

	memset(data, 0, sizeof(data));
	data[0] = 0x01;
	data[1] = 0x02;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_POWER, data);

	return ret;
}

int ms912x_power_off(struct ms912x_device *ms912x)
{
	int ret;
	u8 data[6];

	memset(data, 0, sizeof(data));
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_POWER, data);

	return ret;
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
	int pixel_format = mode->pix_fmt;
	int mode_num = mode->mode;

	/* Sequence obtained from Windows USB captures*/
	memset(data, 0, sizeof(data));
	data[0] = 0;
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_UNKNOWN2, data);
	if (ret < 0)
		return ret;

	ret = ms912x_read_byte(ms912x, 0x30);
	if (ret < 0)
		return ret;
	ret = ms912x_read_byte(ms912x, 0x33);
	if (ret < 0)
		return ret;
	ret = ms912x_read_byte(ms912x, 0xc620);
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
	resolution_request.pixel_format = cpu_to_be16(pixel_format);
	ret = ms912x_write_6_bytes(ms912x, MS912X_CMD_RESOLUTION,
				   &resolution_request);
	if (ret < 0)
		return ret;

	/* Write mode */
	mode_request.mode = cpu_to_be16(mode_num);
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
