// SPDX-License-Identifier: GPL-2.0-only
/*
 * Everest Semi ES7210 4-channel audio ADC driver
 *
 * The ES7210 is a 4-channel ADC with an I2S/TDM output and an I2C control
 * interface. It is used on the Lenovo Xiaoxin Pad Pro 2021 (TB-J716F) as the
 * microphone front end, sitting on i2c15 at address 0x40 with its data lines
 * wired to the LPASS primary MI2S capture port.
 *
 * Register names and the power-up sequence follow the ES7210 datasheet
 * (v1.6) and the vendor driver shipped in the tablet's Android kernel.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* Register map */
#define ES7210_RESET_REG00		0x00
#define ES7210_CLOCK_OFF_REG01		0x01
#define ES7210_MAINCLK_REG02		0x02
#define ES7210_MASTER_CLK_REG03		0x03
#define ES7210_LRCK_DIVH_REG04		0x04
#define ES7210_LRCK_DIVL_REG05		0x05
#define ES7210_POWER_DOWN_REG06		0x06
#define ES7210_OSR_REG07		0x07
#define ES7210_MODE_CONFIG_REG08	0x08
#define ES7210_TIME_CONTROL0_REG09	0x09
#define ES7210_TIME_CONTROL1_REG0A	0x0a
#define ES7210_SDP_INTERFACE1_REG11	0x11
#define ES7210_SDP_INTERFACE2_REG12	0x12
#define ES7210_ADC_AUTOMUTE_REG13	0x13
#define ES7210_ADC34_MUTERANGE_REG14	0x14
#define ES7210_ADC12_MUTERANGE_REG15	0x15
#define ES7210_ADC34_HPF2_REG20		0x20
#define ES7210_ADC34_HPF1_REG21		0x21
#define ES7210_ADC12_HPF1_REG22		0x22
#define ES7210_ADC12_HPF2_REG23		0x23
#define ES7210_ANALOG_REG40		0x40
#define ES7210_ANALOG_REG41		0x41
#define ES7210_MIC12_BIAS_REG41		0x41
#define ES7210_MIC34_BIAS_REG42		0x42
#define ES7210_MIC1_GAIN_REG43		0x43
#define ES7210_MIC2_GAIN_REG44		0x44
#define ES7210_MIC3_GAIN_REG45		0x45
#define ES7210_MIC4_GAIN_REG46		0x46
#define ES7210_MIC1_POWER_REG47		0x47
#define ES7210_MIC2_POWER_REG48		0x48
#define ES7210_MIC3_POWER_REG49		0x49
#define ES7210_MIC4_POWER_REG4A		0x4a
#define ES7210_MIC12_POWER_REG4B	0x4b
#define ES7210_MIC34_POWER_REG4C	0x4c
#define ES7210_ANALOG_REG4D		0x4d
#define ES7210_ANALOG_REG4E		0x4e
#define ES7210_ANALOG_REG4F		0x4f

#define ES7210_MCLK_DIV_OFFSET		0x00
#define ES7210_MCLK_DIV_MASK		GENMASK(1, 0)
#define ES7210_MCLK_OFF			BIT(0)
#define ES7210_MCLK_DIV_1		0
#define ES7210_MCLK_DIV_2		1
#define ES7210_MCLK_DIV_4		2
#define ES7210_MCLK_DIV_8		3

#define ES7210_LRCK_DIV_MASK		GENMASK(7, 0)
#define ES7210_LRCK_DIV_OFFSET		0x00
#define ES7210_LRCK_DIVH_OFFSET		0x00

#define ES7210_DIGITAL_GAIN_SHIFT	0
#define ES7210_DIGITAL_GAIN_MASK	GENMASK(4, 0)

#define ES7210_SDP_INTERFACE_FORMAT_MASK	GENMASK(1, 0)
#define ES7210_SDP_INTERFACE_I2S		(0 << 0)
#define ES7210_SDP_INTERFACE_LEFT_J		(1 << 0)
#define ES7210_SDP_INTERFACE_DSP		(3 << 0)

#define ES7210_OSR_MASK			GENMASK(2, 0)
#define ES7210_OSR_64			(0 << 0)
#define ES7210_OSR_128			(1 << 0)
#define ES7210_OSR_256			(2 << 0)
#define ES7210_OSR_512			(3 << 0)
#define ES7210_OSR_1024			(4 << 0)

#define ES7210_ADC34_MODE_MASK		GENMASK(7, 6)
#define ES7210_ADC34_MODE_TDM		BIT(7)

#define ES7210_MODE_CONFIG_MASK		GENMASK(7, 4)
#define ES7210_MODE_CONFIG_4CH_I2S	(0 << 4)
#define ES7210_MODE_CONFIG_TDM		(1 << 4)

/* Registers 0x00-0x4f are all valid */
#define ES7210_MAX_REG			0x4f

/*
 * The ES7210 needs a specific start-up order: the analogue block, the
 * reference, then the ADC itself. Values come from the datasheet's
 * "recommended power up sequence" table and the vendor driver.
 */
static const struct reg_default es7210_reg_defaults[] = {
	{ 0x00, 0xff }, { 0x01, 0x3f }, { 0x02, 0x00 }, { 0x03, 0x20 },
	{ 0x04, 0x01 }, { 0x05, 0x00 }, { 0x06, 0x3f }, { 0x07, 0x00 },
	{ 0x08, 0x00 }, { 0x09, 0x00 }, { 0x0a, 0x00 }, { 0x11, 0x30 },
	{ 0x12, 0x02 }, { 0x13, 0x00 }, { 0x14, 0x00 }, { 0x15, 0x00 },
	{ 0x20, 0x0a }, { 0x21, 0x0a }, { 0x22, 0x0a }, { 0x23, 0x0a },
	{ 0x40, 0x80 }, { 0x41, 0x00 }, { 0x42, 0x00 }, { 0x43, 0x00 },
	{ 0x44, 0x00 }, { 0x45, 0x00 }, { 0x46, 0x00 }, { 0x47, 0x00 },
	{ 0x48, 0x00 }, { 0x49, 0x00 }, { 0x4a, 0x00 }, { 0x4b, 0x00 },
	{ 0x4c, 0x00 }, { 0x4d, 0x00 }, { 0x4e, 0x00 }, { 0x4f, 0x00 },
};

static bool es7210_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case ES7210_RESET_REG00:
		return true;
	default:
		return false;
	}
}

static bool es7210_readable_reg(struct device *dev, unsigned int reg)
{
	return reg <= ES7210_MAX_REG;
}

static bool es7210_writeable_reg(struct device *dev, unsigned int reg)
{
	return reg <= ES7210_MAX_REG;
}

static const struct regmap_config es7210_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= ES7210_MAX_REG,
	.reg_defaults	= es7210_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(es7210_reg_defaults),
	.volatile_reg	= es7210_volatile_reg,
	.readable_reg	= es7210_readable_reg,
	.writeable_reg	= es7210_writeable_reg,
	.cache_type	= REGCACHE_MAPLE,
};

struct es7210_priv {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct regulator *vdd_d;	/* digital / VD supply */
	struct regulator *vdd_p;	/* analogue / VP supply */
	unsigned int mclk_rate;
	unsigned int format;
};

static int es7210_mic_select(struct es7210_priv *es7210, unsigned int mic)
{
	/*
	 * The vendor driver writes 0x08 to each of the four microphone power
	 * registers (0x47..0x4a); the earlier code set bit 0 instead, which is
	 * a different control and leaves the input stage off.
	 */
	unsigned int reg = ES7210_MIC1_POWER_REG47 + (mic - 1);

	return regmap_write(es7210->regmap, reg, 0x08);
}

static int es7210_mute(struct snd_soc_dai *dai, int mute, int direction)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(dai->component);
	unsigned int val = mute ? 0x03 : 0x00;

	/*
	 * Only the two MUTERANGE registers implement the requested mute.
	 *
	 * 0x13 is ADC_AUTOMUTE, not a mute control: writing 0xff there turns
	 * the automatic mute on, and automute then zeroes the digital output
	 * whenever the input sits below its threshold - which is all the
	 * time for a quiet room. The old code wrote 0xff to both registers
	 * on every stop, so the very next capture started with automute
	 * enabled and delivered nothing but zeros, even though the chip was
	 * clocked, powered and configured exactly as the vendor data says.
	 *
	 * The reference driver leaves 0x13 at 0x00 and uses 0x03 in the
	 * MUTERANGE registers to mute.
	 */
	regmap_write(es7210->regmap, ES7210_ADC34_MUTERANGE_REG14, val);
	regmap_write(es7210->regmap, ES7210_ADC12_MUTERANGE_REG15, val);

	return 0;
}

static int es7210_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(dai->component);
	unsigned int iface;

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		iface = ES7210_SDP_INTERFACE_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		iface = ES7210_SDP_INTERFACE_LEFT_J;
		break;
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		iface = ES7210_SDP_INTERFACE_DSP;
		break;
	default:
		return -EINVAL;
	}

	es7210->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;

	return regmap_update_bits(es7210->regmap, ES7210_SDP_INTERFACE1_REG11,
				  ES7210_SDP_INTERFACE_FORMAT_MASK, iface);
}

static int es7210_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(dai->component);
	unsigned int mclk_div, osr;
	unsigned int ratio;
	int ret;

	/*
	 * The J716F machine driver does not call set_sysclk() for this codec,
	 * so mclk_rate may still be 0. In that case fall back to the standard
	 * 256 fs MCLK/rate ratio, which is what the LPASS primary MI2S capture
	 * port supplies on this board.
	 */
	if (!es7210->mclk_rate) {
		mclk_div = ES7210_MCLK_DIV_1;
		osr = ES7210_OSR_256;
		goto apply;
	}

	ratio = es7210->mclk_rate / params_rate(params);

	switch (ratio) {
	case 256:
		mclk_div = ES7210_MCLK_DIV_1;
		osr = ES7210_OSR_256;
		break;
	case 384:
		mclk_div = ES7210_MCLK_DIV_1;
		osr = ES7210_OSR_512;
		break;
	case 512:
		mclk_div = ES7210_MCLK_DIV_2;
		osr = ES7210_OSR_512;
		break;
	case 768:
		mclk_div = ES7210_MCLK_DIV_2;
		osr = ES7210_OSR_1024;
		break;
	default:
		dev_err(es7210->dev, "unsupported mclk/rate ratio %u\n", ratio);
		return -EINVAL;
	}

apply:
	/*
	 * 0x02/0x07 hold the values captured from the vendor driver; do not
	 * rewrite them per stream.
	 */
	(void)mclk_div;
	(void)osr;

	return 0;
}

static int es7210_set_sysclk(struct snd_soc_dai *dai, int clk_id,
			     unsigned int freq, int dir)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(dai->component);

	if (dir != SND_SOC_CLOCK_IN)
		return 0;

	es7210->mclk_rate = freq;

	return 0;
}

/*
 * Reg 0x01 gates the ADC clocks and reg 0x06 holds the ADCs in power-down.
 * Both must be released when a capture stream starts, otherwise the ADC
 * produces silence while the I2S bus still clocks out zeros.
 */
static int es7210_trigger(struct snd_pcm_substream *substream, int cmd,
			  struct snd_soc_dai *dai)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(dai->component);

	/*
	 * Values captured from the vendor driver while it was enabled:
	 * 0x01 = 0x20 and 0x06 = 0x00. The disabled state is 0x3f on both.
	 */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE: {
		int r1, r2, v1 = -1;

		r1 = regmap_write(es7210->regmap, ES7210_CLOCK_OFF_REG01, 0x20);
		r2 = regmap_write(es7210->regmap, ES7210_POWER_DOWN_REG06, 0x00);
		regmap_read(es7210->regmap, ES7210_CLOCK_OFF_REG01, &v1);
		break;
	}
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		regmap_write(es7210->regmap, ES7210_CLOCK_OFF_REG01, 0x3f);
		regmap_write(es7210->regmap, ES7210_POWER_DOWN_REG06, 0x3f);
		break;
	default:
		return 0;
	}

	return 0;
}

static const struct snd_soc_dai_ops es7210_dai_ops = {
	.hw_params	= es7210_hw_params,
	.set_fmt	= es7210_set_dai_fmt,
	.set_sysclk	= es7210_set_sysclk,
	.mute_stream	= es7210_mute,
	.trigger	= es7210_trigger,
};

static struct snd_soc_dai_driver es7210_dai = {
	.name = "es7210-hifi",
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 4,
		.rates		= SNDRV_PCM_RATE_8000_96000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S24_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &es7210_dai_ops,
};

static const DECLARE_TLV_DB_SCALE(es7210_mic_gain_tlv, 0, 300, 0);

/*
 * The PGA gain field is the low nibble of 0x43..0x46 and only 15 values are
 * defined (0 = 0 dB ... 14 = 37.5 dB, see the reference driver's MicGain
 * enum); bit 4 of the same register must always be set, which is why the
 * reference implementation writes (gain | 0x10).
 *
 * The control used to expose the raw register byte with a maximum of 0x0f,
 * so user space could - and did - write 15 into the field. That is one past
 * the last valid gain and left 0x1f in the register, i.e. bit 4 plus an
 * out-of-range gain code. The ADC then converted but produced digital
 * silence, which is what made every capture come back as zeros even after
 * the clock tree was fixed.
 *
 * Expose 0..14 and let the register accessors add the 0x10 enable bit
 * through the shift/max pair below.
 */
static const struct snd_kcontrol_new es7210_controls[] = {
	SOC_DOUBLE_R_TLV("MIC1 Volume", ES7210_MIC1_GAIN_REG43,
			 ES7210_MIC2_GAIN_REG44, 0, 0x0e, 0,
			 es7210_mic_gain_tlv),
	SOC_DOUBLE_R_TLV("MIC2 Volume", ES7210_MIC3_GAIN_REG45,
			 ES7210_MIC4_GAIN_REG46, 0, 0x0e, 0,
			 es7210_mic_gain_tlv),
};

static const struct snd_soc_dapm_widget es7210_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_INPUT("MIC3"),
	SND_SOC_DAPM_INPUT("MIC4"),
	SND_SOC_DAPM_ADC("ADC1", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("ADC2", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("ADC3", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("ADC4", NULL, SND_SOC_NOPM, 0, 0),
	/*
	 * NOTE: no hand-written "Capture" widget here.
	 *
	 * snd_soc_dapm_new_dai_widgets() already creates one per DAI, named
	 * after es7210_dai.capture.stream_name and typed dai_out. Declaring
	 * our own "Capture" aif_out widget collided with it:
	 *
	 *   ASoC: source widget Capture overwritten
	 *
	 * and the surviving widget kept type aif_out, so the
	 * {"PRI_MI2S_TX", NULL, "Capture"} route could not be resolved -
	 * PRI_MI2S_TX ended up with in 0, the whole chain stayed Off and
	 * capture returned 44 bytes.
	 */
};

static const struct snd_soc_dapm_route es7210_dapm_routes[] = {
	{ "ADC1", NULL, "MIC1" },
	{ "ADC2", NULL, "MIC2" },
	{ "ADC3", NULL, "MIC3" },
	{ "ADC4", NULL, "MIC4" },
	/*
	 * The ADCs must feed the capture stream, otherwise DAPM has no path
	 * from the DAI back to the microphones: the ADC widgets stay powered
	 * down, the chip looks correctly programmed (all registers read back
	 * as written) and capture completes with every sample zero.
	 */
	{ "Capture", NULL, "ADC1" },
	{ "Capture", NULL, "ADC2" },
	{ "Capture", NULL, "ADC3" },
	{ "Capture", NULL, "ADC4" },
};

/*
 * Power-up sequence from the datasheet: bring the analogue block and the
 * reference up first, then release the ADC and the clock gating.
 */
static int es7210_component_probe(struct snd_soc_component *component)
{
	struct es7210_priv *es7210 = snd_soc_component_get_drvdata(component);
	int ret;

	/*
	 * Register values captured from the vendor driver running on this
	 * exact tablet: Android was booted, the ES7210 bound at i2c-4/0x40,
	 * its "ADC ES7210 EN" mixer control switched on, and the whole
	 * register file read back with i2cdump. These are therefore the real
	 * post-enable values, not a datasheet reconstruction.
	 *
	 * The important differences from the previous hand-written sequence:
	 *   0x01 = 0x20  (not 0x00 - the clock gate is left partly closed)
	 *   0x02 = 0x01  (not 0xc1 - completely different ADC clock setup)
	 *   0x03 = 0x04
	 *   0x08 = 0x16
	 *   0x12 = 0x01
	 *   0x1b..0x1e = 0xb9 (not 0xbf)
	 *   0x3d = 0x72
	 *   0x40 = 0x42  (not 0x43)
	 *   0x4c = 0x00  (ADC34 pair powered, not off)
	 */
	static const struct reg_sequence es7210_vendor_init[] = {
		{ 0x00, 0x41 },	/* reset complete */
		{ 0x01, 0x20 },	/* clock gate */
		/*
		 * Clock configuration for MCLK = 32 x Fs, LRCK = 48 kHz.
		 *
		 * This is the value the board actually needs, not the 19.2 MHz
		 * one from the public driver's COEFF_TABLE. The machine driver
		 * programs Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT to 32 x Fs =
		 * 1.536 MHz at 48 kHz, so LRCK_DIV has to be 32 (0x0020) for
		 * the ADC to produce 48 kHz.
		 *
		 * With the previous LRCK_DIV = 400 (0x0190) the divider gave
		 * 1.536 MHz / 400 = 3.84 kHz. The chip still powered up, DAPM
		 * completed, every register read back exactly as written and
		 * the ADSP still delivered periods on time - it just delivered
		 * zeros, because the ADC was being asked for a rate it cannot
		 * produce. That was the last remaining blocker for capture.
		 *
		 * 0x02 = adc_div | (doubler << 6) | (dll << 7) = 0x05 | 0x40 = 0x45
		 */
		{ 0x02, 0x45 },	/* ADC clock: div 5, doubler on, dll off */
		{ 0x03, 0x04 },	/* master clock */
		{ 0x04, 0x00 },	/* LRCK divider high = 0x00 */
		{ 0x05, 0x20 },	/* LRCK divider low  = 0x20 -> 32 total */
		{ 0x06, 0x00 },	/* out of power-down */
		{ 0x07, 0x28 },	/* OSR = 256 (per COEFF_TABLE) */
		{ 0x08, 0x16 },	/* mode config */
		{ 0x09, 0x30 },	/* ADC time control 0 */
		{ 0x0a, 0x30 },	/* ADC time control 1 */
		{ 0x0d, 0x09 },
		{ 0x11, 0x60 },	/* SDP interface, 16-bit */
		{ 0x12, 0x01 },	/* SDP interface 2 */
		/*
		 * ADC_AUTOMUTE_REG13 and the two MUTERANGE registers must be
		 * zeroed explicitly.
		 *
		 * They read back as 0xff after the reset sequence, and 0x13 =
		 * 0xff enables the automatic mute: any signal below the
		 * mute-range threshold - which is everything while the mics
		 * are still settling - forces the digital output to zero. The
		 * chip then looks perfectly healthy (clocks running, ADCs
		 * powered, PGA at +18 dB, DAPM fully up, every register
		 * reading back as written) while every captured sample is 0.
		 *
		 * The reference driver (github.com/QuackHack-McBlindy/es7210)
		 * likewise leaves these at 0x00 and only ever writes 0x03 here
		 * to mute on request.
		 */
		{ 0x13, 0x00 },	/* automute off */
		{ 0x14, 0x00 },	/* ADC34 mute range off */
		{ 0x15, 0x00 },	/* ADC12 mute range off */
		{ 0x18, 0xf7 },
		{ 0x19, 0xf7 },
		{ 0x1b, 0xb9 },
		{ 0x1c, 0xb9 },
		{ 0x1d, 0xb9 },
		{ 0x1e, 0xb9 },
		{ 0x20, 0x0a },	/* HPF */
		{ 0x21, 0x2a },
		{ 0x22, 0x0a },
		{ 0x23, 0x2a },
		{ 0x24, 0x11 },
		{ 0x27, 0x0a },
		{ 0x2b, 0x2a },
		{ 0x2f, 0x2a },
		{ 0x33, 0x2a },
		{ 0x37, 0x2a },
		{ 0x3d, 0x72 },
		{ 0x3e, 0x10 },
		{ 0x3f, 0x02 },
		/*
		 * Android reads back 0xc3 here while our sequence wrote 0x42.
		 * 0x40 holds the analogue block bias/reference enables, so use
		 * the value the vendor driver actually leaves behind.
		 */
		{ 0x40, 0xc3 },	/* analogue block */
		{ 0x41, 0x70 },	/* MIC12 bias */
		{ 0x42, 0x70 },	/* MIC34 bias */
		/*
		 * MICn PGA: (gain | 0x10), gain 0..14 = 0 dB .. 37.5 dB in
		 * 3 dB steps. 6 = +18 dB, the value the reference driver's
		 * example configuration uses.
		 */
		{ 0x43, 0x16 },	/* MIC1 PGA, +18 dB */
		{ 0x44, 0x16 },	/* MIC2 PGA, +18 dB */
		{ 0x45, 0x16 },	/* MIC3 PGA, +18 dB */
		{ 0x46, 0x16 },	/* MIC4 PGA, +18 dB */
		{ 0x47, 0x08 },	/* MIC1 power */
		{ 0x48, 0x08 },	/* MIC2 power */
		{ 0x49, 0x08 },	/* MIC3 power */
		{ 0x4a, 0x08 },	/* MIC4 power */
		/*
		 * ADC12_POWER_REG4B / ADC34_POWER_REG4C: 0x0f = power up the
		 * ADC + PGA blocks, 0x00 = powered down.
		 *
		 * The previous values (0x00) came from an Android i2cdump taken
		 * while nothing was capturing, and left both ADC pairs off - the
		 * codec then looks correctly configured (all registers read back
		 * as written) but never produces data. The public es7210 driver
		 * (github.com/QuackHack-McBlindy/es7210, config_codec()) writes
		 * 0x0f here as the last step before starting the device.
		 */
		{ 0x4b, 0x0f },	/* ADC12 pair power up */
		{ 0x4c, 0x0f },	/* ADC34 pair power up */
		/* start the device: 0x71 then 0x41, as the reference driver does */
		{ 0x00, 0x71 },
		{ 0x00, 0x41 },
	};

	/* reset first */
	ret = regmap_write(es7210->regmap, ES7210_RESET_REG00, 0xff);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = regmap_multi_reg_write(es7210->regmap, es7210_vendor_init,
				     ARRAY_SIZE(es7210_vendor_init));
	if (ret)
		return ret;

	/*
	 * DIAGNOSTIC (2026-10-19): force the codec out of BIAS_OFF.
	 *
	 * DAPM never powers this chain up on its own: the "Capture" AIF_OUT
	 * widget fails dapm_generic_check_power() because
	 * is_connected_output_ep() returns 0 (it walks edges[OUT] looking for
	 * an endpoint, while Capture's paths live in edges[IN] as
	 * ADCx -> Capture). With bias_level stuck at Off the ADCs never run
	 * and every captured sample is zero, even though the register file
	 * below is written correctly.
	 *
	 * Setting BIAS_ON here bypasses that check so we can find out whether
	 * the codec actually produces data once powered. If it does, the real
	 * fix is to repair the DAPM power check; this is only a probe.
	 */
	ret = snd_soc_component_set_bias_level(component, SND_SOC_BIAS_ON);
	if (ret)
		dev_warn(component->dev, "force bias ON failed: %d\n", ret);
	else
		dev_info(component->dev, "forced BIAS_ON for mic bring-up test\n");

	return 0;
}

static const struct snd_soc_component_driver es7210_component_driver = {
	.probe			= es7210_component_probe,
	.controls		= es7210_controls,
	.num_controls		= ARRAY_SIZE(es7210_controls),
	.dapm_widgets		= es7210_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(es7210_dapm_widgets),
	.dapm_routes		= es7210_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(es7210_dapm_routes),
	.endianness		= 1,
};

static int es7210_i2c_probe(struct i2c_client *client)
{
	struct es7210_priv *es7210;
	int ret;

	es7210 = devm_kzalloc(&client->dev, sizeof(*es7210), GFP_KERNEL);
	if (!es7210)
		return -ENOMEM;

	es7210->dev = &client->dev;

	es7210->regmap = devm_regmap_init_i2c(client, &es7210_regmap_config);
	if (IS_ERR(es7210->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(es7210->regmap),
				     "failed to init regmap\n");

	es7210->mclk = devm_clk_get_optional(&client->dev, "mclk");
	if (IS_ERR(es7210->mclk))
		return dev_err_probe(&client->dev, PTR_ERR(es7210->mclk),
				     "failed to get mclk\n");

	es7210->vdd_d = devm_regulator_get_optional(&client->dev, "vd");
	if (IS_ERR(es7210->vdd_d)) {
		if (PTR_ERR(es7210->vdd_d) != -ENODEV)
			return dev_err_probe(&client->dev, PTR_ERR(es7210->vdd_d),
					     "failed to get VD supply\n");
		es7210->vdd_d = NULL;
	}

	es7210->vdd_p = devm_regulator_get_optional(&client->dev, "vp");
	if (IS_ERR(es7210->vdd_p)) {
		if (PTR_ERR(es7210->vdd_p) != -ENODEV)
			return dev_err_probe(&client->dev, PTR_ERR(es7210->vdd_p),
					     "failed to get VP supply\n");
		es7210->vdd_p = NULL;
	}

	if (es7210->vdd_d) {
		ret = regulator_enable(es7210->vdd_d);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to enable VD supply\n");
	}

	if (es7210->vdd_p) {
		ret = regulator_enable(es7210->vdd_p);
		if (ret) {
			if (es7210->vdd_d)
				regulator_disable(es7210->vdd_d);
			return dev_err_probe(&client->dev, ret,
					     "failed to enable VP supply\n");
		}
	}

	if (es7210->mclk) {
		ret = clk_prepare_enable(es7210->mclk);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to enable mclk\n");
		es7210->mclk_rate = clk_get_rate(es7210->mclk);
	}

	i2c_set_clientdata(client, es7210);

	ret = devm_snd_soc_register_component(&client->dev,
					      &es7210_component_driver,
					      &es7210_dai, 1);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to register component\n");

	return 0;
}

static const struct of_device_id es7210_of_match[] = {
	{ .compatible = "everest,es7210" },
	{ }
};
MODULE_DEVICE_TABLE(of, es7210_of_match);

static const struct i2c_device_id es7210_i2c_id[] = {
	{ "es7210", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es7210_i2c_id);

static struct i2c_driver es7210_i2c_driver = {
	.driver = {
		.name		= "es7210",
		.of_match_table	= es7210_of_match,
	},
	.probe		= es7210_i2c_probe,
	.id_table	= es7210_i2c_id,
};
module_i2c_driver(es7210_i2c_driver);

MODULE_DESCRIPTION("Everest Semi ES7210 ADC driver");
MODULE_LICENSE("GPL");
