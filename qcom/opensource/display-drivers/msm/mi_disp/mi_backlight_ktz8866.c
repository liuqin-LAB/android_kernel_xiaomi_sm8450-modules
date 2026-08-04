// SPDX-License-Identifier: GPL-2.0-only
/*
 * KTZ Semiconductor KTZ8866 LED Driver
 *
 * Copyright (C) 2013 Ideas on board SPRL
 *
 * Contact: Zhang Teng <zhangteng3@xiaomi.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#define pr_fmt(fmt)	"ktz8866:[%s:%d] " fmt, __func__, __LINE__

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "mi_backlight_ktz8866.h"
#include "mi_disp_print.h"
#include "mi_dsi_display.h"
#include "mi_panel_id.h"

#define M80_NORMAL_MAX_DBV 1737
#define M81_NORMAL_MAX_DBV 1700

static struct i2c_client *g_client;
static struct i2c_client *g_clientb;

static struct ktz8866_led g_ktz8866_led;

static int __maybe_unused ktz8866_read(struct i2c_client *client, u8 reg, u8 *data)
{
	int ret;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0) {
		mi_disp_printk(KERN_ERR, "[E]ktz8866 i2c failed reading at 0x%02x\n",
				(unsigned int)reg);
		return ret;
	}

	*data = (u8)ret;
	return 0;
}

static int ktz8866_write(struct i2c_client *client, u8 reg, u8 data)
{
	return i2c_smbus_write_byte_data(client, reg, data);
}

static int dualktz8866_write(struct dsi_panel *panel, u8 reg, u8 data)
{
	int ret;
	bool dual;

	if (!panel)
		return -EINVAL;

	dual = mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M80_PANEL_PA;
	if (!g_client || (dual && !g_clientb))
		return -ENODEV;

	ret = ktz8866_write(g_client, reg, data);
	if (ret || !dual)
		return ret;

	return ktz8866_write(g_clientb, reg, data);
}

static int ktz_update_status(struct ktz8866_led *ktz, struct dsi_panel *panel,
		unsigned int level, unsigned int normal_max_dbv)
{
	unsigned int exponential_bl = level;
	enum mi_project_panel_id panel_id = mi_get_panel_id(panel->mi_cfg.mi_panel_id);
	bool m81 = panel_id == M81_PANEL_PA || panel_id == M81_PANEL_PB;
	int brightness, ret = 0;
	unsigned int msb, lsb;

	if (m81 || ktz->hbm_enabled) {
		if (exponential_bl <= BL_LEVEL_MAX) {
			exponential_bl = (exponential_bl * normal_max_dbv) / 2047;
		} else if (exponential_bl <= BL_LEVEL_MAX_HBM) {
			exponential_bl = ((exponential_bl - 2048) *
					(2047 - normal_max_dbv)) / 2047 + normal_max_dbv;
		} else {
			return -EINVAL;
		}
	}
	if (exponential_bl > BL_LEVEL_MAX)
		return -EINVAL;

	brightness = mi_bl_level_remap[exponential_bl];
	mutex_lock(&ktz->lock);
	if (brightness == ktz->level)
		goto out;

	if (brightness > 0) {
		if (!ktz->ktz8866_status) {
			ret = dualktz8866_write(panel, KTZ8866_DISP_BL_ENABLE, 0x7f);
			if (ret)
				goto out;
			ktz->ktz8866_status = true;
		}
	} else {
		ret = dualktz8866_write(panel, KTZ8866_DISP_BL_ENABLE,
				m81 ? 0x1f : 0x3f);
		if (ret)
			goto out;
		ktz->ktz8866_status = false;
		if (m81)
			usleep_range(10 * 1000, 10 * 1000 + 10);
	}

	lsb = brightness & 0x7;
	msb = (brightness >> 3) & 0xff;
	ret = dualktz8866_write(panel, KTZ8866_DISP_BB_LSB, lsb);
	if (ret)
		goto out;
	ret = dualktz8866_write(panel, KTZ8866_DISP_BB_MSB, msb);
	if (ret)
		goto out;

	ktz->level = brightness;
out:
	mutex_unlock(&ktz->lock);
	return ret;
}

int ktz8866_backlight_update_status(struct dsi_panel *panel,
		unsigned int level)
{
	unsigned int normal_max_dbv = M80_NORMAL_MAX_DBV;

	if (!panel)
		return -EINVAL;

	if (mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M81_PANEL_PA ||
			mi_get_panel_id(panel->mi_cfg.mi_panel_id) == M81_PANEL_PB)
		normal_max_dbv = M81_NORMAL_MAX_DBV;

	return ktz_update_status(&g_ktz8866_led, panel, level, normal_max_dbv);
}

static int ktz8866_probe(struct i2c_client *i2c,
		const struct i2c_device_id *id)
{
	int device_id;

	if (!i2c_check_functionality(i2c->adapter,
			I2C_FUNC_SMBUS_BYTE_DATA)) {
		mi_disp_printk(KERN_ERR,
				"[E]ktz8866 I2C adapter doesn't support I2C_FUNC_SMBUS_BYTE\n");
		return -EIO;
	}

	if (!id) {
		mi_disp_printk(KERN_ERR, "[E]ktz8866 device_id is NULL !!!! \n");
		goto out;
	}

	if (id->driver_data) {
		g_clientb = i2c;
		device_id = i2c_smbus_read_byte_data(i2c, KTZ8866_DISP_ID);
		if (device_id < 0) {
			mi_disp_printk(KERN_ERR,
					"[E]ktz8866 i2c failed reading at 0x%02x\n",
					(unsigned int)KTZ8866_DISP_ID);
			device_id = 0;
		}
		mi_disp_printk(KERN_INFO,
				"[I]ktz8866 B reading  0x%02x device id is 0x%02x\n",
				(unsigned int)KTZ8866_DISP_ID, device_id);
	} else {
		g_client = i2c;
		g_ktz8866_led.hbm_enabled = of_property_read_bool(i2c->dev.of_node,
				"ktz8866,backlight-HBM-enable");
		device_id = i2c_smbus_read_byte_data(i2c, KTZ8866_DISP_ID);
		if (device_id < 0) {
			mi_disp_printk(KERN_ERR,
					"[E]ktz8866 i2c failed reading at 0x%02x\n",
					(unsigned int)KTZ8866_DISP_ID);
			device_id = 0;
		}
		mi_disp_printk(KERN_INFO,
				"[I]ktz8866 A reading 0x%02x device id is 0x%02x\n",
				(unsigned int)KTZ8866_DISP_ID, device_id);
	}


	mi_disp_printk(KERN_INFO, "[I]ktz8866 init success\n");

	return 0;

out:
	return -ENODEV;
}

static int ktz8866_remove(struct i2c_client *i2c)
{
	struct dsi_display *display;
	struct dsi_panel *panel;

	display = mi_get_primary_dsi_display();
	if (!display || !display->panel) {
		mi_disp_printk(KERN_ERR, "[E]invalid dsi_display or dsi_panel ptr\n");
		return -EINVAL;
	}

	panel = display->panel;
	ktz8866_backlight_update_status(panel, 0);

	return 0;
}

static const struct i2c_device_id ktz8866_ids[] = {
	{ "ktz8866", 0 },
	{ "ktz8866b", 1 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ktz8866_ids);

static const struct of_device_id ktz8866_match_table[] = {
	{ .compatible = "ktz,ktz8866", },
	{ .compatible = "ktz,ktz8866b", },
	{ }
};

static struct i2c_driver ktz8866_driver = {
	.driver = {
		.name = "ktz8866",
		.of_match_table = ktz8866_match_table,
	},
	.probe = ktz8866_probe,
	.remove = ktz8866_remove,
	.id_table = ktz8866_ids,
};

int mi_backlight_ktz8866_init(void)
{
	int ret;

	mutex_init(&g_ktz8866_led.lock);
	ret = i2c_register_driver(THIS_MODULE, &ktz8866_driver);

	return ret;
}

void mi_backlight_ktz8866_deinit(void)
{
	i2c_del_driver(&ktz8866_driver);
	g_client = NULL;
	g_clientb = NULL;
}
