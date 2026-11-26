// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <dt-bindings/sound/qcom,q6asm.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <linux/spinlock.h>
#include <sound/compress_driver.h>
#include <asm/div64.h>
#include <asm/dma.h>
#include <linux/dma-mapping.h>
#include <sound/pcm_params.h>
#include "q6asm.h"
#include "q6routing.h"
#include "q6dsp-errno.h"

#define DRV_NAME	"q6asm-fe-dai"

/*
 * Lenovo J716F (SM8250-AC / kona ADSP): the ADSP answers
 * ASM_CMD_SHARED_MEM_MAP_REGIONS with ADSP_EFAILED for any buffer address
 * outside the LPASS audio SMMU window.  The vendor kernel maps its audio
 * buffers from 0x1000_0000-0x1FFF_FFFF (qcom,iommu-dma-addr-pool on its
 * msm-audio-ion node); with the upstream 32-bit DMA mask iommu-dma hands out
 * an IOVA near the top of the 32-bit space and the DSP rejects it.
 * q6asm_dai_pcm_new() therefore limits the audio DMA mask to 29 bits, which
 * puts the IOVA at 0x1FFF_xxxx inside that window; with that in place the
 * upstream address layout (dma_addr | sid << 32) is what the kona ADSP
 * accepts for both directions (confirmed on hardware 2026-11-07).
 */

#define PLAYBACK_MIN_NUM_PERIODS    2
#define PLAYBACK_MAX_NUM_PERIODS   8
#define PLAYBACK_MAX_PERIOD_SIZE    65536
#define PLAYBACK_MIN_PERIOD_SIZE    128
#define CAPTURE_MIN_NUM_PERIODS     2
#define CAPTURE_MAX_NUM_PERIODS     8
/*
 * Lenovo J716F: the capture period size has to be able to hold what the DSP
 * delivers per ASM_DATA_CMD_READ. With the upstream 4096-byte limit ALSA
 * could not carve the 24000-byte DSP periods, ended up with a single-period
 * buffer and never advanced hw_ptr, so arecord failed with -EIO even though
 * the DSP was returning data. Match the playback limit.
 */
#define CAPTURE_MAX_PERIOD_SIZE     PLAYBACK_MAX_PERIOD_SIZE
#define CAPTURE_MIN_PERIOD_SIZE     320
#define SID_MASK_DEFAULT	0xF

/* Default values used if user space does not set */
#define COMPR_PLAYBACK_MIN_FRAGMENT_SIZE (8 * 1024)
#define COMPR_PLAYBACK_MAX_FRAGMENT_SIZE (128 * 1024)
#define COMPR_PLAYBACK_MIN_NUM_FRAGMENTS (4)
#define COMPR_PLAYBACK_MAX_NUM_FRAGMENTS (16 * 4)

#define ALAC_CH_LAYOUT_MONO   ((101 << 16) | 1)
#define ALAC_CH_LAYOUT_STEREO ((101 << 16) | 2)

enum stream_state {
	Q6ASM_STREAM_IDLE = 0,
	Q6ASM_STREAM_PREPARED,
	Q6ASM_STREAM_STARTING,
	Q6ASM_STREAM_RUNNING,
	Q6ASM_STREAM_PAUSING,
	Q6ASM_STREAM_PAUSED,
	Q6ASM_STREAM_STOPPING,
	Q6ASM_STREAM_STOPPED,
	Q6ASM_STREAM_ERROR,
};

/*
 * Mapping state of a stream's DMA buffer.  A MAP command that timed out may
 * still have executed at the DSP, so "unknown" is a real state: the teardown
 * must try to unmap and, if it cannot, keep the pages alive.
 */
enum q6asm_map_state {
	Q6ASM_MAP_NONE = 0,	/* no mapping was requested or it was released */
	Q6ASM_MAP_CONFIRMED,	/* the DSP returned a map handle */
	Q6ASM_MAP_UNKNOWN,	/* MAP may have executed; no reply seen */
};

struct q6asm_dai_rtd {
	struct snd_pcm_substream *substream;
	struct snd_compr_stream *cstream;
	struct snd_codec codec;
	struct snd_dma_buffer dma_buffer;
	/*
	 * Protects the state machine and the failure/elapsed counters.  The
	 * lock is never held across a call into ALSA or a sleeping Q6ASM
	 * command: the PRTD lock order is stream lock -> rtd->lock ->
	 * ac->lock, and snd_pcm_period_elapsed() takes the stream lock.
	 */
	spinlock_t lock;
	phys_addr_t phys;
	unsigned int pcm_size;
	unsigned int pcm_count;
	unsigned int periods;
	uint64_t bytes_sent;
	uint64_t bytes_received;
	uint64_t copied_total;
	uint16_t bits_per_sample;
	snd_pcm_uframes_t queue_ptr;
	uint16_t source; /* Encoding source bit mask */
	struct audio_client *audio_client;
	struct device *dev;
	uint32_t next_track_stream_id;
	bool next_track;
	uint32_t stream_id;
	/* Stream closed with a nowait CLOSE at a gapless switch. */
	uint32_t prev_stream_id;
	uint16_t session_id;
	enum stream_state state;
	/* ALSA has seen the START trigger; period_elapsed is legal. */
	bool started;
	/* A period completed while ALSA was not running yet. */
	bool elapsed_pending;
	bool dma_held;
	/* Whether OPEN_WRITE/OPEN_READ succeeded and CMD_CLOSE is needed */
	bool stream_opened;
	/* Whether the remote teardown already failed once; do not retry. */
	bool teardown_failed;
	/* Whether the audio buffer is mapped into the ADSP address space */
	enum q6asm_map_state map_state;
	/* True when set_params() or free() could not confirm the teardown. */
	bool teardown_unknown;
	/* Copy the fixed PCM ring before it can be freed under a live map. */
	bool needs_hold;
	uint32_t initial_samples_drop;
	uint32_t trailing_samples_drop;
	bool notify_on_drain;
	/* The kona ADSP needs capture started before the first userspace read. */
	bool early_capture_start;
	bool j716f;
};

struct q6asm_dai_data {
	struct snd_soc_dai_driver *dais;
	int num_dais;
	long long int sid;
	bool j716f;
};

static const struct snd_pcm_hardware q6asm_dai_hardware_capture = {
	.info =                 (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_BATCH |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_NO_REWINDS | SNDRV_PCM_INFO_SYNC_APPLPTR |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_48000,
	.rate_min =             8000,
	.rate_max =             48000,
	.channels_min =         1,
	.channels_max =         4,
	.buffer_bytes_max =     CAPTURE_MAX_NUM_PERIODS * 4096,
	.period_bytes_min =	CAPTURE_MIN_PERIOD_SIZE,
	.period_bytes_max =     4096,
	.periods_min =          CAPTURE_MIN_NUM_PERIODS,
	.periods_max =          CAPTURE_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

/*
 * Lenovo J716F: the capture period has to be able to hold what the DSP
 * delivers per ASM_DATA_CMD_READ.  With the upstream 4096-byte limit ALSA
 * could not carve the 24000-byte DSP periods, ended up with a single-period
 * buffer and never advanced hw_ptr, so arecord failed with -EIO even though
 * the DSP was returning data.  The limit is board specific: raise it only
 * where that firmware behaviour has been observed.
 */
static const struct snd_pcm_hardware q6asm_dai_hardware_capture_j716f = {
	.info =                 (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_BATCH |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_NO_REWINDS | SNDRV_PCM_INFO_SYNC_APPLPTR |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_48000,
	.rate_min =             8000,
	.rate_max =             48000,
	.channels_min =         1,
	.channels_max =         4,
	.buffer_bytes_max =     CAPTURE_MAX_NUM_PERIODS * CAPTURE_MAX_PERIOD_SIZE,
	.period_bytes_min =	CAPTURE_MIN_PERIOD_SIZE,
	.period_bytes_max =     CAPTURE_MAX_PERIOD_SIZE,
	.periods_min =          CAPTURE_MIN_NUM_PERIODS,
	.periods_max =          CAPTURE_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

static const struct snd_pcm_hardware q6asm_dai_hardware_playback = {
	.info =                 (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_BATCH |
				SNDRV_PCM_INFO_BLOCK_TRANSFER |
				SNDRV_PCM_INFO_MMAP_VALID |
				SNDRV_PCM_INFO_NO_REWINDS | SNDRV_PCM_INFO_SYNC_APPLPTR |
				SNDRV_PCM_INFO_INTERLEAVED |
				SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME),
	.formats =              (SNDRV_PCM_FMTBIT_S16_LE |
				SNDRV_PCM_FMTBIT_S24_LE),
	.rates =                SNDRV_PCM_RATE_8000_192000,
	.rate_min =             8000,
	.rate_max =             192000,
	.channels_min =         1,
	.channels_max =         8,
	.buffer_bytes_max =     (PLAYBACK_MAX_NUM_PERIODS *
				PLAYBACK_MAX_PERIOD_SIZE),
	.period_bytes_min =	PLAYBACK_MIN_PERIOD_SIZE,
	.period_bytes_max =     PLAYBACK_MAX_PERIOD_SIZE,
	.periods_min =          PLAYBACK_MIN_NUM_PERIODS,
	.periods_max =          PLAYBACK_MAX_NUM_PERIODS,
	.fifo_size =            0,
};

#define Q6ASM_FEDAI_DRIVER(num) { \
		.playback = {						\
			.stream_name = "MultiMedia"#num" Playback",	\
			.rates = (SNDRV_PCM_RATE_8000_48000 |		\
				  SNDRV_PCM_RATE_12000 |		\
				  SNDRV_PCM_RATE_24000 |		\
				  SNDRV_PCM_RATE_88200 |		\
				  SNDRV_PCM_RATE_96000 |		\
				  SNDRV_PCM_RATE_176400 |		\
				  SNDRV_PCM_RATE_192000),		\
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |		\
					SNDRV_PCM_FMTBIT_S24_LE),	\
			.channels_min = 1,				\
			.channels_max = 8,				\
			.rate_min =     8000,				\
			.rate_max =	192000,				\
		},							\
		.capture = {						\
			.stream_name = "MultiMedia"#num" Capture",	\
			.rates = (SNDRV_PCM_RATE_8000_48000 |		\
				  SNDRV_PCM_RATE_12000 |		\
				  SNDRV_PCM_RATE_24000),		\
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |		\
				    SNDRV_PCM_FMTBIT_S24_LE),		\
			.channels_min = 1,				\
			.channels_max = 4,				\
			.rate_min =     8000,				\
			.rate_max =	48000,				\
		},							\
		.name = "MultiMedia"#num,				\
		.id = MSM_FRONTEND_DAI_MULTIMEDIA##num,			\
	}

static const struct snd_compr_codec_caps q6asm_compr_caps = {
	.num_descriptors = 1,
	.descriptor[0].max_ch = 2,
	.descriptor[0].sample_rates = {	8000, 11025, 12000, 16000, 22050,
					24000, 32000, 44100, 48000, 88200,
					96000, 176400, 192000 },
	.descriptor[0].num_sample_rates = 13,
	.descriptor[0].bit_rate[0] = 320,
	.descriptor[0].bit_rate[1] = 128,
	.descriptor[0].num_bitrates = 2,
	.descriptor[0].profiles = 0,
	.descriptor[0].modes = SND_AUDIOCHANMODE_MP3_STEREO,
	.descriptor[0].formats = 0,
};

static int q6asm_dai_fill_capture(struct q6asm_dai_rtd *prtd)
{
	int i, rc = 0;

	for (i = 0; i < prtd->periods; i++) {
		rc = q6asm_read(prtd->audio_client, prtd->stream_id);
		if (rc < 0)
			break;
	}

	return rc;
}

/*
 * A failed READ post (other than "the ring is full") means the stream can
 * no longer be fed.  Report it as an XRUN instead of leaving the PCM in a
 * RUNNING state that never produces data again.
 */
static void q6asm_dai_fill_or_fail(struct q6asm_dai_rtd *prtd,
				   struct snd_pcm_substream *substream)
{
	bool xrun;
	int rc;

	rc = q6asm_dai_fill_capture(prtd);
	if (rc >= 0 || rc == -EAGAIN)
		return;

	dev_err_ratelimited(prtd->dev, "capture read post failed: %d\n", rc);
	spin_lock(&prtd->lock);
	prtd->state = Q6ASM_STREAM_ERROR;
	xrun = prtd->started;
	spin_unlock(&prtd->lock);
	if (xrun)
		snd_pcm_stop_xrun(substream);
}

static bool q6asm_dai_capture_active(struct q6asm_dai_rtd *prtd)
{
	bool active;

	spin_lock(&prtd->lock);
	active = prtd->state == Q6ASM_STREAM_RUNNING;
	spin_unlock(&prtd->lock);

	return active;
}

static void event_handler(const struct q6asm_client_event *event, void *priv)
{
	struct q6asm_dai_rtd *prtd = priv;
	struct snd_pcm_substream *substream = prtd->substream;
	bool elapsed = false, xrun = false, retry = false;

	if (!substream)
		return;

	switch (event->id) {
	case Q6ASM_EVENT_RUN_DONE:
		spin_lock(&prtd->lock);
		if (!event->status) {
			if (prtd->state == Q6ASM_STREAM_STARTING)
				prtd->state = Q6ASM_STREAM_RUNNING;
			if (prtd->started && prtd->elapsed_pending) {
				prtd->elapsed_pending = false;
				elapsed = true;
			}
			retry = (substream->stream ==
				 SNDRV_PCM_STREAM_CAPTURE) &&
				prtd->state == Q6ASM_STREAM_RUNNING;
		} else {
			prtd->state = Q6ASM_STREAM_ERROR;
			xrun = prtd->started;
		}
		spin_unlock(&prtd->lock);
		if (retry)
			q6asm_dai_fill_or_fail(prtd, substream);
		if (elapsed)
			snd_pcm_period_elapsed(substream);
		if (xrun)
			snd_pcm_stop_xrun(substream);
		break;

	case Q6ASM_EVENT_PAUSE_DONE:
		spin_lock(&prtd->lock);
		if (prtd->state == Q6ASM_STREAM_PAUSING) {
			if (!event->status)
				prtd->state = Q6ASM_STREAM_PAUSED;
			else
				prtd->state = Q6ASM_STREAM_RUNNING;
		}
		spin_unlock(&prtd->lock);
		break;

	case Q6ASM_EVENT_EOS_DONE:
		spin_lock(&prtd->lock);
		if (prtd->state == Q6ASM_STREAM_STOPPING)
			prtd->state = Q6ASM_STREAM_STOPPED;
		spin_unlock(&prtd->lock);
		break;

	case Q6ASM_EVENT_READ_DONE:
		spin_lock(&prtd->lock);
		if (prtd->state == Q6ASM_STREAM_RUNNING) {
			if (prtd->started)
				elapsed = true;
			else
				prtd->elapsed_pending = true;
			retry = true;
		}
		spin_unlock(&prtd->lock);
		if (elapsed)
			snd_pcm_period_elapsed(substream);
		if (retry && q6asm_dai_capture_active(prtd))
			q6asm_dai_fill_or_fail(prtd, substream);
		break;

	case Q6ASM_EVENT_READ_FAILED:
		/*
		 * A rejected READ left a hole in the ring.  Re-posting it
		 * would put later periods ahead of it in the DSP queue, so
		 * the only correct answer is an XRUN; the next prepare
		 * rebuilds the queue in order.
		 */
		spin_lock(&prtd->lock);
		prtd->state = Q6ASM_STREAM_ERROR;
		xrun = prtd->started;
		spin_unlock(&prtd->lock);
		if (xrun)
			snd_pcm_stop_xrun(substream);
		break;

	case Q6ASM_EVENT_WRITE_DONE:
		spin_lock(&prtd->lock);
		if (prtd->started)
			elapsed = true;
		else
			prtd->elapsed_pending = true;
		spin_unlock(&prtd->lock);
		if (elapsed)
			snd_pcm_period_elapsed(substream);
		break;

	case Q6ASM_EVENT_WRITE_FAILED:
		spin_lock(&prtd->lock);
		prtd->state = Q6ASM_STREAM_ERROR;
		xrun = prtd->started;
		spin_unlock(&prtd->lock);
		if (xrun)
			snd_pcm_stop_xrun(substream);
		break;

	default:
		break;
	}
}

/*
 * Lenovo J716F: map the period ring into the ADSP.
 *
 * The address keeps the upstream layout (IOVA | sid << 32); the 29-bit DMA
 * mask set in q6asm_dai_pcm_new() keeps the IOVA inside the LPASS audio
 * window 0x1000_0000-0x1FFF_FFFF, which is what the kona ADSP requires.
 * Only this board's firmware has shown the early-boot rejection that makes
 * the second, plain-IOVA encoding necessary.
 */
static int q6asm_dai_map_regions(struct device *dev,
				 struct snd_pcm_substream *substream,
				 struct q6asm_dai_rtd *prtd, u64 sid_bits)
{
	struct snd_dma_buffer *dma = substream->runtime->dma_buffer_p;
	dma_addr_t dma_addr;
	phys_addr_t phys;
	size_t period_sz;
	int ret, attempt, last_ret = -ENOMEM;

	if (!prtd->periods || !dma) {
		dev_err(dev, "J716F: no DMA buffer or periods for the DMA map\n");
		return -EINVAL;
	}
	dma_addr = dma->addr;
	period_sz = prtd->pcm_size / prtd->periods;

	if (prtd->j716f && lower_32_bits(dma_addr) < 0x10000000) {
		dev_err(dev,
			"J716F: DMA address %#llx is below the LPASS window 0x10000000; refusing to map\n",
			(unsigned long long)dma_addr);
		return -ERANGE;
	}

	for (attempt = 0; attempt < (prtd->j716f ? 2 : 1); attempt++) {
		/* 0: upstream layout, 1: plain IOVA (no SID in the msw) */
		if (attempt == 0)
			phys = dma_addr | sid_bits;
		else
			phys = (phys_addr_t)lower_32_bits(dma_addr);

		ret = q6asm_map_memory_regions(substream->stream,
					       prtd->audio_client, phys,
					       period_sz, prtd->periods);
		if (ret >= 0) {
			prtd->phys = phys;
			prtd->map_state = Q6ASM_MAP_CONFIRMED;
			dev_dbg(dev,
				"J716F: map dir=%d mode=%d phys=%#llx dma=%#llx period=%zu periods=%u accepted\n",
				substream->stream, attempt,
				(unsigned long long)phys,
				(unsigned long long)dma_addr, period_sz,
				prtd->periods);
			return 0;
		}

		last_ret = ret;
		dev_err(dev,
			"J716F: map dir=%d mode=%d phys=%#llx dma=%#llx period=%zu periods=%u failed (%d)\n",
			substream->stream, attempt, (unsigned long long)phys,
			(unsigned long long)dma_addr, period_sz,
			prtd->periods, ret);

		/*
		 * A timeout, a missing handle or a malformed answer means the
		 * DSP may have executed the MAP without the host seeing the
		 * result.  Retiring the session is the only safe answer.
		 */
		if (ret == -ETIMEDOUT || ret == -EPROTO) {
			dev_err(dev,
				"J716F: MAP unresolved; session %d quarantined\n",
				prtd->stream_id);
			q6asm_quarantine_session(prtd->audio_client);
			prtd->map_state = Q6ASM_MAP_UNKNOWN;
			return ret;
		}
		if (ret != -EREMOTEIO)
			return ret;
	}

	return last_ret;
}

/*
 * Release the remote stream, then its mapping.  A step is only skipped when
 * it is known to have completed.  On any unresolved step the session is
 * quarantined and the PCM pages are moved out of ALSA so they cannot be
 * freed while the ADSP may still hold the mapping.
 */
static int q6asm_dai_release_remote(struct snd_soc_component *component,
				    struct snd_pcm_substream *substream,
				    struct q6asm_dai_rtd *prtd)
{
	struct snd_soc_pcm_runtime *soc_prtd =
		snd_soc_substream_to_rtd(substream);
	bool failed = false;
	int ret = 0;

	if (prtd->teardown_failed)
		return -EIO;

	if (prtd->stream_opened) {
		ret = q6asm_cmd(prtd->audio_client, prtd->stream_id, CMD_CLOSE);
		if (ret < 0) {
			dev_err(component->dev,
				"J716F: CMD_CLOSE failed %d; session %d quarantined\n",
				ret, prtd->stream_id);
			q6asm_quarantine_session(prtd->audio_client);
			failed = true;
		} else {
			prtd->stream_opened = false;
		}
	}

	if (!failed && prtd->map_state == Q6ASM_MAP_CONFIRMED) {
		ret = q6asm_unmap_memory_regions(substream->stream,
						 prtd->audio_client);
		if (ret < 0) {
			dev_err(component->dev,
				"J716F: UNMAP failed %d after CLOSE; session %d quarantined\n",
				ret, prtd->stream_id);
			q6asm_quarantine_session(prtd->audio_client);
			failed = true;
		} else {
			prtd->map_state = Q6ASM_MAP_NONE;
		}
	} else if (!failed && prtd->map_state == Q6ASM_MAP_UNKNOWN) {
		/* The mapping was never resolved; there is no handle to unmap. */
		failed = true;
		ret = -EIO;
	}

	if (!failed) {
		q6routing_stream_close(soc_prtd->dai_link->id,
				       substream->stream);
		prtd->state = Q6ASM_STREAM_IDLE;
		return 0;
	}

	prtd->teardown_failed = true;
	prtd->needs_hold = true;
	return ret;
}

/*
 * Move a PCM DMA buffer that the DSP may still own out of ALSA's reach.
 * For the fixed pre-allocation this means detaching substream->dma_buffer;
 * for a dynamically allocated runtime buffer the snd_dma_buffer itself is
 * detached and freed after its contents are copied into the hold record.
 */
static void q6asm_dai_hold_pcm_buffer(struct snd_pcm_substream *substream,
				      struct q6asm_dai_rtd *prtd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_dma_buffer *dma = runtime->dma_buffer_p;
	bool dynamic;

	if (prtd->dma_held || !dma)
		return;

	dynamic = dma != &substream->dma_buffer;
	q6asm_hold_buffer(prtd->audio_client, dma, prtd->session_id);
	prtd->dma_held = true;

	if (dynamic) {
		/* ALSA would free this at hw_free; the hold record owns it now. */
		runtime->dma_buffer_p = NULL;
		runtime->dma_area = NULL;
		runtime->dma_addr = 0;
		runtime->dma_bytes = 0;
		kfree(dma);
	}
}

static int q6asm_dai_prepare(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = snd_soc_substream_to_rtd(substream);
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct q6asm_dai_data *pdata;
	struct device *dev = component->dev;
	bool was_active;
	int ret;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata)
		return -EINVAL;

	if (!prtd || !prtd->audio_client)
		return -EINVAL;

	if (q6asm_session_is_quarantined(prtd->audio_client)) {
		dev_err(dev, "session %d is quarantined; refusing prepare\n",
			prtd->stream_id);
		return -EIO;
	}

	/*
	 * Lenovo J716F: keep pcm_count consistent with the period size that is
	 * handed to the DSP in q6asm_map_memory_regions() below
	 * (pcm_size / periods).  snd_pcm_lib_period_bytes() can differ from
	 * that, which made the driver ask the DSP for a different amount of
	 * data per period than the mapping described.
	 */
	prtd->pcm_count = prtd->periods ?
		(prtd->pcm_size / prtd->periods) :
		snd_pcm_lib_period_bytes(substream);
	prtd->queue_ptr = 0;

	spin_lock(&prtd->lock);
	prtd->started = false;
	prtd->elapsed_pending = false;
	spin_unlock(&prtd->lock);

	/*
	 * Retire the old remote stream before opening the new one.  The state
	 * is moved to IDLE first so a completion that is still in the APR
	 * worker cannot re-post reads or call period_elapsed during teardown.
	 */
	spin_lock(&prtd->lock);
	was_active = prtd->state != Q6ASM_STREAM_IDLE;
	prtd->state = Q6ASM_STREAM_IDLE;
	spin_unlock(&prtd->lock);

	if (was_active) {
		ret = q6asm_dai_release_remote(component, substream, prtd);
		if (ret)
			return ret;
	}

	if (prtd->dma_held) {
		/* The previous close could not prove the DSP released the ring. */
		return -EIO;
	}

	ret = q6asm_dai_map_regions(dev, substream, prtd,
				    pdata->sid < 0 ? 0 : ((u64)pdata->sid << 32));
	if (ret < 0) {
		dev_err(dev, "Audio Start: DMA mapping failed rc = %d\n", ret);
		return ret;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = q6asm_open_write(prtd->audio_client, prtd->stream_id,
				       FORMAT_LINEAR_PCM, 0,
				       prtd->bits_per_sample, false);
	else
		ret = q6asm_open_read(prtd->audio_client, prtd->stream_id,
				      FORMAT_LINEAR_PCM,
				      prtd->bits_per_sample);
	if (ret < 0) {
		dev_err(dev, "q6asm_open failed %d\n", ret);
		goto err_unmap;
	}
	prtd->stream_opened = true;

	prtd->session_id = q6asm_get_session_id(prtd->audio_client);
	ret = q6routing_stream_open(soc_prtd->dai_link->id, LEGACY_PCM_MODE,
				    prtd->session_id, substream->stream);
	if (ret) {
		dev_err(dev, "stream reg failed ret:%d\n", ret);
		goto err_release;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = q6asm_media_format_block_multi_ch_pcm(
				prtd->audio_client, prtd->stream_id,
				runtime->rate, runtime->channels, NULL,
				prtd->bits_per_sample);
		if (ret < 0) {
			dev_err(dev, "CMD Format block failed %d\n", ret);
			goto err_release;
		}
		spin_lock(&prtd->lock);
		prtd->state = Q6ASM_STREAM_PREPARED;
		spin_unlock(&prtd->lock);
	} else {
		ret = q6asm_enc_cfg_blk_pcm_format_support(prtd->audio_client,
							   prtd->stream_id,
							   runtime->rate,
							   runtime->channels,
							   prtd->bits_per_sample);
		if (ret < 0) {
			dev_err(dev, "capture format block failed %d\n", ret);
			goto err_release;
		}

		if (prtd->early_capture_start) {
			/*
			 * The kona ADSP answers READ_V2 with ADSP_ENOTREADY
			 * until the session is running, and its first
			 * completion takes longer than ALSA's default read
			 * wait.  Start the session here (prepare is sleepable)
			 * and let the RUN_DONE event queue the reads.
			 */
			spin_lock(&prtd->lock);
			prtd->state = Q6ASM_STREAM_STARTING;
			spin_unlock(&prtd->lock);

			ret = q6asm_run(prtd->audio_client, prtd->stream_id,
					0, 0, 0);
			if (ret < 0) {
				dev_err(dev, "capture run failed %d\n", ret);
				goto err_release;
			}
		} else {
			/*
			 * Other firmware accepts reads before RUN; keep the
			 * upstream ordering there.
			 */
			spin_lock(&prtd->lock);
			prtd->state = Q6ASM_STREAM_PREPARED;
			spin_unlock(&prtd->lock);
			q6asm_dai_fill_capture(prtd);
		}
	}

	return 0;

err_release:
	q6asm_dai_release_remote(component, substream, prtd);
	return ret;

err_unmap:
	if (prtd->map_state == Q6ASM_MAP_CONFIRMED) {
		int urc = q6asm_unmap_memory_regions(substream->stream,
						     prtd->audio_client);

		if (urc < 0) {
			dev_err(dev, "cleanup UNMAP failed %d\n", urc);
			q6asm_quarantine_session(prtd->audio_client);
			prtd->map_state = Q6ASM_MAP_UNKNOWN;
			prtd->needs_hold = true;
		} else {
			prtd->map_state = Q6ASM_MAP_NONE;
		}
	}
	return ret;
}

static int q6asm_dai_ack(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	snd_pcm_uframes_t appl, qp, delta, boundary;
	unsigned int avail;
	bool running;
	int i, rc;

	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	spin_lock(&prtd->lock);
	running = prtd->state == Q6ASM_STREAM_RUNNING;
	spin_unlock(&prtd->lock);
	if (!running)
		return 0;

	if (!runtime->period_size || !runtime->periods)
		return 0;

	boundary = runtime->boundary;
	if (!boundary)
		return -EINVAL;

	/*
	 * appl_ptr wraps at runtime->boundary; a plain subtraction can
	 * underflow and make the loop below queue an unbounded number of
	 * periods under the PCM stream lock.
	 */
	appl = runtime->control->appl_ptr;
	qp = prtd->queue_ptr;
	if (qp >= boundary)
		qp %= boundary;

	delta = (appl >= qp) ? appl - qp : boundary - qp + appl;
	if (delta > runtime->buffer_size) {
		dev_warn_ratelimited(component->dev,
				     "J716F: ack delta %lu > buffer %lu, clamping\n",
				     (unsigned long)delta,
				     (unsigned long)runtime->buffer_size);
		delta = runtime->buffer_size;
	}

	avail = delta / runtime->period_size;
	if (!avail)
		return 0;

	for (i = 0; i < (int)avail; i++) {
		rc = q6asm_write_async(prtd->audio_client, prtd->stream_id,
				       prtd->pcm_count, 0, 0, 0);
		if (rc < 0)
			break;
		prtd->queue_ptr = (qp + runtime->period_size) % boundary;
		qp = prtd->queue_ptr;
	}

	/* A full ring is not an error; the next completion frees a slot. */
	if (rc == -EAGAIN)
		return 0;

	return rc;
}

static int q6asm_dai_trigger(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream, int cmd)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	bool flush = false;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		bool send_run;

		spin_lock(&prtd->lock);
		switch (prtd->state) {
		case Q6ASM_STREAM_RUNNING:
			/* The remote is already running (capture prepare). */
			prtd->started = true;
			flush = prtd->elapsed_pending;
			prtd->elapsed_pending = false;
			send_run = false;
			break;
		case Q6ASM_STREAM_STARTING:
			/* RUN is in flight; RUN_DONE will arm the queue. */
			prtd->started = true;
			send_run = false;
			break;
		case Q6ASM_STREAM_PAUSED:
		case Q6ASM_STREAM_PAUSING:
			/* PAUSE_RELEASE/RESUME must restart the paused stream. */
			prtd->state = Q6ASM_STREAM_STARTING;
			prtd->started = true;
			send_run = true;
			break;
		case Q6ASM_STREAM_PREPARED:
			prtd->state = Q6ASM_STREAM_STARTING;
			prtd->started = true;
			send_run = true;
			break;
		default:
			spin_unlock(&prtd->lock);
			return -EINVAL;
		}
		spin_unlock(&prtd->lock);

		if (send_run) {
			ret = q6asm_run_nowait(prtd->audio_client,
					       prtd->stream_id, 0, 0, 0);
			if (ret < 0) {
				spin_lock(&prtd->lock);
				prtd->started = false;
				prtd->state = Q6ASM_STREAM_ERROR;
				spin_unlock(&prtd->lock);
			}
		}
		if (flush)
			snd_pcm_period_elapsed_under_stream_lock(substream);
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE &&
		    q6asm_dai_capture_active(prtd)) {
			int frc = q6asm_dai_fill_capture(prtd);

			if (frc < 0 && frc != -EAGAIN) {
				spin_lock(&prtd->lock);
				prtd->state = Q6ASM_STREAM_ERROR;
				spin_unlock(&prtd->lock);
				return frc;
			}
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP: {
		enum stream_state old;

		spin_lock(&prtd->lock);
		old = prtd->state;
		prtd->started = false;
		prtd->elapsed_pending = false;
		if (old == Q6ASM_STREAM_RUNNING ||
		    old == Q6ASM_STREAM_STARTING ||
		    old == Q6ASM_STREAM_PAUSED ||
		    old == Q6ASM_STREAM_PAUSING)
			prtd->state = Q6ASM_STREAM_STOPPING;
		else
			prtd->state = Q6ASM_STREAM_STOPPED;
		spin_unlock(&prtd->lock);

		if (old != Q6ASM_STREAM_RUNNING &&
		    old != Q6ASM_STREAM_STARTING &&
		    old != Q6ASM_STREAM_PAUSED &&
		    old != Q6ASM_STREAM_PAUSING)
			break;

		/*
		 * Playback is ended with EOS; a capture stream has no EOS, so
		 * the vendor firmware uses PAUSE.
		 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = q6asm_cmd_nowait(prtd->audio_client,
					       prtd->stream_id, CMD_EOS);
		else
			ret = q6asm_cmd_nowait(prtd->audio_client,
					       prtd->stream_id, CMD_PAUSE);
		if (ret < 0)
			dev_dbg_ratelimited(component->dev,
					    "stop command failed %d\n", ret);
		break;
	}

	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH: {
		bool running;

		spin_lock(&prtd->lock);
		running = prtd->state == Q6ASM_STREAM_RUNNING ||
			  prtd->state == Q6ASM_STREAM_STARTING;
		if (running)
			prtd->state = Q6ASM_STREAM_PAUSING;
		prtd->started = false;
		spin_unlock(&prtd->lock);

		if (!running)
			break;
		ret = q6asm_cmd_nowait(prtd->audio_client, prtd->stream_id,
				       CMD_PAUSE);
		if (ret < 0) {
			spin_lock(&prtd->lock);
			prtd->state = Q6ASM_STREAM_RUNNING;
			spin_unlock(&prtd->lock);
		}
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6asm_dai_open(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct snd_soc_pcm_runtime *soc_prtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(soc_prtd, 0);
	const struct snd_pcm_hardware *hw;
	struct q6asm_dai_rtd *prtd;
	struct q6asm_dai_data *pdata;
	struct device *dev = component->dev;
	int ret;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata) {
		dev_err(dev, "Drv data not found ..\n");
		return -EINVAL;
	}

	prtd = kzalloc_obj(*prtd);
	if (prtd == NULL)
		return -ENOMEM;

	spin_lock_init(&prtd->lock);
	prtd->substream = substream;
	prtd->j716f = pdata->j716f;
	prtd->early_capture_start = pdata->j716f &&
				    substream->stream == SNDRV_PCM_STREAM_CAPTURE;
	prtd->audio_client = q6asm_audio_client_alloc(dev, event_handler, prtd,
						      cpu_dai->driver->id,
						      LEGACY_PCM_MODE);
	if (IS_ERR(prtd->audio_client)) {
		ret = PTR_ERR(prtd->audio_client);
		dev_err(dev, "cannot allocate an audio client: %d\n", ret);
		kfree(prtd);
		return ret;
	}
	if (pdata->j716f &&
	    substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		q6asm_audio_client_set_flags(prtd->audio_client,
					     Q6ASM_CLIENT_STRICT_CAPTURE);

	/* DSP expects stream id from 1 */
	prtd->stream_id = 1;

	/*
	 * Lenovo J716F: the ADSP needs ~230 ms before it raises the first
	 * ASM_CLIENT_EVENT_DATA_READ_DONE, while ALSA's default capture read
	 * wait is only buffer_size * 1100 / rate (~220 ms for the same
	 * buffer).  prepare() starts the stream and queues the reads so the
	 * data is normally ready by the first userspace read; keep a floor
	 * for the case where it is not.  Playback keeps the default.
	 */
	if (prtd->early_capture_start)
		substream->wait_time = 10000;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		hw = &q6asm_dai_hardware_playback;
	else if (pdata->j716f)
		hw = &q6asm_dai_hardware_capture_j716f;
	else
		hw = &q6asm_dai_hardware_capture;

	runtime->hw = *hw;
	ret = snd_pcm_hw_constraint_integer(runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		goto err_client;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_pcm_hw_constraint_minmax(runtime,
			SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
			PLAYBACK_MIN_NUM_PERIODS * PLAYBACK_MIN_PERIOD_SIZE,
			PLAYBACK_MAX_NUM_PERIODS * PLAYBACK_MAX_PERIOD_SIZE);
		if (ret < 0)
			goto err_client;
	}

	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 480);
	if (ret < 0)
		goto err_client;
	ret = snd_pcm_hw_constraint_step(runtime, 0,
		SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 480);
	if (ret < 0)
		goto err_client;

	runtime->private_data = prtd;
	snd_soc_set_runtime_hwparams(substream, hw);
	runtime->dma_bytes = hw->buffer_bytes_max;

	/*
	 * Lenovo J716F: the CS35L41 ASP path is wired for 16-bit slots.
	 * The ADSP accepts S24_LE but the amplifier output is heavily
	 * attenuated (measured in R4/M43: only audible with the ear on the
	 * speaker at full volume); S16_LE is the verified format.  Do not
	 * advertise a format this board cannot reproduce correctly.
	 */
	if (pdata->j716f)
		runtime->hw.formats = SNDRV_PCM_FMTBIT_S16_LE;

	return 0;

err_client:
	q6asm_audio_client_free(prtd->audio_client);
	kfree(prtd);
	return ret;
}

static int q6asm_dai_hw_free(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	int ret;

	if (!prtd || !prtd->audio_client)
		return 0;

	spin_lock(&prtd->lock);
	prtd->started = false;
	prtd->elapsed_pending = false;
	spin_unlock(&prtd->lock);

	ret = q6asm_dai_release_remote(component, substream, prtd);
	if (prtd->needs_hold) {
		q6asm_dai_hold_pcm_buffer(substream, prtd);
		prtd->needs_hold = false;
	}

	return 0;
}

static int q6asm_dai_close(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	int ret;

	if (!prtd)
		return 0;

	if (prtd->audio_client) {
		spin_lock(&prtd->lock);
		prtd->started = false;
		prtd->elapsed_pending = false;
		prtd->state = Q6ASM_STREAM_STOPPED;
		spin_unlock(&prtd->lock);

		q6asm_audio_client_clear_callback(prtd->audio_client);
		ret = q6asm_dai_release_remote(component, substream, prtd);
		if (ret && prtd->needs_hold) {
			q6asm_dai_hold_pcm_buffer(substream, prtd);
			prtd->needs_hold = false;
		}
		q6asm_audio_client_free(prtd->audio_client);
		prtd->audio_client = NULL;
	}
	kfree(prtd);
	return 0;
}

static snd_pcm_uframes_t q6asm_dai_pointer(struct snd_soc_component *component,
					   struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	snd_pcm_uframes_t ptr;

	if (!runtime->periods || !prtd || !prtd->audio_client)
		return 0;

	ptr = q6asm_get_hw_pointer(prtd->audio_client, substream->stream) %
	      runtime->periods;
	return ptr * runtime->period_size;
}

static int q6asm_dai_hw_params(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;

	prtd->pcm_size = params_buffer_bytes(params);
	prtd->periods = params_periods(params);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		prtd->bits_per_sample = 16;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		prtd->bits_per_sample = 24;
		break;
	}

	return 0;
}

static void compress_event_handler(const struct q6asm_client_event *event,
				   void *priv)
{
	struct q6asm_dai_rtd *prtd = priv;
	struct snd_compr_stream *substream = prtd->cstream;
	u32 wflags = 0;
	uint32_t bytes_to_write;
	bool is_last_buffer = false;

	guard(spinlock_irqsave)(&prtd->lock);

	switch (event->id) {
	case Q6ASM_EVENT_RUN_DONE:
		if (event->status) {
			prtd->state = Q6ASM_STREAM_ERROR;
			break;
		}
		prtd->state = Q6ASM_STREAM_RUNNING;
		if (!prtd->bytes_sent) {
			q6asm_stream_remove_initial_silence(prtd->audio_client,
						    prtd->stream_id,
						    prtd->initial_samples_drop);

			if (q6asm_write_async(prtd->audio_client,
					      prtd->stream_id, prtd->pcm_count,
					      0, 0, 0) < 0) {
				prtd->state = Q6ASM_STREAM_ERROR;
				break;
			}
			prtd->bytes_sent += prtd->pcm_count;
		}
		break;

	case Q6ASM_EVENT_EOS_DONE:
		if (event->status)
			dev_warn_ratelimited(prtd->dev,
					     "compressed EOS rejected: %u\n",
					     event->status);
		if (prtd->notify_on_drain) {
			if (substream->partial_drain) {
				/*
				 * Close the old stream and make it stale; keep
				 * its id so teardown can confirm the close.
				 */
				prtd->prev_stream_id = prtd->stream_id;
				q6asm_cmd_nowait(prtd->audio_client,
						 prtd->stream_id, CMD_CLOSE);
				/* Valid stream ids start from 1. */
				prtd->stream_id = (prtd->stream_id == 1 ? 2 : 1);
			}

			snd_compr_drain_notify(prtd->cstream);
			prtd->notify_on_drain = false;
		}
		break;

	case Q6ASM_EVENT_WRITE_DONE:
		/*
		 * The submitted length is authoritative; the token no longer
		 * carries it, so a 64 KiB period is accounted correctly.
		 */
		prtd->copied_total += event->len;
		snd_compr_fragment_elapsed(substream);

		if (prtd->state != Q6ASM_STREAM_RUNNING)
			break;

		if (prtd->bytes_sent > prtd->bytes_received) {
			prtd->state = Q6ASM_STREAM_ERROR;
			break;
		}
		if (prtd->bytes_received - prtd->bytes_sent > prtd->pcm_count) {
			bytes_to_write = prtd->pcm_count;
		} else {
			if (substream->partial_drain || prtd->notify_on_drain)
				is_last_buffer = true;
			bytes_to_write = prtd->bytes_received - prtd->bytes_sent;
		}

		if (bytes_to_write) {
			if (substream->partial_drain && is_last_buffer) {
				wflags |= ASM_LAST_BUFFER_FLAG;
				q6asm_stream_remove_trailing_silence(prtd->audio_client,
						     prtd->stream_id,
						     prtd->trailing_samples_drop);
			}

			if (q6asm_write_async(prtd->audio_client,
					      prtd->stream_id, bytes_to_write,
					      0, 0, wflags) < 0) {
				prtd->state = Q6ASM_STREAM_ERROR;
				break;
			}
			prtd->bytes_sent += bytes_to_write;
		}

		if (prtd->notify_on_drain && is_last_buffer)
			q6asm_cmd_nowait(prtd->audio_client,
					 prtd->stream_id, CMD_EOS);
		break;

	case Q6ASM_EVENT_WRITE_FAILED:
		dev_err_ratelimited(prtd->dev,
				    "compressed write rejected: %u\n",
				    event->status);
		prtd->state = Q6ASM_STREAM_ERROR;
		break;

	default:
		break;
	}
}

static int q6asm_dai_compr_open(struct snd_soc_component *component,
				struct snd_compr_stream *stream)
{
	struct snd_soc_pcm_runtime *rtd = stream->private_data;
	struct snd_compr_runtime *runtime = stream->runtime;
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct q6asm_dai_data *pdata;
	struct device *dev = component->dev;
	struct q6asm_dai_rtd *prtd;
	int stream_id, size, ret;

	stream_id = cpu_dai->driver->id;
	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata) {
		dev_err(dev, "Drv data not found ..\n");
		return -EINVAL;
	}

	prtd = kzalloc_obj(*prtd);
	if (!prtd)
		return -ENOMEM;

	/* DSP expects stream id from 1 */
	prtd->stream_id = 1;
	prtd->dev = dev;
	spin_lock_init(&prtd->lock);

	prtd->cstream = stream;
	prtd->audio_client = q6asm_audio_client_alloc(dev,
					(q6asm_cb)compress_event_handler,
					prtd, stream_id, LEGACY_PCM_MODE);
	if (IS_ERR(prtd->audio_client)) {
		dev_err(dev, "Could not allocate memory\n");
		ret = PTR_ERR(prtd->audio_client);
		goto free_prtd;
	}

	size = COMPR_PLAYBACK_MAX_FRAGMENT_SIZE *
			COMPR_PLAYBACK_MAX_NUM_FRAGMENTS;
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, dev, size,
				  &prtd->dma_buffer);
	if (ret) {
		dev_err(dev, "Cannot allocate buffer(s)\n");
		goto free_client;
	}

	if (pdata->sid < 0)
		prtd->phys = prtd->dma_buffer.addr;
	else
		prtd->phys = prtd->dma_buffer.addr | (pdata->sid << 32);

	snd_compr_set_runtime_buffer(stream, &prtd->dma_buffer);
	runtime->private_data = prtd;

	return 0;

free_client:
	q6asm_audio_client_free(prtd->audio_client);
free_prtd:
	kfree(prtd);

	return ret;
}

/*
 * Give a compressed stream back in the order the ADSP expects: CLOSE the
 * stream, then UNMAP the buffer, then drop the routing.  A step is only
 * skipped when it is known to have completed, so one failure leaves the
 * remaining ownership question to the caller.  Returns true when the remote
 * state could not be confirmed.
 */
static bool q6asm_dai_compr_teardown(struct snd_soc_component *component,
				     struct snd_soc_pcm_runtime *rtd,
				     struct q6asm_dai_rtd *prtd, int dir)
{
	struct audio_client *ac = prtd->audio_client;
	bool failed = false;
	int ret;

	if (prtd->stream_opened) {
		ret = q6asm_cmd(ac, prtd->stream_id, CMD_CLOSE);
		if (ret < 0) {
			dev_err(component->dev,
				"J716F: compr CMD_CLOSE failed %d; session %d quarantined\n",
				ret, prtd->stream_id);
			q6asm_quarantine_session(ac);
			failed = true;
		} else {
			prtd->stream_opened = false;
		}
	}

	if (prtd->next_track_stream_id) {
		ret = q6asm_cmd(ac, prtd->next_track_stream_id, CMD_CLOSE);
		if (ret < 0) {
			dev_err(component->dev,
				"J716F: compr gapless CMD_CLOSE failed %d\n", ret);
			q6asm_quarantine_session(ac);
			failed = true;
		} else {
			prtd->next_track_stream_id = 0;
		}
	}

	if (prtd->prev_stream_id) {
		/*
		 * This id was closed with a nowait CLOSE at a gapless switch.
		 * A rejection here means there is no such stream left (the
		 * asynchronous close already ran); it cannot leak anything.
		 */
		ret = q6asm_cmd(ac, prtd->prev_stream_id, CMD_CLOSE);
		if (ret < 0)
			dev_warn(component->dev,
				 "J716F: gapless stream %u was already closed (%d)\n",
				 prtd->prev_stream_id, ret);
		prtd->prev_stream_id = 0;
	}

	if (!failed && prtd->map_state == Q6ASM_MAP_CONFIRMED) {
		ret = q6asm_unmap_memory_regions(dir, ac);
		if (ret < 0) {
			dev_err(component->dev,
				"J716F: compr UNMAP failed %d after CLOSE; session %d quarantined\n",
				ret, prtd->stream_id);
			q6asm_quarantine_session(ac);
			failed = true;
		} else {
			prtd->map_state = Q6ASM_MAP_NONE;
		}
	} else if (!failed && prtd->map_state == Q6ASM_MAP_UNKNOWN) {
		/* No handle was ever received; only an ADSP stop clears it. */
		failed = true;
	}

	if (!failed)
		q6routing_stream_close(rtd->dai_link->id, dir);
	else if (prtd->map_state != Q6ASM_MAP_NONE)
		prtd->teardown_unknown = true;

	return failed;
}

static int q6asm_dai_compr_free(struct snd_soc_component *component,
				struct snd_compr_stream *stream)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = stream->private_data;

	if (prtd->audio_client) {
		q6asm_audio_client_clear_callback(prtd->audio_client);

		/*
		 * set_params() already ran this teardown when it failed.  Do
		 * not repeat a command whose remote result is unknown; the
		 * session is quarantined and the pages stay held.
		 */
		if (!prtd->teardown_unknown &&
		    q6asm_dai_compr_teardown(component, rtd, prtd,
					     stream->direction))
			prtd->teardown_unknown = true;

		if (prtd->teardown_unknown && !prtd->dma_held) {
			q6asm_hold_buffer(prtd->audio_client, &prtd->dma_buffer,
					  prtd->stream_id);
			prtd->dma_held = true;
		} else if (!prtd->dma_held) {
			snd_dma_free_pages(&prtd->dma_buffer);
		}
		q6asm_audio_client_free(prtd->audio_client);
		prtd->audio_client = NULL;
	}
	kfree(prtd);

	return 0;
}

static int __q6asm_dai_compr_set_codec_params(struct snd_soc_component *component,
					      struct snd_compr_stream *stream,
					      struct snd_codec *codec,
					      int stream_id)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct q6asm_flac_cfg flac_cfg;
	struct q6asm_wma_cfg wma_cfg;
	struct q6asm_alac_cfg alac_cfg;
	struct q6asm_ape_cfg ape_cfg;
	unsigned int wma_v9 = 0;
	struct device *dev = component->dev;
	int ret;
	union snd_codec_options *codec_options;
	struct snd_dec_flac *flac;
	struct snd_dec_wma *wma;
	struct snd_dec_alac *alac;
	struct snd_dec_ape *ape;

	codec_options = &(prtd->codec.options);

	memcpy(&prtd->codec, codec, sizeof(*codec));

	switch (codec->id) {
	case SND_AUDIOCODEC_FLAC:

		memset(&flac_cfg, 0x0, sizeof(struct q6asm_flac_cfg));
		flac = &codec_options->flac_d;

		flac_cfg.ch_cfg = codec->ch_in;
		flac_cfg.sample_rate = codec->sample_rate;
		flac_cfg.stream_info_present = 1;
		flac_cfg.sample_size = flac->sample_size;
		flac_cfg.min_blk_size = flac->min_blk_size;
		flac_cfg.max_blk_size = flac->max_blk_size;
		flac_cfg.max_frame_size = flac->max_frame_size;
		flac_cfg.min_frame_size = flac->min_frame_size;

		ret = q6asm_stream_media_format_block_flac(prtd->audio_client,
							   stream_id,
							   &flac_cfg);
		if (ret < 0) {
			dev_err(dev, "FLAC CMD Format block failed:%d\n", ret);
			return -EIO;
		}
		break;

	case SND_AUDIOCODEC_WMA:
		wma = &codec_options->wma_d;

		memset(&wma_cfg, 0x0, sizeof(struct q6asm_wma_cfg));

		wma_cfg.sample_rate =  codec->sample_rate;
		wma_cfg.num_channels = codec->ch_in;
		wma_cfg.bytes_per_sec = codec->bit_rate / 8;
		wma_cfg.block_align = codec->align;
		wma_cfg.bits_per_sample = prtd->bits_per_sample;
		wma_cfg.enc_options = wma->encoder_option;
		wma_cfg.adv_enc_options = wma->adv_encoder_option;
		wma_cfg.adv_enc_options2 = wma->adv_encoder_option2;

		if (wma_cfg.num_channels == 1)
			wma_cfg.channel_mask = 4; /* Mono Center */
		else if (wma_cfg.num_channels == 2)
			wma_cfg.channel_mask = 3; /* Stereo FL/FR */
		else
			return -EINVAL;

		/* check the codec profile */
		switch (codec->profile) {
		case SND_AUDIOPROFILE_WMA9:
			wma_cfg.fmtag = 0x161;
			wma_v9 = 1;
			break;

		case SND_AUDIOPROFILE_WMA10:
			wma_cfg.fmtag = 0x166;
			break;

		case SND_AUDIOPROFILE_WMA9_PRO:
			wma_cfg.fmtag = 0x162;
			break;

		case SND_AUDIOPROFILE_WMA9_LOSSLESS:
			wma_cfg.fmtag = 0x163;
			break;

		case SND_AUDIOPROFILE_WMA10_LOSSLESS:
			wma_cfg.fmtag = 0x167;
			break;

		default:
			dev_err(dev, "Unknown WMA profile:%x\n",
				codec->profile);
			return -EIO;
		}

		if (wma_v9)
			ret = q6asm_stream_media_format_block_wma_v9(
					prtd->audio_client, stream_id,
					&wma_cfg);
		else
			ret = q6asm_stream_media_format_block_wma_v10(
					prtd->audio_client, stream_id,
					&wma_cfg);
		if (ret < 0) {
			dev_err(dev, "WMA9 CMD failed:%d\n", ret);
			return -EIO;
		}
		break;

	case SND_AUDIOCODEC_ALAC:
		memset(&alac_cfg, 0x0, sizeof(alac_cfg));
		alac = &codec_options->alac_d;

		alac_cfg.sample_rate = codec->sample_rate;
		alac_cfg.avg_bit_rate = codec->bit_rate;
		alac_cfg.bit_depth = prtd->bits_per_sample;
		alac_cfg.num_channels = codec->ch_in;

		alac_cfg.frame_length = alac->frame_length;
		alac_cfg.pb = alac->pb;
		alac_cfg.mb = alac->mb;
		alac_cfg.kb = alac->kb;
		alac_cfg.max_run = alac->max_run;
		alac_cfg.compatible_version = alac->compatible_version;
		alac_cfg.max_frame_bytes = alac->max_frame_bytes;

		switch (codec->ch_in) {
		case 1:
			alac_cfg.channel_layout_tag = ALAC_CH_LAYOUT_MONO;
			break;
		case 2:
			alac_cfg.channel_layout_tag = ALAC_CH_LAYOUT_STEREO;
			break;
		}
		ret = q6asm_stream_media_format_block_alac(prtd->audio_client,
							   stream_id,
							   &alac_cfg);
		if (ret < 0) {
			dev_err(dev, "ALAC CMD Format block failed:%d\n", ret);
			return -EIO;
		}
		break;

	case SND_AUDIOCODEC_APE:
		memset(&ape_cfg, 0x0, sizeof(ape_cfg));
		ape = &codec_options->ape_d;

		ape_cfg.sample_rate = codec->sample_rate;
		ape_cfg.num_channels = codec->ch_in;
		ape_cfg.bits_per_sample = prtd->bits_per_sample;

		ape_cfg.compatible_version = ape->compatible_version;
		ape_cfg.compression_level = ape->compression_level;
		ape_cfg.format_flags = ape->format_flags;
		ape_cfg.blocks_per_frame = ape->blocks_per_frame;
		ape_cfg.final_frame_blocks = ape->final_frame_blocks;
		ape_cfg.total_frames = ape->total_frames;
		ape_cfg.seek_table_present = ape->seek_table_present;

		ret = q6asm_stream_media_format_block_ape(prtd->audio_client,
							  stream_id,
							  &ape_cfg);
		if (ret < 0) {
			dev_err(dev, "APE CMD Format block failed:%d\n", ret);
			return -EIO;
		}
		break;

	default:
		break;
	}

	return 0;
}

static int q6asm_dai_compr_set_params(struct snd_soc_component *component,
				      struct snd_compr_stream *stream,
				      struct snd_compr_params *params)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct snd_soc_pcm_runtime *rtd = stream->private_data;
	int dir = stream->direction;
	struct q6asm_dai_data *pdata;
	struct device *dev = component->dev;
	int ret;

	pdata = snd_soc_component_get_drvdata(component);
	if (!pdata)
		return -EINVAL;

	if (!prtd || !prtd->audio_client) {
		dev_err(dev, "private data null or audio client freed\n");
		return -EINVAL;
	}

	if (q6asm_session_is_quarantined(prtd->audio_client)) {
		dev_err(dev, "J716F: session %d is quarantined; refusing compr params\n",
			prtd->stream_id);
		return -EIO;
	}

	if (!runtime->fragments || !runtime->fragment_size)
		return -EINVAL;

	/*
	 * A second set_params() on the same stream must not open a second
	 * stream or map a second time; release the previous configuration
	 * first.
	 */
	if (prtd->stream_opened || prtd->map_state != Q6ASM_MAP_NONE) {
		if (q6asm_dai_compr_teardown(component, rtd, prtd, dir)) {
			prtd->teardown_unknown = true;
			return -EIO;
		}
	}

	prtd->periods = runtime->fragments;
	prtd->pcm_count = runtime->fragment_size;
	prtd->pcm_size = runtime->fragments * runtime->fragment_size;
	prtd->bits_per_sample = 16;
	prtd->state = Q6ASM_STREAM_PREPARED;

	if (dir == SND_COMPRESS_PLAYBACK) {
		ret = q6asm_open_write(prtd->audio_client, prtd->stream_id,
				       params->codec.id, params->codec.profile,
				       prtd->bits_per_sample, true);
		if (ret < 0) {
			dev_err(dev, "q6asm_open_write failed %d\n", ret);
			goto open_err;
		}
		prtd->stream_opened = true;
	}

	prtd->session_id = q6asm_get_session_id(prtd->audio_client);
	ret = q6routing_stream_open(rtd->dai_link->id, LEGACY_PCM_MODE,
				    prtd->session_id, dir);
	if (ret) {
		dev_err(dev, "Stream reg failed ret:%d\n", ret);
		goto q6_err;
	}

	ret = __q6asm_dai_compr_set_codec_params(component, stream,
						 &params->codec,
						 prtd->stream_id);
	if (ret) {
		dev_err(dev, "codec param setup failed ret:%d\n", ret);
		goto q6_err;
	}

	ret = q6asm_map_memory_regions(dir, prtd->audio_client, prtd->phys,
				       (prtd->pcm_size / prtd->periods),
				       prtd->periods);
	if (ret < 0) {
		dev_err(dev, "Buffer Mapping failed ret:%d\n", ret);
		/*
		 * A timeout, a missing handle (-EPROTO) or a malformed answer
		 * means the DSP may have mapped the region anyway.  Keep the
		 * real error and retire the session: only an ADSP stop can
		 * clear a mapping the host cannot address.
		 */
		if (ret == -ETIMEDOUT || ret == -EPROTO || ret == -EIO) {
			prtd->map_state = Q6ASM_MAP_UNKNOWN;
			q6asm_quarantine_session(prtd->audio_client);
		}
		goto q6_err;
	}

	prtd->map_state = Q6ASM_MAP_CONFIRMED;
	return 0;

q6_err:
	/*
	 * Roll back through the same ordered teardown free() uses.  If it
	 * cannot confirm that the ADSP released the stream, remember that so
	 * free() keeps the DMA buffer instead of handing it back to the page
	 * allocator while the DSP may still be using it.
	 */
	if (q6asm_dai_compr_teardown(component, rtd, prtd, dir))
		prtd->teardown_unknown = true;

open_err:
	return ret;
}

static int q6asm_dai_compr_set_metadata(struct snd_soc_component *component,
					struct snd_compr_stream *stream,
					struct snd_compr_metadata *metadata)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	int ret = 0;

	switch (metadata->key) {
	case SNDRV_COMPRESS_ENCODER_PADDING:
		prtd->trailing_samples_drop = metadata->value[0];
		break;
	case SNDRV_COMPRESS_ENCODER_DELAY:
		prtd->initial_samples_drop = metadata->value[0];
		if (prtd->next_track_stream_id) {
			ret = q6asm_open_write(prtd->audio_client,
					       prtd->next_track_stream_id,
					       prtd->codec.id,
					       prtd->codec.profile,
					       prtd->bits_per_sample,
				       true);
			if (ret < 0) {
				dev_err(component->dev, "q6asm_open_write failed\n");
				return ret;
			}
			ret = __q6asm_dai_compr_set_codec_params(component, stream,
								 &prtd->codec,
								 prtd->next_track_stream_id);
			if (ret < 0) {
				dev_err(component->dev, "q6asm_open_write failed\n");
				return ret;
			}

			ret = q6asm_stream_remove_initial_silence(prtd->audio_client,
						    prtd->next_track_stream_id,
						    prtd->initial_samples_drop);
			prtd->next_track_stream_id = 0;

		}

		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6asm_dai_compr_trigger(struct snd_soc_component *component,
				   struct snd_compr_stream *stream, int cmd)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE: {
		bool run = false;

		spin_lock(&prtd->lock);
		if (prtd->state != Q6ASM_STREAM_RUNNING &&
		    prtd->state != Q6ASM_STREAM_STARTING) {
			prtd->state = Q6ASM_STREAM_STARTING;
			run = true;
		}
		spin_unlock(&prtd->lock);

		if (run) {
			ret = q6asm_run_nowait(prtd->audio_client,
					       prtd->stream_id, 0, 0, 0);
			if (ret < 0) {
				spin_lock(&prtd->lock);
				prtd->state = Q6ASM_STREAM_ERROR;
				spin_unlock(&prtd->lock);
			}
		}
		break;
	}
	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock(&prtd->lock);
		prtd->state = Q6ASM_STREAM_STOPPING;
		spin_unlock(&prtd->lock);
		ret = q6asm_cmd_nowait(prtd->audio_client, prtd->stream_id,
				       CMD_EOS);
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock(&prtd->lock);
		if (prtd->state == Q6ASM_STREAM_RUNNING)
			prtd->state = Q6ASM_STREAM_PAUSING;
		spin_unlock(&prtd->lock);
		ret = q6asm_cmd_nowait(prtd->audio_client, prtd->stream_id,
				       CMD_PAUSE);
		break;
	case SND_COMPR_TRIGGER_NEXT_TRACK:
		prtd->next_track = true;
		prtd->next_track_stream_id = (prtd->stream_id == 1 ? 2 : 1);
		break;
	case SND_COMPR_TRIGGER_DRAIN:
	case SND_COMPR_TRIGGER_PARTIAL_DRAIN:
		prtd->notify_on_drain = true;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int q6asm_dai_compr_pointer(struct snd_soc_component *component,
				   struct snd_compr_stream *stream,
				   struct snd_compr_tstamp64 *tstamp)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	uint64_t temp_copied_total;

	guard(spinlock_irqsave)(&prtd->lock);

	if (!prtd->pcm_size) {
		tstamp->copied_total = 0;
		tstamp->byte_offset = 0;
		return 0;
	}

	tstamp->copied_total = prtd->copied_total;
	temp_copied_total = tstamp->copied_total;
	tstamp->byte_offset = do_div(temp_copied_total, prtd->pcm_size);

	return 0;
}

static int q6asm_compr_copy(struct snd_soc_component *component,
			    struct snd_compr_stream *stream, char __user *buf,
			    size_t count)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	u32 wflags = 0;
	uint64_t avail, bytes_in_flight = 0;
	void *dstn;
	size_t copy;
	u32 app_pointer;
	uint64_t bytes_received;
	uint64_t temp_bytes_received;

	if (!prtd->pcm_size || !prtd->pcm_count ||
	    count > prtd->pcm_size)
		return -EINVAL;
	if (!count)
		return 0;

	bytes_received = prtd->bytes_received;
	temp_bytes_received = bytes_received;

	/**
	 * Make sure that next track data pointer is aligned at 32 bit boundary
	 * This is a Mandatory requirement from DSP data buffers alignment
	 */
	if (prtd->next_track) {
		bytes_received = ALIGN(prtd->bytes_received, prtd->pcm_count);
		temp_bytes_received = bytes_received;
	}

	app_pointer = do_div(temp_bytes_received, prtd->pcm_size);
	dstn = prtd->dma_buffer.area + app_pointer;

	if (count < prtd->pcm_size - app_pointer) {
		if (copy_from_user(dstn, buf, count))
			return -EFAULT;
	} else {
		copy = prtd->pcm_size - app_pointer;
		if (copy_from_user(dstn, buf, copy))
			return -EFAULT;
		if (copy_from_user(prtd->dma_buffer.area, buf + copy,
				   count - copy))
			return -EFAULT;
	}

	guard(spinlock_irqsave)(&prtd->lock);

	if (prtd->copied_total > prtd->bytes_received) {
		dev_warn_ratelimited(prtd->dev,
				     "compressed counters went backwards (%llu > %llu)\n",
				     prtd->copied_total, prtd->bytes_received);
		bytes_in_flight = 0;
	} else {
		bytes_in_flight = prtd->bytes_received - prtd->copied_total;
	}

	if (prtd->next_track) {
		prtd->next_track = false;
		prtd->copied_total = ALIGN(prtd->copied_total, prtd->pcm_count);
		prtd->bytes_sent = ALIGN(prtd->bytes_sent, prtd->pcm_count);
	}

	prtd->bytes_received = bytes_received + count;

	/* Kick off the data to the DSP if it is starving. */
	if (prtd->state == Q6ASM_STREAM_RUNNING && bytes_in_flight == 0) {
		uint32_t bytes_to_write = prtd->pcm_count;

		avail = prtd->bytes_received - prtd->bytes_sent;
		if (avail < prtd->pcm_count)
			bytes_to_write = avail;

		if (bytes_to_write) {
			if (q6asm_write_async(prtd->audio_client,
					      prtd->stream_id, bytes_to_write,
					      0, 0, wflags) < 0) {
				prtd->state = Q6ASM_STREAM_ERROR;
				return count;
			}
			prtd->bytes_sent += bytes_to_write;
		}
	}

	return count;
}

static int q6asm_dai_compr_mmap(struct snd_soc_component *component,
				struct snd_compr_stream *stream,
				struct vm_area_struct *vma)
{
	struct snd_compr_runtime *runtime = stream->runtime;
	struct q6asm_dai_rtd *prtd = runtime->private_data;
	struct device *dev = component->dev;

	return dma_mmap_coherent(dev, vma,
			prtd->dma_buffer.area, prtd->dma_buffer.addr,
			prtd->dma_buffer.bytes);
}

static int q6asm_dai_compr_get_caps(struct snd_soc_component *component,
				    struct snd_compr_stream *stream,
				    struct snd_compr_caps *caps)
{
	caps->direction = SND_COMPRESS_PLAYBACK;
	caps->min_fragment_size = COMPR_PLAYBACK_MIN_FRAGMENT_SIZE;
	caps->max_fragment_size = COMPR_PLAYBACK_MAX_FRAGMENT_SIZE;
	caps->min_fragments = COMPR_PLAYBACK_MIN_NUM_FRAGMENTS;
	caps->max_fragments = COMPR_PLAYBACK_MAX_NUM_FRAGMENTS;
	caps->num_codecs = 5;
	caps->codecs[0] = SND_AUDIOCODEC_MP3;
	caps->codecs[1] = SND_AUDIOCODEC_FLAC;
	caps->codecs[2] = SND_AUDIOCODEC_WMA;
	caps->codecs[3] = SND_AUDIOCODEC_ALAC;
	caps->codecs[4] = SND_AUDIOCODEC_APE;

	return 0;
}

static int q6asm_dai_compr_get_codec_caps(struct snd_soc_component *component,
					  struct snd_compr_stream *stream,
					  struct snd_compr_codec_caps *codec)
{
	switch (codec->codec) {
	case SND_AUDIOCODEC_MP3:
		*codec = q6asm_compr_caps;
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_compress_ops q6asm_dai_compress_ops = {
	.open		= q6asm_dai_compr_open,
	.free		= q6asm_dai_compr_free,
	.set_params	= q6asm_dai_compr_set_params,
	.set_metadata	= q6asm_dai_compr_set_metadata,
	.pointer	= q6asm_dai_compr_pointer,
	.trigger	= q6asm_dai_compr_trigger,
	.get_caps	= q6asm_dai_compr_get_caps,
	.get_codec_caps	= q6asm_dai_compr_get_codec_caps,
	.mmap		= q6asm_dai_compr_mmap,
	.copy		= q6asm_compr_copy,
};

static int q6asm_dai_pcm_new(struct snd_soc_component *component,
			     struct snd_soc_pcm_runtime *rtd)
{
	struct snd_pcm *pcm = rtd->pcm;
	size_t size = q6asm_dai_hardware_playback.buffer_bytes_max;

	/*
	 * Lenovo J716F: the vendor kernel maps audio buffers from the LPASS
	 * audio SMMU window 0x1000_0000-0x1FFF_FFFF (qcom,iommu-dma-addr-pool
	 * on its msm-audio-ion node). Keep the IOVA in the same window so the
	 * address handed to the ADSP is inside the range its firmware was
	 * built for. A 29-bit mask makes iommu-dma allocate top-down at
	 * 0x1FFF_xxxx, i.e. inside that window.
	 */
	if (of_machine_is_compatible("lenovo,j716f")) {
		int ret = dma_set_mask_and_coherent(component->dev,
						    DMA_BIT_MASK(29));

		if (ret) {
			dev_err(component->dev,
				"J716F: cannot limit the audio DMA mask to the LPASS window (%d); refusing PCM registration\n",
				ret);
			return ret;
		}
	}

	return snd_pcm_set_fixed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV,
					    component->dev, size);
}

static const struct snd_soc_dapm_widget q6asm_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("MM_DL1", "MultiMedia1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL2", "MultiMedia2 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL3", "MultiMedia3 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL4", "MultiMedia4 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL5", "MultiMedia5 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL6", "MultiMedia6 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL7", "MultiMedia7 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("MM_DL8", "MultiMedia8 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL1", "MultiMedia1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL2", "MultiMedia2 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL3", "MultiMedia3 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL4", "MultiMedia4 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL5", "MultiMedia5 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL6", "MultiMedia6 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL7", "MultiMedia7 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("MM_UL8", "MultiMedia8 Capture", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_component_driver q6asm_fe_dai_component = {
	.name			= DRV_NAME,
	.open			= q6asm_dai_open,
	.hw_params		= q6asm_dai_hw_params,
	.hw_free		= q6asm_dai_hw_free,
	.close			= q6asm_dai_close,
	.prepare		= q6asm_dai_prepare,
	.trigger		= q6asm_dai_trigger,
	.ack			= q6asm_dai_ack,
	.pointer		= q6asm_dai_pointer,
	.pcm_new		= q6asm_dai_pcm_new,
	.compress_ops		= &q6asm_dai_compress_ops,
	.dapm_widgets		= q6asm_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(q6asm_dapm_widgets),
	.legacy_dai_naming	= 1,
};

static struct snd_soc_dai_driver q6asm_fe_dais_template[] = {
	Q6ASM_FEDAI_DRIVER(1),
	Q6ASM_FEDAI_DRIVER(2),
	Q6ASM_FEDAI_DRIVER(3),
	Q6ASM_FEDAI_DRIVER(4),
	Q6ASM_FEDAI_DRIVER(5),
	Q6ASM_FEDAI_DRIVER(6),
	Q6ASM_FEDAI_DRIVER(7),
	Q6ASM_FEDAI_DRIVER(8),
};

static const struct snd_soc_dai_ops q6asm_dai_ops = {
	.compress_new = snd_soc_new_compress,
};

static int of_q6asm_parse_dai_data(struct device *dev,
				    struct q6asm_dai_data *pdata)
{
	struct snd_soc_dai_driver *dai_drv;
	struct snd_soc_pcm_stream empty_stream;
	struct device_node *node;
	int ret, id, dir, idx = 0;


	pdata->num_dais = of_get_child_count(dev->of_node);
	if (!pdata->num_dais) {
		dev_err(dev, "No dais found in DT\n");
		return -EINVAL;
	}

	pdata->dais = devm_kcalloc(dev, pdata->num_dais, sizeof(*dai_drv),
				   GFP_KERNEL);
	if (!pdata->dais)
		return -ENOMEM;

	memset(&empty_stream, 0, sizeof(empty_stream));

	for_each_child_of_node(dev->of_node, node) {
		ret = of_property_read_u32(node, "reg", &id);
		if (ret || id >= MAX_SESSIONS || id < 0) {
			dev_err(dev, "valid dai id not found:%d\n", ret);
			continue;
		}

		dai_drv = &pdata->dais[idx++];
		*dai_drv = q6asm_fe_dais_template[id];

		ret = of_property_read_u32(node, "direction", &dir);
		if (ret)
			continue;

		if (dir == Q6ASM_DAI_RX)
			dai_drv->capture = empty_stream;
		else if (dir == Q6ASM_DAI_TX)
			dai_drv->playback = empty_stream;

		if (of_property_read_bool(node, "is-compress-dai"))
			dai_drv->ops = &q6asm_dai_ops;
	}

	return 0;
}

static int q6asm_dai_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	struct of_phandle_args args;
	struct q6asm_dai_data *pdata;
	int rc;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	rc = of_parse_phandle_with_fixed_args(node, "iommus", 1, 0, &args);
	if (rc < 0)
		pdata->sid = -1;
	else
		pdata->sid = args.args[0] & SID_MASK_DEFAULT;

	pdata->j716f = of_machine_is_compatible("lenovo,j716f");
	dev_set_drvdata(dev, pdata);

	rc = of_q6asm_parse_dai_data(dev, pdata);
	if (rc)
		return rc;

	/*
	 * Resolve DMA buffers held after a failed teardown before this
	 * device's IOMMU domain disappears.
	 */
	rc = q6asm_dma_device_register(dev);
	if (rc)
		return rc;

	return devm_snd_soc_register_component(dev, &q6asm_fe_dai_component,
					       pdata->dais, pdata->num_dais);
}

#ifdef CONFIG_OF
static const struct of_device_id q6asm_dai_device_id[] = {
	{ .compatible = "qcom,q6asm-dais" },
	{},
};
MODULE_DEVICE_TABLE(of, q6asm_dai_device_id);
#endif

static struct platform_driver q6asm_dai_platform_driver = {
	.driver = {
		.name = "q6asm-dai",
		.of_match_table = of_match_ptr(q6asm_dai_device_id),
	},
	.probe = q6asm_dai_probe,
};
module_platform_driver(q6asm_dai_platform_driver);

MODULE_DESCRIPTION("Q6ASM dai driver");
MODULE_LICENSE("GPL v2");
