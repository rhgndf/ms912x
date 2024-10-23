
#include <linux/dma-buf.h>
#include <linux/vmalloc.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "ms912x.h"

static void ms912x_request_timeout(struct timer_list *t)
{
	struct ms912x_usb_request *request = from_timer(request, t, timer);
	usb_sg_cancel(&request->sgr);
}

static void ms912x_request_work(struct work_struct *work)
{
	struct ms912x_usb_request *request =
		container_of(work, struct ms912x_usb_request, work);
	struct ms912x_device *ms912x = request->ms912x;
	struct usb_device *usbdev = interface_to_usbdev(ms912x->intf);
	struct usb_sg_request *sgr = &request->sgr;
	struct sg_table *transfer_sgt = &request->transfer_sgt;

	timer_setup(&request->timer, ms912x_request_timeout, 0);
	usb_sg_init(sgr, usbdev, usb_sndbulkpipe(usbdev, 0x04), 0,
		    transfer_sgt->sgl, transfer_sgt->nents,
		    request->transfer_len, GFP_KERNEL);
	mod_timer(&request->timer, jiffies + msecs_to_jiffies(5000));
	usb_sg_wait(sgr);
	del_timer_sync(&request->timer);
	complete(&request->done);
}

void ms912x_free_request(struct ms912x_usb_request *request)
{
	if (!request->transfer_buffer)
		return;
	sg_free_table(&request->transfer_sgt);
	vfree(request->transfer_buffer);
	request->transfer_buffer = NULL;
	request->alloc_len = 0;
}

int ms912x_init_request(struct ms912x_device *ms912x,
			struct ms912x_usb_request *request, size_t len)
{
	int ret, i;
	unsigned int num_pages;
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

	request->alloc_len = len;
	request->transfer_buffer = data;
	request->ms912x = ms912x;

	init_completion(&request->done);
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

static int ms912x_xrgb_to_yuv422_line(u8 *transfer_buffer,
				      struct iosys_map *xrgb_buffer,
				      size_t offset, size_t width,
				      u32 *temp_buffer)
{
	unsigned int i, dst_offset = 0;
	unsigned int pixel1, pixel2;
	unsigned int r1, g1, b1, r2, g2, b2;
	unsigned int v, y1, u, y2;
	iosys_map_memcpy_from(temp_buffer, xrgb_buffer, offset, width * 4);
	for (i = 0; i < width; i += 2) {
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

		transfer_buffer[dst_offset++] = u;
		transfer_buffer[dst_offset++] = y1;
		transfer_buffer[dst_offset++] = v;
		transfer_buffer[dst_offset++] = y2;
	}
	return offset;
}

static const u8 ms912x_end_of_buffer[8] = { 0xff, 0xc0, 0x00, 0x00,
					    0x00, 0x00, 0x00, 0x00 };

static int ms912x_fb_xrgb8888_to_yuv422(void *dst, const struct iosys_map *src,
					struct drm_framebuffer *fb,
					struct drm_rect *rect)
{
	struct ms912x_frame_update_header *header =
		(struct ms912x_frame_update_header *)dst;
	struct iosys_map fb_map;
	int i, x, y1, y2, width;
	void *temp_buffer;

	y1 = rect->y1;
	y2 = min((unsigned int)rect->y2, fb->height);
	x = rect->x1;
	width = drm_rect_width(rect);

	temp_buffer = kmalloc(width * 4, GFP_KERNEL);
	if (!temp_buffer)
		return -ENOMEM;

	header->header = cpu_to_be16(0xff00);
	header->x = x / 16;
	header->y = cpu_to_be16(y1);
	header->width = width / 16;
	header->height = cpu_to_be16(drm_rect_height(rect));
	dst += sizeof(*header);

	fb_map = IOSYS_MAP_INIT_OFFSET(src, y1 * fb->pitches[0]);
	for (i = y1; i < y2; i++) {
		ms912x_xrgb_to_yuv422_line(dst, &fb_map, x * 4, width, temp_buffer);
		iosys_map_incr(&fb_map, fb->pitches[0]);
		dst += width * 2;
	}

	kfree(temp_buffer);
	memcpy(dst, ms912x_end_of_buffer, sizeof(ms912x_end_of_buffer));
	return 0;
}

int ms912x_fb_send_rect(struct drm_framebuffer *fb, const struct iosys_map *map,
			struct drm_rect *rect)
{
	int ret = 0, idx;
	struct ms912x_device *ms912x = to_ms912x(fb->dev);
	struct drm_device *drm = &ms912x->drm;
	struct ms912x_usb_request *prev_request, *current_request;
	int x, width;

	/* Seems like hardware can only update framebuffer 
	 * in multiples of 16 horizontally
	 */
	x = ALIGN_DOWN(rect->x1, 16);
	/* Resolutions that are not a multiple of 16 like 1366*768 
	 * need to be aligned
	 */
	width = min(ALIGN(rect->x2, 16), ALIGN_DOWN((int)fb->width, 16)) - x;
	rect->x1 = x;
	rect->x2 = x + width;
	current_request = &ms912x->requests[ms912x->current_request];
	prev_request = &ms912x->requests[1 - ms912x->current_request];

	drm_dev_enter(drm, &idx);

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto dev_exit;

	ret = ms912x_fb_xrgb8888_to_yuv422(current_request->transfer_buffer,
					   map, fb, rect);
	
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret < 0)
		goto dev_exit;

	/* Sending frames too fast, drop it */
	if (!wait_for_completion_timeout(&prev_request->done,
					 msecs_to_jiffies(10))) {

		ret = -ETIMEDOUT;
		goto dev_exit;
	}

	current_request->transfer_len = width * 2 * drm_rect_height(rect) + 16;
	queue_work(system_long_wq, &current_request->work);
	ms912x->current_request = 1 - ms912x->current_request;
dev_exit:
	drm_dev_exit(idx);
	return ret;
}
