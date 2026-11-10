// SPDX-License-Identifier: GPL-2.0-only
//
// Qualcomm PMIC VBUS output regulator driver
//
// Copyright (c) 2020, The Linux Foundation. All rights reserved.

#include <linux/module.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regmap.h>

#define CMD_OTG				0x40
#define OTG_EN				BIT(0)
#define OTG_CURRENT_LIMIT_CFG		0x52
#define OTG_CURRENT_LIMIT_MASK		GENMASK(2, 0)
#define OTG_CFG				0x53
#define OTG_EN_SRC_CFG			BIT(1)
#define OTG_DIS_OTG_ON_TLIM		BIT(5)
#define OTG_ENG_OTG_CFG			0xC0
#define OTG_ENG_BUCKBOOST_HALT1_8_MODE	BIT(0)
#define OTG_FAULT_CONDITION_CFG		0x56
#define OTG_USBIN_MID_COMP_FAULT_EN	BIT(5)
#define OTG_USBIN_COLLAPSE_FAULT_EN	BIT(4)
#define DCDC_FSW_SEL			0x50
#define DCDC_FSW_800KHZ			0x0b

#define PM4125_VBOOST_EN		0x50
#define PM4125_VBOOST_SEL		0x52
#define PM4125_VBOOST_CFG_MASK		GENMASK(1, 0)
#define PM4125_VBOOST_CFG		0x56
#define PM4125_VBOOST_EN_SRC_CFG	BIT(0)

struct qcom_usb_vbus_reg_data {
	u16 cmd_otg;
	u16 otg_cfg;
	u8  otg_en_src_cfg;
	u8  otg_dis_tlim_mask;
	u16 csel_reg;
	u8 csel_mask;
	u16 otg_eng_cfg;
	u8 otg_eng_halt_mask;
	u16 otg_fault_cfg;
	u8 otg_fault_mask;
	const unsigned int *curr_table;
	unsigned int n_current_limits;
	u16 vsel_reg;
	u8 vsel_mask;
	const unsigned int *volt_table;
	unsigned int n_voltages;
	const struct regulator_ops *ops;
};

static const unsigned int curr_table[] = {
	500000, 1000000, 1500000, 2000000, 2500000, 3000000,
};

static const unsigned int pm4125_vboost_table[] = {
	4250000, 4500000, 4750000, 5000000,
};

static const struct regulator_ops qcom_usb_vbus_reg_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_current_limit = regulator_get_current_limit_regmap,
	.set_current_limit = regulator_set_current_limit_regmap,
};

static const struct qcom_usb_vbus_reg_data pm8150b_data = {
	.cmd_otg = CMD_OTG,
	.otg_cfg = OTG_CFG,
	.otg_en_src_cfg = OTG_EN_SRC_CFG,
	.otg_dis_tlim_mask = OTG_DIS_OTG_ON_TLIM,
	.csel_reg = OTG_CURRENT_LIMIT_CFG,
	.csel_mask = OTG_CURRENT_LIMIT_MASK,
	.otg_eng_cfg = OTG_ENG_OTG_CFG,
	.otg_eng_halt_mask = OTG_ENG_BUCKBOOST_HALT1_8_MODE,
	.otg_fault_cfg = OTG_FAULT_CONDITION_CFG,
	.otg_fault_mask = OTG_USBIN_MID_COMP_FAULT_EN |
			 OTG_USBIN_COLLAPSE_FAULT_EN,
	.curr_table = curr_table,
	.n_current_limits = ARRAY_SIZE(curr_table),
	.ops = &qcom_usb_vbus_reg_ops,
};

static const struct regulator_ops qcom_usb_vbus_pm4125_reg_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.list_voltage = regulator_list_voltage_table,
};

static const struct qcom_usb_vbus_reg_data pm4125_data = {
	.cmd_otg = PM4125_VBOOST_EN,
	.otg_cfg = PM4125_VBOOST_CFG,
	.otg_en_src_cfg = PM4125_VBOOST_EN_SRC_CFG,
	.vsel_reg = PM4125_VBOOST_SEL,
	.vsel_mask = PM4125_VBOOST_CFG_MASK,
	.volt_table = pm4125_vboost_table,
	.n_voltages = ARRAY_SIZE(pm4125_vboost_table),
	.ops = &qcom_usb_vbus_pm4125_reg_ops,
};

static int qcom_usb_vbus_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct qcom_usb_vbus_reg_data *data;
	struct regulator_dev *rdev;
	struct regulator_desc *rdesc;
	struct regmap *regmap;
	struct regulator_config config = { };
	struct regulator_init_data *init_data;
	int ret;
	u32 base;

	ret = of_property_read_u32(dev->of_node, "reg", &base);
	if (ret < 0) {
		dev_err(dev, "no base address found\n");
		return ret;
	}

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap) {
		dev_err(dev, "Failed to get regmap\n");
		return -ENOENT;
	}

	rdesc = devm_kzalloc(dev, sizeof(*rdesc), GFP_KERNEL);
	if (!rdesc)
		return -ENOMEM;

	rdesc->name = "usb_vbus";
	rdesc->ops = data->ops;
	rdesc->owner = THIS_MODULE;
	rdesc->type = REGULATOR_VOLTAGE;
	rdesc->enable_reg = base + data->cmd_otg;
	rdesc->enable_mask = OTG_EN;

	if (data->curr_table) {
		rdesc->curr_table = data->curr_table;
		rdesc->n_current_limits = data->n_current_limits;
		rdesc->csel_reg = base + data->csel_reg;
		rdesc->csel_mask = data->csel_mask;
	}

	if (data->volt_table) {
		rdesc->volt_table = data->volt_table;
		rdesc->n_voltages = data->n_voltages;
		rdesc->vsel_reg = base + data->vsel_reg;
		rdesc->vsel_mask = data->vsel_mask;
	}

	init_data = of_get_regulator_init_data(dev, dev->of_node, rdesc);
	if (!init_data)
		return -ENOMEM;

	config.dev = dev;
	config.init_data = init_data;
	config.of_node = dev->of_node;
	config.regmap = regmap;

	rdev = devm_regulator_register(dev, rdesc, &config);
	if (IS_ERR(rdev)) {
		ret = PTR_ERR(rdev);
		dev_err(dev, "not able to register vbus reg %d\n", ret);
		return ret;
	}

	/* Disable HW logic for VBUS enable */
	regmap_update_bits(regmap, base + data->otg_cfg, data->otg_en_src_cfg, 0);

	/*
	 * Downstream SMB5 sets the charger switching frequency to 800 kHz
	 * before enabling OTG.  PM8150B powers up at a different frequency;
	 * select the same value the vendor charger uses for host mode.
	 */
	if (data == &pm8150b_data)
		regmap_write(regmap, base + DCDC_FSW_SEL, DCDC_FSW_800KHZ);

	/*
	 * PM8150B powers up with DIS_OTG_ON_TLIM set.  The downstream SMB5
	 * charger clears this before enabling the boost; without it the OTG
	 * command bit is set but no 5 V appears on the connector.
	 */
	if (data->otg_dis_tlim_mask)
		regmap_update_bits(regmap, base + data->otg_cfg,
				   data->otg_dis_tlim_mask, 0);

	/*
	 * The downstream SMB5 charger selects the buckboost halt-1/8 mode
	 * before enabling OTG.  PM8150B powers up with this bit clear and the
	 * boost does not start reliably without it.
	 */
	if (data->otg_eng_cfg)
		regmap_update_bits(regmap, base + data->otg_eng_cfg,
				   data->otg_eng_halt_mask,
				   data->otg_eng_halt_mask);

	/*
	 * The downstream SMB5 charger disables the USBIN mid-comp and collapse
	 * fault responses while acting as a Type-C source.  PM8150B powers up
	 * with both enabled; leaving them enabled keeps the OTG boost off.
	 */
	if (data->otg_fault_cfg)
		regmap_update_bits(regmap, base + data->otg_fault_cfg,
				   data->otg_fault_mask, 0);

	return 0;
}

static const struct of_device_id qcom_usb_vbus_regulator_match[] = {
	{ .compatible = "qcom,pm8150b-vbus-reg", .data = &pm8150b_data },
	{ .compatible = "qcom,pm4125-vbus-reg",  .data = &pm4125_data },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_usb_vbus_regulator_match);

static struct platform_driver qcom_usb_vbus_regulator_driver = {
	.driver		= {
		.name	= "qcom-usb-vbus-regulator",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = qcom_usb_vbus_regulator_match,
	},
	.probe		= qcom_usb_vbus_regulator_probe,
};
module_platform_driver(qcom_usb_vbus_regulator_driver);

MODULE_DESCRIPTION("Qualcomm USB vbus regulator driver");
MODULE_LICENSE("GPL v2");
