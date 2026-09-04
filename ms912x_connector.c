// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>

#include "ms912x.h"

static int ms912x_get_connector_type(struct ms912x_device *ms912x)
{
	int port_type;

	port_type = ms912x_read_byte(ms912x, MS912X_REG_VIDEO_PORT);
	if (port_type < 0) {
		drm_warn(&ms912x->drm,
			 "failed to read video port type: %d\n", port_type);
		ms912x->port_type = MS912X_VIDEO_PORT_UNKNOWN;
		return DRM_MODE_CONNECTOR_Unknown;
	}

	ms912x->port_type = port_type;

	switch (ms912x->port_type) {
	case MS912X_VIDEO_PORT_CVBS:
		return DRM_MODE_CONNECTOR_Composite;
	case MS912X_VIDEO_PORT_SVIDEO:
		return DRM_MODE_CONNECTOR_SVIDEO;
	case MS912X_VIDEO_PORT_VGA:
		return DRM_MODE_CONNECTOR_VGA;
	case MS912X_VIDEO_PORT_YPBPR:
		return DRM_MODE_CONNECTOR_Component;
	case MS912X_VIDEO_PORT_HDMI:
		return DRM_MODE_CONNECTOR_HDMIA;
	case MS912X_VIDEO_PORT_DIGITAL:
		return DRM_MODE_CONNECTOR_DPI;
	case MS912X_VIDEO_PORT_CVBS_SVIDEO:
	default:
		drm_warn(&ms912x->drm,
			 "unknown video port type: %d\n", port_type);
		ms912x->port_type = MS912X_VIDEO_PORT_UNKNOWN;
		return DRM_MODE_CONNECTOR_Unknown;
	}
}

static int
ms912x_get_encoder_type(enum ms912x_video_port port_type)
{
	switch (port_type) {
	case MS912X_VIDEO_PORT_VGA:
		return DRM_MODE_ENCODER_DAC;
	case MS912X_VIDEO_PORT_CVBS:
	case MS912X_VIDEO_PORT_SVIDEO:
	case MS912X_VIDEO_PORT_YPBPR:
	case MS912X_VIDEO_PORT_CVBS_SVIDEO:
		return DRM_MODE_ENCODER_TVDAC;
	case MS912X_VIDEO_PORT_HDMI:
		return DRM_MODE_ENCODER_TMDS;
	case MS912X_VIDEO_PORT_DIGITAL:
		return DRM_MODE_ENCODER_DPI;
	case MS912X_VIDEO_PORT_UNKNOWN:
	default:
		return DRM_MODE_ENCODER_NONE;
	}
}

static int ms912x_read_edid(void *data, u8 *buf, unsigned int block, size_t len)
{
	struct ms912x_device *ms912x = data;
	int offset = block * EDID_LENGTH;
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		u16 address = MS912X_REG_EDID_BASE + offset + i;

		ret = ms912x_read_byte(ms912x, address);
		if (ret < 0)
			return ret;
		buf[i] = ret;
	}
	return 0;
}

static int ms912x_add_cea_modes(struct drm_connector *connector,
				const u8 *vics, size_t num_vics)
{
	struct drm_display_mode *mode;
	unsigned int i;
	int count = 0;

	drm_edid_connector_update(connector, NULL);

	for (i = 0; i < num_vics; i++) {
		mode = drm_display_mode_from_cea_vic(connector->dev, vics[i]);
		if (!mode)
			continue;

		if (!count)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static int ms912x_add_cvbs_svideo_modes(struct drm_connector *connector)
{
	static const u8 vics[] = {
		2,  /* 720x480p60 */
		17, /* 720x576p50 */
	};

	return ms912x_add_cea_modes(connector, vics, ARRAY_SIZE(vics));
}

static int ms912x_add_ypbpr_modes(struct drm_connector *connector)
{
	static const u8 vics[] = {
		4,  /* 1280x720p60 */
		16, /* 1920x1080p60 */
		2,  /* 720x480p60 */
		17, /* 720x576p50 */
	};

	return ms912x_add_cea_modes(connector, vics, ARRAY_SIZE(vics));
}

static int
ms912x_add_default_hdmi_vga_modes(struct drm_connector *connector)
{
	struct drm_mode_config *mode_config = &connector->dev->mode_config;
	int count;

	drm_edid_connector_update(connector, NULL);
	count = drm_add_modes_noedid(connector, mode_config->max_width,
				     mode_config->max_height);
	if (count)
		drm_set_preferred_mode(connector, 1024, 768);

	return count;
}

static int ms912x_connector_get_modes(struct drm_connector *connector)
{
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	const struct drm_edid *edid;
	int ret;

	if (ms912x->port_type == MS912X_VIDEO_PORT_CVBS ||
	    ms912x->port_type == MS912X_VIDEO_PORT_SVIDEO ||
	    ms912x->port_type == MS912X_VIDEO_PORT_CVBS_SVIDEO ||
	    ms912x->port_type == MS912X_VIDEO_PORT_UNKNOWN)
		return ms912x_add_cvbs_svideo_modes(connector);

	if (ms912x->port_type == MS912X_VIDEO_PORT_YPBPR)
		return ms912x_add_ypbpr_modes(connector);

	edid = drm_edid_read_custom(connector, ms912x_read_edid, ms912x);
	if (!edid)
		return ms912x_add_default_hdmi_vga_modes(connector);

	ret = drm_edid_connector_update(connector, edid);
	if (ret < 0) {
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
	int status = ms912x_read_byte(ms912x, MS912X_REG_DISPLAY_STATUS);

	if (status < 0)
		return connector_status_unknown;

	return status == 1 ? connector_status_connected :
			     connector_status_disconnected;
}

static const struct drm_encoder_funcs ms912x_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

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
	int connector_type;
	int encoder_type;

	connector_type = ms912x_get_connector_type(ms912x);
	encoder_type = ms912x_get_encoder_type(ms912x->port_type);

	ret = drm_encoder_init(&ms912x->drm, &ms912x->encoder,
			       &ms912x_encoder_funcs, encoder_type, NULL);
	if (ret)
		return ret;
	ms912x->encoder.possible_crtcs = drm_crtc_mask(&ms912x->crtc);

	drm_connector_helper_add(&ms912x->connector,
				 &ms912x_connector_helper_funcs);
	ret = drm_connector_init(&ms912x->drm, &ms912x->connector,
				 &ms912x_connector_funcs, connector_type);
	if (ret)
		return ret;

	ms912x->connector.polled =
		DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	return drm_connector_attach_encoder(&ms912x->connector,
					    &ms912x->encoder);
}
