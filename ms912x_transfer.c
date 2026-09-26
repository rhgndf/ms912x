// SPDX-License-Identifier: GPL-2.0-only

#include <asm/byteorder.h>
#include <linux/align.h>
#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/dma-direction.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>
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

#define MS912X_BULK_CHUNK_LEN	(64 * 1024u)

static int ms912x_send_buffer(struct ms912x_device *ms912x,
			      struct usb_device *usbdev,
			      u8 *buf, size_t len)
{
	unsigned int offset = 0;
	unsigned int remaining = len;
	u8 *chunk;
	int ret = 0;

	/* The 912C firmware silently drops whole multi-MB bulk transfers
	 * even when the USB stack reports success. The official driver
	 * splits every frame into 64KB chunks; do the same via a bounce
	 * buffer (the transfer buffer is vmalloc'd, not DMA-suitable).
	 */
	chunk = kmalloc(MS912X_BULK_CHUNK_LEN, GFP_KERNEL);
	if (!chunk)
		return -ENOMEM;

	while (remaining > 0) {
		unsigned int chunk_len = min(remaining, MS912X_BULK_CHUNK_LEN);
		int actual = 0;

		memcpy(chunk, buf + offset, chunk_len);
		ret = usb_bulk_msg(usbdev, ms912x->bulk_pipe, chunk,
				   chunk_len, &actual, 2000);
		if (ret) {
			drm_err_ratelimited(&ms912x->drm,
					    "bulk chunk failed: %d (off %u len %u)\n",
					    ret, offset, chunk_len);
			break;
		}
		if (actual != (int)chunk_len) {
			ret = -EIO;
			break;
		}
		offset += chunk_len;
		remaining -= chunk_len;
	}
	kfree(chunk);

	if (!ret) {
		/* Every frame ends with a zero-length packet. */
		int actual = 0;
		u8 dummy = 0;

		ret = usb_bulk_msg(usbdev, ms912x->bulk_pipe, &dummy, 0,
				   &actual, 2000);
		if (ret)
			drm_err_ratelimited(&ms912x->drm,
					    "failed to send zero packet: %d\n",
					    ret);
	}

	return ret;
}

static void ms912x_request_work(struct work_struct *work)
{
	struct ms912x_usb_request *request =
		container_of(work, struct ms912x_usb_request, work);
	struct ms912x_device *ms912x = request->ms912x;
	struct usb_device *usbdev = interface_to_usbdev(ms912x->intf);
	int idx, ret;

	if (!drm_dev_enter(&ms912x->drm, &idx)) {
		ret = -ENODEV;
		goto complete;
	}

	ret = ms912x_send_buffer(ms912x, usbdev, request->transfer_buffer,
				 request->transfer_len);

	mutex_lock(&ms912x->update_lock);
	ms912x->last_send = jiffies;
	if (!ret) {
		ms912x->has_frame = true;
		ms912x->last_request = (int)(request - ms912x->requests);
		if (ms912x->screen_muted) {
			/* First frame transferred: unmute the screen. The 913x
			 * firmware keeps the panel black without this.
			 * Keep the flag on failure so the next frame retries.
			 */
			if (!ms912x_screen_enable(ms912x, 1))
				ms912x->screen_muted = false;
		}
	}
	mutex_unlock(&ms912x->update_lock);

	drm_dev_exit(idx);
complete:
	if (ret < 0 && ret != -ENODEV)
		drm_err_ratelimited(&ms912x->drm,
				    "failed to send framebuffer: %d\n", ret);
	complete(&request->done);
}

void ms912x_free_request(struct ms912x_usb_request *request)
{
	if (!request->transfer_buffer)
		return;

	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
}

int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len)
{
	void *data;

	data = vmalloc_32(len);
	if (!data)
		return -ENOMEM;

	request->transfer_buffer = data;
	request->ms912x = ms912x;

	init_completion(&request->done);
	complete(&request->done);
	INIT_WORK(&request->work, ms912x_request_work);
	return 0;
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
				       const struct iosys_map *xrgb_buffer,
				       size_t offset, size_t width,
				       __le32 *temp_buffer)
{
	unsigned int i, dst_offset = 0;
	unsigned int pixel1, pixel2;
	unsigned int r1, g1, b1, r2, g2, b2;
	unsigned int v, y1, u, y2;

	iosys_map_memcpy_from(temp_buffer, xrgb_buffer, offset, width * 4);
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

static const u8 ms912x_end_of_buffer[] = { 0xff, 0xc0, 0x00, 0x00,
				    0x00, 0x00, 0x00, 0x00 };

static int ms912x_fb_xrgb8888_to_yuv422(void *dst,
					const struct iosys_map *src,
					struct drm_framebuffer *fb,
					const struct drm_rect *rect,
					struct drm_format_conv_state *fmtcnv_state)
{
	struct ms912x_frame_update_header *header = dst;
	struct iosys_map fb_map;
	u32 position, dimensions;
	int i, x, y1, y2, width;
	__le32 *temp_buffer;

	y1 = rect->y1;
	y2 = min_t(unsigned int, rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	temp_buffer = drm_format_conv_state_reserve(fmtcnv_state,
						    width * sizeof(*temp_buffer),
						    GFP_KERNEL);
	if (!temp_buffer)
		return -ENOMEM;

	header->marker = cpu_to_be16(0xff00);
	position = ((x & 0xfff) << 12) | (y1 & 0xfff);
	dimensions = ((width & 0xfff) << 12) |
		     (drm_rect_height(rect) & 0xfff);
	put_unaligned_be24(position, header->position);
	put_unaligned_be24(dimensions, header->dimensions);
	dst += sizeof(*header);

	fb_map = IOSYS_MAP_INIT_OFFSET(src, y1 * fb->pitches[0]);
	for (i = y1; i < y2; i++) {
		ms912x_xrgb_to_yuv422_line(dst, &fb_map, x * 4, width,
					   temp_buffer);
		iosys_map_incr(&fb_map, fb->pitches[0]);
		dst += width * 2;
	}

	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return 0;
}

int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_format_conv_state *fmtcnv_state,
			struct drm_rect *rect)
{
	int ret = 0, idx;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_usb_request *current_request;
	int x, width;

	/* UYVY stores pixels in pairs. Expand damage to a complete pair. */
	x = ALIGN_DOWN(rect->x1, 2);
	width = min_t(int, ALIGN(rect->x2, 2), fb->width) - x;
	rect->x1 = x;
	rect->x2 = x + width;
	current_request = &ms912x->requests[ms912x->current_request];

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	mutex_lock(&ms912x->update_lock);

	/* Transfer buffer still in use, drop this frame. */
	if (!wait_for_completion_timeout(&current_request->done,
					 msecs_to_jiffies(10))) {
		ret = -ETIMEDOUT;
		goto unlock;
	}

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto request_complete;

	ret = ms912x_fb_xrgb8888_to_yuv422(current_request->transfer_buffer,
					   map, fb, rect, fmtcnv_state);

	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto request_complete;

	current_request->transfer_len =
		width * 2 * drm_rect_height(rect) + MS912X_FRAME_OVERHEAD;
	queue_work(ms912x->workqueue, &current_request->work);
	ms912x->current_request = 1 - ms912x->current_request;
	goto unlock;

request_complete:
	complete(&current_request->done);
unlock:
	mutex_unlock(&ms912x->update_lock);
	drm_dev_exit(idx);
	return ret;
}

void ms912x_idle_work(struct work_struct *work)
{
	struct ms912x_device *ms912x =
		container_of(work, struct ms912x_device, idle_work.work);
	struct ms912x_usb_request *request;
	int idx;

	if (!drm_dev_enter(&ms912x->drm, &idx))
		goto reschedule;

	mutex_lock(&ms912x->update_lock);
	if (ms912x->idle_refresh_ms && !ms912x->screen_muted &&
	    ms912x->has_frame &&
	    time_after_eq(jiffies,
			  ms912x->last_send +
			  msecs_to_jiffies(ms912x->idle_refresh_ms))) {
		/* Resend the last completed frame to keep the panel lit. */
		request = &ms912x->requests[ms912x->last_request];
		if (completion_done(&request->done)) {
			reinit_completion(&request->done);
			if (!queue_work(ms912x->workqueue, &request->work))
				complete(&request->done);
			else
				ms912x->last_send = jiffies;
		}
	}
	mutex_unlock(&ms912x->update_lock);
	drm_dev_exit(idx);

reschedule:
	/* Always re-arm, even when disabled: a no-op wakeup every 2.5 s
	 * keeps the loop alive so re-enabling the interval through
	 * debugfs takes effect without waiting for a modeset.
	 */
	schedule_delayed_work(&ms912x->idle_work,
			      msecs_to_jiffies(ms912x->idle_refresh_ms ?
					       ms912x->idle_refresh_ms : 2500));
}
