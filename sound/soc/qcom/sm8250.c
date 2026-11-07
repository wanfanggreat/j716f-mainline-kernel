// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2020, Linaro Limited

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
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
	int dmic_en_state;
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
static int sm8250_dmic_en_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = data->dmic_en_state;

	return 0;
}

static int sm8250_dmic_en_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(card);
	int on = !!ucontrol->value.integer.value[0];

	if (!data->dmic_en_gpio)
		return 0;

	if (on == data->dmic_en_state)
		return 0;

	gpiod_set_value_cansleep(data->dmic_en_gpio, on);
	data->dmic_en_state = on;
	/*
	 * The vendor waits 5 ms here for the rail to settle before letting
	 * DAPM re-evaluate the widgets that depend on it.
	 */
	usleep_range(5000, 5100);
	snd_soc_dapm_sync(card->dapm);

	dev_dbg(card->dev, "J716F: DMIC EN -> %d\n", on);

	return 0;
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
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	/*
	 * Lenovo J716F: the tablet's microphones are the LPASS digital MEMS
	 * mics, captured through the TX macro on TX_CODEC_DMA_TX_3 (the path
	 * the stock Android recorder uses). Those AFE ports need an explicit
	 * TX channel mask, otherwise q6dma_set_channel_map() returns -EINVAL
	 * ("tx slot not found") and the DSP never starts the port. The mask
	 * has to be set on the backend DAI (the AFE port), not the front end.
	 */
	if (cpu_dai->driver->ops &&
	    cpu_dai->driver->ops->set_channel_map &&
	    ((cpu_dai->id >= VA_CODEC_DMA_TX_0 &&
	      cpu_dai->id <= VA_CODEC_DMA_TX_2) ||
	     (cpu_dai->id >= TX_CODEC_DMA_TX_0 &&
	      cpu_dai->id <= TX_CODEC_DMA_TX_5))) {
		/*
		 * q6dma_set_channel_map() reads only tx_ch_mask[0] and uses it
		 * verbatim as active_channels_mask, so the array element must be
		 * a bitmask, not a channel index. 0x3 enables channels 0 and 1;
		 * passing 0 here (as an index list would) leaves the AFE port
		 * with no active channels, which makes the DSP send EOS instead
		 * of data.
		 */
		unsigned int tx_ch = 0x3;
		int __r = snd_soc_dai_set_channel_map(cpu_dai, 2, &tx_ch, 0, NULL);

		if (__r)
			dev_warn(rtd->card->dev,
				 "J716F-CHMAP: dai_id=%d set_channel_map rc=%d\n",
				 cpu_dai->id, __r);
		else
			pr_debug("J716F-CHMAP: dai_id=%d set_channel_map rc=%d\n",
				 cpu_dai->id, __r);
	}

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
	int i, ret;

	pr_debug("J716F-CLK: sm8250_snd_startup called\n");

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
	if (cpu_dai->id == TX_CODEC_DMA_TX_3 || cpu_dai->id == VA_CODEC_DMA_TX_0) {
		/*
		 * Select the internal microphone path first, then power the
		 * rail, so the mics are connected when the DMIC clock starts.
		 */
		if (data->dmic_sel_gpio)
			gpiod_set_value_cansleep(data->dmic_sel_gpio, 1);
		if (data->dmic_en_gpio) {
			gpiod_set_value_cansleep(data->dmic_en_gpio, 1);
			/* let the rail settle before the clock starts */
			usleep_range(2000, 2500);
		}
		pr_debug("J716F-DMIC: power on (dai id %d)\n", cpu_dai->id);
	}

	switch (cpu_dai->id) {
	case PRIMARY_MI2S_RX:
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
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
	/*
	 * Lenovo J716F: the primary MI2S also carries the ES7210 microphone
	 * ADC on its capture direction. Without a TX case here the capture
	 * backend never gets its bit clock configured and recording fails.
	 */
	case PRIMARY_MI2S_TX:
		pr_debug("J716F-CLK: PRIMARY_MI2S_TX startup, setting IBIT %u\n",
			MI2S_BCLK_RATE);
		codec_dai_fmt |= SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_I2S;
		/*
		 * Lenovo J716F: the ES7210 microphone ADC is clocked from the
		 * I2S bit clock. The vendor driver's coeff_div[] table programs
		 * it for MCLK = 256 x Fs (12.288 MHz at 48 kHz, lrck divider
		 * 0x0100), so the capture direction needs that rate rather than
		 * the 32 x Fs rate the speaker amplifiers use. With the slow
		 * clock the chip is configured correctly, the DAPM path comes up
		 * and the registers release, but the ADC cannot produce samples,
		 * so every recording is silence.
		 */
		/*
		 * IBIT only, as upstream sm8250 does.
		 *
		 * The vendor machine driver (audio_machine_kona.ko) programs both
		 * IBIT and EBIT at 1.536 MHz here; its tables were read out of
		 * the module and are:
		 *
		 *   .data  mi2s_clk[]       { 0x100, 1536000, 1, 1 }
		 *                           { 0x102, 1536000, 1, 1 }
		 *                           { 0x104, 1536000, 1, 1 }
		 *   .rodata mi2s_ebit_clk[] { 0x101, 0x103, 0x105 }
		 *
		 * Adding the EBIT was tried and makes the capture record **zero
		 * bytes**, so only IBIT stays. Note also that both the vendor and
		 * this driver use 32 x Fs, not the 256 x Fs the ES7210's
		 * coeff_div[] table documents - which is where the microphone
		 * bring-up currently stands.
		 */
		{
			int __rc = snd_soc_dai_set_sysclk(cpu_dai,
				Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
				MI2S_BCLK_RATE, SNDRV_PCM_STREAM_CAPTURE);
			if (__rc)
				dev_warn(rtd->card->dev,
					 "J716F-CLK: set_sysclk IBIT rc=%d\n", __rc);
			else
				pr_debug("J716F-CLK: set_sysclk IBIT rc=%d\n", __rc);
		}
		/*
		 * NOTE: sdm845/sc7180 additionally program
		 * Q6AFE_LPASS_CLK_ID_MCLK_1 here to give the codec a master
		 * clock. That was tried for the ES7210 and the ADSP rejects it
		 * ("AFE set params failed -22"), so the capture port is left
		 * with just the bit clock.
		 */
		snd_soc_dai_set_fmt(cpu_dai, fmt);
		snd_soc_dai_set_fmt(codec_dai, codec_dai_fmt);
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

	return qcom_snd_sdw_startup(substream);
}

static void sm8250_snd_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct sm8250_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	if (data->dmic_en_gpio &&
	    (cpu_dai->id == TX_CODEC_DMA_TX_3 ||
	     cpu_dai->id == VA_CODEC_DMA_TX_0)) {
		gpiod_set_value_cansleep(data->dmic_en_gpio, 0);
		pr_debug("J716F-DMIC: power off (dai id %d)\n", cpu_dai->id);
	}

	qcom_snd_sdw_shutdown(substream);
}

static int sm8250_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai;
	const unsigned int left_slots[] = { 0, 1 };
	const unsigned int right_slots[] = { 1, 0 };
	int i, ret;

	(void)params;

	/*
	 * Lenovo J716F: the CS35L41 amplifiers take their PLL reference from
	 * the I2S bit clock.  Program the component side (PLL source and
	 * divider) and the ASP channel map for every amplifier on the link.
	 * 0x40/0x43 are on the left, 0x41/0x42 on the right.
	 */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		const unsigned int *rx_slots = left_slots;
		u32 address;

		if (!strstr(codec_dai->name, "cs35l41"))
			continue;

		if (of_machine_is_compatible("lenovo,j716f")) {
			ret = of_property_read_u32(codec_dai->component->dev->of_node,
						   "reg", &address);
			if (ret)
				return ret;
			switch (address) {
			case 0x40:
			case 0x43:
				break;
			case 0x41:
			case 0x42:
				rx_slots = right_slots;
				break;
			default:
				return -EINVAL;
			}
		}

		ret = snd_soc_dai_set_channel_map(codec_dai, 0, NULL,
						  ARRAY_SIZE(left_slots),
						  rx_slots);
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
	.hw_params = sm8250_snd_hw_params,
	.hw_free = sm8250_snd_hw_free,
	.prepare = sm8250_snd_prepare,
};

static void sm8250_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->init = sm8250_snd_init;
			link->exit = sm8250_snd_exit;
			link->be_hw_params_fixup = sm8250_be_hw_params_fixup;
			link->ops = &sm8250_be_ops;
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
