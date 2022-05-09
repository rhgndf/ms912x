
#include <uapi/linux/hid.h>

#include "ms912x.h"

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address)
{
	int ret;
	struct usb_interface *intf = ms912x->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ms912x_request *request = kzalloc(8, GFP_KERNEL);

	request->type = 0xb5;
	request->addr = cpu_to_be16(address);
	usb_control_msg(usb_dev, usb_sndctrlpipe(usb_dev, 0),
			HID_REQ_SET_REPORT,
			USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			0x0300, 0, request, 8, USB_CTRL_SET_TIMEOUT);
	ret = usb_control_msg(usb_dev, usb_rcvctrlpipe(usb_dev, 0),
			      HID_REQ_GET_REPORT,
			      USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			      0x0300, 0, request, 8, USB_CTRL_GET_TIMEOUT);

	if (ret < 0)
		return ret;
	ret = request->data[0];
	kfree(request);
	return ret;
}

static inline int ms912x_write_6_bytes(struct ms912x_device *ms912x,
				       u16 address, void *data)
{
	int ret;
	struct usb_interface *intf = ms912x->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ms912x_write_request *request = kzalloc(8, GFP_KERNEL);

	request->type = 0xa6;
	request->addr = address;
	memcpy(request->data, data, 6);

	ret = usb_control_msg(
		usb_dev, usb_sndctrlpipe(usb_dev, 0), HID_REQ_SET_REPORT,
		USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0x0300, 0,
		request, 8, USB_CTRL_SET_TIMEOUT);
	kfree(request);
	return ret;
}

int ms912x_power_on(struct ms912x_device *ms912x)
{
	int ret;
	u8 data[6];
	memset(data, 0, sizeof(data));
	data[0] = 0x01;
	data[1] = 0x02;
	ret = ms912x_write_6_bytes(ms912x, 0x07, data);

	return ret;
}

int ms912x_power_off(struct ms912x_device *ms912x)
{
	int ret;
	u8 data[6];
	memset(data, 0, sizeof(data));
	ret = ms912x_write_6_bytes(ms912x, 0x07, data);

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

	/* ??? Unknown */
	memset(data, 0, sizeof(data));
	data[0] = 0;
	ret = ms912x_write_6_bytes(ms912x, 0x04, data);
	if (ret < 0)
		return ret;

	ms912x_read_byte(ms912x, 0x30);
	ms912x_read_byte(ms912x, 0x33);
	ms912x_read_byte(ms912x, 0xc620);

	/* ??? Unknown */
	memset(data, 0, sizeof(data));
	data[0] = 0x03;
	ret = ms912x_write_6_bytes(ms912x, 0x03, data);
	if (ret < 0)
		return ret;

	/* Write resolution */
	resolution_request.width = cpu_to_be16(width);
	resolution_request.height = cpu_to_be16(height);
	resolution_request.pixel_format = cpu_to_be16(pixel_format);
	ret = ms912x_write_6_bytes(ms912x, 0x01, &resolution_request);
	if (ret < 0)
		return ret;

	/* Write mode */
	mode_request.mode = cpu_to_be16(mode_num);
	mode_request.width = cpu_to_be16(width);
	mode_request.height = cpu_to_be16(height);
	ret = ms912x_write_6_bytes(ms912x, 0x02, &mode_request);
	if (ret < 0)
		return ret;

	/* ??? Unknown */
	memset(data, 0, sizeof(data));
	data[0] = 1;
	ret = ms912x_write_6_bytes(ms912x, 0x04, data);
	if (ret < 0)
		return ret;

	/* ??? Unknown */
	memset(data, 0, sizeof(data));
	data[0] = 1;
	ret = ms912x_write_6_bytes(ms912x, 0x05, data);
	if (ret < 0)
		return ret;

	return 0;
}