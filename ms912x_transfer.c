// SPDX-License-Identifier: GPL-2.0-only

#include <asm/byteorder.h>
#include <linux/align.h>
#include <linux/container_of.h>
#include <linux/dma-direction.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_print.h>

#include "ms912x.h"

static void ms912x_request_timeout(struct timer_list *t)
{
	struct ms912x_usb_request *request = timer_container_of(request, t, timer);

	usb_sg_cancel(&request->sgr);
}

static const u8 ms912x_end_of_buffer[] = { 0xff, 0xc0, 0x00, 0x00,
				    0x00, 0x00, 0x00, 0x00 };

void ms912x_clear_rect(struct drm_rect *rect)
{
	rect->x1 = INT_MAX;
	rect->y1 = INT_MAX;
	rect->x2 = 0;
	rect->y2 = 0;
}

static void ms912x_xrgb_to_yuv422_line(u8 *transfer_buffer,
				       const __le32 *temp_buffer, size_t width);

/* Converts rect from the shadow buffer into a frame update. Returns its size. */
static size_t ms912x_fill_transfer(void *dst, const void *shadow,
				   const struct drm_rect *rect)
{
	struct ms912x_frame_update_header *header = dst;
	int y, width = drm_rect_width(rect), height = drm_rect_height(rect);
	u32 position, dimensions;

	header->marker = cpu_to_be16(0xff00);
	position = ((rect->x1 & 0xfff) << 12) | (rect->y1 & 0xfff);
	dimensions = ((width & 0xfff) << 12) | (height & 0xfff);
	put_unaligned_be24(position, header->position);
	put_unaligned_be24(dimensions, header->dimensions);
	dst += sizeof(*header);

	for (y = rect->y1; y < rect->y2; y++) {
		ms912x_xrgb_to_yuv422_line(dst, shadow + y * MS912X_SHADOW_PITCH +
					   rect->x1 * 4, width);
		dst += width * 2;
	}

	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return width * 2 * height + MS912X_FRAME_OVERHEAD;
}

static int ms912x_send_request(struct ms912x_usb_request *request)
{
	struct ms912x_device *ms912x = request->ms912x;
	struct usb_device *usbdev = interface_to_usbdev(ms912x->intf);
	struct usb_sg_request *sgr = &request->sgr;
	struct sg_table *transfer_sgt = &request->transfer_sgt;
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		return -ENODEV;

	ret = usb_sg_init(sgr, usbdev, ms912x->bulk_pipe, 0, transfer_sgt->sgl,
			  transfer_sgt->nents, request->transfer_len,
			  GFP_KERNEL);
	if (ret)
		goto dev_exit;

	mod_timer(&request->timer, jiffies + msecs_to_jiffies(5000));
	usb_sg_wait(sgr);

	if (!timer_delete_sync(&request->timer))
		ret = -ETIMEDOUT;
	else if (sgr->status < 0)
		ret = sgr->status;
	else if (sgr->bytes != request->transfer_len)
		ret = -EIO;

dev_exit:
	drm_dev_exit(idx);
	return ret;
}

/*
 * Sends damage accumulated in the shadow buffer until there is none left.
 * Damage arriving during a transfer is merged and sent by the next loop, so
 * nothing is dropped and the compositor never waits on USB.
 */
static void ms912x_request_work(struct work_struct *work)
{
	struct ms912x_usb_request *request =
		container_of(work, struct ms912x_usb_request, work);
	struct ms912x_device *ms912x = request->ms912x;
	struct drm_rect rect;
	int ret;

	for (;;) {
		mutex_lock(&ms912x->shadow_lock);
		if (drm_rect_width(&ms912x->dirty_rect) <= 0 ||
		    drm_rect_height(&ms912x->dirty_rect) <= 0) {
			mutex_unlock(&ms912x->shadow_lock);
			return;
		}

		/*
		 * The device double buffers, so also resend the last rect.
		 * UYVY stores pixels in pairs, so expand to complete pairs.
		 */
		rect.x1 = ALIGN_DOWN(min(ms912x->dirty_rect.x1,
					 ms912x->sent_rect.x1), 2);
		rect.y1 = min(ms912x->dirty_rect.y1, ms912x->sent_rect.y1);
		rect.x2 = min_t(int, ALIGN(max(ms912x->dirty_rect.x2,
					       ms912x->sent_rect.x2), 2),
				ms912x->fb_width);
		rect.y2 = min(max(ms912x->dirty_rect.y2, ms912x->sent_rect.y2),
			      ms912x->fb_height);
		ms912x->sent_rect = ms912x->dirty_rect;
		ms912x_clear_rect(&ms912x->dirty_rect);

		request->transfer_len = ms912x_fill_transfer(
			request->transfer_buffer, ms912x->shadow, &rect);
		mutex_unlock(&ms912x->shadow_lock);

		ret = ms912x_send_request(request);
		if (ret < 0 && ret != -ENODEV)
			drm_err_ratelimited(&ms912x->drm,
					    "failed to send framebuffer: %d\n",
					    ret);
	}
}

void ms912x_free_request(struct ms912x_usb_request *request)
{
	if (!request->transfer_buffer)
		return;

	timer_shutdown_sync(&request->timer);
	sg_free_table(&request->transfer_sgt);
	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
}

int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len)
{
	int ret;
	unsigned int i, num_pages;
	void *data;
	struct page **pages;
	void *ptr;

	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	num_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		ret = -ENOMEM;
		goto err_vfree;
	}

	for (i = 0, ptr = data; i < num_pages; i++, ptr += PAGE_SIZE)
		pages[i] = vmalloc_to_page(ptr);
	ret = sg_alloc_table_from_pages(&request->transfer_sgt, pages,
					num_pages, 0, len, GFP_KERNEL);
	kfree(pages);
	if (ret)
		goto err_vfree;

	request->transfer_buffer = data;
	request->ms912x = ms912x;

	timer_setup(&request->timer, ms912x_request_timeout, 0);
	INIT_WORK(&request->work, ms912x_request_work);
	return 0;

err_vfree:
	vfree(data);
	return ret;
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

static void ms912x_xrgb_to_yuv422_line(u8 *transfer_buffer,
				       const __le32 *temp_buffer, size_t width)
{
	unsigned int i, dst_offset = 0;
	unsigned int pixel1, pixel2;
	unsigned int r1, g1, b1, r2, g2, b2;
	unsigned int v, y1, u, y2;

	for (i = 0; i < width; i += 2) {
		pixel1 = le32_to_cpup(&temp_buffer[i]);
		pixel2 = le32_to_cpup(&temp_buffer[i + 1]);

		r1 = (pixel1 >> 16) & 0xff;
		g1 = (pixel1 >> 8) & 0xff;
		b1 = pixel1 & 0xff;
		r2 = (pixel2 >> 16) & 0xff;
		g2 = (pixel2 >> 8) & 0xff;
		b2 = pixel2 & 0xff;

		y1 = ms912x_rgb_to_y(r1, g1, b1);
		y2 = ms912x_rgb_to_y(r2, g2, b2);

		v = (ms912x_rgb_to_v(r1, g1, b1) +
		     ms912x_rgb_to_v(r2, g2, b2)) /
		    2;
		u = (ms912x_rgb_to_u(r1, g1, b1) +
		     ms912x_rgb_to_u(r2, g2, b2)) /
		    2;

		transfer_buffer[dst_offset++] = u;
		transfer_buffer[dst_offset++] = y1;
		transfer_buffer[dst_offset++] = v;
		transfer_buffer[dst_offset++] = y2;
	}
}

/*
 * Copies damage into the shadow buffer and marks what really changed as dirty.
 * Clients often report the whole window as damaged for a few changed pixels,
 * and every byte counts on USB 2.
 */
int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_format_conv_state *fmtcnv_state,
			struct drm_rect *rect)
{
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_rect *dirty = &ms912x->dirty_rect;
	struct drm_rect changed;
	struct iosys_map fb_map;
	__le32 *line, *old;
	int ret, y, l, r, width;

	rect->x2 = min_t(int, rect->x2, fb->width);
	rect->y2 = min_t(int, rect->y2, fb->height);
	width = drm_rect_width(rect);
	if (width <= 0)
		return 0;

	line = drm_format_conv_state_reserve(fmtcnv_state,
					     width * sizeof(*line), GFP_KERNEL);
	if (!line)
		return -ENOMEM;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		return ret;

	ms912x_clear_rect(&changed);
	mutex_lock(&ms912x->shadow_lock);
	fb_map = IOSYS_MAP_INIT_OFFSET(map, rect->y1 * fb->pitches[0]);
	for (y = rect->y1; y < rect->y2; y++) {
		iosys_map_memcpy_from(line, &fb_map, rect->x1 * 4, width * 4);
		iosys_map_incr(&fb_map, fb->pitches[0]);
		old = ms912x->shadow + y * MS912X_SHADOW_PITCH + rect->x1 * 4;

		/* After a mode set the device content is unknown, send all. */
		if (ms912x->shadow_valid) {
			if (!memcmp(line, old, width * 4))
				continue;
			for (l = 0; line[l] == old[l]; l++)
				;
			for (r = width; line[r - 1] == old[r - 1]; r--)
				;
		} else {
			l = 0;
			r = width;
		}

		memcpy(old + l, line + l, (r - l) * 4);
		changed.x1 = min(changed.x1, rect->x1 + l);
		changed.x2 = max(changed.x2, rect->x1 + r);
		changed.y1 = min(changed.y1, y);
		changed.y2 = y + 1;
	}
	ms912x->shadow_valid = true;

	dirty->x1 = min(dirty->x1, changed.x1);
	dirty->y1 = min(dirty->y1, changed.y1);
	dirty->x2 = max(dirty->x2, changed.x2);
	dirty->y2 = max(dirty->y2, changed.y2);
	ms912x->fb_width = fb->width;
	ms912x->fb_height = fb->height;
	mutex_unlock(&ms912x->shadow_lock);

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (changed.y2)
		queue_work(ms912x->workqueue, &ms912x->request.work);
	return 0;
}
