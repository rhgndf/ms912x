// SPDX-License-Identifier: GPL-2.0-only

#include <linux/array_size.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modes.h>
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
		return DRM_MODE_CONNECTOR_9PinDIN;
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
	unsigned int offset;
	u16 address;

	if (len > EDID_LENGTH ||
	    block > (MS912X_XDATA_ADDR_MAX - MS912X_REG_EDID_BASE) /
		    EDID_LENGTH)
		return -EINVAL;

	offset = block * EDID_LENGTH;
	address = MS912X_REG_EDID_BASE + offset;

	return ms912x_read_bytes(ms912x, address, buf, len);
}

#define MS912X_HPD_POLL_MS	500
#define MS912X_HPD_SETTLE_MS	200
#define MS912X_EDID_RETRIES	3
#define MS912X_EDID_RETRY_MS	50

static int ms912x_read_hpd_status(struct ms912x_device *ms912x)
{
	int ret = ms912x_read_byte(ms912x, MS912X_REG_DISPLAY_STATUS);

	if (ret < 0)
		return ret;

	return ret == 1;
}

/*
 * Poll HPD status in our own worker.  On connect, read the EDID before
 * reporting the connection, so the userspace probe never has to wait for
 * the slow HID bus.  On disconnect, EDID can be dropped immediately.
 */
void ms912x_hpd_work(struct work_struct *work)
{
	struct ms912x_device *ms912x =
		container_of(to_delayed_work(work), struct ms912x_device,
			     hpd_work);
	struct drm_connector *connector = &ms912x->connector;
	enum drm_connector_status old_status, new_status;
	enum ms912x_edid_state edid_state;
	const struct drm_edid *edid = NULL;
	bool do_read, enabled;
	int raw;

	if (drm_dev_is_unplugged(&ms912x->drm))
		return;

	raw = ms912x_read_hpd_status(ms912x);
	if (raw < 0)
		goto reschedule;

	new_status = raw ? connector_status_connected :
			   connector_status_disconnected;

	mutex_lock(&ms912x->hpd_lock);
	old_status = ms912x->hpd_status;
	edid_state = ms912x->edid_state;
	enabled = ms912x->hpd_enabled;
	mutex_unlock(&ms912x->hpd_lock);
	if (!enabled)
		return;

	/* Ignore a single-sample HPD-status glitch.  The Windows driver
	 * also waits 200 ms after HPD before reading EDID.
	 */
	if (new_status != old_status) {
		msleep(MS912X_HPD_SETTLE_MS);

		raw = ms912x_read_hpd_status(ms912x);
		if (raw < 0)
			goto reschedule;

		if ((raw ? connector_status_connected :
			   connector_status_disconnected) != new_status)
			goto reschedule;
	}

	do_read = new_status == connector_status_connected &&
		  (new_status != old_status ||
		   edid_state == MS912X_EDID_NONE);

	mutex_lock(&ms912x->hpd_lock);
	enabled = ms912x->hpd_enabled;
	mutex_unlock(&ms912x->hpd_lock);
	if (!enabled)
		return;

	if (do_read) {
		int retry;

		for (retry = 0; retry < MS912X_EDID_RETRIES; retry++) {
			edid = drm_edid_read_custom(connector, ms912x_read_edid,
						    ms912x);
			if (edid)
				break;
			if (retry + 1 < MS912X_EDID_RETRIES)
				msleep(MS912X_EDID_RETRY_MS);
		}

		/* The monitor may have disappeared while we were reading. */
		raw = ms912x_read_hpd_status(ms912x);
		if (raw < 0) {
			drm_edid_free(edid);
			goto reschedule;
		}

		if (!raw) {
			drm_edid_free(edid);
			edid = NULL;
			new_status = connector_status_disconnected;
			do_read = false;

			if (new_status == old_status)
				goto reschedule;
		}
	} else if (new_status == old_status) {
		goto reschedule;
	}

	mutex_lock(&ms912x->hpd_lock);
	if (!ms912x->hpd_enabled) {
		mutex_unlock(&ms912x->hpd_lock);
		drm_edid_free(edid);
		return;
	}

	if (new_status == connector_status_disconnected) {
		drm_edid_free(ms912x->edid);
		ms912x->edid = NULL;
		ms912x->edid_state = MS912X_EDID_NONE;
	} else {
		drm_edid_free(ms912x->edid);
		ms912x->edid = edid;
		edid = NULL;
		ms912x->edid_state = ms912x->edid ? MS912X_EDID_VALID :
						   MS912X_EDID_FAILED;
	}

	ms912x->hpd_status = new_status;
	mutex_unlock(&ms912x->hpd_lock);

	drm_edid_free(edid);

	mutex_lock(&ms912x->hpd_lock);
	enabled = ms912x->hpd_enabled;
	mutex_unlock(&ms912x->hpd_lock);

	if (enabled && (new_status != old_status || do_read) &&
	    !drm_dev_is_unplugged(&ms912x->drm))
		drm_kms_helper_hotplug_event(&ms912x->drm);

reschedule:
	mutex_lock(&ms912x->hpd_lock);
	enabled = ms912x->hpd_enabled;
	mutex_unlock(&ms912x->hpd_lock);

	if (enabled && !drm_dev_is_unplugged(&ms912x->drm))
		queue_delayed_work(system_long_wq, &ms912x->hpd_work,
				   msecs_to_jiffies(MS912X_HPD_POLL_MS));
}

void ms912x_hpd_start(struct ms912x_device *ms912x)
{
	mutex_lock(&ms912x->hpd_lock);
	if (ms912x->hpd_enabled) {
		mutex_unlock(&ms912x->hpd_lock);
		return;
	}
	ms912x->hpd_enabled = true;
	mutex_unlock(&ms912x->hpd_lock);

	queue_delayed_work(system_long_wq, &ms912x->hpd_work, 0);
}

void ms912x_hpd_stop(struct ms912x_device *ms912x)
{
	mutex_lock(&ms912x->hpd_lock);
	ms912x->hpd_enabled = false;
	drm_edid_free(ms912x->edid);
	ms912x->edid = NULL;
	ms912x->edid_state = MS912X_EDID_NONE;
	mutex_unlock(&ms912x->hpd_lock);

	cancel_delayed_work_sync(&ms912x->hpd_work);
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

static int ms912x_add_custom_mode(struct drm_connector *connector)
{
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	struct drm_display_mode *mode;
	unsigned int i;
	int count = 0;

	for (i = 0; i < ms912x->num_custom_modes; i++) {
		mode = drm_mode_duplicate(
			connector->dev,
			&ms912x->custom_modes[i].display_mode);
		if (!mode)
			continue;

		drm_mode_probed_add(connector, mode);
		count++;
	}

	return count;
}

static int ms912x_connector_get_modes(struct drm_connector *connector)
{
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	const struct drm_edid *edid = NULL;
	int ret = ms912x_add_custom_mode(connector);

	if (ms912x->port_type == MS912X_VIDEO_PORT_CVBS ||
	    ms912x->port_type == MS912X_VIDEO_PORT_SVIDEO ||
	    ms912x->port_type == MS912X_VIDEO_PORT_CVBS_SVIDEO ||
	    ms912x->port_type == MS912X_VIDEO_PORT_UNKNOWN)
		return ret + ms912x_add_cvbs_svideo_modes(connector);

	if (ms912x->port_type == MS912X_VIDEO_PORT_YPBPR)
		return ret + ms912x_add_ypbpr_modes(connector);

	/* Never touch USB here: get_modes can be called while userspace
	 * holds a compositor-wide lock.  Use the prefetched EDID, or fall
	 * back to the generic HDMI/VGA modes until the worker has it.
	 */
	mutex_lock(&ms912x->hpd_lock);
	if (ms912x->edid)
		edid = drm_edid_dup(ms912x->edid);
	mutex_unlock(&ms912x->hpd_lock);

	if (!edid)
		return ret + ms912x_add_default_hdmi_vga_modes(connector);

	if (drm_edid_connector_update(connector, edid) >= 0)
		ret += drm_edid_connector_add_modes(connector);
	drm_edid_free(edid);
	return ret;
}

static enum drm_connector_status ms912x_detect(struct drm_connector *connector,
					       bool force)
{
	struct ms912x_device *ms912x = to_ms912x(connector->dev);
	enum drm_connector_status status;

	(void)force;

	mutex_lock(&ms912x->hpd_lock);
	status = ms912x->hpd_status;
	mutex_unlock(&ms912x->hpd_lock);

	return status;
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

	/* HPD is polled by ms912x_hpd_work(), not by DRM core. */
	ms912x->connector.polled = 0;

	return drm_connector_attach_encoder(&ms912x->connector,
					    &ms912x->encoder);
}
