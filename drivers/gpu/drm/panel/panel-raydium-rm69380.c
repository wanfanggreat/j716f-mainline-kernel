// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree.
 * Copyright (c) 2024 David Wronek <david@mainlining.org>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct rm69380_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi[2];
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	/* Last brightness written; see rm69380_bl_get_brightness(). */
	u16 brightness;
};

static inline
struct rm69380_panel *to_rm69380_panel(struct drm_panel *panel)
{
	return container_of(panel, struct rm69380_panel, panel);
}

static void rm69380_reset(struct rm69380_panel *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(15000, 16000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(30);
}

/*
 * Send one DCS command to *both* DSI links.
 *
 * This panel is dual-DSI: the RM69380 drives two 1280x1600 halves, each with
 * its own DSI link. Both controllers must be initialised, otherwise the half
 * attached to the un-initialised link never lights up. The generated driver
 * only ever wrote to ctx->dsi[0], so dsi[1] was registered, attached and had
 * its mode_flags tweaked -- but never received a single init command.
 *
 * Same approach as the mainline-verified dual-DSI panel driver
 * (panel-novatek-nt36523.c: mipi_dsi_dual_dcs_write_seq()).
 */
#define rm69380_dual_write_seq(ctx0, ctx1, cmd, seq...)			\
	do {								\
		mipi_dsi_dcs_write_seq_multi(ctx0, cmd, seq);		\
		if (ctx1)						\
			mipi_dsi_dcs_write_seq_multi(ctx1, cmd, seq);	\
	} while (0)

#define rm69380_dual_tear_on(ctx0, ctx1, mode)				\
	do {								\
		mipi_dsi_dcs_set_tear_on_multi(ctx0, mode);		\
		if (ctx1)						\
			mipi_dsi_dcs_set_tear_on_multi(ctx1, mode);	\
	} while (0)

#define rm69380_dual_exit_sleep(ctx0, ctx1)				\
	do {								\
		mipi_dsi_dcs_exit_sleep_mode_multi(ctx0);		\
		if (ctx1)						\
			mipi_dsi_dcs_exit_sleep_mode_multi(ctx1);	\
	} while (0)

#define rm69380_dual_display_on(ctx0, ctx1)				\
	do {								\
		mipi_dsi_dcs_set_display_on_multi(ctx0);		\
		if (ctx1)						\
			mipi_dsi_dcs_set_display_on_multi(ctx1);	\
	} while (0)

static int rm69380_on(struct rm69380_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	/* second link; NULL when the panel is driven single-DSI */
	struct mipi_dsi_multi_context dsi1_ctx = { .dsi = ctx->dsi[1] };
	struct mipi_dsi_multi_context *c1 = ctx->dsi[1] ? &dsi1_ctx : NULL;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	if (ctx->dsi[1])
		ctx->dsi[1]->mode_flags |= MIPI_DSI_MODE_LPM;

	rm69380_dual_write_seq(&dsi_ctx, c1, 0xfe, 0xd4);
	rm69380_dual_write_seq(&dsi_ctx, c1, 0x00, 0x80);
	rm69380_dual_write_seq(&dsi_ctx, c1, 0xfe, 0xd0);
	rm69380_dual_write_seq(&dsi_ctx, c1, 0x48, 0x00);

	/*
	 * Page-0x26 settings. The vendor sends these to the PRIMARY link only;
	 * its "second" panel node omits exactly this group and is otherwise
	 * identical (verified by decoding both blobs from the device's dtbo).
	 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x26);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x75, 0x3f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1d, 0x1a);

	rm69380_dual_write_seq(&dsi_ctx, c1, 0xfe, 0x00);
	rm69380_dual_write_seq(&dsi_ctx, c1, MIPI_DCS_WRITE_CONTROL_DISPLAY,
			       0x28);
	rm69380_dual_write_seq(&dsi_ctx, c1, 0xc2, 0x08);

	rm69380_dual_tear_on(&dsi_ctx, c1, MIPI_DSI_DCS_TEAR_MODE_VBLANK);

	/*
	 * Set the initial brightness, as the vendor sequence does:
	 *     39 01 00 00 00 00 03 51 07 ff
	 * i.e. DCS 0x51 (set_display_brightness) with the 16-bit value 0x07ff
	 * = 2047 = max. Written as a raw sequence because only the 8-bit
	 * _multi helper exists (mipi_dsi_dcs_set_display_brightness_multi).
	 *
	 * The generated driver omitted this, and nothing else sets it either:
	 * backlight_update_status() only runs on a sysfs write or on
	 * register/resume, so the panel came up with its brightness register
	 * at 0 and the AMOLED stayed black even though the DSI link was
	 * streaming correctly. The mainline-verified SM8250 panel driver sets
	 * an initial brightness in its enable path too
	 * (panel-novatek-nt36523.c: mipi_dsi_dcs_set_display_brightness(dsi, 18)).
	 */
	rm69380_dual_write_seq(&dsi_ctx, c1, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
			       0x07, 0xff);

	rm69380_dual_exit_sleep(&dsi_ctx, c1);
	mipi_dsi_msleep(&dsi_ctx, 20);
	if (c1)
		mipi_dsi_msleep(c1, 20);

	rm69380_dual_display_on(&dsi_ctx, c1);
	mipi_dsi_msleep(&dsi_ctx, 36);
	if (c1)
		mipi_dsi_msleep(c1, 36);

	return dsi_ctx.accum_err ? dsi_ctx.accum_err : (c1 ? dsi1_ctx.accum_err : 0);
}

static void rm69380_off(struct rm69380_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi[0];
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };
	struct mipi_dsi_multi_context dsi1_ctx = { .dsi = ctx->dsi[1] };
	struct mipi_dsi_multi_context *c1 = ctx->dsi[1] ? &dsi1_ctx : NULL;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	if (ctx->dsi[1])
		ctx->dsi[1]->mode_flags &= ~MIPI_DSI_MODE_LPM;

	/* both links must be taken down, mirroring rm69380_on() */
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	if (c1)
		mipi_dsi_dcs_set_display_off_multi(c1);
	mipi_dsi_msleep(&dsi_ctx, 35);
	if (c1)
		mipi_dsi_msleep(c1, 35);

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	if (c1)
		mipi_dsi_dcs_enter_sleep_mode_multi(c1);
	mipi_dsi_msleep(&dsi_ctx, 20);
	if (c1)
		mipi_dsi_msleep(c1, 20);
}

static int rm69380_prepare(struct drm_panel *panel)
{
	struct rm69380_panel *ctx = to_rm69380_panel(panel);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(&ctx->dsi[0]->dev,
			"failed to enable regulators: %d\n", ret);
		return ret;
	}

	rm69380_reset(ctx);

	ret = rm69380_on(ctx);
	if (ret < 0) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	}

	return ret;
}

static int rm69380_unprepare(struct drm_panel *panel)
{
	struct rm69380_panel *ctx = to_rm69380_panel(panel);

	rm69380_off(ctx);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

/*
 * Dual-DSI timing. The panel is two 1280x1600 links driven as one 2560x1600
 * surface, and msm halves the horizontal timings before programming each link
 * (dsi_calc_pclk() -> pclk = htotal/2 * vtotal * fps for bonded DSI).
 *
 * The horizontal porches must therefore be expressed for the *full* width, i.e.
 * doubled, so that the halving reproduces the panel's per-link values from the
 * vendor DT (h-front-porch 32, h-pulse-width 12, h-back-porch 38):
 *
 *     htotal 2724 / 2 = 1362 = 1280 + 32 + 12 + 38   (vendor, per link)
 *     pclk = 2724 * 1632 * 90 / 2 = 200.05 MHz       (vendor, per link)
 *
 * Using undoubled porches (htotal 2642) yields only 1321 per link and a
 * 194.03 MHz pixel clock -- 3% low. Measured on the device, that is exactly
 * what msm programmed ("dsi_calc_pclk pclk=194028000"), and the panel never
 * locked: the DPU enabled its CRTC, the DSI PHY came up and the PLL locked,
 * but no frames appeared. Note the vertical porches are NOT split, so they stay
 * at the vendor values.
 *
 * This matches the convention used by the working dual-DSI reference
 * (sm8250-xiaomi-elish: per-link mode with matching per-link porches).
 */
static const struct drm_display_mode rm69380_mode = {
	.clock = (2560 + 64 + 24 + 76) * (1600 + 20 + 4 + 8) * 90 / 1000,
	.hdisplay = 2560,
	.hsync_start = 2560 + 64,
	.hsync_end = 2560 + 64 + 24,
	.htotal = 2560 + 64 + 24 + 76,
	.vdisplay = 1600,
	.vsync_start = 1600 + 20,
	.vsync_end = 1600 + 20 + 4,
	.vtotal = 1600 + 20 + 4 + 8,
	.width_mm = 248,
	.height_mm = 155,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int rm69380_get_modes(struct drm_panel *panel,
					struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &rm69380_mode);
}

static const struct drm_panel_funcs rm69380_panel_funcs = {
	.prepare = rm69380_prepare,
	.unprepare = rm69380_unprepare,
	.get_modes = rm69380_get_modes,
};

static int rm69380_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct rm69380_panel *ctx = mipi_dsi_get_drvdata(dsi);
	u16 brightness = backlight_get_brightness(bl);
	unsigned long mode_flags[2];
	int i, ret = 0;

	if (!ctx)
		return -ENODEV;

	/*
	 * In MSM bonded sync mode a command on DSI_0 returns success without
	 * transmitting. DSI_1 triggers the same payload on both controllers,
	 * so brightness must follow the dual-link initialization sequence.
	 */
	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		if (!ctx->dsi[i])
			continue;
		mode_flags[i] = ctx->dsi[i]->mode_flags;
		ctx->dsi[i]->mode_flags &= ~MIPI_DSI_MODE_LPM;
	}

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		if (!ctx->dsi[i])
			continue;
		ret = mipi_dsi_dcs_set_display_brightness_large(ctx->dsi[i],
							 brightness);
		if (ret < 0)
			break;
	}

	/* Restore each link's previous transfer mode, including on failure. */
	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++)
		if (ctx->dsi[i])
			ctx->dsi[i]->mode_flags = mode_flags[i];
	if (ret < 0)
		return ret;

	ctx->brightness = brightness;

	return 0;
}

static int rm69380_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct rm69380_panel *ctx = mipi_dsi_get_drvdata(dsi);

	/*
	 * Return the cached value rather than issuing DCS 0x52.
	 *
	 * This panel does not answer get_display_brightness_large: every read
	 * logs "msm_dsi_host_cmd_rx:Invalid response cmd" and fails, so
	 * actual_brightness reads back as -ENODATA and GNOME hides the
	 * brightness slider entirely.
	 */
	if (ctx)
		return ctx->brightness;

	return 0;
}

static const struct backlight_ops rm69380_bl_ops = {
	.update_status = rm69380_bl_update_status,
	.get_brightness = rm69380_bl_get_brightness,
};

static struct backlight_device *
rm69380_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 511,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &rm69380_bl_ops, &props);
}

static int rm69380_probe(struct mipi_dsi_device *dsi)
{
	struct mipi_dsi_host *dsi_sec_host;
	struct rm69380_panel *ctx;
	struct device *dev = &dsi->dev;
	struct device_node *dsi_sec;
	int ret, i;

	ctx = devm_drm_panel_alloc(dev, struct rm69380_panel, panel,
				   &rm69380_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "avdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies),
				      ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	dsi_sec = of_graph_get_remote_node(dsi->dev.of_node, 1, -1);

	if (dsi_sec) {
		const struct mipi_dsi_device_info info = { "RM69380 DSI1", 0,
							   dsi_sec };

		dsi_sec_host = of_find_mipi_dsi_host_by_node(dsi_sec);
		of_node_put(dsi_sec);
		if (!dsi_sec_host)
			return dev_err_probe(dev, -EPROBE_DEFER,
					     "Cannot get secondary DSI host\n");

		ctx->dsi[1] =
			devm_mipi_dsi_device_register_full(dev, dsi_sec_host, &info);
		if (IS_ERR(ctx->dsi[1]))
			return dev_err_probe(dev, PTR_ERR(ctx->dsi[1]),
					     "Cannot get secondary DSI node\n");

		mipi_dsi_set_drvdata(ctx->dsi[1], ctx);
	}

	ctx->dsi[0] = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = rm69380_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	for (i = 0; i < ARRAY_SIZE(ctx->dsi); i++) {
		if (!ctx->dsi[i])
			continue;

		dev_dbg(&ctx->dsi[i]->dev, "Binding DSI %d\n", i);

		ctx->dsi[i]->lanes = 4;
		ctx->dsi[i]->format = MIPI_DSI_FMT_RGB888;
		/*
		 * Command burst mode: MIPI_DSI_MODE_VIDEO_BURST |
		 * MIPI_DSI_CLOCK_NON_CONTINUOUS (no MIPI_DSI_MODE_VIDEO).
		 *
		 * msm_dsi_is_cmd_mode() tests !(host_flags & MIPI_DSI_MODE_VIDEO),
		 * so omitting MIPI_DSI_MODE_VIDEO selects dpu_encoder_phys_cmd,
		 * which uses TE from gpio66 (wired to mdp_vsync in the DTS) to
		 * pace kickoffs.
		 *
		 * This matches the upstream driver for lenovo,j716f-edo-rm69380
		 * and the vendor DT's "dsi_cmd_mode" topology.
		 */
		ctx->dsi[i]->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
					  MIPI_DSI_CLOCK_NON_CONTINUOUS;

		ret = devm_mipi_dsi_attach(dev, ctx->dsi[i]);
		if (ret < 0) {
			drm_panel_remove(&ctx->panel);
			return dev_err_probe(dev, ret,
					     "Failed to attach to DSI%d\n", i);
		}
	}

	return 0;
}

static void rm69380_remove(struct mipi_dsi_device *dsi)
{
	struct rm69380_panel *ctx = mipi_dsi_get_drvdata(dsi);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id rm69380_of_match[] = {
	{ .compatible = "lenovo,j716f-edo-rm69380" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rm69380_of_match);

static struct mipi_dsi_driver rm69380_panel_driver = {
	.probe = rm69380_probe,
	.remove = rm69380_remove,
	.driver = {
		.name = "panel-raydium-rm69380",
		.of_match_table = rm69380_of_match,
	},
};
module_mipi_dsi_driver(rm69380_panel_driver);

MODULE_AUTHOR("David Wronek <david@mainlining.org");
MODULE_DESCRIPTION("DRM driver for Raydium RM69380-equipped DSI panels");
MODULE_LICENSE("GPL");
