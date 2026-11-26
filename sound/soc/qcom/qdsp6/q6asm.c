// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <dt-bindings/sound/qcom,q6asm.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/list.h>
#include <sound/pcm.h>
#include <linux/soc/qcom/apr.h>
#include <linux/device.h>
#include <linux/of_platform.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/notifier.h>
#include <linux/remoteproc.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <linux/of.h>
#include <uapi/sound/asound.h>
#include <uapi/sound/compress_params.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include "q6asm.h"
#include "q6core.h"
#include "q6dsp-errno.h"
#include "q6dsp-common.h"

#define ASM_STREAM_CMD_CLOSE			0x00010BCD
#define ASM_STREAM_CMD_FLUSH			0x00010BCE
#define ASM_SESSION_CMD_PAUSE			0x00010BD3
#define ASM_DATA_CMD_EOS			0x00010BDB
#define ASM_DATA_EVENT_RENDERED_EOS		0x00010C1C
#define ASM_NULL_POPP_TOPOLOGY			0x00010C68
#define ASM_STREAM_CMD_FLUSH_READBUFS		0x00010C09
#define ASM_STREAM_CMD_SET_ENCDEC_PARAM		0x00010C10
#define ASM_STREAM_POSTPROC_TOPO_ID_NONE	0x00010C68
#define ASM_CMD_SHARED_MEM_MAP_REGIONS		0x00010D92
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS	0x00010D93
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS	0x00010D94
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2	0x00010D98
#define ASM_DATA_EVENT_WRITE_DONE_V2		0x00010D99
#define ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2	0x00010DA3
#define ASM_SESSION_CMD_RUN_V2			0x00010DAA
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2	0x00010DA5
#define ASM_MEDIA_FMT_MP3			0x00010BE9
#define ASM_MEDIA_FMT_FLAC			0x00010C16
#define ASM_MEDIA_FMT_WMA_V9			0x00010DA8
#define ASM_MEDIA_FMT_WMA_V10			0x00010DA7
#define ASM_DATA_CMD_WRITE_V2			0x00010DAB
#define ASM_DATA_CMD_READ_V2			0x00010DAC
#define ASM_SESSION_CMD_SUSPEND			0x00010DEC
#define ASM_STREAM_CMD_OPEN_WRITE_V3		0x00010DB3
#define ASM_STREAM_CMD_OPEN_READ_V3                 0x00010DB4
#define ASM_DATA_EVENT_READ_DONE_V2 0x00010D9A
#define ASM_STREAM_CMD_OPEN_READWRITE_V2        0x00010D8D
#define ASM_MEDIA_FMT_ALAC			0x00012f31
#define ASM_MEDIA_FMT_APE			0x00012f32
#define ASM_DATA_CMD_REMOVE_INITIAL_SILENCE	0x00010D67
#define ASM_DATA_CMD_REMOVE_TRAILING_SILENCE	0x00010D68


#define ASM_LEGACY_STREAM_SESSION	0
/* Bit shift for the stream_perf_mode subfield. */
#define ASM_SHIFT_STREAM_PERF_MODE_FLAG_IN_OPEN_READ              29
#define ASM_END_POINT_DEVICE_MATRIX	0
#define ASM_DEFAULT_APP_TYPE		0
#define ASM_SYNC_IO_MODE		0x0001
#define ASM_ASYNC_IO_MODE		0x0002
#define ASM_TUN_READ_IO_MODE		0x0004	/* tunnel read write mode */
#define ASM_TUN_WRITE_IO_MODE		0x0008	/* tunnel read write mode */
#define ASM_SHIFT_GAPLESS_MODE_FLAG	31
#define ADSP_MEMORY_MAP_SHMEM8_4K_POOL	3

struct avs_cmd_shared_mem_map_regions {
	u16 mem_pool_id;
	u16 num_regions;
	u32 property_flag;
} __packed;

struct avs_shared_map_region_payload {
	u32 shm_addr_lsw;
	u32 shm_addr_msw;
	u32 mem_size_bytes;
} __packed;

struct avs_cmd_shared_mem_unmap_regions {
	u32 mem_map_handle;
} __packed;

struct asm_data_cmd_media_fmt_update_v2 {
	u32 fmt_blk_size;
} __packed;

struct asm_multi_channel_pcm_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u16 is_signed;
	u16 reserved;
	u8 channel_mapping[PCM_MAX_NUM_CHANNEL];
} __packed;

struct asm_flac_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16 is_stream_info_present;
	u16 num_channels;
	u16 min_blk_size;
	u16 max_blk_size;
	u16 md5_sum[8];
	u32 sample_rate;
	u32 min_frame_size;
	u32 max_frame_size;
	u16 sample_size;
	u16 reserved;
} __packed;

struct asm_wmastdv9_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16          fmtag;
	u16          num_channels;
	u32          sample_rate;
	u32          bytes_per_sec;
	u16          blk_align;
	u16          bits_per_sample;
	u32          channel_mask;
	u16          enc_options;
	u16          reserved;
} __packed;

struct asm_wmaprov10_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16          fmtag;
	u16          num_channels;
	u32          sample_rate;
	u32          bytes_per_sec;
	u16          blk_align;
	u16          bits_per_sample;
	u32          channel_mask;
	u16          enc_options;
	u16          advanced_enc_options1;
	u32          advanced_enc_options2;
} __packed;

struct asm_alac_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u32 frame_length;
	u8 compatible_version;
	u8 bit_depth;
	u8 pb;
	u8 mb;
	u8 kb;
	u8 num_channels;
	u16 max_run;
	u32 max_frame_bytes;
	u32 avg_bit_rate;
	u32 sample_rate;
	u32 channel_layout_tag;
} __packed;

struct asm_ape_fmt_blk_v2 {
	struct asm_data_cmd_media_fmt_update_v2 fmt_blk;
	u16 compatible_version;
	u16 compression_level;
	u32 format_flags;
	u32 blocks_per_frame;
	u32 final_frame_blocks;
	u32 total_frames;
	u16 bits_per_sample;
	u16 num_channels;
	u32 sample_rate;
	u32 seek_table_present;
} __packed;

struct asm_stream_cmd_set_encdec_param {
	u32                  param_id;
	u32                  param_size;
} __packed;

struct asm_enc_cfg_blk_param_v2 {
	u32                  frames_per_buf;
	u32                  enc_cfg_blk_size;
} __packed;

struct asm_multi_channel_pcm_enc_cfg_v2 {
	struct asm_stream_cmd_set_encdec_param  encdec;
	struct asm_enc_cfg_blk_param_v2	encblk;
	uint16_t  num_channels;
	uint16_t  bits_per_sample;
	uint32_t  sample_rate;
	uint16_t  is_signed;
	uint16_t  reserved;
	uint8_t   channel_mapping[8];
} __packed;

struct asm_data_cmd_read_v2 {
	u32                  buf_addr_lsw;
	u32                  buf_addr_msw;
	u32                  mem_map_handle;
	u32                  buf_size;
	u32                  seq_id;
} __packed;

/*
 * ASM_DATA_EVENT_READ_DONE_V2 carries eleven words on the kona ADSP:
 * status, address, map handle, actual size, offset, timestamps, flags,
 * frame count and the seq_id that was submitted with the READ.  A firmware
 * that only sends the first three words is still accepted: the token then
 * identifies the period, exactly as before.
 */
struct asm_data_cmd_read_v2_done {
	u32	status;
	u32	buf_addr_lsw;
	u32	buf_addr_msw;
	u32	mem_map_handle;
	u32	buf_size;
	u32	offset;
	u32	lsw_ts;
	u32	msw_ts;
	u32	flags;
	u32	num_frames;
	u32	seq_id;
};

#define Q6ASM_READ_DONE_MIN	12
#define Q6ASM_READ_DONE_FULL	44

struct asm_stream_cmd_open_read_v3 {
	u32                    mode_flags;
	u32                    src_endpointype;
	u32                    preprocopo_id;
	u32                    enc_cfg_id;
	u16                    bits_per_sample;
	u16                    reserved;
} __packed;

struct asm_data_cmd_write_v2 {
	u32 buf_addr_lsw;
	u32 buf_addr_msw;
	u32 mem_map_handle;
	u32 buf_size;
	u32 seq_id;
	u32 timestamp_lsw;
	u32 timestamp_msw;
	u32 flags;
} __packed;

struct asm_stream_cmd_open_write_v3 {
	uint32_t mode_flags;
	uint16_t sink_endpointype;
	uint16_t bits_per_sample;
	uint32_t postprocopo_id;
	uint32_t dec_fmt_id;
} __packed;

struct asm_session_cmd_run_v2 {
	u32 flags;
	u32 time_lsw;
	u32 time_msw;
} __packed;

struct audio_buffer {
	phys_addr_t phys;
	uint32_t size;		/* size of buffer */
};

/*
 * Accounting for one period that has been handed to the ADSP.  A completion
 * is only accepted while its slot is in flight, its stream matches and the
 * generation the host put in the token is still the current one.  The ADSP
 * echoes the token's byte 2 (bits 16-23); the vendor kernel uses that byte
 * for its buffer index, so it is a safe host-owned field.
 */
struct q6asm_submit {
	u32 generation;
	u32 stream_id;
	u32 len;
	phys_addr_t phys;
	bool in_flight;
	/* Completed but not yet counted (a later period completed first). */
	bool done;
};

struct audio_port_data {
	struct audio_buffer *buf;
	struct q6asm_submit *submit;
	uint32_t num_periods;
	uint32_t dsp_buf;
	uint32_t mem_map_handle;
	/* Data-path instance; bumped whenever a new mapping is installed. */
	uint32_t generation;
	/* Next period index that may complete; completions are ring-ordered. */
	uint32_t next_done;
	uint32_t failures;
	atomic_t hw_ptr;
};

/*
 * One synchronous command at a time per client.  The APR RX worker writes
 * the response fields while the caller sleeps in wait_event_timeout(); the
 * lock covers the fields, the waitqueue is only woken under it.
 *
 * A response is only allowed to complete the command that owns it.  When a
 * synchronous command times out its response may still arrive, so the
 * opcode is retired: a later command of the same opcode is refused until
 * that response has been consumed or a confirmed CLOSE has retired the
 * session.  Fire-and-forget commands need a credit for the same reason, so
 * their responses cannot be mistaken for a synchronous waiter (or the other
 * way round) and a duplicate cannot drive a state event a second time.
 */
#define Q6ASM_RSP_STALE_MAX	4
#define Q6ASM_RSP_ASYNC_MAX	4

struct q6asm_cmd_rsp {
	spinlock_t lock;
	wait_queue_head_t wait;
	u32 opcode;
	u32 status;
	u32 handle;
	/* Opcodes whose answer never arrived. */
	u32 stale[Q6ASM_RSP_STALE_MAX];
	/* In-flight fire-and-forget commands, one entry per opcode. */
	struct {
		u32 opcode;
		u32 count;
	} async[Q6ASM_RSP_ASYNC_MAX];
	/* Too many unanswered opcodes to track: refuse all new commands. */
	bool overflow;
	bool pending;
	bool done;
};

enum q6asm_rsp_owner {
	Q6ASM_RSP_NONE = 0,
	Q6ASM_RSP_SYNC,
	Q6ASM_RSP_ASYNC,
};

struct q6asm;

#define Q6ASM_TOKEN_INDEX_MASK		GENMASK(7, 0)
#define Q6ASM_TOKEN_GEN_SHIFT		16
#define Q6ASM_TOKEN_GEN_MASK		GENMASK(23, 16)
#define Q6ASM_TOKEN_GEN_MAX		(Q6ASM_TOKEN_GEN_MASK >> \
					 Q6ASM_TOKEN_GEN_SHIFT)

/*
 * The data-path generation is a 32-bit host value, but only its low byte
 * travels in the APR token.  Keep the two apart: the full value proves that
 * a completion belongs to this mapping, the encoded byte is the part the
 * firmware echoes back.  Comparing the full value against the decoded byte
 * (as the first revision did) discarded every valid completion once the
 * generation passed 255.
 */
static inline u32 q6asm_token_gen(u32 generation)
{
	return generation & Q6ASM_TOKEN_GEN_MAX;
}

/*
 * Data-path generations are host owned and unique across clients and driver
 * rebinds.  A client that restarted at 1 would accept a completion left in
 * the APR queue by a destroyed client with the same session id; drawing
 * every mapping from one module-wide counter removes that alias.  Values
 * whose encoded byte is zero are skipped, because zero means "no
 * generation" in the protocol.
 */
static atomic_t q6asm_generation_seq = ATOMIC_INIT(0);

static u32 q6asm_next_generation(void)
{
	u32 generation = (u32)atomic_inc_return(&q6asm_generation_seq);

	if (!q6asm_token_gen(generation))
		generation = (u32)atomic_inc_return(&q6asm_generation_seq);

	return generation;
}

struct audio_client {
	int session;
	q6asm_cb cb;
	void *priv;
	unsigned int flags;
	uint32_t io_mode;
	struct apr_device *adev;
	struct mutex cmd_lock;
	/* APR RX callbacks execute in a workqueue. */
	struct mutex cb_lock;
	spinlock_t lock;
	struct kref refcount;
	/* idx:1 out port, 0: in port */
	struct audio_port_data port[2];
	struct q6asm_cmd_rsp rsp;
	int perf_mode;
	struct q6asm *q6asm;
	struct device *dev;
	/* Pre-allocated owner for a buffer the DSP may still be using. */
	struct q6asm_held_buffer *hold_slot;
#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	/*
	 * Counters that let the device tests prove which branch handled a
	 * completion or a command response instead of inferring it from
	 * behaviour.  Debug builds only.
	 */
	struct {
		atomic_t accepted;
		atomic_t dropped;
		atomic_t late;
		atomic_t unowned;
	} stats;
#endif
};

enum q6asm_stat {
	Q6ASM_STAT_ACCEPTED,
	Q6ASM_STAT_DROPPED,
	Q6ASM_STAT_LATE,
	Q6ASM_STAT_UNOWNED,
};

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
static inline void q6asm_stat_inc(struct audio_client *ac,
				  enum q6asm_stat stat)
{
	switch (stat) {
	case Q6ASM_STAT_ACCEPTED:
		atomic_inc(&ac->stats.accepted);
		break;
	case Q6ASM_STAT_DROPPED:
		atomic_inc(&ac->stats.dropped);
		break;
	case Q6ASM_STAT_LATE:
		atomic_inc(&ac->stats.late);
		break;
	case Q6ASM_STAT_UNOWNED:
		atomic_inc(&ac->stats.unowned);
		break;
	}
}
#else
static inline void q6asm_stat_inc(struct audio_client *ac,
				  enum q6asm_stat stat)
{
}
#endif

struct q6asm {
	struct apr_device *adev;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	spinlock_t slock;
	struct audio_client *session[MAX_SESSIONS + 1];
#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	/*
	 * One-shot error injection (J716F debug aid).  A root user writes
	 * inject_fault in sysfs; the next matching operation fails.
	 */
	unsigned int fault;
	bool fault_timeout;
#endif
};

enum q6asm_fault {
	Q6ASM_FAULT_NONE = 0,
	Q6ASM_FAULT_MAP,
	Q6ASM_FAULT_CLOSE,
	Q6ASM_FAULT_UNMAP,
	Q6ASM_FAULT_READ,
	Q6ASM_FAULT_WRITE,
	Q6ASM_FAULT_RUN,
	Q6ASM_FAULT_READ_REJECT,
	Q6ASM_FAULT_WRITE_REJECT,
	Q6ASM_FAULT_PAUSE,
	Q6ASM_FAULT_PAUSE_REJECT,
	Q6ASM_FAULT_RUN_REJECT,
};

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
static const char *const q6asm_fault_names[] = {
	[Q6ASM_FAULT_NONE] = "none",
	[Q6ASM_FAULT_MAP] = "map",
	[Q6ASM_FAULT_CLOSE] = "close",
	[Q6ASM_FAULT_UNMAP] = "unmap",
	[Q6ASM_FAULT_READ] = "read",
	[Q6ASM_FAULT_WRITE] = "write",
	[Q6ASM_FAULT_RUN] = "run",
	[Q6ASM_FAULT_READ_REJECT] = "readreject",
	[Q6ASM_FAULT_WRITE_REJECT] = "writereject",
	[Q6ASM_FAULT_PAUSE] = "pause",
	[Q6ASM_FAULT_PAUSE_REJECT] = "pausereject",
	[Q6ASM_FAULT_RUN_REJECT] = "runreject",
};
#endif

/*
 * Compressed-audio buffers whose remote ownership is unknown after a failed
 * teardown are moved here.  The record is allocated together with the audio
 * client so the failure path never has to allocate memory.  The list is
 * module scoped on purpose: a q6asm device removal (APR rebind, ADSP reset)
 * must not lose the only record of a buffer the DSP may still access.
 */
struct q6asm_held_buffer {
	struct list_head node;
	struct snd_dma_buffer dma;
	/* Referenced until the pages are released or the device is gone. */
	struct device *dev;
	u32 session;
	/* The owning device was removed before the ADSP stopped. */
	bool dev_gone;
};

/*
 * The ADSP is shared by every q6asm instance, so a driver rebind does not
 * reset remote sessions.  Quarantined ids and held buffers are therefore
 * module state, released only when the remoteproc reports that the ADSP
 * actually stopped.
 */
typedef void *(*q6asm_ssr_register_fn)(const char *name,
				      struct notifier_block *nb);
typedef int (*q6asm_ssr_unregister_fn)(void *notify,
				      struct notifier_block *nb);

struct q6asm_global {
	struct mutex lock;
	q6asm_ssr_register_fn ssr_register;
	q6asm_ssr_unregister_fn ssr_unregister;
	unsigned long session_quarantine[BITS_TO_LONGS(MAX_SESSIONS + 1)];
	struct list_head held_buffers;
	struct notifier_block ssr_nb;
	struct srcu_notifier_head *ssr_head;
	bool adsp_offline;
	bool reclaim_registered;
	unsigned int leaked_buffers;
	size_t leaked_bytes;
};

static struct q6asm_global q6asm_global = {
	.lock = __MUTEX_INITIALIZER(q6asm_global.lock),
	.held_buffers = LIST_HEAD_INIT(q6asm_global.held_buffers),
	.adsp_offline = false,
};

/* SSR name the audio DSP registers on every supported remoteproc. */
static const char q6asm_ssr_name[] = "lpass";

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
/*
 * Consume a pending injection for @fault.  Returns 0 when nothing is armed
 * for this operation, else the errno the caller should fail with.
 */
static int q6asm_fault_take(struct audio_client *ac, unsigned int fault)
{
	struct q6asm *a = ac->q6asm;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&a->slock, flags);
	if (a->fault == fault) {
		a->fault = Q6ASM_FAULT_NONE;
		rc = a->fault_timeout ? -ETIMEDOUT : -EINVAL;
	}
	spin_unlock_irqrestore(&a->slock, flags);

	if (rc)
		dev_err(ac->dev,
			"J716F: injected %s failure on session %d\n",
			q6asm_fault_names[fault], ac->session);

	return rc;
}

/*
 * Consume a pending "DSP rejects this transfer" injection and report the
 * status the front end should see.  The command itself is not modified, so
 * this exercises the real completion-correlation path.
 */
static u32 q6asm_fault_status_take(struct audio_client *ac, unsigned int fault,
				   u32 status)
{
	struct q6asm *a = ac->q6asm;
	unsigned long flags;
	u32 rc = 0;

	spin_lock_irqsave(&a->slock, flags);
	if (a->fault == fault) {
		a->fault = Q6ASM_FAULT_NONE;
		rc = status;
	}
	spin_unlock_irqrestore(&a->slock, flags);

	if (rc)
		dev_err(ac->dev,
			"J716F: injected %s rejection on session %d\n",
			q6asm_fault_names[fault], ac->session);

	return rc;
}
#else
static inline int q6asm_fault_take(struct audio_client *ac, unsigned int fault)
{
	return 0;
}

static inline u32 q6asm_fault_status_take(struct audio_client *ac,
					  unsigned int fault, u32 status)
{
	return 0;
}
#endif

/**
 * q6asm_hold_buffer() - take ownership of a buffer the DSP may still use
 *
 * @ac: audio client the buffer was mapped through
 * @dma: buffer to keep alive; it is left detached on return
 * @session: session id the buffer was mapped for, for diagnostics
 *
 * Called when CMD_CLOSE or UNMAP failed, so the host can no longer tell
 * whether the ADSP still reads or writes the pages.  The buffer moves to a
 * module-wide list that outlives the APR device, and is freed only when the
 * remoteproc reports that the ADSP actually stopped.  The record itself was
 * allocated with the client, so this path cannot fail for lack of memory.
 *
 * A reference on the owning device keeps its IOMMU domain alive, so the
 * pages stay mapped (and unreachable to the host allocator) until the stop
 * notification or the device removal.
 */
bool q6asm_hold_buffer(struct audio_client *ac, struct snd_dma_buffer *dma,
		       u32 session)
{
	struct q6asm_held_buffer *held = ac->hold_slot;

	if (!held)
		held = kzalloc_obj(*held);
	if (!held) {
		dev_err(ac->dev,
			"J716F: no memory to hold the session %u DMA buffer\n",
			session);
		return false;
	}
	/* Take ownership of the pre-allocated record before any failure path. */
	ac->hold_slot = NULL;
	if (!dma->dev.dev) {
		dev_err(ac->dev,
			"J716F: the session %u DMA buffer has no device\n",
			session);
		kfree(held);
		return false;
	}

	held->dma = *dma;
	held->dev = dma->dev.dev;
	held->session = session;
	held->dev_gone = false;
	INIT_LIST_HEAD(&held->node);
	get_device(held->dev);

	mutex_lock(&q6asm_global.lock);
	list_add_tail(&held->node, &q6asm_global.held_buffers);
	mutex_unlock(&q6asm_global.lock);

	/* The held record owns the pages now; do not free them again. */
	dma->area = NULL;
	dma->bytes = 0;
	dev_warn(ac->dev,
		 "J716F: holding DMA buffer for session %u until the ADSP stops\n",
		 session);
	return true;
}
EXPORT_SYMBOL_GPL(q6asm_hold_buffer);

/*
 * The IOMMU domain is detached while the device is deleted, so a held
 * buffer can no longer be returned through the DMA API afterwards.  If the
 * ADSP is already down, free now (the device is still alive); otherwise mark
 * the record so the stop handler accounts it as lost instead of freeing
 * pages through a dead device.
 */
static void q6asm_dma_device_remove(void *data)
{
	struct device *dev = data;
	struct q6asm_held_buffer *held, *tmp;
	LIST_HEAD(gone);

	mutex_lock(&q6asm_global.lock);
	list_for_each_entry_safe(held, tmp, &q6asm_global.held_buffers, node) {
		if (held->dev != dev)
			continue;
		list_del(&held->node);
		if (q6asm_global.adsp_offline) {
			snd_dma_free_pages(&held->dma);
			put_device(held->dev);
			kfree(held);
		} else {
			/*
			 * Keep the record for the stop handler to account, but
			 * the pages can no longer be freed through this device.
			 */
			held->dev_gone = true;
			list_add_tail(&held->node, &gone);
		}
	}
	list_splice(&gone, &q6asm_global.held_buffers);
	mutex_unlock(&q6asm_global.lock);
}

/**
 * q6asm_dma_device_register() - watch a device used for audio DMA
 *
 * @dev: device whose buffers may be handed to q6asm_hold_buffer()
 *
 * Registers a devres action that resolves held buffers before the device's
 * IOMMU domain disappears.  The front-end driver calls this once per device
 * that can own an audio DMA buffer.
 */
int q6asm_dma_device_register(struct device *dev)
{
	return devm_add_action_or_reset(dev, q6asm_dma_device_remove, dev);
}
EXPORT_SYMBOL_GPL(q6asm_dma_device_register);

/*
 * AFTER_SHUTDOWN is emitted from the remoteproc unprepare stage, i.e. after
 * the remote has been halted.  That is the only point at which the host may
 * conclude that the ADSP no longer accesses audio DMA memory.  Sessions
 * quarantined by a failed teardown become usable again here, not on a mere
 * driver rebind.
 *
 * The audio DSP is the "lpass" subsystem on every remoteproc descriptor that
 * this driver serves.  Check the name carried by the event so a future alias
 * cannot let an unrelated remote stop free audio DMA or clear isolation.
 */
static int q6asm_ssr_notify(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	const struct qcom_ssr_notify_data *notify = data;
	struct q6asm_held_buffer *held, *tmp;
	unsigned int freed = 0, lost = 0;
	size_t lost_bytes = 0;

	if (!notify || !notify->name || strcmp(notify->name, q6asm_ssr_name))
		return NOTIFY_DONE;

	switch (event) {
	case QCOM_SSR_AFTER_POWERUP:
		mutex_lock(&q6asm_global.lock);
		q6asm_global.adsp_offline = false;
		mutex_unlock(&q6asm_global.lock);
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		mutex_lock(&q6asm_global.lock);
		q6asm_global.adsp_offline = true;
		bitmap_zero(q6asm_global.session_quarantine,
			    MAX_SESSIONS + 1);
		list_for_each_entry_safe(held, tmp, &q6asm_global.held_buffers,
					 node) {
			list_del(&held->node);
			if (held->dev_gone) {
				lost++;
				lost_bytes += held->dma.bytes;
				q6asm_global.leaked_buffers++;
				q6asm_global.leaked_bytes += held->dma.bytes;
			} else {
				snd_dma_free_pages(&held->dma);
				freed++;
			}
			put_device(held->dev);
			kfree(held);
		}
		mutex_unlock(&q6asm_global.lock);
		pr_info("q6asm: ADSP stopped: released %u held DMA buffer(s), %u already orphaned by a removed device (%zu bytes)\n",
			freed, lost, lost_bytes);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

/*
 * Registering and unregistering must not take q6asm_global.lock.  Removing
 * an SRCU notifier waits for a callback that is already running, and that
 * callback takes q6asm_global.lock to clear the quarantine; holding the
 * state lock across the removal would therefore wait for itself.  A
 * separate mutex serializes the two operations instead.
 */
static DEFINE_MUTEX(q6asm_reclaim_lock);

static void q6asm_reclaim_register(void)
{
	struct srcu_notifier_head *head;

	mutex_lock(&q6asm_reclaim_lock);
	if (q6asm_global.reclaim_registered)
		goto out;

	if (!q6asm_global.ssr_register) {
		q6asm_global.ssr_register =
			symbol_get(qcom_register_ssr_notifier);
		q6asm_global.ssr_unregister =
			symbol_get(qcom_unregister_ssr_notifier);
		if (!q6asm_global.ssr_register ||
		    !q6asm_global.ssr_unregister) {
			if (q6asm_global.ssr_register)
				symbol_put(qcom_register_ssr_notifier);
			if (q6asm_global.ssr_unregister)
				symbol_put(qcom_unregister_ssr_notifier);
			q6asm_global.ssr_register = NULL;
			q6asm_global.ssr_unregister = NULL;
			pr_err("q6asm: qcom_common is not available; ADSP stop events cannot be followed\n");
			goto out;
		}
	}

	q6asm_global.ssr_nb.notifier_call = q6asm_ssr_notify;
	head = q6asm_global.ssr_register(q6asm_ssr_name,
					 &q6asm_global.ssr_nb);
	if (IS_ERR(head)) {
		pr_err("q6asm: cannot follow %s stop events: %ld\n",
		       q6asm_ssr_name, PTR_ERR(head));
		/*
		 * Nothing is registered, so no callback can be running and the
		 * symbol references can be dropped again.  Isolation stays
		 * fail-closed without them.
		 */
		symbol_put(qcom_register_ssr_notifier);
		symbol_put(qcom_unregister_ssr_notifier);
		q6asm_global.ssr_register = NULL;
		q6asm_global.ssr_unregister = NULL;
		goto out;
	}

	q6asm_global.ssr_head = head;
	q6asm_global.reclaim_registered = true;
out:
	mutex_unlock(&q6asm_reclaim_lock);
}

static void q6asm_reclaim_unregister(void)
{
	mutex_lock(&q6asm_reclaim_lock);
	if (q6asm_global.reclaim_registered) {
		/*
		 * The removal waits for a running callback, which only needs
		 * q6asm_global.lock, so it must not be held here.
		 */
		q6asm_global.ssr_unregister(q6asm_global.ssr_head,
					    &q6asm_global.ssr_nb);
		q6asm_global.ssr_head = NULL;
		q6asm_global.reclaim_registered = false;
	}
	if (q6asm_global.ssr_register) {
		symbol_put(qcom_register_ssr_notifier);
		q6asm_global.ssr_register = NULL;
	}
	if (q6asm_global.ssr_unregister) {
		symbol_put(qcom_unregister_ssr_notifier);
		q6asm_global.ssr_unregister = NULL;
	}
	mutex_unlock(&q6asm_reclaim_lock);
}

static inline void q6asm_add_hdr(struct audio_client *ac, struct apr_hdr *hdr,
				 uint32_t pkt_size, bool cmd_flg,
				 uint32_t stream_id)
{
	hdr->hdr_field = APR_SEQ_CMD_HDR_FIELD;
	hdr->src_port = ((ac->session << 8) & 0xFF00) | (stream_id);
	hdr->dest_port = ((ac->session << 8) & 0xFF00) | (stream_id);
	hdr->pkt_size = pkt_size;
	if (cmd_flg)
		hdr->token = ac->session;
}

/*
 * A synchronous command is correlated with the response that names it:
 * result->opcode for a basic response, or the command opcode for a MAP
 * CMDRSP.  Responses for anything else never touch this command's result,
 * so an asynchronous READ/WRITE result or a late answer cannot complete or
 * corrupt a newer command.
 */
/* All q6asm_rsp_*_locked() helpers require rsp->lock. */
static bool q6asm_rsp_stale_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i;

	for (i = 0; i < Q6ASM_RSP_STALE_MAX; i++)
		if (rsp->stale[i] == opcode)
			return true;

	return false;
}

static void q6asm_rsp_mark_stale_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i, slot = -1;

	if (q6asm_rsp_stale_locked(rsp, opcode))
		return;

	for (i = 0; i < Q6ASM_RSP_STALE_MAX; i++) {
		if (!rsp->stale[i]) {
			slot = i;
			break;
		}
	}

	if (slot < 0) {
		rsp->overflow = true;
		return;
	}
	rsp->stale[slot] = opcode;
}

static bool q6asm_rsp_clear_stale_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i;

	for (i = 0; i < Q6ASM_RSP_STALE_MAX; i++) {
		if (rsp->stale[i] == opcode) {
			rsp->stale[i] = 0;
			return true;
		}
	}

	return false;
}

static bool q6asm_rsp_async_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i;

	for (i = 0; i < Q6ASM_RSP_ASYNC_MAX; i++)
		if (rsp->async[i].opcode == opcode && rsp->async[i].count)
			return true;

	return false;
}

static int q6asm_rsp_async_begin_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i, slot = -1;

	for (i = 0; i < Q6ASM_RSP_ASYNC_MAX; i++) {
		if (rsp->async[i].opcode == opcode) {
			rsp->async[i].count++;
			return 0;
		}
		if (!rsp->async[i].opcode && slot < 0)
			slot = i;
	}

	if (slot < 0)
		return -EBUSY;

	rsp->async[slot].opcode = opcode;
	rsp->async[slot].count = 1;
	return 0;
}

static void q6asm_rsp_async_end_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i;

	for (i = 0; i < Q6ASM_RSP_ASYNC_MAX; i++) {
		if (rsp->async[i].opcode == opcode) {
			if (--rsp->async[i].count == 0)
				rsp->async[i].opcode = 0;
			return;
		}
	}
}

static bool q6asm_rsp_async_take_locked(struct q6asm_cmd_rsp *rsp, u32 opcode)
{
	int i;

	for (i = 0; i < Q6ASM_RSP_ASYNC_MAX; i++) {
		if (rsp->async[i].opcode == opcode && rsp->async[i].count) {
			if (--rsp->async[i].count == 0)
				rsp->async[i].opcode = 0;
			return true;
		}
	}

	return false;
}

static int q6asm_cmd_rsp_arm(struct audio_client *ac, u32 opcode)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&rsp->lock, flags);
	if (rsp->overflow || q6asm_rsp_stale_locked(rsp, opcode) ||
	    q6asm_rsp_async_locked(rsp, opcode)) {
		/*
		 * An answer for this opcode is still owed (or its owner can
		 * no longer be told apart), so a new command must not reuse
		 * the opcode: the late response would complete it.
		 */
		rc = -EIO;
	} else {
		rsp->opcode = opcode;
		rsp->status = 0;
		rsp->handle = 0;
		rsp->done = false;
		rsp->pending = true;
	}
	spin_unlock_irqrestore(&rsp->lock, flags);

	return rc;
}

static void q6asm_cmd_rsp_disarm(struct audio_client *ac)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;

	spin_lock_irqsave(&rsp->lock, flags);
	rsp->pending = false;
	spin_unlock_irqrestore(&rsp->lock, flags);
}

/*
 * A confirmed CLOSE ends the stream, and the ADSP answers the commands of
 * one session in order.  Every earlier response has therefore been consumed
 * (or is the one still owed and about to be delivered), so the stream's
 * opcodes may be used again on the next session.
 */
static void q6asm_cmd_rsp_forget_stream(struct audio_client *ac)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;

	spin_lock_irqsave(&rsp->lock, flags);
	memset(rsp->stale, 0, sizeof(rsp->stale));
	memset(rsp->async, 0, sizeof(rsp->async));
	rsp->overflow = false;
	spin_unlock_irqrestore(&rsp->lock, flags);
}

/*
 * Attribute one response to its owner.  A timed-out opcode consumes its late
 * answer, a fire-and-forget command consumes one credit, and a duplicate or
 * otherwise unowned response changes nothing.  A basic MAP result with
 * status 0 is an "accepted" notice, not the mapping result, so the caller
 * passes it only when it carries a real failure.
 */
static enum q6asm_rsp_owner q6asm_cmd_rsp_claim(struct audio_client *ac,
						u32 opcode, u32 status,
						u32 handle)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;
	enum q6asm_rsp_owner owner = Q6ASM_RSP_NONE;

	spin_lock_irqsave(&rsp->lock, flags);
	if (rsp->pending && rsp->opcode == opcode) {
		rsp->status = status;
		rsp->handle = handle;
		rsp->done = true;
		rsp->pending = false;
		owner = Q6ASM_RSP_SYNC;
		wake_up(&rsp->wait);
	} else if (q6asm_rsp_clear_stale_locked(rsp, opcode)) {
		q6asm_stat_inc(ac, Q6ASM_STAT_LATE);
		dev_dbg_ratelimited(ac->dev,
				    "late response for retired command %#x dropped\n",
				    opcode);
	} else if (q6asm_rsp_async_take_locked(rsp, opcode)) {
		owner = Q6ASM_RSP_ASYNC;
	} else {
		q6asm_stat_inc(ac, Q6ASM_STAT_UNOWNED);
		dev_dbg_ratelimited(ac->dev,
				    "unowned response %#x dropped\n", opcode);
	}
	spin_unlock_irqrestore(&rsp->lock, flags);

	return owner;
}

/* Reserve a credit before sending so a fast response cannot arrive first. */
static int q6asm_cmd_rsp_async_begin(struct audio_client *ac, u32 opcode)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&rsp->lock, flags);
	if (rsp->overflow || q6asm_rsp_stale_locked(rsp, opcode) ||
	    (rsp->pending && rsp->opcode == opcode))
		rc = -EIO;
	else
		rc = q6asm_rsp_async_begin_locked(rsp, opcode);
	spin_unlock_irqrestore(&rsp->lock, flags);

	return rc;
}

static void q6asm_cmd_rsp_async_end(struct audio_client *ac, u32 opcode)
{
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;

	spin_lock_irqsave(&rsp->lock, flags);
	q6asm_rsp_async_end_locked(rsp, opcode);
	spin_unlock_irqrestore(&rsp->lock, flags);
}

static int q6asm_send_cmd_sync(struct audio_client *ac, struct apr_pkt *pkt,
			       u32 expected, u32 *handle)
{
	struct q6asm *a = ac->q6asm;
	struct q6asm_cmd_rsp *rsp = &ac->rsp;
	unsigned long flags;
	u32 status = 0;
	bool done;
	int rc;

	mutex_lock(&ac->cmd_lock);
	rc = q6asm_cmd_rsp_arm(ac, expected);
	if (rc < 0) {
		dev_err_ratelimited(ac->dev,
				    "CMD %#x refused: an earlier answer is still owed\n",
				    expected);
		goto out;
	}

	rc = apr_send_pkt(a->adev, pkt);
	if (rc < 0) {
		/* The packet never left the host; the remote state is unchanged. */
		q6asm_cmd_rsp_disarm(ac);
		goto out;
	}

	wait_event_timeout(rsp->wait, READ_ONCE(rsp->done), 5 * HZ);

	/*
	 * Read the result under the lock even after a timeout: a response can
	 * complete the command in the same jiffy the timeout fires.
	 */
	spin_lock_irqsave(&rsp->lock, flags);
	done = rsp->done;
	status = rsp->status;
	if (handle)
		*handle = rsp->handle;
	if (!done) {
		rsp->pending = false;
		q6asm_rsp_mark_stale_locked(rsp, expected);
	}
	spin_unlock_irqrestore(&rsp->lock, flags);

	if (!done) {
		dev_err(ac->dev,
			"CMD %#x timeout; its response is retired\n",
			pkt->hdr.opcode);
		rc = -ETIMEDOUT;
	} else if (status) {
		dev_err(ac->dev,
			"DSP command %#x session %d returned error[%#x]\n",
			pkt->hdr.opcode, ac->session, status);
		rc = -EREMOTEIO;
	} else {
		rc = 0;
	}

out:
	mutex_unlock(&ac->cmd_lock);
	return rc;
}

static int __q6asm_memory_unmap(struct audio_client *ac, int dir)
{
	struct avs_cmd_shared_mem_unmap_regions *mem_unmap;
	struct apr_pkt *pkt;
	unsigned long flags;
	int rc, pkt_size;

	if (ac->port[dir].mem_map_handle == 0) {
		dev_err(ac->dev, "invalid mem handle\n");
		return -EINVAL;
	}

	pkt_size = APR_HDR_SIZE + sizeof(*mem_unmap);
	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	mem_unmap = p + APR_HDR_SIZE;

	pkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	pkt->hdr.src_port = 0;
	pkt->hdr.dest_port = 0;
	pkt->hdr.pkt_size = pkt_size;
	/*
	 * Keep the direction in the low nibble: q6asm_stream_callback()
	 * recovers it with "hdr->token & 0x0F" to store the memory-map handle
	 * in the right port.  Sending it in the flag byte (the vendor
	 * kernel's layout) made every capture response land on the playback
	 * port, so capture reads used a zero mem_map_handle and returned
	 * silence.
	 */
	pkt->hdr.token = ((ac->session << 8) | dir);

	pkt->hdr.opcode = ASM_CMD_SHARED_MEM_UNMAP_REGIONS;
	mem_unmap->mem_map_handle = ac->port[dir].mem_map_handle;

	rc = q6asm_send_cmd_sync(ac, pkt, ASM_CMD_SHARED_MEM_UNMAP_REGIONS,
				 NULL);
	if (rc < 0)
		return rc;

	spin_lock_irqsave(&ac->lock, flags);
	ac->port[dir].mem_map_handle = 0;
	spin_unlock_irqrestore(&ac->lock, flags);

	return 0;
}


static void q6asm_audio_client_free_buf(struct audio_client *ac,
					struct audio_port_data *port)
{
	unsigned long flags;

	struct audio_buffer *buf;
	struct q6asm_submit *submit;

	spin_lock_irqsave(&ac->lock, flags);
	buf = port->buf;
	submit = port->submit;
	port->buf = NULL;
	port->submit = NULL;
	port->num_periods = 0;
	port->dsp_buf = 0;
	port->next_done = 0;
	atomic_set(&port->hw_ptr, 0);
	spin_unlock_irqrestore(&ac->lock, flags);
	kfree(buf);
	kfree(submit);
}

/**
 * q6asm_unmap_memory_regions() - unmap memory regions in the dsp.
 *
 * @dir: direction of audio stream
 * @ac: audio client instanace
 *
 * Return: Will be an negative value on failure or zero on success
 */
int q6asm_unmap_memory_regions(unsigned int dir, struct audio_client *ac)
{
	struct audio_port_data *port;
	int cnt = 0;
	int rc = 0;

	rc = q6asm_fault_take(ac, Q6ASM_FAULT_UNMAP);
	if (rc)
		return rc;

	port = &ac->port[dir];
	if (!port->buf) {
		rc = -EINVAL;
		goto err;
	}

	cnt = port->num_periods - 1;
	if (cnt >= 0) {
		rc = __q6asm_memory_unmap(ac, dir);
		if (rc < 0) {
			dev_err(ac->dev, "%s: Memory_unmap_regions failed %d\n",
				__func__, rc);
			goto err;
		}
	}

	q6asm_audio_client_free_buf(ac, port);

err:
	return rc;
}
EXPORT_SYMBOL_GPL(q6asm_unmap_memory_regions);

static int __q6asm_memory_map_regions(struct audio_client *ac, int dir,
				      size_t period_sz, unsigned int periods,
				      bool is_contiguous)
{
	struct avs_cmd_shared_mem_map_regions *cmd = NULL;
	struct avs_shared_map_region_payload *mregions = NULL;
	struct audio_port_data *port = NULL;
	struct audio_buffer *ab = NULL;
	struct apr_pkt *pkt;
	unsigned long flags;
	u32 handle = 0;
	uint32_t num_regions, buf_sz;
	int i, pkt_size, rc;

	if (is_contiguous) {
		num_regions = 1;
		buf_sz = period_sz * periods;
	} else {
		buf_sz = period_sz;
		num_regions = periods;
	}

	/* DSP expects size should be aligned to 4K */
	buf_sz = ALIGN(buf_sz, 4096);

	pkt_size = APR_HDR_SIZE + sizeof(*cmd) +
		   (sizeof(*mregions) * num_regions);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	cmd = p + APR_HDR_SIZE;
	mregions = p + APR_HDR_SIZE +  sizeof(*cmd);

	pkt->hdr.hdr_field = APR_SEQ_CMD_HDR_FIELD;
	pkt->hdr.src_port = 0;
	pkt->hdr.dest_port = 0;
	pkt->hdr.pkt_size = pkt_size;
	/* Direction in the low nibble, see __q6asm_memory_unmap(). */
	pkt->hdr.token = ((ac->session << 8) | dir);
	pkt->hdr.opcode = ASM_CMD_SHARED_MEM_MAP_REGIONS;

	cmd->mem_pool_id = ADSP_MEMORY_MAP_SHMEM8_4K_POOL;
	cmd->num_regions = num_regions;
	cmd->property_flag = 0x00;

	spin_lock_irqsave(&ac->lock, flags);
	port = &ac->port[dir];

	for (i = 0; i < num_regions; i++) {
		ab = &port->buf[i];
		mregions->shm_addr_lsw = lower_32_bits(ab->phys);
		mregions->shm_addr_msw = upper_32_bits(ab->phys);
		mregions->mem_size_bytes = buf_sz;
		++mregions;
	}
	spin_unlock_irqrestore(&ac->lock, flags);

	rc = q6asm_send_cmd_sync(ac, pkt, ASM_CMD_SHARED_MEM_MAP_REGIONS,
				 &handle);
	if (rc < 0)
		return rc;

	/*
	 * A successful status without a handle is a protocol anomaly: the
	 * mapping cannot be addressed or unmapped, so fail closed instead of
	 * handing a zero handle to READ/WRITE.
	 */
	if (!handle) {
		dev_err(ac->dev,
			"Memory_map_regions returned no handle for dir %u\n", dir);
		return -EPROTO;
	}

	spin_lock_irqsave(&ac->lock, flags);
	ac->port[dir].mem_map_handle = handle;
	spin_unlock_irqrestore(&ac->lock, flags);

	return 0;
}

/**
 * q6asm_map_memory_regions() - map memory regions in the dsp.
 *
 * @dir: direction of audio stream
 * @ac: audio client instanace
 * @phys: physical address that needs mapping.
 * @period_sz: audio period size
 * @periods: number of periods
 *
 * Return: Will be an negative value on failure or zero on success
 */
int q6asm_map_memory_regions(unsigned int dir, struct audio_client *ac,
			     phys_addr_t phys,
			     size_t period_sz, unsigned int periods)
{
	struct q6asm_submit *submit;
	struct audio_buffer *buf;
	unsigned long flags;
	int cnt;
	int rc;

	rc = q6asm_fault_take(ac, Q6ASM_FAULT_MAP);
	if (rc)
		return rc;

	if (!periods || !period_sz)
		return -EINVAL;

	spin_lock_irqsave(&ac->lock, flags);
	if (ac->port[dir].buf) {
		dev_err(ac->dev, "Buffer already allocated\n");
		spin_unlock_irqrestore(&ac->lock, flags);
		return -EBUSY;
	}

	buf = kzalloc_objs(*buf, periods, GFP_ATOMIC);
	submit = kzalloc_objs(*submit, periods, GFP_ATOMIC);
	if (!buf || !submit) {
		spin_unlock_irqrestore(&ac->lock, flags);
		kfree(buf);
		kfree(submit);
		return -ENOMEM;
	}

	ac->port[dir].buf = buf;
	ac->port[dir].submit = submit;

	buf[0].phys = phys;
	buf[0].size = period_sz;

	for (cnt = 1; cnt < periods; cnt++) {
		if (period_sz > 0) {
			buf[cnt].phys = buf[0].phys + (cnt * period_sz);
			buf[cnt].size = period_sz;
		}
	}
	ac->port[dir].num_periods = periods;
	ac->port[dir].dsp_buf = 0;
	atomic_set(&ac->port[dir].hw_ptr, 0);

	spin_unlock_irqrestore(&ac->lock, flags);

	rc = __q6asm_memory_map_regions(ac, dir, period_sz, periods, 1);
	if (rc < 0) {
		dev_err(ac->dev, "Memory_map_regions failed\n");
		q6asm_audio_client_free_buf(ac, &ac->port[dir]);
		return rc;
	}

	/*
	 * A new mapping starts a new data-path instance.  The generation is
	 * carried in the token so a completion that belonged to the previous
	 * stream can never move this ring's hw_ptr or free its slots.
	 */
	spin_lock_irqsave(&ac->lock, flags);
	ac->port[dir].generation = q6asm_next_generation();
	memset(ac->port[dir].submit, 0,
	       sizeof(*ac->port[dir].submit) * ac->port[dir].num_periods);
	ac->port[dir].dsp_buf = 0;
	ac->port[dir].next_done = 0;
	ac->port[dir].failures = 0;
	atomic_set(&ac->port[dir].hw_ptr, 0);
	spin_unlock_irqrestore(&ac->lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(q6asm_map_memory_regions);

static void q6asm_audio_client_release(struct kref *ref)
{
	struct audio_client *ac;
	struct q6asm *a;
	unsigned long flags;

	ac = container_of(ref, struct audio_client, refcount);
	a = ac->q6asm;

	spin_lock_irqsave(&a->slock, flags);
	a->session[ac->session] = NULL;
	spin_unlock_irqrestore(&a->slock, flags);

	/* Host-side ring descriptions; the DMA pages are not owned here. */
	q6asm_audio_client_free_buf(ac, &ac->port[SNDRV_PCM_STREAM_PLAYBACK]);
	q6asm_audio_client_free_buf(ac, &ac->port[SNDRV_PCM_STREAM_CAPTURE]);

	kfree(ac->hold_slot);
	kfree(ac);
}

/**
 * q6asm_audio_client_free() - Freee allocated audio client
 *
 * @ac: audio client to free
 */
void q6asm_audio_client_clear_callback(struct audio_client *ac)
{
	mutex_lock(&ac->cb_lock);
	ac->cb = NULL;
	ac->priv = NULL;
	mutex_unlock(&ac->cb_lock);
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_clear_callback);

void q6asm_audio_client_free(struct audio_client *ac)
{
	q6asm_audio_client_clear_callback(ac);
	kref_put(&ac->refcount, q6asm_audio_client_release);
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_free);

/**
 * q6asm_quarantine_session() - keep a session id unusable after a failed teardown
 *
 * @ac: audio client whose ADSP-side state could not be confirmed
 *
 * A failed CMD_CLOSE or UNMAP leaves the DSP owning a stream and/or DMA
 * mapping that the host can no longer address.  Retiring the id prevents a
 * later client from reusing the same session and colliding with that state.
 * The bitmap is module scoped: a q6asm driver rebind does not reset the
 * ADSP, so the id may only be reused after the remoteproc reports that the
 * ADSP actually stopped.
 */
void q6asm_quarantine_session(struct audio_client *ac)
{
	mutex_lock(&q6asm_global.lock);
	set_bit(ac->session, q6asm_global.session_quarantine);
	mutex_unlock(&q6asm_global.lock);

	dev_err(ac->dev, "session %d quarantined; ADSP ownership unknown\n",
		ac->session);
}
EXPORT_SYMBOL_GPL(q6asm_quarantine_session);

/**
 * q6asm_session_is_quarantined() - check a client's session quarantine bit
 *
 * @ac: audio client to test
 *
 * The DAI must refuse a new prepare() on an existing client whose session
 * was quarantined by an earlier failed teardown.
 */
bool q6asm_session_is_quarantined(struct audio_client *ac)
{
	bool quarantined;

	mutex_lock(&q6asm_global.lock);
	quarantined = test_bit(ac->session, q6asm_global.session_quarantine);
	mutex_unlock(&q6asm_global.lock);

	return quarantined;
}
EXPORT_SYMBOL_GPL(q6asm_session_is_quarantined);

static struct audio_client *q6asm_get_audio_client(struct q6asm *a,
						   int session_id)
{
	struct audio_client *ac = NULL;

	guard(spinlock_irqsave)(&a->slock);
	if ((session_id <= 0) || (session_id > MAX_SESSIONS)) {
		dev_err(a->dev, "invalid session: %d\n", session_id);
		goto err;
	}

	/* check for valid session */
	if (!a->session[session_id])
		goto err;
	else if (a->session[session_id]->session != session_id)
		goto err;

	ac = a->session[session_id];
	if (!kref_get_unless_zero(&ac->refcount))
		ac = NULL;
err:
	return ac;
}

int q6asm_get_hw_pointer(struct audio_client *ac, unsigned int dir)
{
	struct audio_port_data *data = &ac->port[dir];

	return (int)atomic_read(&data->hw_ptr);
}
EXPORT_SYMBOL_GPL(q6asm_get_hw_pointer);

/**
 * q6asm_get_buffer_generation() - identity of the current data path
 *
 * @ac: audio client
 * @dir: playback or capture port
 *
 * The front end records this value after a mapping and compares it with the
 * generation carried by a completion event.  An event that was validated
 * just before a re-prepare installed a new mapping is then dropped instead
 * of advancing the new stream.
 */
u32 q6asm_get_buffer_generation(struct audio_client *ac, unsigned int dir)
{
	unsigned long flags;
	u32 generation;

	if (dir >= ARRAY_SIZE(ac->port))
		return 0;

	spin_lock_irqsave(&ac->lock, flags);
	generation = ac->port[dir].generation;
	spin_unlock_irqrestore(&ac->lock, flags);

	return generation;
}
EXPORT_SYMBOL_GPL(q6asm_get_buffer_generation);

static void q6asm_notify_client(struct audio_client *ac,
				const struct q6asm_client_event *event)
{
	mutex_lock(&ac->cb_lock);
	if (ac->cb)
		ac->cb(event, ac->priv);
	mutex_unlock(&ac->cb_lock);
}

static void q6asm_emit_simple(struct audio_client *ac,
			      enum q6asm_client_event_id id, u32 stream_id,
			      u32 status)
{
	struct q6asm_client_event event = {
		.id = id,
		.stream_id = stream_id,
		.status = status,
	};

	q6asm_notify_client(ac, &event);
}

/*
 * Match a completion or a rejection to the period that was actually posted.
 * A completion may only be accepted while its slot is in flight, it belongs
 * to the current data-path generation and it names the submitted stream and
 * buffer.  Anything else (a duplicate, an answer from a closed stream, a
 * stale token) is dropped before it can move hw_ptr or free a slot.
 */
static bool q6asm_complete_transfer(struct audio_client *ac, unsigned int dir,
				    u32 index, u32 generation, u32 stream_id,
				    phys_addr_t phys, bool check_phys,
				    bool strict, u32 seq_id, u32 map_handle,
				    u32 len, u32 status)
{
	struct q6asm_client_event event = {
		.stream_id = stream_id,
		.index = index,
		.status = status,
	};
	struct audio_port_data *port = &ac->port[dir];
	struct q6asm_submit *submit;
	unsigned long flags;
	bool notify = false;

	if (!(ac->io_mode & ASM_SYNC_IO_MODE))
		return false;

	spin_lock_irqsave(&ac->lock, flags);
	if (!port->buf || !port->submit || index >= port->num_periods)
		goto out;

	submit = &port->submit[index];
	if (!submit->in_flight || submit->generation != port->generation)
		goto out;
	/*
	 * Only firmware shown to echo the token's generation byte may use it
	 * as a discriminator; other boards keep the historical matching.  The
	 * decoded byte is compared with the encoded half of submit->generation,
	 * never with the full host value.
	 */
	if ((ac->flags & Q6ASM_CLIENT_TOKEN_GENERATION) &&
	    q6asm_token_gen(submit->generation) != generation)
		goto out;
	if (stream_id && submit->stream_id != stream_id)
		goto out;
	if (check_phys && submit->phys != phys)
		goto out;
	if (strict) {
		if (seq_id != index) {
			dev_dbg_ratelimited(ac->dev,
					    "read completion seq %u token %u\n",
					    seq_id, index);
			goto out;
		}
		if (map_handle && map_handle != port->mem_map_handle) {
			dev_dbg_ratelimited(ac->dev,
					    "read completion map handle %#x (port %#x)\n",
					    map_handle, port->mem_map_handle);
			goto out;
		}
	}

	submit->in_flight = false;
	if (status) {
		port->failures++;
		event.id = (dir == SNDRV_PCM_STREAM_CAPTURE) ?
			Q6ASM_EVENT_READ_FAILED : Q6ASM_EVENT_WRITE_FAILED;
		event.len = 0;
	} else {
		/*
		 * Count every period that is now complete in ring order.  An
		 * out-of-order completion is remembered and counted when the
		 * periods before it have completed, so hw_ptr never skips.
		 */
		submit->done = true;
		while (port->submit[port->next_done].done) {
			port->submit[port->next_done].done = false;
			port->next_done = (port->next_done + 1) %
					   port->num_periods;
			atomic_inc(&port->hw_ptr);
		}
		event.id = (dir == SNDRV_PCM_STREAM_CAPTURE) ?
			Q6ASM_EVENT_READ_DONE : Q6ASM_EVENT_WRITE_DONE;
		event.len = len ? min(len, submit->len) : submit->len;
	}
	/* Let the front end drop an event that outlived a re-prepare. */
	event.generation = port->generation;
	notify = true;
out:
	spin_unlock_irqrestore(&ac->lock, flags);
	if (notify) {
		q6asm_stat_inc(ac, Q6ASM_STAT_ACCEPTED);
		q6asm_notify_client(ac, &event);
	} else {
		q6asm_stat_inc(ac, Q6ASM_STAT_DROPPED);
	}

	return notify;
}

static void q6asm_data_failed(struct audio_client *ac, unsigned int dir,
			      u32 token, u32 status, u32 stream_id)
{
	struct audio_port_data *port = &ac->port[dir];
	u32 index = token & Q6ASM_TOKEN_INDEX_MASK;
	u32 generation = (token & Q6ASM_TOKEN_GEN_MASK) >>
			 Q6ASM_TOKEN_GEN_SHIFT;
	unsigned long flags;
	u32 cur_gen;
	bool found = false;
	int i;

	if (q6asm_complete_transfer(ac, dir, index, generation, stream_id, 0,
				    false, false, 0, 0, 0, status))
		return;

	/*
	 * The token did not name a live period.  If it carries a generation
	 * that is not the current one, the rejection belongs to an older
	 * stream instance and must not touch this ring.  Otherwise the
	 * firmware echoed the generation but not the index: fail the oldest
	 * live period so the front end can report the failure.
	 */
	spin_lock_irqsave(&ac->lock, flags);
	cur_gen = port->generation;
	if ((ac->flags & Q6ASM_CLIENT_TOKEN_GENERATION) &&
	    q6asm_token_gen(cur_gen) != generation) {
		/* A stale or unusable token must not fail a live period. */
		found = false;
	} else if (port->submit && port->num_periods) {
		for (i = 0; i < port->num_periods; i++) {
			index = (port->next_done + i) % port->num_periods;
			if (port->submit[index].in_flight &&
			    port->submit[index].generation == cur_gen &&
			    (!stream_id ||
			     port->submit[index].stream_id == stream_id)) {
				found = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&ac->lock, flags);

	if (!found) {
		dev_dbg_ratelimited(ac->dev,
				    "data rejection token %#x matched no live period\n",
				    token);
		return;
	}

	dev_dbg_ratelimited(ac->dev,
			    "data rejection token %#x: failing period %u\n",
			    token, index);
	q6asm_complete_transfer(ac, dir, index, q6asm_token_gen(cur_gen),
				stream_id, 0, false, false, 0, 0, 0, status);
}

static int32_t q6asm_stream_callback(struct apr_device *adev,
				     const struct apr_resp_pkt *data,
				     int session_id)
{
	struct q6asm *q6asm = dev_get_drvdata(&adev->dev);
	const struct aprv2_ibasic_rsp_result_t *result;
	const struct apr_hdr *hdr = &data->hdr;
	struct audio_client *ac;
	u32 stream_id = hdr->dest_port & 0xFF;
	int ret = 0;

	ac = q6asm_get_audio_client(q6asm, session_id);
	if (!ac)/* Audio client might already be freed by now */
		return 0;

	result = data->payload;

	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT:
		if (data->payload_size < sizeof(*result)) {
			ret = -EPROTO;
			goto done;
		}

		switch (result->opcode) {
		case ASM_DATA_CMD_READ_V2:
		case ASM_DATA_CMD_WRITE_V2:
			/*
			 * A basic result for a data command is only meaningful
			 * when it carries a failure.  A status-0 ACK says the
			 * command was accepted; the *_DONE event reports the
			 * transfer.
			 */
			if (!result->status)
				break;
			q6asm_data_failed(ac,
					  result->opcode ==
					  ASM_DATA_CMD_READ_V2 ?
					  SNDRV_PCM_STREAM_CAPTURE :
					  SNDRV_PCM_STREAM_PLAYBACK,
					  hdr->token, result->status,
					  stream_id);
			break;
		case ASM_SESSION_CMD_RUN_V2: {
			u32 status = result->status;
			u32 injected;
			enum q6asm_rsp_owner owner;

			/*
			 * The ADSP answers ADSP_EALREADY when the session is
			 * already running (a duplicated trigger); that is a
			 * success for the state machine, not a failure.
			 */
			if (status == ADSP_EALREADY)
				status = ADSP_EOK;
			injected = q6asm_fault_status_take(ac,
							   Q6ASM_FAULT_RUN_REJECT,
							   ADSP_EFAILED);
			if (injected)
				status = injected;
			owner = q6asm_cmd_rsp_claim(ac, result->opcode, status, 0);
			if (owner != Q6ASM_RSP_NONE)
				q6asm_emit_simple(ac, Q6ASM_EVENT_RUN_DONE,
						  stream_id, status);
			break;
		}
		case ASM_SESSION_CMD_PAUSE: {
			u32 status = result->status;
			u32 injected;
			enum q6asm_rsp_owner owner;

			if (status == ADSP_EALREADY)
				status = ADSP_EOK;
			injected = q6asm_fault_status_take(ac,
							   Q6ASM_FAULT_PAUSE_REJECT,
							   ADSP_EFAILED);
			if (injected)
				status = injected;
			owner = q6asm_cmd_rsp_claim(ac, result->opcode, status, 0);
			if (owner != Q6ASM_RSP_NONE)
				q6asm_emit_simple(ac, Q6ASM_EVENT_PAUSE_DONE,
						  stream_id, status);
			break;
		}
		case ASM_DATA_CMD_EOS: {
			enum q6asm_rsp_owner owner;

			owner = q6asm_cmd_rsp_claim(ac, result->opcode,
						    result->status, 0);
			if (owner != Q6ASM_RSP_NONE)
				q6asm_emit_simple(ac, Q6ASM_EVENT_EOS_DONE,
						  stream_id, result->status);
			break;
		}
		case ASM_STREAM_CMD_CLOSE:
		case ASM_SESSION_CMD_SUSPEND:
		case ASM_STREAM_CMD_FLUSH:
		case ASM_STREAM_CMD_FLUSH_READBUFS:
			q6asm_cmd_rsp_claim(ac, result->opcode,
					    result->status, 0);
			break;
		case ASM_STREAM_CMD_OPEN_WRITE_V3:
		case ASM_STREAM_CMD_OPEN_READ_V3:
		case ASM_STREAM_CMD_OPEN_READWRITE_V2:
		case ASM_STREAM_CMD_SET_ENCDEC_PARAM:
		case ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2:
		case ASM_DATA_CMD_REMOVE_INITIAL_SILENCE:
		case ASM_DATA_CMD_REMOVE_TRAILING_SILENCE:
			q6asm_cmd_rsp_claim(ac, result->opcode,
					    result->status, 0);
			break;
		default:
			dev_dbg_ratelimited(ac->dev,
					    "basic result for unexpected opcode %#x (status %u)\n",
					    result->opcode, result->status);
			break;
		}
		goto done;

	case ASM_DATA_EVENT_WRITE_DONE_V2: {
		phys_addr_t phys;
		u32 index = hdr->token & Q6ASM_TOKEN_INDEX_MASK;
		u32 generation = (hdr->token & Q6ASM_TOKEN_GEN_MASK) >>
				 Q6ASM_TOKEN_GEN_SHIFT;

		if (data->payload_size < 2 * sizeof(u32)) {
			dev_dbg_ratelimited(ac->dev,
					    "short write completion (%d bytes)\n",
					    data->payload_size);
			break;
		}

		if (q6asm_fault_status_take(ac, Q6ASM_FAULT_WRITE_REJECT,
					    ADSP_EFAILED)) {
			q6asm_complete_transfer(ac,
						SNDRV_PCM_STREAM_PLAYBACK,
						index, generation, stream_id,
						0, false, false, 0, 0, 0,
						ADSP_EFAILED);
			break;
		}

		phys = ((phys_addr_t)result->status << 32) | result->opcode;
		q6asm_complete_transfer(ac, SNDRV_PCM_STREAM_PLAYBACK, index,
					generation, stream_id, phys, true, false,
					0, 0, 0, 0);
		break;
	}
	case ASM_DATA_EVENT_READ_DONE_V2: {
		const struct asm_data_cmd_read_v2_done *done = data->payload;
		phys_addr_t phys;
		u32 index = hdr->token & Q6ASM_TOKEN_INDEX_MASK;
		u32 generation = (hdr->token & Q6ASM_TOKEN_GEN_MASK) >>
				 Q6ASM_TOKEN_GEN_SHIFT;
		u32 len = 0;
		bool full;

		if (data->payload_size < Q6ASM_READ_DONE_MIN) {
			dev_dbg_ratelimited(ac->dev,
					    "short read completion (%d bytes)\n",
					    data->payload_size);
			break;
		}

		if (done->status) {
			if (data->payload_size >= Q6ASM_READ_DONE_FULL)
				index = done->seq_id;
			q6asm_complete_transfer(ac, SNDRV_PCM_STREAM_CAPTURE,
						index, generation, stream_id,
						0, false, false, 0, 0, 0,
						done->status);
			break;
		}

		if (q6asm_fault_status_take(ac, Q6ASM_FAULT_READ_REJECT,
					    ADSP_ENOTREADY)) {
			q6asm_complete_transfer(ac, SNDRV_PCM_STREAM_CAPTURE,
						index, generation, stream_id,
						0, false, false, 0, 0, 0,
						ADSP_ENOTREADY);
			break;
		}

		full = data->payload_size >= Q6ASM_READ_DONE_FULL;
		if (full)
			len = done->buf_size;

		phys = ((phys_addr_t)done->buf_addr_msw << 32) |
		       done->buf_addr_lsw;
		q6asm_complete_transfer(ac, SNDRV_PCM_STREAM_CAPTURE, index,
					generation, stream_id, phys, true,
					full && (ac->flags &
						 Q6ASM_CLIENT_STRICT_CAPTURE),
					full ? done->seq_id : 0,
					full ? done->mem_map_handle : 0,
					len, 0);
		break;
	}
	case ASM_DATA_EVENT_RENDERED_EOS:
		q6asm_emit_simple(ac, Q6ASM_EVENT_EOS_RENDERED, stream_id, 0);
		break;
	default:
		dev_dbg_ratelimited(ac->dev, "unexpected response %#x\n",
				    hdr->opcode);
		break;
	}

done:
	kref_put(&ac->refcount, q6asm_audio_client_release);
	return ret;
}

static int q6asm_srvc_callback(struct apr_device *adev,
			       const struct apr_resp_pkt *data)
{
	struct q6asm *q6asm = dev_get_drvdata(&adev->dev);
	const struct aprv2_ibasic_rsp_result_t *result;
	const struct apr_hdr *hdr = &data->hdr;
	struct audio_client *ac;
	u32 sid, dir;
	int session_id;

	/*
	 * Stream-scoped responses carry the session in the destination port;
	 * MAP/UNMAP commands use port 0 and put the session and direction in
	 * the token instead.
	 */
	session_id = (hdr->dest_port >> 8) & 0xFF;
	if (session_id)
		return q6asm_stream_callback(adev, data, session_id);

	sid = (hdr->token >> 8) & 0x0F;
	if (!sid)
		sid = (hdr->dest_port >> 8) & 0xFF;
	if (!sid || sid > MAX_SESSIONS)
		return 0;

	ac = q6asm_get_audio_client(q6asm, sid);
	if (!ac) {
		dev_dbg_ratelimited(&adev->dev,
				    "map response for inactive session %u\n", sid);
		return 0;
	}

	dir = hdr->token & 0x0F;
	if (dir >= ARRAY_SIZE(ac->port)) {
		dev_dbg_ratelimited(ac->dev, "map response with bad dir %u\n",
				    dir);
		goto done;
	}

	result = data->payload;
	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT:
		if (data->payload_size < sizeof(*result))
			break;
		switch (result->opcode) {
		case ASM_CMD_SHARED_MEM_MAP_REGIONS:
			/*
			 * A status-0 basic result only acknowledges the command;
			 * the mapping handle arrives in the CMDRSP.  A non-zero
			 * status is the real answer and fails the wait.
			 */
			if (result->status)
				q6asm_cmd_rsp_claim(ac, result->opcode,
						    result->status, 0);
			else
				dev_dbg(ac->dev,
					"MAP accepted without a handle, waiting for the response\n");
			break;
		case ASM_CMD_SHARED_MEM_UNMAP_REGIONS:
			q6asm_cmd_rsp_claim(ac, result->opcode,
					    result->status, 0);
			break;
		default:
			dev_dbg_ratelimited(ac->dev,
					    "unexpected map-session result %#x\n",
					    result->opcode);
			break;
		}
		break;
	case ASM_CMDRSP_SHARED_MEM_MAP_REGIONS:
		if (data->payload_size < sizeof(u32))
			break;
		q6asm_cmd_rsp_claim(ac, ASM_CMD_SHARED_MEM_MAP_REGIONS, 0,
				    result->opcode);
		break;
	case ASM_CMD_SHARED_MEM_UNMAP_REGIONS:
		/* Some firmware echoes the command opcode as its answer. */
		q6asm_cmd_rsp_claim(ac, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, 0,
				    0);
		break;
	default:
		dev_dbg_ratelimited(ac->dev, "unexpected map-session opcode %#x\n",
				    hdr->opcode);
		break;
	}

done:
	kref_put(&ac->refcount, q6asm_audio_client_release);
	return 0;
}

/**
 * q6asm_get_session_id() - get session id for audio client
 *
 * @c: audio client pointer
 *
 * Return: Will be an session id of the audio client.
 */
int q6asm_get_session_id(struct audio_client *c)
{
	return c->session;
}
EXPORT_SYMBOL_GPL(q6asm_get_session_id);

/**
 * q6asm_audio_client_alloc() - Allocate a new audio client
 *
 * @dev: Pointer to asm child device.
 * @cb: event callback.
 * @priv: private data associated with this client.
 * @session_id: session id
 * @perf_mode: performace mode for this client
 *
 * Return: Will be an error pointer on error or a valid audio client
 * on success.
 */
struct audio_client *q6asm_audio_client_alloc(struct device *dev, q6asm_cb cb,
					      void *priv, int session_id,
					      int perf_mode)
{
	struct q6asm *a = dev_get_drvdata(dev->parent);
	struct audio_client *ac;
	unsigned long flags;

	if (session_id < 0 || session_id >= MAX_SESSIONS)
		return ERR_PTR(-EINVAL);

	ac = kzalloc_obj(*ac);
	if (!ac)
		return ERR_PTR(-ENOMEM);

	/*
	 * Allocate the held-buffer record up front so that a failed teardown
	 * never has to allocate.  It is only used if CLOSE/UNMAP cannot
	 * confirm that the DSP gave the buffer back.
	 */
	ac->hold_slot = kzalloc_obj(*ac->hold_slot);
	if (!ac->hold_slot) {
		kfree(ac);
		return ERR_PTR(-ENOMEM);
	}

	ac->session = session_id + 1;
	ac->cb = cb;
	ac->dev = dev;
	ac->q6asm = a;
	ac->priv = priv;
	ac->io_mode = ASM_SYNC_IO_MODE;
	ac->perf_mode = perf_mode;
	ac->adev = a->adev;
	kref_init(&ac->refcount);

	init_waitqueue_head(&ac->rsp.wait);
	spin_lock_init(&ac->rsp.lock);
	mutex_init(&ac->cmd_lock);
	mutex_init(&ac->cb_lock);
	spin_lock_init(&ac->lock);

	/* Lock order: global (mutex) -> session table (spinlock). */
	mutex_lock(&q6asm_global.lock);
	spin_lock_irqsave(&a->slock, flags);
	if (test_bit(ac->session, q6asm_global.session_quarantine)) {
		spin_unlock_irqrestore(&a->slock, flags);
		mutex_unlock(&q6asm_global.lock);
		kfree(ac->hold_slot);
		kfree(ac);
		dev_err(dev,
			"session %d is quarantined; refusing a new client\n",
			session_id + 1);
		return ERR_PTR(-EIO);
	}
	if (a->session[ac->session]) {
		spin_unlock_irqrestore(&a->slock, flags);
		mutex_unlock(&q6asm_global.lock);
		kfree(ac->hold_slot);
		kfree(ac);
		return ERR_PTR(-EBUSY);
	}
	a->session[ac->session] = ac;
	spin_unlock_irqrestore(&a->slock, flags);
	mutex_unlock(&q6asm_global.lock);

	return ac;
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_alloc);

void q6asm_audio_client_set_flags(struct audio_client *ac, unsigned int flags)
{
	ac->flags = flags;
}
EXPORT_SYMBOL_GPL(q6asm_audio_client_set_flags);

static int q6asm_ac_send_cmd_sync(struct audio_client *ac, struct apr_pkt *pkt)
{
	return q6asm_send_cmd_sync(ac, pkt, pkt->hdr.opcode, NULL);
}

/**
 * q6asm_open_write() - Open audio client for writing
 * @ac: audio client pointer
 * @stream_id: stream id of q6asm session
 * @format: audio sample format
 * @codec_profile: compressed format profile
 * @bits_per_sample: bits per sample
 * @is_gapless: flag to indicate if this is a gapless stream
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_open_write(struct audio_client *ac, uint32_t stream_id,
		     uint32_t format, u32 codec_profile,
		     uint16_t bits_per_sample, bool is_gapless)
{
	struct asm_stream_cmd_open_write_v3 *open;
	struct apr_pkt *pkt;
	int rc, pkt_size = APR_HDR_SIZE + sizeof(*open);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	open = p + APR_HDR_SIZE;
	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_STREAM_CMD_OPEN_WRITE_V3;
	open->mode_flags = 0x00;
	open->mode_flags |= ASM_LEGACY_STREAM_SESSION;
	if (is_gapless)
		open->mode_flags |= BIT(ASM_SHIFT_GAPLESS_MODE_FLAG);

	/* source endpoint : matrix */
	open->sink_endpointype = ASM_END_POINT_DEVICE_MATRIX;
	open->bits_per_sample = bits_per_sample;
	open->postprocopo_id = ASM_NULL_POPP_TOPOLOGY;

	switch (format) {
	case SND_AUDIOCODEC_MP3:
		open->dec_fmt_id = ASM_MEDIA_FMT_MP3;
		break;
	case FORMAT_LINEAR_PCM:
		open->dec_fmt_id = ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2;
		break;
	case SND_AUDIOCODEC_FLAC:
		open->dec_fmt_id = ASM_MEDIA_FMT_FLAC;
		break;
	case SND_AUDIOCODEC_WMA:
		switch (codec_profile) {
		case SND_AUDIOPROFILE_WMA9:
			open->dec_fmt_id = ASM_MEDIA_FMT_WMA_V9;
			break;
		case SND_AUDIOPROFILE_WMA10:
		case SND_AUDIOPROFILE_WMA9_PRO:
		case SND_AUDIOPROFILE_WMA9_LOSSLESS:
		case SND_AUDIOPROFILE_WMA10_LOSSLESS:
			open->dec_fmt_id = ASM_MEDIA_FMT_WMA_V10;
			break;
		default:
			dev_err(ac->dev, "Invalid codec profile 0x%x\n",
				codec_profile);
			rc = -EINVAL;
			goto err;
		}
		break;
	case SND_AUDIOCODEC_ALAC:
		open->dec_fmt_id = ASM_MEDIA_FMT_ALAC;
		break;
	case SND_AUDIOCODEC_APE:
		open->dec_fmt_id = ASM_MEDIA_FMT_APE;
		break;
	default:
		dev_err(ac->dev, "Invalid format 0x%x\n", format);
		rc = -EINVAL;
		goto err;
	}

	rc = q6asm_ac_send_cmd_sync(ac, pkt);
	if (rc < 0)
		goto err;

	ac->io_mode |= ASM_TUN_WRITE_IO_MODE;

err:
	return rc;
}
EXPORT_SYMBOL_GPL(q6asm_open_write);

static int __q6asm_run(struct audio_client *ac, uint32_t stream_id,
		       uint32_t flags, uint32_t msw_ts, uint32_t lsw_ts,
		       bool wait)
{
	struct asm_session_cmd_run_v2 *run;
	struct apr_pkt *pkt;
	int rc, pkt_size = APR_HDR_SIZE + sizeof(*run);

	rc = q6asm_fault_take(ac, Q6ASM_FAULT_RUN);
	if (rc)
		return rc;

	void *p __free(kfree) = kzalloc(pkt_size, GFP_ATOMIC);
	if (!p)
		return -ENOMEM;

	pkt = p;
	run = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_SESSION_CMD_RUN_V2;
	run->flags = flags;
	run->time_lsw = lsw_ts;
	run->time_msw = msw_ts;
	if (wait) {
		rc = q6asm_ac_send_cmd_sync(ac, pkt);
	} else {
		/*
		 * Reserve the response before the send: the basic result can
		 * arrive on the APR worker as soon as the packet is queued.
		 */
		rc = q6asm_cmd_rsp_async_begin(ac, pkt->hdr.opcode);
		if (rc < 0) {
			dev_err_ratelimited(ac->dev,
					    "nowait RUN refused: an earlier answer is still owed\n");
			return rc;
		}
		rc = apr_send_pkt(ac->adev, pkt);
		if (rc == pkt_size) {
			rc = 0;
		} else {
			q6asm_cmd_rsp_async_end(ac, pkt->hdr.opcode);
			rc = rc < 0 ? rc : -EIO;
		}
	}

	return rc;
}

/**
 * q6asm_run() - start the audio client
 *
 * @ac: audio client pointer
 * @stream_id: stream id of q6asm session
 * @flags: flags associated with write
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_run(struct audio_client *ac, uint32_t stream_id, uint32_t flags,
	      uint32_t msw_ts, uint32_t lsw_ts)
{
	return __q6asm_run(ac, stream_id, flags, msw_ts, lsw_ts, true);
}
EXPORT_SYMBOL_GPL(q6asm_run);

/**
 * q6asm_run_nowait() - start the audio client withou blocking
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @flags: flags associated with write
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_run_nowait(struct audio_client *ac, uint32_t stream_id,
		     uint32_t flags, uint32_t msw_ts, uint32_t lsw_ts)
{
	return __q6asm_run(ac, stream_id, flags, msw_ts, lsw_ts, false);
}
EXPORT_SYMBOL_GPL(q6asm_run_nowait);

/**
 * q6asm_media_format_block_multi_ch_pcm() - setup pcm configuration
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @rate: audio sample rate
 * @channels: number of audio channels.
 * @channel_map: channel map pointer
 * @bits_per_sample: bits per sample
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_media_format_block_multi_ch_pcm(struct audio_client *ac,
					  uint32_t stream_id,
					  uint32_t rate, uint32_t channels,
					  u8 channel_map[PCM_MAX_NUM_CHANNEL],
					  uint16_t bits_per_sample)
{
	struct asm_multi_channel_pcm_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	u8 *channel_mapping;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);
	fmt->num_channels = channels;
	fmt->bits_per_sample = bits_per_sample;
	fmt->sample_rate = rate;
	fmt->is_signed = 1;

	channel_mapping = fmt->channel_mapping;

	if (channel_map) {
		memcpy(channel_mapping, channel_map, PCM_MAX_NUM_CHANNEL);
	} else {
		if (q6dsp_map_channels(channel_mapping, channels)) {
			dev_err(ac->dev, " map channels failed %d\n", channels);
			return -EINVAL;
		}
	}

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_media_format_block_multi_ch_pcm);

int q6asm_stream_media_format_block_flac(struct audio_client *ac,
					 uint32_t stream_id,
					 struct q6asm_flac_cfg *cfg)
{
	struct asm_flac_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);
	fmt->is_stream_info_present = cfg->stream_info_present;
	fmt->num_channels = cfg->ch_cfg;
	fmt->min_blk_size = cfg->min_blk_size;
	fmt->max_blk_size = cfg->max_blk_size;
	fmt->sample_rate = cfg->sample_rate;
	fmt->min_frame_size = cfg->min_frame_size;
	fmt->max_frame_size = cfg->max_frame_size;
	fmt->sample_size = cfg->sample_size;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_stream_media_format_block_flac);

int q6asm_stream_media_format_block_wma_v9(struct audio_client *ac,
					   uint32_t stream_id,
					   struct q6asm_wma_cfg *cfg)
{
	struct asm_wmastdv9_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);
	fmt->fmtag = cfg->fmtag;
	fmt->num_channels = cfg->num_channels;
	fmt->sample_rate = cfg->sample_rate;
	fmt->bytes_per_sec = cfg->bytes_per_sec;
	fmt->blk_align = cfg->block_align;
	fmt->bits_per_sample = cfg->bits_per_sample;
	fmt->channel_mask = cfg->channel_mask;
	fmt->enc_options = cfg->enc_options;
	fmt->reserved = 0;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_stream_media_format_block_wma_v9);

int q6asm_stream_media_format_block_wma_v10(struct audio_client *ac,
					    uint32_t stream_id,
					    struct q6asm_wma_cfg *cfg)
{
	struct asm_wmaprov10_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);
	fmt->fmtag = cfg->fmtag;
	fmt->num_channels = cfg->num_channels;
	fmt->sample_rate = cfg->sample_rate;
	fmt->bytes_per_sec = cfg->bytes_per_sec;
	fmt->blk_align = cfg->block_align;
	fmt->bits_per_sample = cfg->bits_per_sample;
	fmt->channel_mask = cfg->channel_mask;
	fmt->enc_options = cfg->enc_options;
	fmt->advanced_enc_options1 = cfg->adv_enc_options;
	fmt->advanced_enc_options2 = cfg->adv_enc_options2;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_stream_media_format_block_wma_v10);

int q6asm_stream_media_format_block_alac(struct audio_client *ac,
					 uint32_t stream_id,
					 struct q6asm_alac_cfg *cfg)
{
	struct asm_alac_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);

	fmt->frame_length = cfg->frame_length;
	fmt->compatible_version = cfg->compatible_version;
	fmt->bit_depth =  cfg->bit_depth;
	fmt->num_channels = cfg->num_channels;
	fmt->max_run = cfg->max_run;
	fmt->max_frame_bytes = cfg->max_frame_bytes;
	fmt->avg_bit_rate = cfg->avg_bit_rate;
	fmt->sample_rate = cfg->sample_rate;
	fmt->channel_layout_tag = cfg->channel_layout_tag;
	fmt->pb = cfg->pb;
	fmt->mb = cfg->mb;
	fmt->kb = cfg->kb;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_stream_media_format_block_alac);

int q6asm_stream_media_format_block_ape(struct audio_client *ac,
					uint32_t stream_id,
					struct q6asm_ape_cfg *cfg)
{
	struct asm_ape_fmt_blk_v2 *fmt;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*fmt);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	fmt = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2;
	fmt->fmt_blk.fmt_blk_size = sizeof(*fmt) - sizeof(fmt->fmt_blk);

	fmt->compatible_version = cfg->compatible_version;
	fmt->compression_level = cfg->compression_level;
	fmt->format_flags = cfg->format_flags;
	fmt->blocks_per_frame = cfg->blocks_per_frame;
	fmt->final_frame_blocks = cfg->final_frame_blocks;
	fmt->total_frames = cfg->total_frames;
	fmt->bits_per_sample = cfg->bits_per_sample;
	fmt->num_channels = cfg->num_channels;
	fmt->sample_rate = cfg->sample_rate;
	fmt->seek_table_present = cfg->seek_table_present;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_stream_media_format_block_ape);

static int q6asm_stream_remove_silence(struct audio_client *ac, uint32_t stream_id,
				       uint32_t cmd,
				       uint32_t num_samples)
{
	uint32_t *samples;
	struct apr_pkt *pkt;
	int rc, pkt_size = APR_HDR_SIZE + sizeof(uint32_t);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_ATOMIC);
	if (!p)
		return -ENOMEM;

	pkt = p;
	samples = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = cmd;
	*samples = num_samples;
	rc = apr_send_pkt(ac->adev, pkt);
	if (rc == pkt_size)
		rc = 0;

	return rc;
}

int q6asm_stream_remove_initial_silence(struct audio_client *ac,
					uint32_t stream_id,
					uint32_t initial_samples)
{
	return q6asm_stream_remove_silence(ac, stream_id,
					   ASM_DATA_CMD_REMOVE_INITIAL_SILENCE,
					   initial_samples);
}
EXPORT_SYMBOL_GPL(q6asm_stream_remove_initial_silence);

int q6asm_stream_remove_trailing_silence(struct audio_client *ac, uint32_t stream_id,
					 uint32_t trailing_samples)
{
	return q6asm_stream_remove_silence(ac, stream_id,
				   ASM_DATA_CMD_REMOVE_TRAILING_SILENCE,
				   trailing_samples);
}
EXPORT_SYMBOL_GPL(q6asm_stream_remove_trailing_silence);

/**
 * q6asm_enc_cfg_blk_pcm_format_support() - setup pcm configuration for capture
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @rate: audio sample rate
 * @channels: number of audio channels.
 * @bits_per_sample: bits per sample
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_enc_cfg_blk_pcm_format_support(struct audio_client *ac,
					 uint32_t stream_id, uint32_t rate,
					 uint32_t channels,
					 uint16_t bits_per_sample)
{
	struct asm_multi_channel_pcm_enc_cfg_v2  *enc_cfg;
	struct apr_pkt *pkt;
	u8 *channel_mapping;
	u32 frames_per_buf = 0;
	int pkt_size = APR_HDR_SIZE + sizeof(*enc_cfg);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	enc_cfg = p + APR_HDR_SIZE;
	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, true, stream_id);

	pkt->hdr.opcode = ASM_STREAM_CMD_SET_ENCDEC_PARAM;
	enc_cfg->encdec.param_id = ASM_PARAM_ID_ENCDEC_ENC_CFG_BLK_V2;
	enc_cfg->encdec.param_size = sizeof(*enc_cfg) - sizeof(enc_cfg->encdec);
	enc_cfg->encblk.frames_per_buf = frames_per_buf;
	enc_cfg->encblk.enc_cfg_blk_size  = enc_cfg->encdec.param_size -
					sizeof(struct asm_enc_cfg_blk_param_v2);

	enc_cfg->num_channels = channels;
	enc_cfg->bits_per_sample = bits_per_sample;
	enc_cfg->sample_rate = rate;
	enc_cfg->is_signed = 1;
	channel_mapping = enc_cfg->channel_mapping;

	if (q6dsp_map_channels(channel_mapping, channels))
		return -EINVAL;

	return q6asm_ac_send_cmd_sync(ac, pkt);
}
EXPORT_SYMBOL_GPL(q6asm_enc_cfg_blk_pcm_format_support);


/**
 * q6asm_read() - read data of period size from audio client
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 *
 * Return: the period index that was posted, or a negative error.  -EAGAIN
 * means every ring slot is still owned by the ADSP.
 */
static int q6asm_post_transfer(struct audio_client *ac, unsigned int dir,
			       u32 stream_id, u32 len, u32 msw_ts, u32 lsw_ts,
			       u32 wflags)
{
	struct asm_data_cmd_read_v2 *read;
	struct asm_data_cmd_write_v2 *write;
	struct audio_port_data *port;
	struct q6asm_submit *submit;
	struct audio_buffer *ab;
	struct apr_pkt *pkt;
	unsigned long flags;
	u32 index, token;
	int pkt_size, rc;

	if (dir == SNDRV_PCM_STREAM_CAPTURE)
		pkt_size = APR_HDR_SIZE + sizeof(*read);
	else
		pkt_size = APR_HDR_SIZE + sizeof(*write);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_ATOMIC);
	if (!p)
		return -ENOMEM;

	pkt = p;
	read = p + APR_HDR_SIZE;
	write = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr, pkt_size, false, stream_id);
	pkt->hdr.opcode = (dir == SNDRV_PCM_STREAM_CAPTURE) ?
			  ASM_DATA_CMD_READ_V2 : ASM_DATA_CMD_WRITE_V2;

	/*
	 * Send under ac->lock and publish the submit record before the packet
	 * can complete.  A failed send clears the record again, so the ring
	 * cursor and the ADSP stay in sync.
	 */
	spin_lock_irqsave(&ac->lock, flags);
	port = &ac->port[dir];
	if (!port->buf || !port->submit || !port->num_periods ||
	    !port->mem_map_handle) {
		rc = -EINVAL;
		goto unlock;
	}

	/*
	 * Submit exactly the period at the ring cursor.  Skipping an
	 * in-flight slot would let the ADSP fill a later period while ALSA
	 * still expects this one; a busy cursor is simply -EAGAIN.
	 */
	index = port->dsp_buf;
	if (index >= port->num_periods || port->submit[index].in_flight) {
		rc = -EAGAIN;
		goto unlock;
	}

	ab = &port->buf[index];
	token = (q6asm_token_gen(port->generation) << Q6ASM_TOKEN_GEN_SHIFT) |
		(index & Q6ASM_TOKEN_INDEX_MASK);
	pkt->hdr.token = token;

	submit = &port->submit[index];
	submit->generation = port->generation;
	submit->stream_id = stream_id;
	submit->phys = ab->phys;
	submit->len = (dir == SNDRV_PCM_STREAM_CAPTURE) ? ab->size : len;
	submit->in_flight = true;

	if (dir == SNDRV_PCM_STREAM_CAPTURE) {
		read->buf_addr_lsw = lower_32_bits(ab->phys);
		read->buf_addr_msw = upper_32_bits(ab->phys);
		read->mem_map_handle = port->mem_map_handle;
		read->buf_size = ab->size;
		read->seq_id = index;
	} else {
		write->buf_addr_lsw = lower_32_bits(ab->phys);
		write->buf_addr_msw = upper_32_bits(ab->phys);
		write->mem_map_handle = port->mem_map_handle;
		write->buf_size = len;
		write->seq_id = index;
		write->timestamp_lsw = lsw_ts;
		write->timestamp_msw = msw_ts;
		write->flags = wflags;
	}

	rc = apr_send_pkt(ac->adev, pkt);
	if (rc == pkt_size) {
		port->dsp_buf = (index + 1) % port->num_periods;
		rc = index;
	} else {
		submit->in_flight = false;
	}
unlock:
	spin_unlock_irqrestore(&ac->lock, flags);

	if (rc < 0 && rc != -EAGAIN)
		dev_err_ratelimited(ac->dev, "post %s failed: %d\n",
				    dir == SNDRV_PCM_STREAM_CAPTURE ?
				    "read" : "write", rc);

	return rc;
}

int q6asm_read(struct audio_client *ac, uint32_t stream_id)
{
	int rc;

	rc = q6asm_fault_take(ac, Q6ASM_FAULT_READ);
	if (rc)
		return rc;

	return q6asm_post_transfer(ac, SNDRV_PCM_STREAM_CAPTURE, stream_id,
				   0, 0, 0, 0);
}
EXPORT_SYMBOL_GPL(q6asm_read);

static int __q6asm_open_read(struct audio_client *ac, uint32_t stream_id,
		uint32_t format, uint16_t bits_per_sample)
{
	struct asm_stream_cmd_open_read_v3 *open;
	struct apr_pkt *pkt;
	int pkt_size = APR_HDR_SIZE + sizeof(*open);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	open = p + APR_HDR_SIZE;

	q6asm_add_hdr(ac, &pkt->hdr,  pkt_size, true, stream_id);
	pkt->hdr.opcode = ASM_STREAM_CMD_OPEN_READ_V3;
	/* Stream prio : High, provide meta info with encoded frames */
	open->src_endpointype = ASM_END_POINT_DEVICE_MATRIX;

	open->preprocopo_id = ASM_STREAM_POSTPROC_TOPO_ID_NONE;
	open->bits_per_sample = bits_per_sample;
	open->mode_flags = 0x0;

	open->mode_flags |= ASM_LEGACY_STREAM_SESSION <<
				ASM_SHIFT_STREAM_PERF_MODE_FLAG_IN_OPEN_READ;

	switch (format) {
	case FORMAT_LINEAR_PCM:
		open->mode_flags |= 0x00;
		open->enc_cfg_id = ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2;
		break;
	default:
		pr_err("Invalid format[%d]\n", format);
	}

	return q6asm_ac_send_cmd_sync(ac, pkt);
}

/**
 * q6asm_open_read() - Open audio client for reading
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @format: audio sample format
 * @bits_per_sample: bits per sample
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_open_read(struct audio_client *ac, uint32_t stream_id,
		    uint32_t format, uint16_t bits_per_sample)
{
	return __q6asm_open_read(ac, stream_id, format, bits_per_sample);
}
EXPORT_SYMBOL_GPL(q6asm_open_read);

/**
 * q6asm_write_async() - non blocking write
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @len: length in bytes
 * @msw_ts: timestamp msw
 * @lsw_ts: timestamp lsw
 * @wflags: flags associated with write
 *
 * Return: the period index that was posted, or a negative error.  The
 * submitted length is kept by q6asm and reported with the completion; it is
 * not encoded in the token, so a 64 KiB period no longer truncates.
 */
int q6asm_write_async(struct audio_client *ac, uint32_t stream_id, uint32_t len,
		      uint32_t msw_ts, uint32_t lsw_ts, uint32_t wflags)
{
	int rc;

	rc = q6asm_fault_take(ac, Q6ASM_FAULT_WRITE);
	if (rc)
		return rc;

	return q6asm_post_transfer(ac, SNDRV_PCM_STREAM_PLAYBACK, stream_id,
				   len, msw_ts, lsw_ts, wflags);
}
EXPORT_SYMBOL_GPL(q6asm_write_async);

static void q6asm_reset_buf_state(struct audio_client *ac)
{
	struct audio_port_data *port;
	int dir;

	guard(spinlock_irqsave)(&ac->lock);
	for (dir = 0; dir < ARRAY_SIZE(ac->port); dir++) {
		port = &ac->port[dir];
		if (!port->submit)
			continue;
		port->generation = q6asm_next_generation();
		memset(port->submit, 0,
		       sizeof(*port->submit) * port->num_periods);
		port->dsp_buf = 0;
		port->next_done = 0;
		atomic_set(&port->hw_ptr, 0);
	}
}

static int __q6asm_cmd(struct audio_client *ac, uint32_t stream_id, int cmd,
		       bool wait)
{
	struct apr_pkt pkt;
	int rc;

	if (cmd == CMD_CLOSE) {
		rc = q6asm_fault_take(ac, Q6ASM_FAULT_CLOSE);
		if (rc)
			return rc;
	}
	if (cmd == CMD_PAUSE) {
		rc = q6asm_fault_take(ac, Q6ASM_FAULT_PAUSE);
		if (rc)
			return rc;
	}

	q6asm_add_hdr(ac, &pkt.hdr, APR_HDR_SIZE, true, stream_id);

	switch (cmd) {
	case CMD_PAUSE:
		pkt.hdr.opcode = ASM_SESSION_CMD_PAUSE;
		break;
	case CMD_SUSPEND:
		pkt.hdr.opcode = ASM_SESSION_CMD_SUSPEND;
		break;
	case CMD_FLUSH:
		pkt.hdr.opcode = ASM_STREAM_CMD_FLUSH;
		break;
	case CMD_OUT_FLUSH:
		pkt.hdr.opcode = ASM_STREAM_CMD_FLUSH_READBUFS;
		break;
	case CMD_EOS:
		pkt.hdr.opcode = ASM_DATA_CMD_EOS;
		break;
	case CMD_CLOSE:
		pkt.hdr.opcode = ASM_STREAM_CMD_CLOSE;
		break;
	default:
		return -EINVAL;
	}

	if (!wait) {
		/*
		 * apr_send_pkt() reports the packet size on success; the
		 * callers use zero for "accepted" and negative for errors.
		 * Reserve the response before sending, or an immediate basic
		 * result would find no owner.
		 */
		rc = q6asm_cmd_rsp_async_begin(ac, pkt.hdr.opcode);
		if (rc < 0)
			return rc;
		rc = apr_send_pkt(ac->adev, &pkt);
		if (rc < 0) {
			q6asm_cmd_rsp_async_end(ac, pkt.hdr.opcode);
			return rc;
		}
		if (rc != pkt.hdr.pkt_size) {
			q6asm_cmd_rsp_async_end(ac, pkt.hdr.opcode);
			return -EIO;
		}
		return 0;
	}

	rc = q6asm_ac_send_cmd_sync(ac, &pkt);
	if (rc < 0)
		return rc;

	if (cmd == CMD_CLOSE)
		q6asm_cmd_rsp_forget_stream(ac);

	if (cmd == CMD_FLUSH)
		q6asm_reset_buf_state(ac);

	return 0;
}

/**
 * q6asm_cmd() - run cmd on audio client
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @cmd: command to run on audio client.
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_cmd(struct audio_client *ac, uint32_t stream_id, int cmd)
{
	return __q6asm_cmd(ac, stream_id, cmd, true);
}
EXPORT_SYMBOL_GPL(q6asm_cmd);

/**
 * q6asm_cmd_nowait() - non blocking, run cmd on audio client
 *
 * @ac: audio client pointer
 * @stream_id: stream id
 * @cmd: command to run on audio client.
 *
 * Return: Will be an negative value on error or zero on success
 */
int q6asm_cmd_nowait(struct audio_client *ac, uint32_t stream_id, int cmd)
{
	return __q6asm_cmd(ac, stream_id, cmd, false);
}
EXPORT_SYMBOL_GPL(q6asm_cmd_nowait);

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
/*
 * Replay a completion for the first period of an active client.  With
 * @fresh set, the token is valid for the current mapping, so the first
 * delivery is accepted and an immediate second one must be rejected; with
 * it clear the token names another generation and must be dropped.  Either
 * way hw_ptr may only move once.
 */
static void q6asm_inject_completion(struct q6asm *a, bool fresh)
{
	struct audio_client *ac = NULL;
	unsigned long flags;
	int i, dir;

	spin_lock_irqsave(&a->slock, flags);
	for (i = 1; i <= MAX_SESSIONS; i++) {
		if (a->session[i] &&
		    kref_get_unless_zero(&a->session[i]->refcount)) {
			ac = a->session[i];
			break;
		}
	}
	spin_unlock_irqrestore(&a->slock, flags);
	if (!ac)
		return;

	for (dir = 0; dir < ARRAY_SIZE(ac->port); dir++) {
		struct audio_port_data *port = &ac->port[dir];
		unsigned long flags;
		phys_addr_t phys;
		u32 generation;

		if (!port->buf || !port->submit || !port->num_periods)
			continue;
		spin_lock_irqsave(&ac->lock, flags);
		generation = q6asm_token_gen(port->generation);
		if (!fresh) {
			/* Any encoded value except the current one is stale. */
			generation = (generation % Q6ASM_TOKEN_GEN_MAX) + 1;
		}
		phys = port->buf[0].phys;
		spin_unlock_irqrestore(&ac->lock, flags);

		q6asm_complete_transfer(ac, dir, 0, generation, 0, phys, true,
					false, 0, 0, 0, 0);
		if (fresh) {
			/* The same completion again must not count twice. */
			q6asm_complete_transfer(ac, dir, 0, generation, 0, phys,
						true, false, 0, 0, 0, 0);
		}
	}
	kref_put(&ac->refcount, q6asm_audio_client_release);
}

static ssize_t held_dma_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct q6asm_held_buffer *held;
	unsigned int buffers = 0;
	size_t bytes = 0;

	mutex_lock(&q6asm_global.lock);
	list_for_each_entry(held, &q6asm_global.held_buffers, node) {
		buffers++;
		bytes += held->dma.bytes;
	}
	mutex_unlock(&q6asm_global.lock);

	return sysfs_emit(buf, "held %u bytes %zu leaked %u bytes %zu offline %u\n",
			  buffers, bytes, q6asm_global.leaked_buffers,
			  q6asm_global.leaked_bytes, q6asm_global.adsp_offline);
}
static DEVICE_ATTR_RO(held_dma);

/*
 * One line per active session.  The device tests use the generation to prove
 * that a single open/remap loop crossed the 8-bit token boundary, and the
 * counters to prove which correlation branch handled an event.
 */
static ssize_t session_gen_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct q6asm *q6asm = dev_get_drvdata(dev);
	ssize_t len = 0;
	int i;

	for (i = 1; i <= MAX_SESSIONS; i++) {
		struct audio_client *ac;
		unsigned long flags;
		u32 play, capture;

		spin_lock_irqsave(&q6asm->slock, flags);
		ac = q6asm->session[i];
		if (ac && !kref_get_unless_zero(&ac->refcount))
			ac = NULL;
		spin_unlock_irqrestore(&q6asm->slock, flags);
		if (!ac)
			continue;

		spin_lock_irqsave(&ac->lock, flags);
		play = ac->port[SNDRV_PCM_STREAM_PLAYBACK].generation;
		capture = ac->port[SNDRV_PCM_STREAM_CAPTURE].generation;
		spin_unlock_irqrestore(&ac->lock, flags);

		len += sysfs_emit_at(buf, len,
				     "session %d play %u capture %u accepted %d dropped %d late %d unowned %d\n",
				     i, play, capture,
				     atomic_read(&ac->stats.accepted),
				     atomic_read(&ac->stats.dropped),
				     atomic_read(&ac->stats.late),
				     atomic_read(&ac->stats.unowned));
		kref_put(&ac->refcount, q6asm_audio_client_release);
	}

	if (!len)
		len = sysfs_emit(buf, "none\n");

	return len;
}
static DEVICE_ATTR_RO(session_gen);

static ssize_t inject_fault_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct q6asm *q6asm = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int fault;
	bool timeout;

	spin_lock_irqsave(&q6asm->slock, flags);
	fault = q6asm->fault;
	timeout = q6asm->fault_timeout;
	spin_unlock_irqrestore(&q6asm->slock, flags);

	if (fault == Q6ASM_FAULT_NONE)
		return sysfs_emit(buf, "none\n");

	return sysfs_emit(buf, "%s:%s\n", q6asm_fault_names[fault],
			  timeout ? "timeout" : "reject");
}

/*
 * Arm a one-shot failure for one teardown command.  This exists so the
 * quarantine, held-buffer and state-rollback paths can be exercised on real
 * hardware:
 *
 *   echo map:timeout    > .../inject_fault  # MAP fails, session quarantined
 *   echo close:timeout  > .../inject_fault  # CMD_CLOSE fails
 *   echo unmap:reject   > .../inject_fault  # UNMAP fails after CLOSE
 *   echo pause:reject   > .../inject_fault  # CMD_PAUSE is not sent
 *   echo pausereject:reject > .../inject_fault # DSP rejects the next PAUSE
 *   echo runreject:reject   > .../inject_fault # DSP rejects the next RUN
 *   echo duplicate      > .../inject_fault  # replay a completion twice
 *   echo spurious       > .../inject_fault  # replay an old completion
 *   echo clear          > .../inject_fault  # disarm
 *
 * "timeout" returns -ETIMEDOUT, "reject" returns -EINVAL (the value the
 * DSP uses for a refused command).  Every write needs root and affects only
 * the next matching operation.
 */
static ssize_t inject_fault_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct q6asm *q6asm = dev_get_drvdata(dev);
	char op[16] = "";
	char kind[16] = "";
	unsigned long flags;
	unsigned int fault = Q6ASM_FAULT_NONE;
	unsigned int i;
	const char *colon;
	size_t len;
	bool timeout = false;

	if (sysfs_streq(buf, "clear") || sysfs_streq(buf, "none"))
		goto store;

	if (sysfs_streq(buf, "spurious")) {
		q6asm_inject_completion(q6asm, false);
		return count;
	}
	if (sysfs_streq(buf, "duplicate")) {
		q6asm_inject_completion(q6asm, true);
		return count;
	}

	colon = strchr(buf, ':');
	if (!colon)
		return -EINVAL;
	len = colon - buf;
	if (!len || len >= sizeof(op))
		return -EINVAL;
	memcpy(op, buf, len);
	op[len] = '\0';

	colon++;
	len = strlen(colon);
	while (len && (colon[len - 1] == '\n' || colon[len - 1] == '\r'))
		len--;
	if (!len || len >= sizeof(kind))
		return -EINVAL;
	memcpy(kind, colon, len);
	kind[len] = '\0';

	for (i = Q6ASM_FAULT_MAP; i <= Q6ASM_FAULT_RUN_REJECT; i++)
		if (sysfs_streq(op, q6asm_fault_names[i]))
			fault = i;
	if (fault == Q6ASM_FAULT_NONE)
		return -EINVAL;

	if (sysfs_streq(kind, "timeout"))
		timeout = true;
	else if (!sysfs_streq(kind, "reject"))
		return -EINVAL;

store:
	spin_lock_irqsave(&q6asm->slock, flags);
	q6asm->fault = fault;
	q6asm->fault_timeout = timeout;
	spin_unlock_irqrestore(&q6asm->slock, flags);

	return count;
}
static DEVICE_ATTR_RW(inject_fault);

static void q6asm_remove_sysfs(void *data)
{
	device_remove_file(data, &dev_attr_session_gen);
	device_remove_file(data, &dev_attr_inject_fault);
	device_remove_file(data, &dev_attr_held_dma);
}
#endif /* CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT */

static int q6asm_probe(struct apr_device *adev)
{
	struct device *dev = &adev->dev;
	struct q6asm *q6asm;

	q6asm = devm_kzalloc(dev, sizeof(*q6asm), GFP_KERNEL);
	if (!q6asm)
		return -ENOMEM;

	q6core_get_svc_api_info(adev->svc_id, &q6asm->ainfo);

	q6asm->dev = dev;
	q6asm->adev = adev;
	spin_lock_init(&q6asm->slock);
	dev_set_drvdata(dev, q6asm);

	/*
	 * The stop notifier must outlive this device: the ADSP stop event is
	 * what releases held buffers and the session quarantine.  Register it
	 * here, after qcom_common has been loaded by the remoteproc stack.
	 */
	q6asm_reclaim_register();

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	int ret;

	ret = device_create_file(dev, &dev_attr_inject_fault);
	if (ret)
		return ret;
	ret = device_create_file(dev, &dev_attr_held_dma);
	if (ret)
		goto remove_inject;
	ret = device_create_file(dev, &dev_attr_session_gen);
	if (ret)
		goto remove_held;
	ret = devm_add_action_or_reset(dev, q6asm_remove_sysfs, dev);
	if (ret)
		return ret;
#endif

	return devm_of_platform_populate(dev);

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
remove_held:
	device_remove_file(dev, &dev_attr_held_dma);
remove_inject:
	device_remove_file(dev, &dev_attr_inject_fault);
	return ret;
#endif
}

#ifdef CONFIG_OF
static const struct of_device_id q6asm_device_id[]  = {
	{ .compatible = "qcom,q6asm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6asm_device_id);
#endif

static struct apr_driver qcom_q6asm_driver = {
	.probe = q6asm_probe,
	.callback = q6asm_srvc_callback,
	.driver = {
		.name = "qcom-q6asm",
		.of_match_table = of_match_ptr(q6asm_device_id),
	},
};

static int __init q6asm_driver_init(void)
{
	return apr_driver_register(&qcom_q6asm_driver);
}

static void __exit q6asm_driver_exit(void)
{
	apr_driver_unregister(&qcom_q6asm_driver);
	q6asm_reclaim_unregister();
}

module_init(q6asm_driver_init);
module_exit(q6asm_driver_exit);
MODULE_DESCRIPTION("Q6 Audio Stream Manager driver");
MODULE_LICENSE("GPL v2");
