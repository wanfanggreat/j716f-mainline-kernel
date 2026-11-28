// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/cs35l41.h>
#include <linux/soundwire/sdw.h>
#include <sound/jack.h>
#include <linux/input-event-codes.h>
#include "qdsp6/q6afe.h"
#include "common.h"
#include "usb_offload_utils.h"
#include "sdw.h"

#define MI2S_BCLK_RATE		1536000

struct sm8250_snd_data {
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_card *card;
	struct snd_soc_jack jack;
	struct snd_soc_jack usb_offload_jack;
	bool usb_offload_jack_setup;
	struct snd_soc_jack dp_jack;
	bool jack_setup;
	/*
	 * Lenovo J716F: the digital microphones are powered through a PMIC
	 * gpio, exactly as the vendor dtbo declares:
	 *
	 *   qcom,msm-dmic-en-gpio = <&pm8150b_gpios 12 GPIO_ACTIVE_LOW>;
	 *
	 * The vendor machine driver toggles it from msm_dmic_event() while
	 * the DMIC backend is up. mainline has no equivalent, so the rail
	 * stayed off and the DMIC clock pads never toggled even with every
	 * macro register programmed correctly.
	 */
	struct gpio_desc *dmic_en_gpio;
	/* GPIO level currently driven */
	bool dmic_en_state;
	/* Capture backends currently holding the shared rail */
	int dmic_users;
	/* "DMIC EN" mixer-control request */
	bool dmic_manual;
	struct mutex dmic_lock;
	/*
	 * Lenovo J716F: PM8150B GPIO1 selects the internal microphone path
	 * (vendor: qcom,msm-dmic-sel-gpio = <&pm8150b_gpios 1
	 * GPIO_ACTIVE_HIGH>, driven by the "DMIC SEL" mixer control in the
	 * Android HAL).  mainline has no consumer for it; without it the
	 * on-board DMICs are not connected to the SoC.
	 */
	struct gpio_desc *dmic_sel_gpio;
};

/*
 * Lenovo J716F: "DMIC EN" mixer control, mirroring the vendor machine
 * driver's msm_mic_en_ctrl / msm_mic_en_get / msm_mic_en_put trio.
 *
 * The vendor exposes the digital-microphone power rail as a mixer switch
 * rather than driving it from a DAPM event, so Android's audio HAL can
 * turn it on right before it starts the DMIC backend. Its put callback
 * does, per the disassembly of msm_mic_en_put (machine_dlkm.ko 0x4aa8):
 *
 *   gpiod_direction_output_raw(gpio, control == 1);   // ACTIVE_LOW
 *   usleep_range(5000, 5010);
 *   snd_soc_dapm_sync(dapm);
 *
 * We keep the same shape so userspace sees the control it expects.
 */
/*
 * Change the shared rail level from the refcount and the manual control.
 * Return true when the GPIO level changed; the caller does the settle
 * delay and any DAPM sync outside dmic_lock.
 */
static bool sm8250_dmic_refresh(struct sm8250_snd_data *data)
{
	bool on = data->dmic_users > 0 || data->dmic_manual;

	if (!data->dmic_en_gpio || on == data->dmic_en_state)
		return false;

	gpiod_set_value_cansleep(data->dmic_en_gpio, on);
	data->dmic_en_state = on;
	dev_dbg(data->card->dev, "J716F: DMIC EN -> %d\n", on);

	return true;
}

/*
 * The GPIO transition and its settle delay are a single mutex-protected
 * operation so a second capture backend cannot start before the rail is
 * stable.  Only snd_soc_dapm_sync() is called outside the lock.
 */
static void sm8250_dmic_get(struct sm8250_snd_data *data)
{
	bool powered;

	mutex_lock(&data->dmic_lock);
	data->dmic_users++;
	powered = sm8250_dmic_refresh(data);
	if (powered)
		usleep_range(5000, 5100);
	mutex_unlock(&data->dmic_lock);
}

static void sm8250_dmic_put(struct sm8250_snd_data *data)
{
	bool powered;

	mutex_lock(&data->dmic_lock);
	if (data->dmic_users > 0)
		data->dmic_users--;
	powered = sm8250_dmic_refresh(data);
	if (powered)
		usleep_range(5000, 5100);
	mutex_unlock(&data->dmic_lock);
}

static int sm8250_dmic_en_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	int value;

	mutex_lock(&data->dmic_lock);
	value = data->dmic_manual;
	mutex_unlock(&data->dmic_lock);

	ucontrol->value.integer.value[0] = value;

	return 0;
}

static int sm8250_dmic_en_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	int on = !!ucontrol->value.integer.value[0];
	bool changed, gpio_changed;

	if (!data->dmic_en_gpio)
		return 0;

	mutex_lock(&data->dmic_lock);
	changed = data->dmic_manual != on;
	data->dmic_manual = on;
	gpio_changed = sm8250_dmic_refresh(data);
	if (gpio_changed)
		usleep_range(5000, 5100);
	mutex_unlock(&data->dmic_lock);

	/*
	 * Keep the DAPM sync outside dmic_lock: it can re-enter the card's
	 * control/DAPM paths.  The GPIO transition and settle delay stay
	 * inside the lock so a concurrent capture start waits for the rail.
	 */
	if (changed)
		snd_soc_dapm_sync(snd_soc_card_to_dapm(card));

	/* ALSA kcontrol put returns 1 when the value actually changed. */
	return changed ? 1 : 0;
}

static const struct snd_kcontrol_new sm8250_dmic_en_control =
	SOC_SINGLE_BOOL_EXT("DMIC EN", 0,
			    sm8250_dmic_en_get, sm8250_dmic_en_put);

static int sm8250_snd_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case DISPLAY_PORT_RX:
		return qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);
	case USB_RX:
		return qcom_snd_usb_offload_jack_setup(rtd, &data->usb_offload_jack,
						       &data->usb_offload_jack_setup);
	default:
		return qcom_snd_wcd_jack_setup(rtd, &data->jack, &data->jack_setup);
	}
}

static void sm8250_snd_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (cpu_dai->id == USB_RX)
		qcom_snd_usb_offload_jack_remove(rtd,
						 &data->usb_offload_jack_setup);

}

static int sm8250_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8250_snd_startup(struct snd_pcm_substream *substream)
{
	unsigned int fmt = SND_SOC_DAIFMT_BP_FP;
	unsigned int codec_dai_fmt = SND_SOC_DAIFMT_BC_FC;
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	bool dmic_ref = false;
	int i, ret;

	/*
	 * Lenovo J716F: power the digital microphones for the capture
	 * backends. The vendor driver does this in msm_dmic_event() with
	 * qcom,msm-dmic-en-gpio = <&pm8150b_gpios 12 GPIO_ACTIVE_LOW>,
	 * i.e. the rail is enabled by driving the line low.
	 *
	 * Without it the DMIC clock pads stay static no matter what the VA
	 * macro registers say, because the microphones themselves are not
	 * powered.
	 */
	if (data->dmic_en_gpio &&
	    (cpu_dai->id == TX_CODEC_DMA_TX_3 ||
	     cpu_dai->id == VA_CODEC_DMA_TX_0)) {
		/*
		 * Select the internal microphone path first, then take a
		 * reference on the shared rail so one backend's shutdown
		 * cannot power off a microphone still used by the other.
		 */
		if (data->dmic_sel_gpio)
			gpiod_set_value_cansleep(data->dmic_sel_gpio, 1);
		sm8250_dmic_get(data);
		dmic_ref = true;
	}

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		/*
		 * Upstream hands the format to the first codec on the link only and
		 * leaves its clock alone; every other board keeps exactly that.
		 */
		if (!of_machine_is_compatible("lenovo,j716f")) {
			snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
			snd_soc_dai_set_fmt(cpu_dai, fmt);
			snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			break;
		}

		ret = snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			return ret;
		ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
		if (ret)
			return ret;
		/*
		 * Lenovo J716F: the CS35L41 speaker amplifiers derive their
		 * internal PLL and FS monitor from the I2S bit clock. Both the
		 * DAI and the component callbacks must be called (this mirrors
		 * sound/soc/intel/boards/sof_cirrus_common.c): the component one
		 * is what selects the PLL source, programs the reference clock
		 * divider and finally enables the PLL. Without it the DAPM path
		 * powers up and the registers look correct, but the amplifier
		 * never locks its PLL and stays silent.
		 */
		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			ret = snd_soc_dai_set_sysclk(codec_dai, CS35L41_CLKID_SCLK,
						   MI2S_BCLK_RATE, SND_SOC_CLOCK_IN);
			if (ret)
				return ret;
			ret = snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
			if (ret)
				return ret;
		}
		break;
	case SECONDARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case TERTIARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case QUINARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_QUI_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case LPI_MI2S_RX_0:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT0_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_PLAYBACK);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	case LPI_MI2S_TX_3:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		snd_soc_dai_set_sysclk(cpu_dai,
			Q6AFE_LPASS_CLK_ID_INT3_MI2S_IBIT,
			MI2S_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
		break;
	default:
		break;
	}

	ret = qcom_snd_sdw_startup(substream);
	if (ret && dmic_ref)
		sm8250_dmic_put(data);

	return ret;
}

static void sm8250_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	if (data->dmic_en_gpio &&
	    (cpu_dai->id == TX_CODEC_DMA_TX_3 ||
	     cpu_dai->id == VA_CODEC_DMA_TX_0)) {
		sm8250_dmic_put(data);
	}

	qcom_snd_sdw_shutdown(substream);
}

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	int i, ret;

	/*
	 * Lenovo J716F: the CS35L41 amplifiers take their PLL reference and
	 * their channel position from the I2S link.  The board describes the
	 * ASP RX slot pair in cirrus,asp-rx-slots; program the DAI clock and
	 * channel map from it, then the component side (PLL source and
	 * divider).
	 *
	 * An amplifier without the property is skipped: its slot pair is not
	 * known, and guessing it from the I2C address only ever worked for the
	 * board that never needed the guess.
	 */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		unsigned int rx_slots[2];

		if (!strstr(codec_dai->name, "cs35l41"))
			continue;

		ret = of_property_read_u32_array(
			codec_dai->component->dev->of_node,
			"cirrus,asp-rx-slots", rx_slots,
			ARRAY_SIZE(rx_slots));
		if (ret)
			continue;

		ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
			ARRAY_SIZE(rx_slots), rx_slots);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_sysclk(codec_dai, CS35L41_CLKID_SCLK,
			MI2S_BCLK_RATE, SND_SOC_CLOCK_IN);
		if (ret)
			return ret;

		ret = snd_soc_component_set_sysclk(codec_dai->component,
			CS35L41_CLKID_SCLK, 0,
			MI2S_BCLK_RATE,
			SND_SOC_CLOCK_IN);
		if (ret)
			return ret;
	}

	return 0;
}

static int sm8250_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream, &data->stream_prepared[cpu_dai->id]);
}

static int sm8250_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	return qcom_snd_sdw_hw_free(substream, &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops sm8250_be_ops = {
	.startup = sm8250_snd_startup,
	.shutdown = sm8250_snd_shutdown,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

/*
 * Lenovo J716F: only this board needs a hw_params callback on the
 * backends, to clock the CS35L41 amplifiers from the I2S bit clock and
 * hand them their slot map.  Every other board keeps the upstream ops
 * without one.
 */
static const struct snd_soc_ops sm8250_j716f_be_ops = {
	.startup = sm8250_snd_startup,
	.shutdown = sm8250_snd_shutdown,
	.hw_params = sm8250_snd_hw_params,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	const struct snd_soc_ops *ops = &sm8250_be_ops;
	struct snd_soc_dai_link *link;
	int i;

	if (of_machine_is_compatible("lenovo,j716f"))
		ops = &sm8250_j716f_be_ops;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = ops;
		}
	}
}

static int sm8250_j716f_speaker_late_probe(struct snd_soc_card *card)
{
	static const char * const pins[] = { "SPK2 SPK", "SPK3 SPK", "SPK4 SPK" };
	int i, ret;

	/* Leave new outputs off until their routing, gain and physical
	 * position have been checked. Amp1 keeps its existing controls.
	 */
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		ret = snd_soc_dapm_disable_pin(card->dapm, pins[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int sm8250_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8250_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	card->owner = THIS_MODULE;
	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->dev = dev;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	mutex_init(&data->dmic_lock);

	/*
	 * Lenovo J716F: optional digital-microphone power enable.  The
	 * vendor exposes it as qcom,msm-dmic-en-gpio; other boards do not
	 * have the property and get a NULL descriptor.
	 */
	data->dmic_en_gpio = devm_gpiod_get_optional(dev, "qcom,msm-dmic-en",
						     GPIOD_OUT_LOW);
	if (IS_ERR(data->dmic_en_gpio)) {
		ret = PTR_ERR(data->dmic_en_gpio);
		dev_err(dev, "failed to get DMIC enable gpio: %d\n", ret);
		return ret;
	}

	/*
	 * Lenovo J716F: internal-microphone path selector.  The vendor HAL
	 * drives it high before it starts a DMIC capture; do the same from
	 * the machine driver so the mainline capture path works too.
	 */
	data->dmic_sel_gpio = devm_gpiod_get_optional(dev, "qcom,msm-dmic-sel",
						      GPIOD_OUT_LOW);
	if (IS_ERR(data->dmic_sel_gpio)) {
		ret = PTR_ERR(data->dmic_sel_gpio);
		dev_err(dev, "failed to get DMIC selector gpio: %d\n", ret);
		return ret;
	}

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	card->driver_name = of_device_get_match_data(dev);

	if (of_machine_is_compatible("lenovo,j716f") &&
	    of_property_match_string(dev->of_node, "pin-switches",
				     "SPK2 SPK") >= 0)
		card->late_probe = sm8250_j716f_speaker_late_probe;

	sm8250_add_be_ops(card);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		return ret;

	/*
	 * The card-level control helpers need soc_card->snd_card, which only
	 * exists after registration.  Expose the DMIC rail under the vendor
	 * control name when the GPIO is present.
	 */
	if (data->dmic_en_gpio) {
		ret = snd_soc_add_card_controls(card, &sm8250_dmic_en_control, 1);
		if (ret)
			dev_warn(dev, "J716F: failed to add DMIC EN control: %d\n",
				 ret);
	}

	return 0;
}

static const struct of_device_id snd_sm8250_dt_match[] = {
	{ .compatible = "fairphone,fp4-sndcard", .data = "sm7225" },
	{ .compatible = "fairphone,fp5-sndcard", .data = "qcm6490" },
	{ .compatible = "qcom,qrb2210-sndcard", .data = "qcm2290" },
	{ .compatible = "qcom,qrb4210-rb2-sndcard", .data = "sm4250" },
	{ .compatible = "qcom,qrb5165-rb5-sndcard", .data = "sm8250" },
	{ .compatible = "qcom,sdm660-sndcard", .data = "sdm660" },
	{ .compatible = "qcom,sm8250-sndcard", .data = "sm8250" },
	{}
};

MODULE_DEVICE_TABLE(of, snd_sm8250_dt_match);

static struct platform_driver snd_sm8250_driver = {
	.probe  = sm8250_platform_probe,
	.driver = {
		.name = "snd-sm8250",
		.of_match_table = snd_sm8250_dt_match,
	},
};
module_platform_driver(snd_sm8250_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("SM8250 ASoC Machine Driver");
MODULE_LICENSE("GPL");
