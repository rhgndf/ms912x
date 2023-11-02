
#include <linux/dma-buf.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "ms912x.h"

void ms912x_free_urb(struct ms912x_device *ms912x)
{
	unsigned int i, blocks;
	struct ms912x_urb *urb_entry;
	struct usb_device *usb_dev = interface_to_usbdev(ms912x->intf);
	blocks = ms912x->num_urbs;
	for (i = 0; i < blocks; i++) {
		down(&ms912x->urb_available_list_sem);

		spin_lock_irq(&ms912x->urb_available_list_lock);
		urb_entry = list_first_entry(&ms912x->urb_available_list,
					     struct ms912x_urb, entry);
		list_del(&urb_entry->entry);
		spin_unlock_irq(&ms912x->urb_available_list_lock);

		usb_free_coherent(usb_dev, MS912X_MAX_TRANSFER_LENGTH,
				  urb_entry->urb->transfer_buffer,
				  urb_entry->urb->transfer_dma);
		usb_free_urb(urb_entry->urb);
		kfree(urb_entry);
	}
}

void ms912x_urb_completion(struct urb *urb)
{
	struct ms912x_urb *urb_entry = urb->context;
	struct ms912x_device *ms912x = urb_entry->parent;
	unsigned long flags;

	spin_lock_irqsave(&ms912x->urb_available_list_lock, flags);
	list_add_tail(&urb_entry->entry, &ms912x->urb_available_list);
	spin_unlock_irqrestore(&ms912x->urb_available_list_lock, flags);
	up(&ms912x->urb_available_list_sem);
	complete(&ms912x->urb_completion);
}

int ms912x_init_urb(struct ms912x_device *ms912x, size_t blocks)
{
	unsigned int i;
	struct ms912x_urb *urb_entry;
	struct urb *urb;
	void *urb_buf;
	struct usb_device *usb_dev = interface_to_usbdev(ms912x->intf);

	spin_lock_init(&ms912x->urb_available_list_lock);
	INIT_LIST_HEAD(&ms912x->urb_available_list);
	sema_init(&ms912x->urb_available_list_sem, 0);
	ms912x->num_urbs = 0;
	for (i = 0; i < blocks; i++) {
		urb_entry = kzalloc(sizeof(struct ms912x_urb), GFP_KERNEL);
		if (!urb_entry)
			break;
		urb_entry->parent = ms912x;

		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			kfree(urb_entry);
			break;
		}
		urb_entry->urb = urb;

		urb_buf =
			usb_alloc_coherent(usb_dev, MS912X_MAX_TRANSFER_LENGTH,
					   GFP_KERNEL, &urb->transfer_dma);

		if (!urb_buf) {
			usb_free_urb(urb);
			kfree(urb_entry);
			break;
		}

		usb_fill_bulk_urb(urb, usb_dev, usb_sndbulkpipe(usb_dev, 4),
				  urb_buf, MS912X_MAX_TRANSFER_LENGTH,
				  ms912x_urb_completion, urb_entry);
		urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		list_add_tail(&urb_entry->entry, &ms912x->urb_available_list);
		up(&ms912x->urb_available_list_sem);
		ms912x->num_urbs++;
	}
	return ms912x->num_urbs;
}

struct urb *ms912x_get_urb(struct ms912x_device *ms912x)
{
	int ret;
	struct ms912x_urb *urb_entry;

	ret = down_interruptible(&ms912x->urb_available_list_sem);
	if (ret < 0)
		return ERR_PTR(ret);

	spin_lock_irq(&ms912x->urb_available_list_lock);
	urb_entry = list_first_entry(&ms912x->urb_available_list,
				     struct ms912x_urb, entry);
	list_del_init(&urb_entry->entry);
	spin_unlock_irq(&ms912x->urb_available_list_lock);

	return urb_entry->urb;
}

static inline unsigned int ms912x_rgb_to_y(unsigned int r, unsigned int g,
					   unsigned int b)
{
	const unsigned int luma = (16 << 16) + 16763 * r + 32904 * g + 6391 * b;
	return luma >> 16;
}
static inline unsigned int ms912x_rgb_to_u(unsigned int r, unsigned int g,
					   unsigned int b)
{
	const unsigned int u = (128 << 16) - 9676 * r - 18996 * g + 28672 * b;
	return u >> 16;
}
static inline unsigned int ms912x_rgb_to_v(unsigned int r, unsigned int g,
					   unsigned int b)
{
	const unsigned int v = (128 << 16) + 28672 * r - 24009 * g - 4663 * b;
	return v >> 16;
}

static int ms912x_xrgb_to_yuv422_line(u8 *transfer_buffer, u32 *xrgb_buffer,
				      size_t len, u32* temp_buffer)
{
	unsigned int i, offset = 0;
	unsigned int pixel1, pixel2;
	unsigned int r1, g1, b1, r2, g2, b2;
	unsigned int v, y1, u, y2;
	memcpy(temp_buffer, xrgb_buffer, len * sizeof(u32));
	for (i = 0; i < len; i += 2) {
		pixel1 = temp_buffer[i];
		pixel2 = temp_buffer[i + 1];

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

void ms912x_fb_send_rect(struct drm_framebuffer *fb,
			 const struct iosys_map *map, struct drm_rect *rect)
{
	int ret, i;
	void *vaddr = map->vaddr;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_frame_update_header header;
	struct urb *urb;
	void *transfer_buffer;
	int total_length = 0;
	int transfer_blocks, transfer_length;
	/* Hardware can only update framebuffer in multiples of 16 horizontally */
	int x = ALIGN_DOWN(rect->x1, 16);
	/* Resolutions that are not a multiple of 16 like 1366*768 need to align */
	int width =
		min(ALIGN(rect->x2, 16), ALIGN_DOWN((int)fb->width, 16)) - x;
	int y1 = rect->y1;
	int y2 = min(rect->y2, (int)fb->height);
	int idx;
	u32* temp_buffer;

	drm_dev_enter(drm, &idx);

	rect->x1 = x;
	rect->x2 = x + width;

	header.header = cpu_to_be16(0xff00);
	header.x = x / 16;
	header.y = cpu_to_be16(y1);
	header.width = width / 16;
	header.height = cpu_to_be16(drm_rect_height(rect));

	transfer_buffer = vmalloc(width * drm_rect_height(rect) * 2 + 16);
	if (!transfer_buffer)
		goto dev_exit;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto free_transfer_buffer;

	memcpy(transfer_buffer, &header, 8);
	total_length += 8;

	temp_buffer = vmalloc(width * sizeof(u32));
	for (i = y1; i < y2; i++) {
		const int line_offset = fb->pitches[0] * i;
		const int byte_offset = line_offset + (x * 4);
		ms912x_xrgb_to_yuv422_line(transfer_buffer + total_length,
					   vaddr + byte_offset, width, temp_buffer);
		total_length += width * 2;
	}
	vfree(temp_buffer);

	memcpy(transfer_buffer + total_length, ms912x_end_of_buffer, 8);
	total_length += 8;

	transfer_blocks =
		DIV_ROUND_UP(total_length, MS912X_MAX_TRANSFER_LENGTH);

	reinit_completion(&ms912x->urb_completion);
	for (i = 0; i < transfer_blocks; i++) {
		/* Last block may be shorter */
		urb = ms912x_get_urb(ms912x);
		if (IS_ERR(urb))
			break;
		transfer_length = min((i + 1) * MS912X_MAX_TRANSFER_LENGTH,
				      total_length) -
				  i * MS912X_MAX_TRANSFER_LENGTH;

		memcpy(urb->transfer_buffer,
		       transfer_buffer + i * MS912X_MAX_TRANSFER_LENGTH,
		       transfer_length);
		urb->transfer_buffer_length = transfer_length;
		urb->complete = ms912x_urb_completion;
		ret = usb_submit_urb(urb, GFP_KERNEL);
		if (ret < 0) {
			ms912x_urb_completion(urb);
			break;
		}
	}
	for (i = 0; i < transfer_blocks; i++) {
		if (!wait_for_completion_timeout(&ms912x->urb_completion,
						 msecs_to_jiffies(1000))) {
			break;
		}
	}

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
free_transfer_buffer:
	vfree(transfer_buffer);
dev_exit:
	drm_dev_exit(idx);
}