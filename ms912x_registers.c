#include <uapi/linux/hid.h>
#include "ms912x.h"

int ms912x_read_byte(struct ms912x_device *ms912x, u16 address)
{
	int ret;
	struct usb_interface *intf = ms912x->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ms912x_request *request = kzalloc(8, GFP_KERNEL);

	if (!request)
		return -ENOMEM;

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

	ret = (ret > 0) ? request->data[0] : (ret == 0) ? -EIO : ret;

	kfree(request);
	return ret;
}

static inline int ms912x_write_6_bytes(struct ms912x_device *ms912x,
				       u16 address, void *data)
{
	struct usb_interface *intf = ms912x->intf;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct ms912x_write_request *request =
		kzalloc(sizeof(struct ms912x_write_request), GFP_KERNEL);

	if (!request)
		return -ENOMEM;

	request->type = 0xa6;
	request->addr = address;
	memcpy(request->data, data, 6);

	int ret = usb_control_msg(
		usb_dev, usb_sndctrlpipe(usb_dev, 0), HID_REQ_SET_REPORT,
		USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0x0300, 0,
		request, sizeof(*request), USB_CTRL_SET_TIMEOUT);

	kfree(request);
	return ret;
}

int ms912x_power_on(struct ms912x_device *ms912x)
{
	u8 data[6] = { 0x01, 0x02 };
	return ms912x_write_6_bytes(ms912x, 0x07, data);
}

int ms912x_power_off(struct ms912x_device *ms912x)
{
	u8 data[6] = { 0 };
	return ms912x_write_6_bytes(ms912x, 0x07, data);
}

int ms912x_set_resolution(struct ms912x_device *ms912x,
			  const struct ms912x_mode *mode)
{
	u8 data[6] = { 0 };
	int ret;

	ret = ms912x_write_6_bytes(ms912x, 0x04, data);
	if (ret < 0)
		return ret;

	ms912x_read_byte(ms912x, 0x30);
	ms912x_read_byte(ms912x, 0x33);
	ms912x_read_byte(ms912x, 0xc620);

	data[0] = 0x03;
	ret = ms912x_write_6_bytes(ms912x, 0x03, data);
	if (ret < 0)
		return ret;

	struct ms912x_resolution_request resolution_request = {
		.width = cpu_to_be16(mode->width),
		.height = cpu_to_be16(mode->height),
		.pixel_format = cpu_to_be16(mode->pix_fmt)
	};
	ret = ms912x_write_6_bytes(ms912x, 0x01, &resolution_request);
	if (ret < 0)
		return ret;

	struct ms912x_mode_request mode_request = {
		.mode = cpu_to_be16(mode->mode),
		.width = cpu_to_be16(mode->width),
		.height = cpu_to_be16(mode->height)
	};
	ret = ms912x_write_6_bytes(ms912x, 0x02, &mode_request);
	if (ret < 0)
		return ret;

	data[0] = 1;
	ret = ms912x_write_6_bytes(ms912x, 0x04, data);
	if (ret < 0)
		return ret;

	/* Same data reused here */
	ret = ms912x_write_6_bytes(ms912x, 0x05, data);
	return ret;
}
