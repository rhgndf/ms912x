#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "ms912x.h"

static int ms912x_read_edid_block(struct ms912x_device *ms912x, u8 *buf,
				  unsigned int offset, size_t len)
{
	const u16 base = 0xc000 + offset;
	for (size_t i = 0; i < len; i++) {
		u16 address = base + i;
		int ret = ms912x_read_byte(ms912x, address);
		if (ret < 0)
			return ret;
		buf[i] = ret;
	}
	return 0;
}

static int ms912x_read_edid(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct ms912x_device *ms912x = data;
	const int offset = block * EDID_LENGTH;
	int ret = ms912x_read_edid_block(ms912x, buf, offset, len);
	if (ret < 0)
		pr_err("ms912x: failed to read EDID block %u\n", block);
	return ret;
}

static void ms912x_add_fallback_mode(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	mode->clock = 65000;
	mode->hdisplay = 1024;
	mode->hsync_start = 1048;
	mode->hsync_end = 1184;
	mode->htotal = 1344;
	mode->vdisplay = 768;
	mode->vsync_start = 771;
	mode->vsync_end = 777;
	mode->vtotal = 806;
	mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;

	drm_mode_probed_add(connector, mode);
}

static int ms912x_connector_get_modes(struct drm_connector *connector)
{
	int ret = 0;
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	const struct drm_edid *edid;

	edid = drm_edid_read_custom(connector, ms912x_read_edid, ms912x);
	if (!edid) {
		pr_warn("ms912x: EDID not found, falling back to default mode\n");
		ms912x_add_fallback_mode(connector);
		return 1;
	}

	ret = drm_edid_connector_update(connector, edid);
	if (ret < 0) {
		pr_err("ms912x: failed to update EDID connector\n");
		ret = 0;
		goto edid_free;
	}

	ret = drm_edid_connector_add_modes(connector);

edid_free:
	drm_edid_free(edid);
	return ret;
}

static enum drm_connector_status ms912x_detect(struct drm_connector *connector,
					       bool force)
{
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	int status = ms912x_read_byte(ms912x, 0x32);

	if (status < 0) {
		pr_err("ms912x: failed to detect HDMI status\n");
		return connector_status_unknown;
	}

	return (status == 1) ? connector_status_connected :
			       connector_status_disconnected;
}

static const struct drm_connector_helper_funcs ms912x_connector_helper_funcs = {
	.get_modes = ms912x_connector_get_modes,
};

static const struct drm_connector_funcs ms912x_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.detect = ms912x_detect,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int ms912x_connector_init(struct ms912x_device *ms912x)
{
	int ret;

	drm_connector_helper_add(&ms912x->connector,
				 &ms912x_connector_helper_funcs);

	ret = drm_connector_init(&ms912x->drm, &ms912x->connector,
				 &ms912x_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		pr_err("ms912x: failed to initialize connector\n");
		return ret;
	}

	ms912x->connector.polled =
		DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	return 0;
}
