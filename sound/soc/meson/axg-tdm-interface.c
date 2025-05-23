// SPDX-License-Identifier: (GPL-2.0 OR MIT)
//
// Copyright (c) 2018 BayLibre, SAS.
// Author: Jerome Brunet <jbrunet@baylibre.com>

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#include "axg-tdm.h"

/* Maximum bit clock frequency according the datasheets */
#define MAX_SCLK 100000000 /* Hz */

enum {
	TDM_IFACE_PAD,
	TDM_IFACE_LOOPBACK,
};

static unsigned int axg_tdm_slots_total(u32 *mask)
{
	unsigned int slots = 0;
	int i;

	if (!mask)
		return 0;

	/* Count the total number of slots provided by all 4 lanes */
	for (i = 0; i < AXG_TDM_NUM_LANES; i++)
		slots += hweight32(mask[i]);

	return slots;
}

int axg_tdm_set_tdm_slots(struct snd_soc_dai *dai, u32 *tx_mask,
			  u32 *rx_mask, unsigned int slots,
			  unsigned int slot_width)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	struct axg_tdm_stream *tx = snd_soc_dai_dma_data_get_playback(dai);
	struct axg_tdm_stream *rx = snd_soc_dai_dma_data_get_capture(dai);
	unsigned int tx_slots, rx_slots;
	unsigned int fmt = 0;

	tx_slots = axg_tdm_slots_total(tx_mask);
	rx_slots = axg_tdm_slots_total(rx_mask);

	/* We should at least have a slot for a valid interface */
	if (!tx_slots && !rx_slots) {
		dev_err(dai->dev, "interface has no slot\n");
		return -EINVAL;
	}

	iface->slots = slots;

	switch (slot_width) {
	case 0:
		slot_width = 32;
		fallthrough;
	case 32:
		fmt |= SNDRV_PCM_FMTBIT_S32_LE;
		fallthrough;
	case 24:
		fmt |= SNDRV_PCM_FMTBIT_S24_LE;
		fmt |= SNDRV_PCM_FMTBIT_S20_LE;
		fallthrough;
	case 16:
		fmt |= SNDRV_PCM_FMTBIT_S16_LE;
		fallthrough;
	case 8:
		fmt |= SNDRV_PCM_FMTBIT_S8;
		break;
	default:
		dev_err(dai->dev, "unsupported slot width: %d\n", slot_width);
		return -EINVAL;
	}

	iface->slot_width = slot_width;

	/* Amend the dai driver and let dpcm merge do its job */
	if (tx) {
		tx->mask = tx_mask;
		dai->driver->playback.channels_max = tx_slots;
		dai->driver->playback.formats = fmt;
	}

	if (rx) {
		rx->mask = rx_mask;
		dai->driver->capture.channels_max = rx_slots;
		dai->driver->capture.formats = fmt;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(axg_tdm_set_tdm_slots);

static int axg_tdm_iface_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				    unsigned int freq, int dir)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	int ret = -ENOTSUPP;

	if (dir == SND_SOC_CLOCK_OUT && clk_id == 0) {
		if (!iface->mclk) {
			dev_warn(dai->dev, "master clock not provided\n");
		} else {
			ret = clk_set_rate(iface->mclk, freq);
			if (!ret)
				iface->mclk_rate = freq;
		}
	}

	return ret;
}

static int axg_tdm_iface_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		if (!iface->mclk) {
			dev_err(dai->dev, "cpu clock master: mclk missing\n");
			return -ENODEV;
		}
		break;

	case SND_SOC_DAIFMT_BC_FC:
		break;

	case SND_SOC_DAIFMT_BP_FC:
	case SND_SOC_DAIFMT_BC_FP:
		dev_err(dai->dev, "only BP_FP and BC_FC are supported\n");
		fallthrough;
	default:
		return -EINVAL;
	}

	iface->fmt = fmt;
	return 0;
}

static int axg_tdm_iface_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	struct axg_tdm_stream *ts =
		snd_soc_dai_get_dma_data(dai, substream);
	int ret;

	if (!axg_tdm_slots_total(ts->mask)) {
		dev_err(dai->dev, "interface has not slots\n");
		return -EINVAL;
	}

	if (snd_soc_component_active(dai->component)) {
		/* Apply component wide rate symmetry */
		ret = snd_pcm_hw_constraint_single(substream->runtime,
						   SNDRV_PCM_HW_PARAM_RATE,
						   iface->rate);

	} else {
		/* Limit rate according to the slot number and width */
		unsigned int max_rate =
			MAX_SCLK / (iface->slots * iface->slot_width);
		ret = snd_pcm_hw_constraint_minmax(substream->runtime,
						   SNDRV_PCM_HW_PARAM_RATE,
						   0, max_rate);
	}

	if (ret < 0)
		dev_err(dai->dev, "can't set iface rate constraint\n");
	else
		ret = 0;

	return ret;
}

static int axg_tdm_iface_set_stream(struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params,
				    struct snd_soc_dai *dai)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	struct axg_tdm_stream *ts = snd_soc_dai_get_dma_data(dai, substream);
	unsigned int channels = params_channels(params);
	unsigned int width = params_width(params);

	/* Save rate and sample_bits for component symmetry */
	iface->rate = params_rate(params);

	/* Make sure this interface can cope with the stream */
	if (axg_tdm_slots_total(ts->mask) < channels) {
		dev_err(dai->dev, "not enough slots for channels\n");
		return -EINVAL;
	}

	if (iface->slot_width < width) {
		dev_err(dai->dev, "incompatible slots width for stream\n");
		return -EINVAL;
	}

	/* Save the parameter for tdmout/tdmin widgets */
	ts->physical_width = params_physical_width(params);
	ts->width = params_width(params);
	ts->channels = params_channels(params);

	return 0;
}

static int axg_tdm_iface_set_lrclk(struct snd_soc_dai *dai,
				   struct snd_pcm_hw_params *params)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	unsigned int ratio_num;
	int ret;

	ret = clk_set_rate(iface->lrclk, params_rate(params));
	if (ret) {
		dev_err(dai->dev, "setting sample clock failed: %d\n", ret);
		return ret;
	}

	switch (iface->fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_RIGHT_J:
		/* 50% duty cycle ratio */
		ratio_num = 1;
		break;

	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		/*
		 * A zero duty cycle ratio will result in setting the mininum
		 * ratio possible which, for this clock, is 1 cycle of the
		 * parent bclk clock high and the rest low, This is exactly
		 * what we want here.
		 */
		ratio_num = 0;
		break;

	default:
		return -EINVAL;
	}

	ret = clk_set_duty_cycle(iface->lrclk, ratio_num, 2);
	if (ret) {
		dev_err(dai->dev,
			"setting sample clock duty cycle failed: %d\n", ret);
		return ret;
	}

	/* Set sample clock inversion */
	ret = clk_set_phase(iface->lrclk,
			    axg_tdm_lrclk_invert(iface->fmt) ? 180 : 0);
	if (ret) {
		dev_err(dai->dev,
			"setting sample clock phase failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int axg_tdm_iface_set_sclk(struct snd_soc_dai *dai,
				  struct snd_pcm_hw_params *params)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	unsigned long srate;
	int ret;

	srate = iface->slots * iface->slot_width * params_rate(params);

	if (!iface->mclk_rate) {
		/* If no specific mclk is requested, default to bit clock * 2 */
		clk_set_rate(iface->mclk, 2 * srate);
	} else {
		/* Check if we can actually get the bit clock from mclk */
		if (iface->mclk_rate % srate) {
			dev_err(dai->dev,
				"can't derive sclk %lu from mclk %lu\n",
				srate, iface->mclk_rate);
			return -EINVAL;
		}
	}

	ret = clk_set_rate(iface->sclk, srate);
	if (ret) {
		dev_err(dai->dev, "setting bit clock failed: %d\n", ret);
		return ret;
	}

	/* Set the bit clock inversion */
	ret = clk_set_phase(iface->sclk,
			    axg_tdm_sclk_invert(iface->fmt) ? 0 : 180);
	if (ret) {
		dev_err(dai->dev, "setting bit clock phase failed: %d\n", ret);
		return ret;
	}

	return ret;
}

static int axg_tdm_iface_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	struct axg_tdm_stream *ts = snd_soc_dai_get_dma_data(dai, substream);
	int ret;

	switch (iface->fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_RIGHT_J:
		if (iface->slots > 2) {
			dev_err(dai->dev, "bad slot number for format: %d\n",
				iface->slots);
			return -EINVAL;
		}
		break;

	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		break;

	default:
		dev_err(dai->dev, "unsupported dai format\n");
		return -EINVAL;
	}

	ret = axg_tdm_iface_set_stream(substream, params, dai);
	if (ret)
		return ret;

	if ((iface->fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) ==
	    SND_SOC_DAIFMT_BP_FP) {
		ret = axg_tdm_iface_set_sclk(dai, params);
		if (ret)
			return ret;

		ret = axg_tdm_iface_set_lrclk(dai, params);
		if (ret)
			return ret;
	}

	ret = axg_tdm_stream_set_cont_clocks(ts, iface->fmt);
	if (ret)
		dev_err(dai->dev, "failed to apply continuous clock setting\n");

	return ret;
}

static int axg_tdm_iface_hw_free(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct axg_tdm_stream *ts = snd_soc_dai_get_dma_data(dai, substream);

	return axg_tdm_stream_set_cont_clocks(ts, 0);
}

static int axg_tdm_iface_trigger(struct snd_pcm_substream *substream,
				 int cmd,
				 struct snd_soc_dai *dai)
{
	struct axg_tdm_stream *ts =
		snd_soc_dai_get_dma_data(dai, substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		axg_tdm_stream_start(ts);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		axg_tdm_stream_stop(ts);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int axg_tdm_iface_remove_dai(struct snd_soc_dai *dai)
{
	int stream;

	for_each_pcm_streams(stream) {
		struct axg_tdm_stream *ts = snd_soc_dai_dma_data_get(dai, stream);

		if (ts)
			axg_tdm_stream_free(ts);
	}

	return 0;
}

static int axg_tdm_iface_probe_dai(struct snd_soc_dai *dai)
{
	struct axg_tdm_iface *iface = snd_soc_dai_get_drvdata(dai);
	int stream;

	for_each_pcm_streams(stream) {
		struct axg_tdm_stream *ts;

		if (!snd_soc_dai_get_widget(dai, stream))
			continue;

		ts = axg_tdm_stream_alloc(iface);
		if (!ts) {
			axg_tdm_iface_remove_dai(dai);
			return -ENOMEM;
		}
		snd_soc_dai_dma_data_set(dai, stream, ts);
	}

	return 0;
}

static const struct snd_soc_dai_ops axg_tdm_iface_ops = {
	.probe		= axg_tdm_iface_probe_dai,
	.remove		= axg_tdm_iface_remove_dai,
	.set_sysclk	= axg_tdm_iface_set_sysclk,
	.set_fmt	= axg_tdm_iface_set_fmt,
	.startup	= axg_tdm_iface_startup,
	.hw_params	= axg_tdm_iface_hw_params,
	.hw_free	= axg_tdm_iface_hw_free,
	.trigger	= axg_tdm_iface_trigger,
};

/* TDM Backend DAIs */
static const struct snd_soc_dai_driver axg_tdm_iface_dai_drv[] = {
	[TDM_IFACE_PAD] = {
		.name = "TDM Pad",
		.playback = {
			.stream_name	= "Playback",
			.channels_min	= 1,
			.channels_max	= AXG_TDM_CHANNEL_MAX,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 768000,
			.formats	= AXG_TDM_FORMATS,
		},
		.capture = {
			.stream_name	= "Capture",
			.channels_min	= 1,
			.channels_max	= AXG_TDM_CHANNEL_MAX,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 768000,
			.formats	= AXG_TDM_FORMATS,
		},
		.id = TDM_IFACE_PAD,
		.ops = &axg_tdm_iface_ops,
	},
	[TDM_IFACE_LOOPBACK] = {
		.name = "TDM Loopback",
		.capture = {
			.stream_name	= "Loopback",
			.channels_min	= 1,
			.channels_max	= AXG_TDM_CHANNEL_MAX,
			.rates		= SNDRV_PCM_RATE_CONTINUOUS,
			.rate_min	= 5512,
			.rate_max	= 768000,
			.formats	= AXG_TDM_FORMATS,
		},
		.id = TDM_IFACE_LOOPBACK,
		.ops = &axg_tdm_iface_ops,
	},
};

static int axg_tdm_iface_set_bias_level(struct snd_soc_component *component,
					enum snd_soc_bias_level level)
{
	struct axg_tdm_iface *iface = snd_soc_component_get_drvdata(component);
	enum snd_soc_bias_level now =
		snd_soc_component_get_bias_level(component);
	int ret = 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (now == SND_SOC_BIAS_STANDBY)
			ret = clk_prepare_enable(iface->mclk);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (now == SND_SOC_BIAS_PREPARE)
			clk_disable_unprepare(iface->mclk);
		break;

	case SND_SOC_BIAS_OFF:
	case SND_SOC_BIAS_ON:
		break;
	}

	return ret;
}

static const struct snd_soc_dapm_widget axg_tdm_iface_dapm_widgets[] = {
	SND_SOC_DAPM_SIGGEN("Playback Signal"),
};

static const struct snd_soc_dapm_route axg_tdm_iface_dapm_routes[] = {
	{ "Loopback", NULL, "Playback Signal" },
};

static const struct snd_soc_component_driver axg_tdm_iface_component_drv = {
	.dapm_widgets		= axg_tdm_iface_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(axg_tdm_iface_dapm_widgets),
	.dapm_routes		= axg_tdm_iface_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(axg_tdm_iface_dapm_routes),
	.set_bias_level		= axg_tdm_iface_set_bias_level,
};

static const struct of_device_id axg_tdm_iface_of_match[] = {
	{ .compatible = "amlogic,axg-tdm-iface", },
	{}
};
MODULE_DEVICE_TABLE(of, axg_tdm_iface_of_match);

static int axg_tdm_iface_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_dai_driver *dai_drv;
	struct axg_tdm_iface *iface;

	iface = devm_kzalloc(dev, sizeof(*iface), GFP_KERNEL);
	if (!iface)
		return -ENOMEM;
	platform_set_drvdata(pdev, iface);

	/*
	 * Duplicate dai driver: depending on the slot masks configuration
	 * We'll change the number of channel provided by DAI stream, so dpcm
	 * channel merge can be done properly
	 */
	dai_drv = devm_kmemdup_array(dev, axg_tdm_iface_dai_drv, ARRAY_SIZE(axg_tdm_iface_dai_drv),
				     sizeof(axg_tdm_iface_dai_drv[0]), GFP_KERNEL);
	if (!dai_drv)
		return -ENOMEM;

	/* Bit clock provided on the pad */
	iface->sclk = devm_clk_get(dev, "sclk");
	if (IS_ERR(iface->sclk))
		return dev_err_probe(dev, PTR_ERR(iface->sclk), "failed to get sclk\n");

	/* Sample clock provided on the pad */
	iface->lrclk = devm_clk_get(dev, "lrclk");
	if (IS_ERR(iface->lrclk))
		return dev_err_probe(dev, PTR_ERR(iface->lrclk), "failed to get lrclk\n");

	/*
	 * mclk maybe be missing when the cpu dai is in slave mode and
	 * the codec does not require it to provide a master clock.
	 * At this point, ignore the error if mclk is missing. We'll
	 * throw an error if the cpu dai is master and mclk is missing
	 */
	iface->mclk = devm_clk_get_optional(dev, "mclk");
	if (IS_ERR(iface->mclk))
		return dev_err_probe(dev, PTR_ERR(iface->mclk), "failed to get mclk\n");

	return devm_snd_soc_register_component(dev,
					&axg_tdm_iface_component_drv, dai_drv,
					ARRAY_SIZE(axg_tdm_iface_dai_drv));
}

static struct platform_driver axg_tdm_iface_pdrv = {
	.probe = axg_tdm_iface_probe,
	.driver = {
		.name = "axg-tdm-iface",
		.of_match_table = axg_tdm_iface_of_match,
	},
};
module_platform_driver(axg_tdm_iface_pdrv);

MODULE_DESCRIPTION("Amlogic AXG TDM interface driver");
MODULE_AUTHOR("Jerome Brunet <jbrunet@baylibre.com>");
MODULE_LICENSE("GPL v2");
