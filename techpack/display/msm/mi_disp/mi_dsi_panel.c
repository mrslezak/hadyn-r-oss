/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
 * Copyright (C) 2020 XiaoMi, Inc.
 */

#define pr_fmt(fmt)	"mi-dsi-panel:[%s:%d] " fmt, __func__, __LINE__
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/rtc.h>
#include <video/mipi_display.h>

#include <drm/mi_disp.h>
#include <drm/sde_drm.h>
#include "sde_connector.h"
#include "sde_encoder.h"
#include "sde_crtc.h"
#include "sde_trace.h"

#include "dsi_panel.h"
#include "dsi_display.h"
#include "dsi_ctrl_hw.h"
#include "dsi_parser.h"
#include "../../../../kernel/irq/internals.h"
#include "mi_disp_feature.h"
#include "mi_disp_print.h"
#include "mi_dsi_display.h"

extern void mi_sde_connector_fod_ui_ready(struct dsi_display *display, int type, int value);

#define to_dsi_display(x) container_of(x, struct dsi_display, host)

extern const char *cmd_set_prop_map[DSI_CMD_SET_MAX];

struct dsi_read_config g_dsi_read_cfg;
/**
* fold_status :  0 - unfold status
*			1 - fold status
*/
int fold_status;

bool is_aod_and_panel_initialized(struct dsi_panel *panel)
{
	if ((panel->power_mode == SDE_MODE_DPMS_LP1 ||
		panel->power_mode == SDE_MODE_DPMS_LP2) &&
		dsi_panel_initialized(panel)){
		return true;
	} else {
		return false;
	}
}

bool is_backlight_set_skip(struct dsi_panel *panel, u32 bl_lvl)
{
	if (panel->mi_cfg.in_fod_calibration ||
		panel->mi_cfg.feature_val[DISP_FEATURE_HBM_FOD] == FEATURE_ON) {
		DSI_INFO("%s panel skip set backlight %d due to fod hbm "
				"or fod calibration\n", panel->type, bl_lvl);
		return true;
	} else if (panel->mi_cfg.feature_val[DISP_FEATURE_DC] == FEATURE_ON &&
		bl_lvl < panel->mi_cfg.dc_threshold && bl_lvl != 0 && panel->mi_cfg.dc_type) {
		if (panel->mi_cfg.hbm_brightness_flag &&
			panel->mi_cfg.brightness_clone > (int)(panel->mi_cfg.max_brightness_clone / 2)) {
			return false;
		} else {
			DSI_INFO("%s panel skip set backlight %d due to DC on\n", panel->type, bl_lvl);
			return true;
		}
	} else {
		return false;
	}
}

bool is_hbm_fod_on(struct dsi_panel *panel)
{
	if (panel->mi_cfg.feature_val[DISP_FEATURE_HBM_FOD] == FEATURE_ON ||
		panel->mi_cfg.feature_val[DISP_FEATURE_LOCAL_HBM] == LOCAL_HBM_NORMAL_WHITE_1000NIT ||
		panel->mi_cfg.feature_val[DISP_FEATURE_LOCAL_HBM] == LOCAL_HBM_NORMAL_WHITE_110NIT ||
		panel->mi_cfg.feature_val[DISP_FEATURE_LOCAL_HBM] == LOCAL_HBM_HLPM_WHITE_1000NIT ||
		panel->mi_cfg.feature_val[DISP_FEATURE_LOCAL_HBM] == LOCAL_HBM_HLPM_WHITE_110NIT) {
		return true;
	} else {
		return false;
	}
}

int mi_dsi_panel_esd_irq_ctrl(struct dsi_panel *panel,
				bool enable)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	struct irq_desc *desc;

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("Panel not ready!\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	if (gpio_is_valid(mi_cfg->esd_err_irq_gpio)) {
		if (mi_cfg->esd_err_irq) {
			if (enable) {
				if (!mi_cfg->esd_err_enabled) {
					desc = irq_to_desc(mi_cfg->esd_err_irq);
					if (!irq_settings_is_level(desc))
						desc->istate &= ~IRQS_PENDING;
					enable_irq_wake(mi_cfg->esd_err_irq);
					enable_irq(mi_cfg->esd_err_irq);
					mi_cfg->esd_err_enabled = true;
					DISP_INFO("%s panel esd irq is enable\n", panel->type);
				}
			} else {
				if (mi_cfg->esd_err_enabled) {
					disable_irq_wake(mi_cfg->esd_err_irq);
					disable_irq_nosync(mi_cfg->esd_err_irq);
					mi_cfg->esd_err_enabled = false;
					DISP_INFO("%s panel esd irq is disable\n", panel->type);
				}
			}
		}
	} else {
		DISP_INFO("%s panel esd irq gpio invalid\n", panel->type);
	}

	mutex_unlock(&panel->panel_lock);
	return 0;
}

int mi_dsi_panel_write_cmd_set(struct dsi_panel *panel,
				struct dsi_panel_cmd_set *cmd_sets)
{
	int rc = 0, i = 0;
	ssize_t len;
	struct dsi_cmd_desc *cmds;
	u32 count;
	enum dsi_cmd_set_state state;
	struct dsi_display_mode *mode;
	const struct mipi_dsi_host_ops *ops = panel->host->ops;

	if (!panel || !panel->cur_mode) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mode = panel->cur_mode;

	cmds = cmd_sets->cmds;
	count = cmd_sets->count;
	state = cmd_sets->state;

	if (count == 0) {
		DISP_DEBUG("[%s] No commands to be sent for state\n", panel->type);
		goto error;
	}

	for (i = 0; i < count; i++) {
		if (state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;

		if (cmds->last_command)
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;

		len = ops->transfer(panel->host, &cmds->msg);
		if (len < 0) {
			rc = len;
			DISP_ERROR("failed to set cmds, rc=%d\n", rc);
			goto error;
		}
		if (cmds->post_wait_ms)
			usleep_range(cmds->post_wait_ms * 1000,
					((cmds->post_wait_ms * 1000) + 10));
		cmds++;
	}
error:
	return rc;
}

int mi_dsi_panel_read_cmd_set(struct dsi_panel *panel,
				struct dsi_read_config *read_config)
{
	struct dsi_display *display;
	struct dsi_display_ctrl *ctrl;
	struct dsi_cmd_desc *cmds;
	enum dsi_cmd_set_state state;
	int i, rc = 0, count = 0;
	u32 flags = 0;

	if (!panel || !panel->host || !read_config) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	display = to_dsi_display(panel->host);

	/* Avoid sending DCS commands when ESD recovery is pending */
	if (atomic_read(&display->panel->esd_recovery_pending)) {
		DISP_ERROR("[%s] ESD recovery pending\n", panel->type);
		return 0;
	}

	if (!panel->panel_initialized) {
		DISP_INFO("[%s] Panel not initialized\n", panel->type);
		return -EINVAL;
	}

	if (!read_config->is_read) {
		DISP_INFO("[%s] read operation was not permitted\n", panel->type);
		return -EPERM;
	}

	dsi_display_clk_ctrl(display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_ON);

	ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DISP_ERROR("[%s] cmd engine enable failed\n", panel->type);
		rc = -EPERM;
		goto error_disable_clks;
	}

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			DISP_ERROR("[%s] failed to allocate cmd tx buffer\n", panel->type);
			goto error_disable_cmd_engine;
		}
	}

	count = read_config->read_cmd.count;
	cmds = read_config->read_cmd.cmds;
	state = read_config->read_cmd.state;
	if (count == 0) {
		DISP_ERROR("[%s] No commands to be sent\n", panel->type);
		goto error_disable_cmd_engine;
	}
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	if (state == DSI_CMD_SET_STATE_LP)
		cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ |
		  DSI_CTRL_CMD_CUSTOM_DMA_SCHED);

	memset(read_config->rbuf, 0x0, sizeof(read_config->rbuf));
	cmds->msg.rx_buf = read_config->rbuf;
	cmds->msg.rx_len = read_config->cmds_rlen;

	rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &(cmds->msg), &flags);
	if (rc <= 0) {
		DISP_ERROR("[%s] rx cmd transfer failed rc=%d\n", panel->type, rc);
		goto error_disable_cmd_engine;
	}

	/* for debug log */
	for (i = 0; i < read_config->cmds_rlen; i++)
		DISP_DEBUG("[%d] = 0x%02X\n", i, read_config->rbuf[i]);

error_disable_cmd_engine:
	dsi_display_cmd_engine_disable(display);
error_disable_clks:
	dsi_display_clk_ctrl(display->dsi_clk_handle,
		DSI_ALL_CLKS, DSI_CLK_OFF);

	return rc;
}

int mi_dsi_panel_write_mipi_reg(struct dsi_panel *panel,
				char *buf)
{
	struct dsi_panel_cmd_set cmd_sets = {0};
	int rc = 0, dlen = 0;
	u32 packet_count = 0;
	char *token, *input_copy, *input_dup = NULL;
	const char *delim = " ";
	char *buffer = NULL;
	u32 buf_size = 0;
	u32 tmp_data = 0;

	mutex_lock(&panel->panel_lock);

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("Panel not initialized!\n");
		rc = -EAGAIN;
		goto exit_unlock;
	}

	DISP_DEBUG("[%s] input buffer:{%s}\n", panel->type, buf);

	input_copy = kstrdup(buf, GFP_KERNEL);
	if (!input_copy) {
		rc = -ENOMEM;
		goto exit_unlock;
	}

	input_dup = input_copy;
	/* removes leading and trailing whitespace from input_copy */
	input_copy = strim(input_copy);

	/* Split a string into token */
	token = strsep(&input_copy, delim);
	if (token) {
		rc = kstrtoint(token, 10, &tmp_data);
		if (rc) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free0;
		}
		g_dsi_read_cfg.is_read= !!tmp_data;
	}

	/* Removes leading whitespace from input_copy */
	if (input_copy)
		input_copy = skip_spaces(input_copy);
	else
		goto exit_free0;

	token = strsep(&input_copy, delim);
	if (token) {
		rc = kstrtoint(token, 10, &tmp_data);
		if (rc) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free0;
		}
		if (tmp_data > sizeof(g_dsi_read_cfg.rbuf)) {
			DISP_ERROR("read size exceeding the limit %d\n",
					sizeof(g_dsi_read_cfg.rbuf));
			goto exit_free0;
		}
		g_dsi_read_cfg.cmds_rlen = tmp_data;
	}

	/* Removes leading whitespace from input_copy */
	if (input_copy)
		input_copy = skip_spaces(input_copy);
	else
		goto exit_free0;

	buffer = kzalloc(strlen(input_copy), GFP_KERNEL);
	if (!buffer) {
		rc = -ENOMEM;
		goto exit_free0;
	}

	token = strsep(&input_copy, delim);
	while (token) {
		rc = kstrtoint(token, 16, &tmp_data);
		if (rc) {
			DISP_ERROR("input buffer conversion failed\n");
			goto exit_free1;
		}
		DISP_DEBUG("buffer[%d] = 0x%02x\n", buf_size, tmp_data);
		buffer[buf_size++] = (tmp_data & 0xff);
		/* Removes leading whitespace from input_copy */
		if (input_copy) {
			input_copy = skip_spaces(input_copy);
			token = strsep(&input_copy, delim);
		} else {
			token = NULL;
		}
	}

	rc = dsi_panel_get_cmd_pkt_count(buffer, buf_size, &packet_count);
	if (!packet_count) {
		DISP_ERROR("get pkt count failed!\n");
		goto exit_free1;
	}

	rc = dsi_panel_alloc_cmd_packets(&cmd_sets, packet_count);
	if (rc) {
		DISP_ERROR("failed to allocate cmd packets, ret=%d\n", rc);
		goto exit_free1;
	}

	rc = dsi_panel_create_cmd_packets(buffer, dlen, packet_count,
						  cmd_sets.cmds);
	if (rc) {
		DISP_ERROR("failed to create cmd packets, ret=%d\n", rc);
		goto exit_free2;
	}

	if (g_dsi_read_cfg.is_read) {
		g_dsi_read_cfg.read_cmd = cmd_sets;
		rc = mi_dsi_panel_read_cmd_set(panel, &g_dsi_read_cfg);
		if (rc <= 0) {
			DISP_ERROR("[%s]failed to read cmds, rc=%d\n", panel->name, rc);
			goto exit_free3;
		}
	} else {
		g_dsi_read_cfg.read_cmd = cmd_sets;
		rc = mi_dsi_panel_write_cmd_set(panel, &cmd_sets);
		if (rc) {
			DISP_ERROR("[%s] failed to send cmds, rc=%d\n", panel->name, rc);
			goto exit_free3;
		}
	}

	DISP_DEBUG("[%s]: done!\n", panel->name);
	rc = 0;

exit_free3:
	dsi_panel_destroy_cmd_packets(&cmd_sets);
exit_free2:
	dsi_panel_dealloc_cmd_packets(&cmd_sets);
exit_free1:
	kfree(buffer);
exit_free0:
	kfree(input_dup);
exit_unlock:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

ssize_t mi_dsi_panel_read_mipi_reg(struct dsi_panel *panel,
			char *buf, size_t size)
{
	int i = 0;
	ssize_t count = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	if (g_dsi_read_cfg.is_read) {
		for (i = 0; i < g_dsi_read_cfg.cmds_rlen; i++) {
			if (i == g_dsi_read_cfg.cmds_rlen - 1) {
				count += snprintf(buf + count, size - count, "0x%02X\n",
				     g_dsi_read_cfg.rbuf[i]);
			} else {
				count += snprintf(buf + count, size - count, "0x%02X,",
				     g_dsi_read_cfg.rbuf[i]);
			}
		}
	}

	mutex_unlock(&panel->panel_lock);

	return count;
}

int mi_dsi_panel_write_dsi_cmd_set(struct dsi_panel *panel,
			int type)
{
	int rc = 0;
	int i = 0, j = 0;
	u8 *tx_buf = NULL;
	u8 *buffer = NULL;
	int buf_size = 1024;
	u32 cmd_count = 0;
	int buf_count = 1024;
	struct dsi_cmd_desc *cmds;
	enum dsi_cmd_set_state state;
	struct dsi_display_mode *mode;

	if (!panel || !panel->cur_mode || type < 0 || type >= DSI_CMD_SET_MAX) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	buffer = kzalloc(buf_size, GFP_KERNEL);
	if (!buffer) {
		return -ENOMEM;
	}

	mutex_lock(&panel->panel_lock);

	mode = panel->cur_mode;
	cmds = mode->priv_info->cmd_sets[type].cmds;
	cmd_count = mode->priv_info->cmd_sets[type].count;
	state = mode->priv_info->cmd_sets[type].state;

	if (cmd_count == 0) {
		DISP_ERROR("[%s] No commands to be sent\n", cmd_set_prop_map[type]);
		rc = -EAGAIN;
		goto error;
	}

	DISP_INFO("set cmds [%s], count (%d), state(%s)\n",
		cmd_set_prop_map[type], cmd_count,
		(state == DSI_CMD_SET_STATE_LP) ? "dsi_lp_mode" : "dsi_hs_mode");

	for (i = 0; i < cmd_count; i++) {
		memset(buffer, 0, buf_size);
		buf_count = snprintf(buffer, buf_size, "%02X", cmds->msg.tx_len);
		tx_buf = (u8 *)cmds->msg.tx_buf;
		for (j = 0; j < cmds->msg.tx_len ; j++) {
			buf_count += snprintf(buffer + buf_count, buf_size - buf_count, " %02X", tx_buf[j]);
		}
		DISP_DEBUG("[%d] %s\n", i, buffer);
		cmds++;
	}

	rc = dsi_panel_tx_cmd_set(panel, type);

error:
	mutex_unlock(&panel->panel_lock);
	kfree(buffer);
	return rc;
}

ssize_t mi_dsi_panel_show_dsi_cmd_set_type(struct dsi_panel *panel,
			char *buf, size_t size)
{
	ssize_t count = 0;
	int type = 0;

	if (!panel || !buf) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	count = snprintf(buf, size, "%s: dsi cmd_set name\n", "id");

	for (type = DSI_CMD_SET_PRE_ON; type < DSI_CMD_SET_MAX; type++) {
		count += snprintf(buf + count, size - count, "%02d: %s\n",
				     type, cmd_set_prop_map[type]);
	}

	return count;
}

static int mi_dsi_panel_read_gamma_otp_and_flash(struct dsi_panel *panel,
				struct dsi_display_ctrl *ctrl)
{
	int rc = 0;
	int retval = 0;
	int i = 0;
	int retry_cnt = 0;
	u32 flags = 0;
	struct dsi_display_mode *mode;
	struct gamma_cfg *gamma_cfg;
	struct dsi_cmd_desc *cmds;
	enum dsi_cmd_set_state state;
	u32 param_index = 0;
	u8 read_param_buf[256] = {0};
	bool checksum_pass = 0;

	if (!panel || !ctrl || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mode = panel->cur_mode;
	gamma_cfg = &panel->mi_cfg.gamma_cfg;

	/* OTP Read 144hz gamma parameter */
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_OTP_READ_PRE);
	if (rc) {
		DISP_ERROR("Failed to send DSI_CMD_SET_MI_GAMMA_OTP_READ_PRE command\n");
		retval = -EAGAIN;
		goto error;
	}

	DISP_INFO("[%s] Gamma 0xB8 OPT Read 44 Parameter (144Hz)\n", panel->type);
	flags = 0;
	memset(read_param_buf, 0, sizeof(read_param_buf));
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_B8].cmds;
	state = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_B8].state;
	if (state == DSI_CMD_SET_STATE_LP)
		cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ | DSI_CTRL_CMD_CUSTOM_DMA_SCHED);
	cmds->msg.rx_buf = read_param_buf;
	cmds->msg.rx_len = sizeof(gamma_cfg->otp_read_b8);
	rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds->msg, &flags);
	if (rc <= 0) {
		DISP_ERROR("Failed to read DSI_CMD_SET_MI_GAMMA_OTP_READ_B8\n");
		retval = -EAGAIN;
		goto error;
	}
	memcpy(gamma_cfg->otp_read_b8, cmds->msg.rx_buf, sizeof(gamma_cfg->otp_read_b8));

	DISP_INFO("[%s] Gamma 0xB9 OPT Read 237 Parameter (144Hz)\n", panel->type);
	flags = 0;
	memset(read_param_buf, 0, sizeof(read_param_buf));
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_B9].cmds;
	state = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_B9].state;
	if (state == DSI_CMD_SET_STATE_LP)
		cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ | DSI_CTRL_CMD_CUSTOM_DMA_SCHED);
	cmds->msg.rx_buf = read_param_buf;
	cmds->msg.rx_len = sizeof(gamma_cfg->otp_read_b9);
	rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds->msg, &flags);
	if (rc <= 0) {
		DISP_ERROR("Failed to read DSI_CMD_SET_MI_GAMMA_OTP_READ_B9\n");
		retval = -EAGAIN;
		goto error;
	}
	memcpy(gamma_cfg->otp_read_b9, cmds->msg.rx_buf, sizeof(gamma_cfg->otp_read_b9));

	DISP_INFO("[%s] Gamma 0xBA OTP Read 63 Parameter (144Hz)\n", panel->type);
	flags = 0;
	memset(read_param_buf, 0, sizeof(read_param_buf));
	cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_BA].cmds;
	state = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_OTP_READ_BA].state;
	if (state == DSI_CMD_SET_STATE_LP)
		cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
	if (cmds->last_command) {
		cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
		flags |= DSI_CTRL_CMD_LAST_COMMAND;
	}
	flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ | DSI_CTRL_CMD_CUSTOM_DMA_SCHED);
	cmds->msg.rx_buf = read_param_buf;
	cmds->msg.rx_len = sizeof(gamma_cfg->otp_read_ba);
	rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds->msg, &flags);
	if (rc <= 0) {
		DISP_ERROR("Failed to read DSI_CMD_SET_MI_GAMMA_OTP_READ_BA\n");
		retval = -EAGAIN;
		goto error;
	}
	memcpy(gamma_cfg->otp_read_ba, cmds->msg.rx_buf, sizeof(gamma_cfg->otp_read_ba));

	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_OTP_READ_POST);
	if (rc) {
		pr_err("Failed to send DSI_CMD_SET_MI_GAMMA_OTP_READ_POST command\n");
		retval = -EAGAIN;
		goto error;
	}
	DISP_INFO("[%s] OTP Read 144hz gamma done\n", panel->type);

	/* Flash Read 90hz gamma parameter */
	do {
		gamma_cfg->gamma_checksum = 0;

		if (retry_cnt > 0) {
			DISP_ERROR("Failed to flash read 90hz gamma parameters, retry_cnt = %d\n",
					retry_cnt);
			mdelay(80);
		}

		DISP_INFO("Gamma Flash Read1 200 Parameter (90Hz)\n");
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_FLASH_READ1_PRE);
		if (rc) {
			DISP_ERROR("Failed to send DSI_CMD_SET_MI_GAMMA_FLASH_READ1_PRE command\n");
			retval = -EAGAIN;
			goto error;
		}

		flags = 0;
		memset(read_param_buf, 0, sizeof(read_param_buf));
		cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6].cmds;
		state = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6].state;
		if (state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
		if (cmds->last_command) {
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ | DSI_CTRL_CMD_CUSTOM_DMA_SCHED);
		cmds->msg.rx_buf = read_param_buf;
		cmds->msg.rx_len = 200;
		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds->msg, &flags);
		if (rc <= 0) {
			DISP_ERROR("Failed to read DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6\n");
			retval = -EAGAIN;
			goto error;
		}
		memcpy(gamma_cfg->flash_gamma_read, cmds->msg.rx_buf, 200);

		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_FLASH_READ_POST);
		if (rc) {
			DISP_ERROR("Failed to send DSI_CMD_SET_MI_GAMMA_FLASH_READ_POST command\n");
			retval = -EAGAIN;
			goto error;
		}

		DISP_INFO("Gamma Flash Read2 146 Parameter (90Hz)\n");
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_FLASH_READ2_PRE);
		if (rc) {
			DISP_ERROR("Failed to send DSI_CMD_SET_MI_GAMMA_FLASH_READ2_PRE command\n");
			retval = -EAGAIN;
			goto error;
		}

		flags = 0;
		memset(read_param_buf, 0, sizeof(read_param_buf));
		cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6].cmds;
		state = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6].state;
		if (state == DSI_CMD_SET_STATE_LP)
			cmds->msg.flags |= MIPI_DSI_MSG_USE_LPM;
		if (cmds->last_command) {
			cmds->msg.flags |= MIPI_DSI_MSG_LASTCOMMAND;
			flags |= DSI_CTRL_CMD_LAST_COMMAND;
		}
		flags |= (DSI_CTRL_CMD_FETCH_MEMORY | DSI_CTRL_CMD_READ | DSI_CTRL_CMD_CUSTOM_DMA_SCHED);
		cmds->msg.rx_buf = read_param_buf;
		cmds->msg.rx_len = 146;
		rc = dsi_ctrl_cmd_transfer(ctrl->ctrl, &cmds->msg, &flags);
		if (rc <= 0) {
			DISP_ERROR("Failed to read DSI_CMD_SET_MI_GAMMA_FLASH_READ_F6\n");
			retval = -EAGAIN;
			goto error;
		}
		memcpy(&gamma_cfg->flash_gamma_read[200], cmds->msg.rx_buf, 146);

		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_GAMMA_FLASH_READ_POST);
		if (rc) {
			DISP_ERROR("Failed to send DSI_CMD_SET_MI_GAMMA_FLASH_READ_POST command\n");
			retval = -EAGAIN;
			goto error;
		}

		for(i = 0; i < gamma_cfg->flash_read_total_param; i++)
		{
			if (i < sizeof(gamma_cfg->flash_read_b8)) {
				gamma_cfg->flash_read_b8[i] = gamma_cfg->flash_gamma_read[i];
			}
			else if (i < (sizeof(gamma_cfg->flash_read_b8) +
						sizeof(gamma_cfg->flash_read_b9))) {
				param_index = i - sizeof(gamma_cfg->flash_read_b8);
				gamma_cfg->flash_read_b9[param_index] = gamma_cfg->flash_gamma_read[i];
			}
			else if (i < (sizeof(gamma_cfg->flash_read_b8) +
					sizeof(gamma_cfg->flash_read_b9) +
					sizeof(gamma_cfg->flash_read_ba))) {
				param_index = i - (sizeof(gamma_cfg->flash_read_b8) +
								sizeof(gamma_cfg->flash_read_b9));
				gamma_cfg->flash_read_ba[param_index] = gamma_cfg->flash_gamma_read[i];
			}

			if (i < (gamma_cfg->flash_read_total_param - 2)) {
				gamma_cfg->gamma_checksum = gamma_cfg->flash_gamma_read[i] + gamma_cfg->gamma_checksum;
			} else {
				if (i == (gamma_cfg->flash_read_total_param - 2))
					gamma_cfg->flash_read_checksum[0] = gamma_cfg->flash_gamma_read[i];
				if (i == (gamma_cfg->flash_read_total_param - 1))
					gamma_cfg->flash_read_checksum[1] = gamma_cfg->flash_gamma_read[i];
			}
		}
		if (gamma_cfg->gamma_checksum == ((gamma_cfg->flash_read_checksum[0] << 8)
				+ gamma_cfg->flash_read_checksum[1])) {
			checksum_pass = 1;
			DISP_INFO("[%s] Flash Read 90hz gamma done\n", panel->type);
		} else {
			checksum_pass = 0;
		}

		retry_cnt++;
	}
	while (!checksum_pass && (retry_cnt < 5));

	if (checksum_pass) {
		gamma_cfg->read_done = 1;
		DISP_INFO("[%s] Gamma read done\n", panel->type);
		retval = 0;
	} else {
		DISP_ERROR("[%s] Failed to flash read 90hz gamma\n", panel->type);
		retval = -EAGAIN;
	}

error:
	return retval;

}

int mi_dsi_panel_read_gamma_param(struct dsi_panel *panel)
{
	int rc = 0, ret = 0;
	struct dsi_display *display;
	struct dsi_display_ctrl *ctrl;

	if (!panel || !panel->host) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->mi_cfg.gamma_update_flag) {
		DISP_DEBUG("[%s] Gamma_update_flag is not configed\n", panel->type);
		return 0;
	}

	display = to_dsi_display(panel->host);
	if (display == NULL)
		return -EINVAL;

	if (!panel->panel_initialized) {
		DISP_ERROR("[%s] Panel not initialized\n", panel->type);
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	if (panel->mi_cfg.gamma_cfg.read_done) {
		DISP_INFO("[%s] Gamma parameter have read and stored at"
			" POWER ON sequence\n", panel->type);
		goto unlock;
	}

	rc = dsi_display_clk_ctrl(display->dsi_clk_handle, DSI_ALL_CLKS, DSI_CLK_ON);
	if (rc) {
		DISP_ERROR("[%s] failed to enable DSI clocks, rc=%d\n", display->name, rc);
		goto unlock;
	}

	ctrl = &display->ctrl[display->cmd_master_idx];

	rc = dsi_display_cmd_engine_enable(display);
	if (rc) {
		DISP_ERROR("[%s] failed to enable cmd engine, rc=%d\n",
		       display->name, rc);
		goto error_disable_clks;
	}

	if (display->tx_cmd_buf == NULL) {
		rc = dsi_host_alloc_cmd_tx_buffer(display);
		if (rc) {
			DISP_ERROR("failed to allocate cmd tx buffer memory\n");
			goto error_disable_cmd_engine;
		}
	}

	rc = mi_dsi_panel_read_gamma_otp_and_flash(panel, ctrl);
	if (rc) {
		DISP_ERROR("[%s]failed to get gamma parameter, rc=%d\n",
		       display->name, rc);
		goto error_disable_cmd_engine;
	}

error_disable_cmd_engine:
	ret = dsi_display_cmd_engine_disable(display);
	if (ret) {
		DISP_ERROR("[%s]failed to disable DSI cmd engine, rc=%d\n",
				display->name, ret);
	}
error_disable_clks:
	ret = dsi_display_clk_ctrl(display->dsi_clk_handle,
			DSI_ALL_CLKS, DSI_CLK_OFF);
	if (ret) {
		DISP_ERROR("[%s] failed to disable all DSI clocks, rc=%d\n",
		       display->name, ret);
	}
unlock:
	mutex_unlock(&panel->panel_lock);

	return rc;
}


int mi_dsi_panel_update_gamma_param(struct dsi_panel *panel)
{
	struct dsi_display *display;
	struct dsi_display_mode *mode;
	struct gamma_cfg *gamma_cfg;
	struct dsi_cmd_desc *cmds;
	int total_modes;
	u32 i, count;
	u8 *tx_buf;
	size_t tx_len;
	u32 param_len;
	int rc;

	if (!panel || !panel->host){
		DISP_ERROR("Invalid params\n");
		return -EINVAL;
	}

	display = to_dsi_display(panel->host);
	if (!display)
		return -EINVAL;

	if (!panel->mi_cfg.gamma_update_flag) {
		DISP_DEBUG("[%s] Gamma_update_flag is not configed\n", panel->type);
		return 0;
	}

	gamma_cfg = &panel->mi_cfg.gamma_cfg;
	if (!gamma_cfg->read_done) {
		DISP_ERROR("[%s] Gamma parameter not ready, gamma parameter should be"
			" read and stored at POWER ON sequence\n", panel->type);
		return -EAGAIN;
	}

	if (!display->modes) {
		rc = dsi_display_get_modes(display, &mode);
		if (rc) {
			DISP_ERROR("Failed to get display mode for update gamma parameter\n");
			return rc;
		}
	}

	mutex_lock(&panel->panel_lock);
	total_modes = panel->num_display_modes;
	for (i = 0; i < total_modes; i++) {
		mode = &display->modes[i];
		if (mode && mode->priv_info) {
			if (144 == mode->timing.refresh_rate && !gamma_cfg->update_done_144hz) {
				DISP_INFO("[%s] Update GAMMA Parameter (144Hz)\n", panel->type);
				cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_WRITE_GAMMA].cmds;
				count = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_WRITE_GAMMA].count;
				if (cmds && count >= gamma_cfg->update_b8_index &&
					count >= gamma_cfg->update_b9_index &&
					count >= gamma_cfg->update_ba_index) {
					tx_buf = (u8 *)cmds[gamma_cfg->update_b8_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_b8_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->otp_read_b8), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xB8)
						memcpy(&tx_buf[1], gamma_cfg->otp_read_b8, param_len);
					else
						DISP_ERROR("failed to update gamma 0xB8 parameter\n");

					tx_buf = (u8 *)cmds[gamma_cfg->update_b9_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_b9_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->otp_read_b9), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xB9)
						memcpy(&tx_buf[1], gamma_cfg->otp_read_b9, param_len);
					else
						DISP_ERROR("failed to update gamma 0xB9 parameter\n");

					tx_buf = (u8 *)cmds[gamma_cfg->update_ba_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_ba_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->otp_read_ba), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xBA)
						memcpy(&tx_buf[1], gamma_cfg->otp_read_ba, param_len);
					else
						DISP_ERROR("failed to update gamma 0xBA parameter\n");

					gamma_cfg->update_done_144hz = true;
				} else {
					DISP_ERROR("please check gamma update parameter index configuration\n");
				}
			}
			if (90 == mode->timing.refresh_rate && !gamma_cfg->update_done_90hz) {
				DISP_INFO("[%s] Update GAMMA Parameter (90Hz)\n", panel->type);
				cmds = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_WRITE_GAMMA].cmds;
				count = mode->priv_info->cmd_sets[DSI_CMD_SET_MI_WRITE_GAMMA].count;
				if (cmds && count >= gamma_cfg->update_b8_index &&
					count >= gamma_cfg->update_b9_index &&
					count >= gamma_cfg->update_ba_index) {
					tx_buf = (u8 *)cmds[gamma_cfg->update_b8_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_b8_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->flash_read_b8), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xB8)
						memcpy(&tx_buf[1], gamma_cfg->flash_read_b8, param_len);
					else
						DISP_ERROR("failed to update gamma 0xB8 parameter\n");

					tx_buf = (u8 *)cmds[gamma_cfg->update_b9_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_b9_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->flash_read_b9), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xB9)
						memcpy(&tx_buf[1], gamma_cfg->flash_read_b9, param_len);
					else
						DISP_ERROR("failed to update gamma 0xB9 parameter\n");

					tx_buf = (u8 *)cmds[gamma_cfg->update_ba_index].msg.tx_buf;
					tx_len = cmds[gamma_cfg->update_ba_index].msg.tx_len;
					param_len = min(sizeof(gamma_cfg->flash_read_ba), tx_len - 1);
					if (tx_buf && tx_buf[0] == 0xBA)
						memcpy(&tx_buf[1], gamma_cfg->flash_read_ba, param_len);
					else
						DISP_ERROR("failed to update gamma 0xBA parameter\n");

					gamma_cfg->update_done_90hz = true;
				} else {
					DISP_ERROR("please check gamma update parameter index configuration\n");
				}
			}
		}
	}
	mutex_unlock(&panel->panel_lock);

	return 0;
}

ssize_t mi_dsi_panel_print_gamma_param(struct dsi_panel *panel,
				char *buf, size_t size)
{
	int i = 0;
	ssize_t count = 0;
	struct gamma_cfg *gamma_cfg;
	u8 *buffer = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	if (!panel->mi_cfg.gamma_update_flag) {
		DISP_ERROR("gamma_update_flag is not configed\n");
		return -EINVAL;
	}

	gamma_cfg = &panel->mi_cfg.gamma_cfg;

	if (!gamma_cfg->read_done) {
		DISP_INFO("Gamma parameter not read at POWER ON sequence\n");
		return -EAGAIN;
	}

	mutex_lock(&panel->panel_lock);

	count += snprintf(buf + count, size - count,
				"Gamma 0xB8 OPT Read %d Parameter (144Hz)\n",
				sizeof(gamma_cfg->otp_read_b8));
	buffer = gamma_cfg->otp_read_b8;
	for (i = 1; i <= sizeof(gamma_cfg->otp_read_b8); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->otp_read_b8))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->otp_read_b8[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->otp_read_b8[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma 0xB9 OPT Read %d Parameter (144Hz)\n",
				sizeof(gamma_cfg->otp_read_b9));
	buffer = gamma_cfg->otp_read_b9;
	for (i = 1; i <= sizeof(gamma_cfg->otp_read_b9); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->otp_read_b9))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->otp_read_b9[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->otp_read_b9[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma 0xBA OPT Read %d Parameter (144Hz)\n",
				sizeof(gamma_cfg->otp_read_ba));
	buffer = gamma_cfg->otp_read_ba;
	for (i = 1; i <= sizeof(gamma_cfg->otp_read_ba); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->otp_read_ba))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->otp_read_ba[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->otp_read_ba[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma Flash 0xB8 Read %d Parameter (90Hz)\n",
				sizeof(gamma_cfg->flash_read_b8));
	buffer = gamma_cfg->flash_read_b8;
	for (i = 1; i <= sizeof(gamma_cfg->flash_read_b8); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->flash_read_b8))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->flash_read_b8[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->flash_read_b8[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma Flash 0xB9 Read %d Parameter (90Hz)\n",
				sizeof(gamma_cfg->flash_read_b9));
	buffer = gamma_cfg->flash_read_b9;
	for (i = 1; i <= sizeof(gamma_cfg->flash_read_b9); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->flash_read_b9))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->flash_read_b9[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->flash_read_b9[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma Flash 0xBA Read %d Parameter (90Hz)\n",
				sizeof(gamma_cfg->flash_read_ba));
	buffer = gamma_cfg->flash_read_ba;
	for (i = 1; i <= sizeof(gamma_cfg->flash_read_ba); i++) {
		if (i%8 && (i != sizeof(gamma_cfg->flash_read_ba))) {
			count += snprintf(buf + count, size - count, "%02X ",
				gamma_cfg->flash_read_ba[i - 1]);
		} else {
			count += snprintf(buf + count, size - count, "%02X\n",
				gamma_cfg->flash_read_ba[i - 1]);
		}
	}

	count += snprintf(buf + count, size - count,
				"Gamma Flash Read Checksum Decimal(0x%x,0x%x) (90Hz)\n",
				gamma_cfg->flash_read_checksum[0],
				gamma_cfg->flash_read_checksum[1]);
	count += snprintf(buf + count, size - count,
				"Gamma Flash Read 344 Parameter SUM(0x%x) (90Hz)\n",
				gamma_cfg->gamma_checksum);

	mutex_unlock(&panel->panel_lock);

	return count;
}

int mi_dsi_panel_set_doze_brightness(struct dsi_panel *panel,
			u32 doze_brightness)
{
	int rc = 0;
	u32 doze_bl = 0;
	unsigned long mode_flags = 0;
	struct mi_dsi_panel_cfg *mi_cfg;
	struct mipi_dsi_device *dsi = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;

	if (doze_brightness != DOZE_TO_NORMAL && !is_aod_and_panel_initialized(panel)) {
		DISP_ERROR("Skip! %s panel set doze brightness %d, power mode(%s) initialized(%d)\n",
			panel->type, doze_brightness,
			get_display_power_mode_name(panel->power_mode), panel->panel_initialized);
		mi_cfg->doze_brightness = DOZE_TO_NORMAL;
		goto exit;
	}

	if (is_hbm_fod_on(panel) || mi_cfg->local_hbm_to_normal) {
		DISP_INFO("Skip! %s panel set doze brightness  %d due to fod hbm on\n",
			panel->type, doze_brightness);
		mi_cfg->doze_brightness = doze_brightness;
		if (doze_brightness != DOZE_TO_NORMAL)
			mi_cfg->doze_brightness_backup = doze_brightness;
		goto exit;
	}

	if (mi_cfg->doze_brightness != doze_brightness) {
		if (mi_cfg->aod_bl_51ctl) {
			if (panel->power_mode != SDE_MODE_DPMS_LP1) {
				mi_cfg->doze_brightness = doze_brightness;
				goto exit;
			}

			if (panel->bl_config.bl_inverted_dbv)
				doze_bl = (((doze_brightness & 0xff) << 8) | (doze_brightness >> 8));

			dsi = &panel->mipi_device;

			if (unlikely(panel->bl_config.lp_mode)) {
				mode_flags = dsi->mode_flags;
				dsi->mode_flags |= MIPI_DSI_MODE_LPM;
			}

			rc = mipi_dsi_dcs_set_display_brightness(dsi, (u16)doze_bl);
			if (rc < 0)
				DSI_ERR("%s panel failed to update aod backlight:%d\n",
					panel->type, doze_brightness);

			if (unlikely(panel->bl_config.lp_mode))
				dsi->mode_flags = mode_flags;
		} else {
			if (doze_brightness == DOZE_BRIGHTNESS_HBM) {
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_HBM cmd, rc=%d\n",
						panel->name, rc);
				}
				mi_dsi_update_micfg_flags(panel, PANEL_DOZE_HIGH);
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_HBM;
			} else if (doze_brightness == DOZE_BRIGHTNESS_LBM) {
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_LBM cmd, rc=%d\n",
						panel->name, rc);
				}
				mi_dsi_update_micfg_flags(panel, PANEL_DOZE_LOW);
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_LBM;
			}
		}

		mi_cfg->doze_brightness = doze_brightness;

		DISP_UTC_INFO("%s panel set doze brightness to %s\n",
			panel->type, get_doze_brightness_name(doze_brightness));
	} else {
		DISP_INFO("%s panel %s has been set, skip\n", panel->type,
			get_doze_brightness_name(doze_brightness));
	}

exit:
	mutex_unlock(&panel->panel_lock);

	return rc;
}

static int mi_dsi_panel_restore_doze_brightness(struct dsi_panel *panel)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->doze_brightness == DOZE_BRIGHTNESS_HBM) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
		if (rc) {
			DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_HBM cmd, rc=%d\n",
				panel->name, rc);
		}
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
	} else if (mi_cfg->doze_brightness == DOZE_BRIGHTNESS_LBM) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
		if (rc) {
			DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_LBM cmd, rc=%d\n",
				panel->name, rc);
		}
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
	}

	DISP_UTC_INFO("%s panel restore doze brightness to %s\n",
		panel->type, get_doze_brightness_name(mi_cfg->doze_brightness));

	return rc;
}

int mi_dsi_panel_get_doze_brightness(struct dsi_panel *panel,
			u32 *doze_brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	*doze_brightness =  mi_cfg->doze_brightness;

	mutex_unlock(&panel->panel_lock);

	return 0;
}

int mi_dsi_panel_get_brightness(struct dsi_panel *panel,
			u32 *brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	*brightness =  mi_cfg->last_bl_level;

	mutex_unlock(&panel->panel_lock);

	return 0;
}

int mi_dsi_panel_write_dsi_cmd(struct dsi_panel *panel,
			struct dsi_cmd_rw_ctl *ctl)
{
	struct dsi_panel_cmd_set cmd_sets = {0};
	u32 packet_count = 0;
	u32 dlen = 0;
	int rc = 0;

	mutex_lock(&panel->panel_lock);

	if (!panel || !panel->panel_initialized) {
		DISP_ERROR("Panel not initialized!\n");
		rc = -EAGAIN;
		goto exit_unlock;
	}

	if (!ctl->tx_len || !ctl->tx_ptr) {
		DISP_ERROR("%s panel invalid params\n", panel->type);
		rc = -EINVAL;
		goto exit_unlock;
	}

	rc = dsi_panel_get_cmd_pkt_count(ctl->tx_ptr, ctl->tx_len, &packet_count);
	if (rc) {
		DISP_ERROR("%s panel write dsi commands failed, rc=%d\n",
			panel->type, rc);
		goto exit_unlock;
	}

	DISP_DEBUG("%s panel packet-count=%d\n", panel->type, packet_count);

	rc = dsi_panel_alloc_cmd_packets(&cmd_sets, packet_count);
	if (rc) {
		DISP_ERROR("%s panel failed to allocate cmd packets, rc=%d\n",
			panel->type, rc);
		goto exit_unlock;
	}

	rc = dsi_panel_create_cmd_packets(ctl->tx_ptr, dlen, packet_count,
						cmd_sets.cmds);
	if (rc) {
		DISP_ERROR("%s panel failed to create cmd packets, rc=%d\n",
			panel->type, rc);
		goto exit_free1;
	}

	if (ctl->tx_state == MI_DSI_CMD_LP_STATE) {
		cmd_sets.state = DSI_CMD_SET_STATE_LP;
	} else if (ctl->tx_state == MI_DSI_CMD_HS_STATE) {
		cmd_sets.state = DSI_CMD_SET_STATE_HS;
	} else {
		DISP_ERROR("%s panel command state unrecognized-%s\n",
			panel->type, cmd_sets.state);
		goto exit_free1;
	}

	rc = mi_dsi_panel_write_cmd_set(panel, &cmd_sets);
	if (rc) {
		DISP_ERROR("%s panel [%s] failed to send cmds, rc=%d\n",
			panel->type, panel->name, rc);
		goto exit_free2;
	}

exit_free2:
	if (ctl->tx_len && ctl->tx_ptr)
		dsi_panel_destroy_cmd_packets(&cmd_sets);
exit_free1:
	if (ctl->tx_len && ctl->tx_ptr)
		dsi_panel_dealloc_cmd_packets(&cmd_sets);
exit_unlock:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

int mi_dsi_panel_set_brightness_clone(struct dsi_panel *panel,
			u32 brightness_clone)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	mi_cfg->brightness_clone = brightness_clone;
	DISP_UTC_INFO("%s panel set brightness clone to %d\n",
			panel->type, brightness_clone);

	mutex_unlock(&panel->panel_lock);

	return rc;
}

int mi_dsi_panel_get_brightness_clone(struct dsi_panel *panel,
			u32 *brightness_clone)
{
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	mi_cfg = &panel->mi_cfg;
	*brightness_clone =  mi_cfg->brightness_clone;

	mutex_unlock(&panel->panel_lock);

	return 0;
}

int mi_dsi_panel_nolp(struct dsi_panel *panel)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->dfps_bl_ctrl) {
		if (mi_cfg->last_bl_level < mi_cfg->dfps_bl_threshold)
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_NOLP);
		else
			rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);

		if (rc) {
			DISP_INFO("[%s] failed to send DSI_CMD_SET_NOLP cmd, rc=%d\n",
				panel->name, rc);
		}

		return rc;
	}

	switch (mi_cfg->doze_brightness_backup) {
	case DOZE_BRIGHTNESS_HBM:
		DISP_INFO("DOZE HBM NOLP\n");
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM_NOLP);
		if (rc) {
			DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_HBM_NOLP cmd, rc=%d\n",
				panel->name, rc);
		}
		mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
		break;
	case DOZE_BRIGHTNESS_LBM:
		DISP_INFO("DOZE LBM NOLP\n");
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM_NOLP);
		if (rc) {
			DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_LBM_NOLP cmd, rc=%d\n",
				panel->name, rc);
		}
		mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
		break;
	default:
		break;
	}

	return rc;
}

static void mi_disp_set_dimming_delayed_work_handler(struct kthread_work *work)
{
	struct disp_work *cur_work = container_of(work,
					struct disp_work, delayed_work.work);
	struct dsi_panel *panel = (struct dsi_panel *)(cur_work->data);
	struct disp_feature_ctl ctl;

	ctl.feature_id = DISP_FEATURE_DIMMING;
	ctl.feature_val = FEATURE_ON;

	mi_dsi_panel_set_disp_param(panel, &ctl);

	kfree(cur_work);
}

static int mi_disp_set_dimming_queue_delayed_work(struct disp_display *dd_ptr,
			struct dsi_panel *panel)
{
	struct disp_work *cur_work;

	cur_work = kzalloc(sizeof(*cur_work), GFP_ATOMIC);
	if (!cur_work)
		return -ENOMEM;

	kthread_init_delayed_work(&cur_work->delayed_work, mi_disp_set_dimming_delayed_work_handler);
	cur_work->dd_ptr = dd_ptr;
	cur_work->wq = &dd_ptr->pending_wq;
	cur_work->data = panel;

	kthread_queue_delayed_work(&dd_ptr->d_thread.worker, &cur_work->delayed_work,
			msecs_to_jiffies(panel->mi_cfg.panel_on_dimming_delay));

	return 0;
}

int mi_dsi_panel_update_dc_status(struct dsi_panel *panel, int brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	int rc = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -1;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->fod_type == 0) {
		return rc;
	}
	if (brightness > 0 && mi_cfg->last_bl_level == 0 && mi_cfg->dc_type) {
		DISP_INFO("%s panel CRC off\n", panel->type);
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_CRC_OFF);
	}
	if (brightness == 0 && mi_cfg->dc_type) {
		DISP_INFO("%s panel DC off\n", panel->type);
		mi_cfg->feature_val[DISP_FEATURE_DC] = FEATURE_OFF;
	}
	return rc;
}

void mi_dsi_panel_update_last_bl_level(struct dsi_panel *panel, int brightness)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	struct disp_feature *df = mi_get_disp_feature();
	struct dsi_display *display;
	int disp_id = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return;
	}

	display = to_dsi_display(panel->host);
	mi_cfg = &panel->mi_cfg;

	if ((mi_cfg->last_bl_level == 0 || mi_cfg->dimming_state == STATE_DIM_RESTORE) &&
		brightness > 0) {
		disp_id = mi_get_disp_id(display);
		mi_disp_set_dimming_queue_delayed_work(&df->d_display[disp_id], panel);

		if (mi_cfg->dimming_state == STATE_DIM_RESTORE)
			mi_cfg->dimming_state = STATE_NONE;
	}

	mi_cfg->last_bl_level = brightness;
	return;
}

void mi_dsi_update_micfg_flags(struct dsi_panel *panel, int power_mode)
{
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return;
	}

	mi_cfg = &panel->mi_cfg;

	switch (power_mode) {
	case PANEL_OFF:
		mi_cfg->in_fod_calibration = false;
		mi_cfg->fod_hbm_layer_enabled = false;
		mi_cfg->fod_anim_layer_enabled = false;
		mi_cfg->dimming_state = STATE_NONE;
		mi_cfg->panel_state = PANEL_STATE_OFF;
		mi_cfg->local_hbm_to_normal = false;
		mi_cfg->feature_val[DISP_FEATURE_HBM] = FEATURE_OFF;
		break;
	case PANEL_ON:
		mi_cfg->in_fod_calibration = false;
		mi_cfg->fod_hbm_layer_enabled = false;
		mi_cfg->fod_anim_layer_enabled = false;
		mi_cfg->dimming_state = STATE_NONE;
		mi_cfg->panel_state = PANEL_STATE_ON;
		mi_cfg->local_hbm_to_normal = false;
		break;
	case PANEL_NOLP:
		mi_cfg->dimming_state = STATE_DIM_RESTORE;
		mi_cfg->panel_state = PANEL_STATE_ON;
		break;
	case PANEL_DOZE_HIGH:
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		mi_cfg->panel_state = PANEL_STATE_DOZE_HIGH;
		break;
	case PANEL_DOZE_LOW:
		mi_cfg->dimming_state = STATE_DIM_BLOCK;
		mi_cfg->panel_state = PANEL_STATE_DOZE_LOW;
		break;
	case PANEL_LP1:
	case PANEL_LP2:
	default:
		break;
	}

	return;
}

void mi_dsi_dc_mode_enable(struct dsi_panel *panel,
			bool enable)
{
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel && !panel->cur_mode) {
		DISP_ERROR("invalid params\n");
		return;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->dc_type == 0 && enable) {
		if (mi_cfg->dc_threshold > 0 && mi_cfg->last_bl_level > mi_cfg->dc_threshold)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_HBM_ON);
		else
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_ON);
	} else if (mi_cfg->dc_type == 0) {
		dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_OFF);
		mi_cfg->demura_mask = 0;
		mi_dsi_panel_demura_comp(panel, mi_cfg->last_bl_level);
	}
}

int mi_dsi_fps_switch(struct dsi_panel *panel)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_ON
		&& mi_cfg->last_bl_level <= mi_cfg->dc_threshold)
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TIMING_SWITCH_DC_LBM);
	else if (mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_ON
		&& mi_cfg->last_bl_level > mi_cfg->dc_threshold)
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TIMING_SWITCH_DC_HBM);
	else if (mi_cfg->last_bl_level <mi_cfg->dfps_bl_threshold) {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_TIMING_SWITCH);
		mi_cfg->demura_mask = 0;
		mi_dsi_panel_demura_comp(panel, mi_cfg->last_bl_level);
	} else {
		rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_TIMING_SWITCH);
		mi_cfg->demura_mask = 0;
		mi_dsi_panel_demura_comp(panel, mi_cfg->last_bl_level);
	}

	return rc;
}

static int mi_dsi_update_hbm_cmd_51reg(struct dsi_panel *panel,
			enum dsi_cmd_set_type type, int bl_lvl)
{
	struct dsi_display_mode_priv_info *priv_info;
	struct dsi_cmd_desc *cmds = NULL;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;
	u32 count;
	u32 index;
	u8 *tx_buf;
	int rc = 0;

	if (!panel || !panel->cur_mode || !panel->cur_mode->priv_info) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mi_cfg = &panel->mi_cfg;
	priv_info = panel->cur_mode->priv_info;

	switch (type) {
	case DSI_CMD_SET_MI_HBM_ON:
		index = mi_cfg->hbm_on_51_index;
		break;
	case DSI_CMD_SET_MI_HBM_OFF:
		index = mi_cfg->hbm_off_51_index;
		break;
	case DSI_CMD_SET_MI_HBM_FOD_ON:
		index = mi_cfg->hbm_fod_on_51_index;
		break;
	case DSI_CMD_SET_MI_HBM_FOD_OFF:
		index = mi_cfg->hbm_fod_off_51_index;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT:
		index = mi_cfg->local_hbm_on_1000nit_51_index;
		break;
	case DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM:
		index = mi_cfg->local_hbm_off_to_hbm_51_index;
		break;
	default:
		DISP_ERROR("%s panel wrong cmd type!\n", panel->type);
		return -EINVAL;
	}

	/* restore last backlight value when hbm off */
	cmds = priv_info->cmd_sets[type].cmds;
	count = priv_info->cmd_sets[type].count;
	if (cmds && count >= index) {
		tx_buf = (u8 *)cmds[index].msg.tx_buf;
		if (tx_buf && tx_buf[0] == 0x51) {
			tx_buf[1] = (bl_lvl >> 8) & 0x07;
			tx_buf[2] = bl_lvl & 0xff;
		} else {
			if (tx_buf) {
				DISP_ERROR("%s panel tx_buf[0] = 0x%02X, check cmd[%d] 0x51 index\n",
					panel->type, type, tx_buf[0]);
			} else {
				DISP_ERROR("%s panel tx_buf is NULL pointer\n", panel->type);
			}
			rc = -EINVAL;
		}
	} else {
		DISP_ERROR("%s panel cmd[%d] 0x51 index(%d) error\n",
			panel->type, type, index);
		rc = -EINVAL;
	}

	return rc;
}

int mi_dsi_panel_set_disp_param(struct dsi_panel *panel, struct disp_feature_ctl *ctl)
{
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EINVAL;
	}

	mutex_lock(&panel->panel_lock);

	DISP_UTC_INFO("%s panel feature: %s, value: %d\n", panel->type,
		get_disp_feature_id_name(ctl->feature_id), ctl->feature_val);

	if (!panel->panel_initialized &&
			ctl->feature_id != DISP_FEATURE_SENSOR_LUX &&
			ctl->feature_id != DISP_FEATURE_LOW_BRIGHTNESS_FOD &&
			ctl->feature_id != DISP_FEATURE_FOLD_STATUS &&
			ctl->feature_id != DISP_FEATURE_FP_STATUS) {
		DISP_ERROR("Panel not initialized!\n");
		rc = -ENODEV;
		goto exit;
	}

	mi_cfg = &panel->mi_cfg;

	switch (ctl->feature_id) {
	case DISP_FEATURE_DIMMING:
		if (mi_cfg->dimming_state != STATE_DIM_BLOCK) {
			if (ctl->feature_val == FEATURE_ON)
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGON);
			else
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
			mi_cfg->feature_val[DISP_FEATURE_DIMMING] = ctl->feature_val;
		} else
			DISP_INFO("skip dimming %s\n", ctl->feature_val ? "on" : "off");
		break;
	case DISP_FEATURE_HBM:
		if (ctl->feature_val == FEATURE_ON) {
			if (mi_cfg->feature_val[DISP_FEATURE_HBM_FOD] != FEATURE_ON) {
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_ON);
				mi_cfg->dimming_state = STATE_DIM_BLOCK;
			}
		} else {
			if (mi_cfg->feature_val[DISP_FEATURE_HBM_FOD] != FEATURE_ON) {
				if (mi_cfg->hbm_51_ctl_flag && mi_cfg->hbm_off_51_index >= 0)
					mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_HBM_OFF, mi_cfg->last_bl_level);
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_OFF);
				mi_cfg->dimming_state = STATE_DIM_RESTORE;
			}
		}
		mi_cfg->feature_val[DISP_FEATURE_HBM] = ctl->feature_val;
		break;
	case DISP_FEATURE_HBM_FOD:
		if (ctl->feature_val == FEATURE_ON) {
			if (mi_cfg->hbm_51_ctl_flag && mi_cfg->hbm_fod_on_51_index >= 0)
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_HBM_FOD_ON, mi_cfg->hbm_fod_bl_lvl);
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_ON);
			if (mi_cfg->dc_type && mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_ON) {
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_CRC_OFF);
			}
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
		} else {
			if (mi_cfg->feature_val[DISP_FEATURE_HBM] != FEATURE_ON) {
				if (mi_cfg->hbm_51_ctl_flag && mi_cfg->hbm_fod_off_51_index >= 0)
					mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_HBM_FOD_OFF, mi_cfg->last_bl_level);
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_OFF);
				if (mi_cfg->doze_brightness) {
					mi_dsi_panel_restore_doze_brightness(panel);
				} else
					mi_cfg->dimming_state = STATE_DIM_RESTORE;
			} else {
				/* restore outdoor hbm after fod hbm off */
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_ON);
			}
		}
		mi_cfg->feature_val[DISP_FEATURE_HBM_FOD] = ctl->feature_val;
		break;
	case DISP_FEATURE_DOZE_BRIGHTNESS:
#ifdef CONFIG_FACTORY_BUILD
		if (dsi_panel_initialized(panel) &&
			is_aod_brightness(ctl->feature_val)) {
			if (ctl->feature_val == DOZE_BRIGHTNESS_HBM) {
				mi_cfg->doze_brightness = DOZE_BRIGHTNESS_HBM;
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_HBM;
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			} else {
				mi_cfg->doze_brightness = DOZE_BRIGHTNESS_LBM;
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_LBM;
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
			}
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
		} else {
			if (mi_cfg->aod_nolp_command_enabled)
				mi_dsi_panel_nolp(panel);
			else
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
			mi_cfg->doze_brightness = DOZE_TO_NORMAL;
			mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
		}
#else
		if (is_aod_and_panel_initialized(panel) &&
			is_aod_brightness(ctl->feature_val)) {
			if (ctl->feature_val == DOZE_BRIGHTNESS_HBM) {
				mi_cfg->doze_brightness = DOZE_BRIGHTNESS_HBM;
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_HBM;
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
			} else {
				mi_cfg->doze_brightness = DOZE_BRIGHTNESS_LBM;
				mi_cfg->doze_brightness_backup = DOZE_BRIGHTNESS_LBM;
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
			}
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
		} else {
			if (mi_cfg->aod_nolp_command_enabled)
				mi_dsi_panel_nolp(panel);
			else
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_NOLP);
			mi_cfg->doze_brightness = DOZE_TO_NORMAL;
			mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
		}
#endif
		mi_cfg->feature_val[DISP_FEATURE_DOZE_BRIGHTNESS] = ctl->feature_val;
		break;
	case DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS:
		if (ctl->feature_val == -1) {
			DISP_INFO("FOD calibration brightness restore last_bl_level=%d\n",
				mi_cfg->last_bl_level);
			dsi_panel_update_backlight(panel, mi_cfg->last_bl_level);
			mi_cfg->in_fod_calibration = false;
		} else {
			if (ctl->feature_val >= 0 && ctl->feature_val <= panel->bl_config.bl_max_level) {
				mi_cfg->in_fod_calibration = true;
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DIMMINGOFF);
				dsi_panel_update_backlight(panel, ctl->feature_val);
				mi_cfg->dimming_state = STATE_NONE;
			} else {
				mi_cfg->in_fod_calibration = false;
				DISP_ERROR("FOD calibration invalid brightness level:%d\n", ctl->feature_val);
			}
		}
		mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_BRIGHTNESS] = ctl->feature_val;
		break;
	case DISP_FEATURE_FOD_CALIBRATION_HBM:
		if (ctl->feature_val == -1) {
			DISP_INFO("FOD calibration HBM restore last_bl_level=%d\n",
				mi_cfg->last_bl_level);
			if (mi_cfg->hbm_51_ctl_flag && mi_cfg->hbm_fod_off_51_index >= 0)
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_HBM_FOD_OFF, mi_cfg->last_bl_level);
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_OFF);
			mi_cfg->dimming_state = STATE_DIM_RESTORE;
			mi_cfg->in_fod_calibration = false;
		} else {
			mi_cfg->in_fod_calibration = true;
			if (mi_cfg->hbm_51_ctl_flag && mi_cfg->hbm_fod_on_51_index >= 0) {
				if (ctl->feature_val >= mi_cfg->hbm_bl_min_lvl
					&& ctl->feature_val <= mi_cfg->hbm_bl_max_lvl) {
					mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_HBM_FOD_ON, ctl->feature_val);
				} else {
					mi_cfg->in_fod_calibration = false;
					DISP_ERROR("FOD calibration HBM invalid brightness level:%d\n", ctl->feature_val);
				}
			}
			if (mi_cfg->in_fod_calibration) {
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_HBM_FOD_ON);
				mi_cfg->dimming_state = STATE_DIM_BLOCK;
			}
		}
		mi_cfg->feature_val[DISP_FEATURE_FOD_CALIBRATION_HBM] = ctl->feature_val;
		break;
	case DISP_FEATURE_FLAT_MODE:
		if (ctl->feature_val == FEATURE_ON)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_ON);
		else
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_FLAT_MODE_OFF);
		mi_cfg->feature_val[DISP_FEATURE_FLAT_MODE] = ctl->feature_val;
		break;
	case DISP_FEATURE_NATURE_FLAT_MODE:
		if (ctl->feature_val == FEATURE_ON)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_NATURE_FLAT_MODE_ON);
		else
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_NATURE_FLAT_MODE_OFF);
		mi_cfg->feature_val[DISP_FEATURE_NATURE_FLAT_MODE] = ctl->feature_val;
		break;
	case DISP_FEATURE_DC:
		DISP_INFO("DC mode state:%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_DC] = ctl->feature_val;
		mi_dsi_dc_mode_enable(panel, ctl->feature_val == FEATURE_ON);
		break;
	case DISP_FEATURE_CRC:
		if (ctl->feature_val == FEATURE_OFF)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_CRC_OFF);
		break;
	case DISP_FEATURE_LOCAL_HBM:
		switch (ctl->feature_val) {
		case LOCAL_HBM_OFF_TO_NORMAL:
			if (mi_cfg->feature_val[DISP_FEATURE_HBM] == FEATURE_ON) {
				DISP_INFO("LOCAL_HBM_OFF_TO_HBM\n");;
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM, panel->mi_cfg.last_bl_level);
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HBM);
				mi_dsi_update_micfg_flags(panel, PANEL_NOLP);
				mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
				mi_cfg->local_hbm_to_normal = true;
				mi_cfg->dimming_state = STATE_DIM_BLOCK;
			} else {
				DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL\n");
				dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
				mi_dsi_update_micfg_flags(panel, PANEL_NOLP);
				mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
				mi_cfg->local_hbm_to_normal = true;
			}
			break;
		case LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT:
			DISP_INFO("LOCAL_HBM_OFF_TO_NORMAL_BACKLIGHT\n");
			mi_dsi_update_backlight_in_aod(panel, false);
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_NORMAL);
			mi_dsi_update_micfg_flags(panel, PANEL_NOLP);
			mi_cfg->doze_brightness_backup = DOZE_TO_NORMAL;
			mi_cfg->local_hbm_to_normal = true;
			break;
		case LOCAL_HBM_NORMAL_WHITE_1000NIT:
			if (mi_cfg->feature_val[DISP_FEATURE_HBM] == FEATURE_ON) {
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT in HBM\n");
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, panel->bl_config.bl_max_level);
			} else if (mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_ON
						&& panel->mi_cfg.last_bl_level < panel->mi_cfg.dc_threshold
						&& panel->mi_cfg.dc_type) {
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT in DC range\n");
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, panel->mi_cfg.dc_threshold);
			} else {
				DISP_INFO("LOCAL_HBM_NORMAL_WHITE_1000NIT\n");
				mi_dsi_update_hbm_cmd_51reg(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT, panel->mi_cfg.last_bl_level);
			}
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_1000NIT);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			break;
		case LOCAL_HBM_NORMAL_WHITE_750NIT:
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_750NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_750NIT);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			break;
		case LOCAL_HBM_NORMAL_WHITE_500NIT:
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_500NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_500NIT);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			break;
		case LOCAL_HBM_NORMAL_WHITE_110NIT:
			DISP_INFO("LOCAL_HBM_NORMAL_WHITE_110NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_WHITE_110NIT);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			break;
		case LOCAL_HBM_NORMAL_GREEN_500NIT:
			DISP_INFO("LOCAL_HBM_NORMAL_GREEN_500NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_NORMAL_GREEN_500NIT);
			mi_cfg->dimming_state = STATE_DIM_BLOCK;
			break;
		case LOCAL_HBM_HLPM_WHITE_1000NIT:
			DISP_INFO("LOCAL_HBM_HLPM_WHITE_1000NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_1000NIT);
			break;
		case LOCAL_HBM_HLPM_WHITE_110NIT:
			DISP_INFO("LOCAL_HBM_HLPM_WHITE_110NIT\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_HLPM_WHITE_110NIT);
			break;
		case LOCAL_HBM_OFF_TO_HLPM:
			DISP_INFO("LOCAL_HBM_OFF_TO_HLPM\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_HLPM);
			break;
		case LOCAL_HBM_OFF_TO_LLPM:
			DISP_INFO("LOCAL_HBM_OFF_TO_LLPM\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_LOCAL_HBM_OFF_TO_LLPM);
			break;
		default:
			DISP_ERROR("invalid local hbm value\n");
			break;
		}
		mi_cfg->feature_val[DISP_FEATURE_LOCAL_HBM] = ctl->feature_val;
		break;
	case DISP_FEATURE_SENSOR_LUX:
		DISP_DEBUG("DISP_FEATURE_SENSOR_LUX=%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_SENSOR_LUX] = ctl->feature_val;
		break;
	case DISP_FEATURE_LOW_BRIGHTNESS_FOD:
		DISP_INFO("DISP_FEATURE_LOW_BRIGHTNESS_FOD=%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_LOW_BRIGHTNESS_FOD] = ctl->feature_val;
		break;
	case DISP_FEATURE_FP_STATUS:
		DISP_INFO("DISP_FEATURE_FP_STATUS=%d\n", ctl->feature_val);
		mi_cfg->feature_val[DISP_FEATURE_FP_STATUS] = ctl->feature_val;
		if  (ctl->feature_val == ENROLL_STOP ||
				ctl->feature_val == AUTH_STOP ||
				ctl->feature_val == HEART_RATE_STOP) {
			mi_disp_set_fod_queue_work(0, false);
		}
		break;
	case DISP_FEATURE_FOLD_STATUS:
		DISP_INFO("DISP_FEATURE_FOLD_STATUS=%d\n", ctl->feature_val);
		fold_status = ctl->feature_val;
		break;
	case DISP_FEATURE_SPR_RENDER:
		DISP_INFO("DISP_FEATURE_SPR_RENDER=%d\n", ctl->feature_val);
		switch (ctl->feature_val) {
		case SPR_1D_RENDERING:
			DISP_INFO("SPR_1D_RENDERING\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_SPR_1D_RENDERING);
			break;
		case SPR_2D_RENDERING:
			DISP_INFO("SPR_2D_RENDERING\n");
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_SPR_2D_RENDERING);
			break;
		default:
			DISP_ERROR("invalid spr render value\n");
			break;
		}
		mi_cfg->feature_val[DISP_FEATURE_SPR_RENDER] = ctl->feature_val;
		break;
	case DISP_FEATURE_AOD_TO_NORMAL:
		if (is_hbm_fod_on(panel)) {
			DSI_INFO("fod hbm on, skip MI_FOD_AOD_TO_NORMAL %d\n", ctl->feature_val);
			break;
		}

		if (ctl->feature_val == FEATURE_ON) {
			switch (mi_cfg->doze_brightness_backup) {
			case DOZE_BRIGHTNESS_HBM:
				DISP_INFO("enter DOZE HBM NOLP\n");
				mi_dsi_update_backlight_in_aod(panel, false);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM_NOLP);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_HBM_NOLP cmd, rc=%d\n",
						panel->name, rc);
				}
				break;
			case DOZE_BRIGHTNESS_LBM:
				DISP_INFO("enter DOZE LBM NOLP\n");
				mi_dsi_update_backlight_in_aod(panel, false);
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM_NOLP);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_LBM_NOLP cmd, rc=%d\n",
						panel->name, rc);
				}
				break;
			default:
				break;
			}
			mi_dsi_update_micfg_flags(panel, PANEL_NOLP);
			panel->mi_cfg.local_hbm_to_normal = true;
		} else if (ctl->feature_val == FEATURE_OFF) {
			switch (mi_cfg->doze_brightness_backup) {
			case DOZE_BRIGHTNESS_HBM:
				DISP_INFO("enter DOZE HBM\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_HBM);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_HBM_NOLP cmd, rc=%d\n",
						panel->name, rc);
				}
				mi_dsi_update_micfg_flags(panel, PANEL_DOZE_HIGH);
				break;
			case DOZE_BRIGHTNESS_LBM:
				DISP_INFO("enter DOZE LBM\n");
				rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DOZE_LBM);
				if (rc) {
					DISP_ERROR("[%s] failed to send DSI_CMD_SET_MI_DOZE_LBM_NOLP cmd, rc=%d\n",
						panel->name, rc);
				}
				mi_dsi_update_micfg_flags(panel, PANEL_DOZE_LOW);
				break;
			default:
				break;
			}
			panel->mi_cfg.local_hbm_to_normal = false;
		}
		break;
	default:
		DISP_ERROR("invalid feature id\n");
		break;
	}

exit:
	mutex_unlock(&panel->panel_lock);
	return rc;
}

ssize_t mi_dsi_panel_get_disp_param(struct dsi_panel *panel,
			char *buf, size_t size)
{
	struct mi_dsi_panel_cfg *mi_cfg;
	ssize_t count = 0;
	int i = 0;

	if (!panel) {
		DISP_ERROR("invalid params\n");
		return -EAGAIN;
	}

	mi_cfg = &panel->mi_cfg;

	count = snprintf(buf, size, "%040s: feature vaule\n", "feature name[feature id]");

	mutex_lock(&panel->panel_lock);
	for (i = DISP_FEATURE_DIMMING; i < DISP_FEATURE_MAX; i++) {
		count += snprintf(buf + count, size - count, "%036s[%02d]: %d\n",
				     get_disp_feature_id_name(i), i, mi_cfg->feature_val[i]);
	}
	mutex_unlock(&panel->panel_lock);

	return count;
}

void mi_dsi_panel_demura_comp(struct dsi_panel *panel, u32 bl_lvl)
{
	int i = 0;
	int rc = 0;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!panel || !panel->mi_cfg.demura_comp
		|| (panel->mi_cfg.feature_val[DISP_FEATURE_DC] == FEATURE_ON))
		return;

	mi_cfg = &panel->mi_cfg;

	if (bl_lvl == 0) {
		mi_cfg->demura_mask = 0;
		return;
	}

	for (i = 0; i < mi_cfg->demura_bl_num; i++) {
		if (bl_lvl <= mi_cfg->demura_bl[i])
			break;
	}

	if (i == mi_cfg->demura_bl_num || mi_cfg->demura_mask & (1 << i))
		return;

	DISP_INFO("mi_dsi_panel_demura_comp, set demura level:%d\n", i);
	rc = dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DEMURA_L1+i);
	if (rc < 0)
			DSI_ERR("failed to send DSI_CMD_SET_MI_BL_DEMURA, bl:%d\n", bl_lvl);

	mi_cfg->demura_mask = (1 << i);
}

void mi_dsi_panel_dc_vi_setting(struct dsi_panel *panel, u32 bl_lvl)
{
	struct mi_dsi_panel_cfg *mi_cfg  = NULL;

	if (!panel && !bl_lvl) {
		DISP_ERROR("invalid params\n");
		return;
	}

	mi_cfg = &panel->mi_cfg;

	if (mi_cfg->dc_type == 0 && mi_cfg->feature_val[DISP_FEATURE_DC] == FEATURE_ON
		&& mi_cfg->dc_threshold > 0) {
		if (mi_cfg->last_bl_level <= mi_cfg->dc_threshold && bl_lvl > mi_cfg->dc_threshold)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_VI_HBM);
		else if ((mi_cfg->last_bl_level == 0 || mi_cfg->last_bl_level > mi_cfg->dc_threshold)
			&& bl_lvl <= mi_cfg->dc_threshold)
			dsi_panel_tx_cmd_set(panel, DSI_CMD_SET_MI_DC_VI_LBM);
	}
}


struct fod_work_data {
	struct dsi_display *display;
	bool from_touch;
	int fod_btn;
};

static void mi_sde_connector_fod_lhbm_notify(struct drm_connector *conn, int fod_btn)
{
	struct sde_connector *c_conn;
	bool icon;
	static bool last_icon = false;
	struct dsi_display *display;

	if (!conn) {
		DISP_ERROR("invalid params\n");
		return;
	}

	c_conn = to_sde_connector(conn);
	if (c_conn->connector_type != DRM_MODE_CONNECTOR_DSI) {
		DISP_ERROR("not DRM_MODE_CONNECTOR_DSIl\n");
		return;
	}

	display = (struct dsi_display *) c_conn->display;
	if (!display || !display->panel) {
		DISP_ERROR("invalid display/panel ptr\n");
		return;
	}

	if (mi_get_disp_id(display) != MI_DISP_PRIMARY)
		return;

	icon = (fod_btn == 1);
	if (last_icon != icon) {
		if (icon) {
			/* With local hbm, it will enable after first frame tx complete.
			 * So skip fod notify before local fod hbm enabled.
			 */
			if (display->panel->mi_cfg.local_hbm_enabled
				&& !display->panel->mi_cfg.fod_hbm_layer_enabled) {
				return;
			}
			/* Make sure icon was displayed on panel before notifying
			 * fingerprint to capture image */
			if (display->panel->mi_cfg.fod_hbm_layer_enabled) {
#if 0
				sde_encoder_wait_for_event(c_conn->encoder,
						MSM_ENC_TX_COMPLETE);
				sde_encoder_wait_for_event(c_conn->encoder,
						MSM_ENC_VBLANK);
#endif
			}
			SDE_ATRACE_BEGIN("mi_sde_connector_fod_ui_ready");
			mi_sde_connector_fod_ui_ready(display, 2, 1);
			SDE_ATRACE_END("mi_sde_connector_fod_ui_ready");
		} else {
			mi_sde_connector_fod_ui_ready(display, 2, 0);
		}
	}
	last_icon = icon;
}

static int mi_sde_connector_fod_lhbm(struct drm_connector *connector, bool from_touch, int fod_btn)
{
	int rc = 0;
	struct sde_connector *c_conn;
	struct dsi_display *display;
	bool btn_down;
	struct mi_dsi_panel_cfg *mi_cfg;

	if (!connector) {
		DISP_ERROR("invalid connector ptr\n");
		return -EINVAL;
	}

	c_conn = to_sde_connector(connector);

	if (c_conn->connector_type != DRM_MODE_CONNECTOR_DSI) {
		DISP_ERROR("not DRM_MODE_CONNECTOR_DSI\n");
		return -EINVAL;
	}

	display = (struct dsi_display *) c_conn->display;
	if (!display || !display->panel) {
		DISP_ERROR("invalid display/panel ptr\n");
		return -EINVAL;
	}

	if (mi_get_disp_id(display) != MI_DISP_PRIMARY) {
		DISP_ERROR("not MI_DISP_PRIMARY\n");
		return -EINVAL;
	}

	mi_cfg = &display->panel->mi_cfg;
	btn_down = (fod_btn == 1);

	DISP_INFO("dsi_mi_sde_connector_fod_lhbm=%d\n", btn_down);
	if (btn_down) {
		if (mi_cfg->fod_hbm_layer_enabled == false) {
			if (!c_conn->allow_bl_update) { /* Wait first frame tx done */
				DISP_INFO("LHBM on !allow_bl_update\n");
				mi_cfg->pending_lhbm_state = 1;
				rc = -ENODEV;
			} else {
				if (!mi_cfg->pending_lhbm_state && !from_touch) {
					rc = -EINVAL;
					DISP_INFO("LHBM on from display skip\n");
				} else {
					rc = mi_sde_connector_panel_ctl(connector, MI_FOD_HBM_ON);
					if (!rc) {
						mi_cfg->fod_hbm_layer_enabled = true;
						mi_cfg->pending_lhbm_state = 0;
					} else if (rc == -ENODEV) {
						DISP_INFO("LHBM on !panel_initialized rc=%d\n", rc);
						mi_cfg->pending_lhbm_state = 1;
					} else {
						DISP_INFO("LHBM on failed rc=%d\n", rc);
					}
				}
			}
		}
	} else {
		mi_cfg->pending_lhbm_state = 0;
		if (mi_cfg->fod_hbm_layer_enabled == true) {
			rc = mi_sde_connector_panel_ctl(connector, MI_FOD_HBM_OFF);
			mi_cfg->fod_hbm_layer_enabled = false;
		}
	}

	return rc;
}

static void mi_disp_set_fod_work_handler(struct kthread_work *work)
{
	int rc = 0;
	struct fod_work_data *fod_data = NULL;
	struct disp_work *cur_work = container_of(work,
					struct disp_work, work);

	if (cur_work) {
		fod_data = (struct fod_work_data *)(cur_work->data);
	}

	if (fod_data && fod_data->display) {
		if (fod_data->fod_btn == 0) {
			mi_sde_connector_fod_lhbm_notify(fod_data->display->drm_conn, fod_data->fod_btn);
		}

		rc = mi_sde_connector_fod_lhbm(fod_data->display->drm_conn,
			fod_data->from_touch, fod_data->fod_btn);

		if (fod_data->fod_btn == 1) {
			if (rc) {
				DISP_ERROR("LHBM on failed rc=%d, not notify\n", rc);
			} else {
				mi_sde_connector_fod_lhbm_notify(fod_data->display->drm_conn, fod_data->fod_btn);
			}
		}
	}

	if (fod_data)
		kfree(fod_data);
	kfree(cur_work);
}

int mi_disp_set_fod_queue_work(u32 fod_btn, bool from_touch)
{
	struct disp_work *cur_work;
	struct dsi_display *display = mi_get_primary_dsi_display();
	struct disp_feature *df = mi_get_disp_feature();
	static u32 sfod_btn = 0;
	struct fod_work_data *fod_data;
	struct mi_dsi_panel_cfg *mi_cfg;
	int fp_state = FINGERPRINT_NONE;

#ifdef CONFIG_FACTORY_BUILD
	return 0;
#endif

	if (from_touch) {
		if (sfod_btn == fod_btn) {
			return 0;
		} else {
			sfod_btn = fod_btn;
		}
	}

	if (display != NULL && display->panel != NULL) {
		mi_cfg = &display->panel->mi_cfg;
		fp_state = mi_cfg->feature_val[DISP_FEATURE_FP_STATUS];
		if (fp_state == ENROLL_STOP || fp_state == AUTH_STOP || fp_state == HEART_RATE_STOP) {
			if (fod_btn == 1) {
				DISP_INFO("fp_state=%d, skip\n", fp_state);
				return 0;
			}
		}
	}

	if (display != NULL) {
		cur_work = kzalloc(sizeof(*cur_work), GFP_ATOMIC);
		if (!cur_work)
			return -ENOMEM;

		fod_data = kzalloc(sizeof(*fod_data), GFP_ATOMIC);
		if (!fod_data)
			return -ENOMEM;

		DISP_INFO("dsi_mi_disp_set_fod_queue_work=%d\n", fod_btn);

		kthread_init_work(&cur_work->work, mi_disp_set_fod_work_handler);

		fod_data->display = display;
		fod_data->from_touch = from_touch;
		fod_data->fod_btn = fod_btn;

		cur_work->dd_ptr = &df->d_display[MI_DISP_PRIMARY];
		cur_work->wq = &df->d_display[MI_DISP_PRIMARY].fod_pending_wq;
		cur_work->data = fod_data;

		kthread_queue_work(&df->d_display[MI_DISP_PRIMARY].fod_thread.worker, &cur_work->work);
	}

	return 0;
}

void mi_dsi_update_backlight_in_aod(struct dsi_panel *panel, bool clear_backlight)
{
	int bl_lvl = 0;
	struct mi_dsi_panel_cfg *mi_cfg = &panel->mi_cfg;
	struct mipi_dsi_device *dsi = &panel->mipi_device;

	if (clear_backlight)
		bl_lvl = 0;
	else {
		switch (mi_cfg->doze_brightness_backup) {
			case DOZE_BRIGHTNESS_HBM:
				bl_lvl = mi_cfg->doze_hbm_dbv_level;
				break;
			case DOZE_BRIGHTNESS_LBM:
				bl_lvl = mi_cfg->doze_lbm_dbv_level;
				break;
			default:
				return;
		}
	}
	DISP_DEBUG("update backlight to %d\n", bl_lvl);
	if (panel->bl_config.bl_inverted_dbv)
		bl_lvl = (((bl_lvl & 0xff) << 8) | (bl_lvl >> 8));
	mipi_dsi_dcs_set_display_brightness(dsi, bl_lvl);

	return;
}

EXPORT_SYMBOL_GPL(mi_disp_set_fod_queue_work);
