// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/soc/qcom/apr.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <sound/asound.h>
#include "q6adm.h"
#include "q6afe.h"
#include "q6core.h"
#include "q6dsp-common.h"
#include "q6dsp-errno.h"

#define ADM_CMD_DEVICE_OPEN_V5		0x00010326
#define ADM_CMDRSP_DEVICE_OPEN_V5	0x00010329
#define ADM_CMD_DEVICE_CLOSE_V5		0x00010327
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5	0x00010325

#define TIMEOUT_MS 1000
#define RESET_COPP_ID 99
#define INVALID_COPP_ID 0xFF
/* Definition for a legacy device session. */
#define ADM_LEGACY_DEVICE_SESSION	0
#define ADM_MATRIX_ID_AUDIO_RX		0
#define ADM_MATRIX_ID_AUDIO_TX		1

struct q6copp {
	int afe_port;
	int copp_idx;
	int id;
	int topology;
	int mode;
	int rate;
	int bit_width;
	int channels;
	int app_type;
	int acdb_id;
	/* The CMDRSP delivered the COPP id; the basic ACK alone does not. */
	bool id_valid;
	/* The DSP accepted DEVICE_OPEN but never delivered a COPP id. */
	bool acked;
	/* Generation carried in the token; rejects older COPP's responses. */
	u8 gen;
	/* Opcode of the DEVICE_OPEN waiting on copp->wait, or 0. */
	u32 pending_opcode;

	struct aprv2_ibasic_rsp_result_t result;
	struct kref refcount;
	wait_queue_head_t wait;
	struct list_head node;
	struct q6adm *adm;
};

/*
 * A DEVICE_CLOSE outlives the COPP it closes: the object is already off the
 * list and about to be freed when the command is queued.  Copy everything
 * the command and its response need, and confirm it from a workqueue worker
 * so the single-threaded APR RX worker never waits for a response it has to
 * deliver itself.
 */
struct q6adm_close_req {
	struct work_struct work;
	struct q6adm *adm;
	struct list_head node;
	wait_queue_head_t wait;
	int afe_port;
	int copp_idx;
	int copp_id;
	u8 gen;
	bool acked;
	u32 status;
};

struct q6adm {
	struct apr_device *apr;
	struct device *dev;
	struct q6core_svc_api_info ainfo;
	unsigned long copp_bitmap[AFE_MAX_PORTS];
	struct list_head copps_list;
	/* COPP close commands waiting for their basic result. */
	struct list_head closing;
	spinlock_t copps_list_lock;
	struct workqueue_struct *close_wq;
	unsigned int closes_unconfirmed;
#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	atomic_t fault;
#endif
	struct aprv2_ibasic_rsp_result_t result;
	/*
	 * Protects result/map_pending: the RX worker must not complete a
	 * command that already timed out, or one that was not sent yet.
	 */
	spinlock_t result_lock;
	bool map_pending;
	u32 map_gen;
	u8 copp_gen;
	/* J716F firmware is verified to echo token bits 8-15. */
	bool token_gen;
	struct mutex lock;
	wait_queue_head_t matrix_map_wait;
};

struct q6adm_cmd_device_open_v5 {
	u16 flags;
	u16 mode_of_operation;
	u16 endpoint_id_1;
	u16 endpoint_id_2;
	u32 topology_id;
	u16 dev_num_channel;
	u16 bit_width;
	u32 sample_rate;
	u8 dev_channel_mapping[8];
} __packed;

struct q6adm_cmd_matrix_map_routings_v5 {
	u32 matrix_id;
	u32 num_sessions;
} __packed;

struct q6adm_session_map_node_v5 {
	u16 session_id;
	u16 num_copps;
} __packed;

static struct q6copp *q6adm_find_copp(struct q6adm *adm, int port_idx,
				  int copp_idx)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_for_each_entry(c, &adm->copps_list, node) {
		if ((port_idx == c->afe_port) && (copp_idx == c->copp_idx)) {
			if (kref_get_unless_zero(&c->refcount))
				ret = c;
			break;
		}
	}

	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;

}

static int q6adm_apr_send_copp_pkt(struct q6adm *adm, struct q6copp *copp,
				   struct apr_pkt *pkt)
{
	struct device *dev = adm->dev;
	unsigned long flags;
	int ret;

	mutex_lock(&adm->lock);
	spin_lock_irqsave(&adm->copps_list_lock, flags);
	copp->result.opcode = 0;
	copp->result.status = 0;
	copp->pending_opcode = pkt->hdr.opcode;
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "Failed to send APR packet\n");
		ret = -EINVAL;
		goto err;
	}

	/*
	 * DEVICE_OPEN returns a basic ACK and a CMDRSP that carries the COPP
	 * id.  The ACK only says the command was accepted; returning on it
	 * would let the caller map a route with an id the DSP never assigned.
	 * Wait for the id, or for a rejection.
	 */
	ret = wait_event_timeout(copp->wait,
				 READ_ONCE(copp->id_valid) ||
				 READ_ONCE(copp->result.status) > 0,
				 msecs_to_jiffies(TIMEOUT_MS));
	if (!ret) {
		/*
		 * Distinguish "the DSP never sent the CMDRSP" from "the DSP
		 * answered but the response could not be matched": without
		 * this, a firmware that only sends the basic ACK would make
		 * every open time out with the same generic message.
		 */
		if (READ_ONCE(copp->result.opcode) == pkt->hdr.opcode)
			dev_err(dev,
				"DEVICE_OPEN acked without a COPP id (CMDRSP missing or unmatched)\n");
		else
			dev_err(dev, "ADM copp cmd timedout\n");
		ret = -ETIMEDOUT;
	} else if (READ_ONCE(copp->result.status) > 0) {
		dev_err(dev, "DSP returned error[%u]\n",
			READ_ONCE(copp->result.status));
		ret = -EINVAL;
	} else {
		ret = 0;
	}

err:
	spin_lock_irqsave(&adm->copps_list_lock, flags);
	copp->pending_opcode = 0;
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	mutex_unlock(&adm->lock);
	return ret;
}

#define Q6ADM_CLOSE_RETRIES	3
#define Q6ADM_CLOSE_RETRY_MS	20

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
/*
 * Debug builds only: exercises the three ways a close can end without a
 * confirmed release.  CLOSE_FAIL persists until it is cleared so all bounded
 * retries are consumed; the other two are one-shot.
 */
enum q6adm_fault {
	Q6ADM_FAULT_NONE = 0,
	Q6ADM_FAULT_CLOSE_FAIL,
	Q6ADM_FAULT_CLOSE_REJECT,
	Q6ADM_FAULT_CLOSE_TIMEOUT,
};
#endif

static int q6adm_send_close(struct q6adm *adm, const struct q6adm_close_req *req)
{
	struct apr_pkt close = { };

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	if (atomic_read(&adm->fault) == Q6ADM_FAULT_CLOSE_FAIL)
		return -EIO;
#endif
	close.hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					    APR_HDR_LEN(APR_HDR_SIZE),
					    APR_PKT_VER);
	close.hdr.pkt_size = sizeof(close);
	close.hdr.src_port = req->afe_port;
	close.hdr.dest_port = req->copp_id;
	close.hdr.token = req->afe_port << 16 | req->copp_idx;
	if (adm->token_gen)
		close.hdr.token |= req->gen << 8;
	close.hdr.opcode = ADM_CMD_DEVICE_CLOSE_V5;

	return apr_send_pkt(adm->apr, &close);
}

/*
 * Runs outside the APR RX worker.  A send that failed never reached the DSP,
 * so retrying it is safe and bounded.  A send that succeeded is only ever
 * sent once: the first copy may already have closed the COPP, and a second
 * one would either be an error or, worse, close a recycled COPP id.
 */
static void q6adm_close_work(struct work_struct *work)
{
	struct q6adm_close_req *req = container_of(work,
						   struct q6adm_close_req,
						   work);
	struct q6adm *adm = req->adm;
	unsigned long flags;
	bool acked;
	u32 status;
	int attempt, ret = -EIO;

	for (attempt = 0; attempt < Q6ADM_CLOSE_RETRIES; attempt++) {
		ret = q6adm_send_close(adm, req);
		if (ret >= 0)
			break;
		msleep(Q6ADM_CLOSE_RETRY_MS);
	}

	if (ret < 0) {
		dev_warn_ratelimited(adm->dev,
				     "COPP %d:%d close not sent after %d attempts: %d\n",
				     req->afe_port, req->copp_idx,
				     Q6ADM_CLOSE_RETRIES, ret);
		goto out;
	}

	if (!adm->token_gen)
		goto out;

	/* The packet is on the wire; wait for the DSP's answer, never resend. */
	wait_event_timeout(req->wait, READ_ONCE(req->acked),
			   msecs_to_jiffies(TIMEOUT_MS));

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	acked = req->acked;
	status = req->status;
	if (!acked)
		adm->closes_unconfirmed++;
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	if (!acked) {
		dev_warn_ratelimited(adm->dev,
				     "COPP %d:%d close unconfirmed (%u so far)\n",
				     req->afe_port, req->copp_idx,
				     READ_ONCE(adm->closes_unconfirmed));
	} else if (status) {
		dev_err_ratelimited(adm->dev,
				    "COPP %d:%d close rejected: %u\n",
				    req->afe_port, req->copp_idx, status);
	}

out:
	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_del(&req->node);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	kfree(req);
}

/* Caller holds no lock; the request copies everything it needs. */
static void q6adm_schedule_close(struct q6adm *adm, struct q6copp *copp)
{
	struct q6adm_close_req *req;
	unsigned long flags;

	req = kzalloc_obj(*req, GFP_ATOMIC);
	if (!req) {
		dev_warn_ratelimited(adm->dev,
				     "COPP %d:%d left open: no memory to close it\n",
				     copp->afe_port, copp->copp_idx);
		return;
	}

	INIT_WORK(&req->work, q6adm_close_work);
	init_waitqueue_head(&req->wait);
	req->adm = adm;
	req->afe_port = copp->afe_port;
	req->copp_idx = copp->copp_idx;
	req->copp_id = copp->id;
	req->gen = copp->gen;

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_add_tail(&req->node, &adm->closing);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	if (!queue_work(adm->close_wq, &req->work)) {
		spin_lock_irqsave(&adm->copps_list_lock, flags);
		list_del(&req->node);
		spin_unlock_irqrestore(&adm->copps_list_lock, flags);
		kfree(req);
	}
}

/* Caller holds copps_list_lock. */
static void q6adm_close_ack_locked(struct q6adm *adm, int port_idx,
				   int copp_idx, u8 gen, u32 status)
{
	struct q6adm_close_req *req;

	if (!adm->token_gen)
		return;

	list_for_each_entry(req, &adm->closing, node) {
		if (req->afe_port == port_idx && req->copp_idx == copp_idx &&
		    req->gen == gen) {
#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
			if (atomic_cmpxchg(&adm->fault,
					   Q6ADM_FAULT_CLOSE_TIMEOUT,
					   Q6ADM_FAULT_NONE) ==
			    Q6ADM_FAULT_CLOSE_TIMEOUT)
				return;	/* simulate a lost answer */
			if (atomic_cmpxchg(&adm->fault,
					   Q6ADM_FAULT_CLOSE_REJECT,
					   Q6ADM_FAULT_NONE) ==
			    Q6ADM_FAULT_CLOSE_REJECT)
				status = ADSP_EFAILED;
#endif
			req->status = status;
			req->acked = true;
			wake_up(&req->wait);
			break;
		}
	}
}

static void q6adm_free_copp(struct kref *ref)
{
	struct q6copp *c = container_of(ref, struct q6copp, refcount);
	struct q6adm *adm = c->adm;
	unsigned long flags;

	/*
	 * Delist before any remote cleanup.  A lookup that finds a COPP with
	 * a zero refcount must not be able to take a reference to it.
	 */
	spin_lock_irqsave(&adm->copps_list_lock, flags);
	list_del(&c->node);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	if (c->id_valid) {
		q6adm_schedule_close(adm, c);
	} else if (c->acked) {
		/*
		 * The DSP accepted DEVICE_OPEN but its CMDRSP never reached
		 * us.  There is no id to close with, so the COPP stays until
		 * the ADSP restarts; this is a protocol limitation, not a
		 * host resource leak.
		 */
		dev_warn_ratelimited(adm->dev,
				     "COPP %d left open on the DSP: acked without an id\n",
				     c->copp_idx);
	}

	/*
	 * The index may be reused once the COPP is off the list.  A late
	 * CLOSE answer is matched by the index-and-generation token of the
	 * close request, so a new COPP with another generation cannot
	 * consume it (the byte can alias after 256 allocations, which is
	 * bounded by the close timeout and only misaccounts a log line).
	 */
	spin_lock_irqsave(&adm->copps_list_lock, flags);
	clear_bit(c->copp_idx, &adm->copp_bitmap[c->afe_port]);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	kfree(c);
}

/* Caller holds copps_list_lock; no reference is taken. */
static struct q6copp *q6adm_peek_copp(struct q6adm *adm, int port_idx,
				      int copp_idx, u8 gen)
{
	struct q6copp *c;

	list_for_each_entry(c, &adm->copps_list, node)
		if (c->afe_port == port_idx && c->copp_idx == copp_idx &&
		    (!adm->token_gen || c->gen == gen))
			return c;

	return NULL;
}

static int q6adm_callback(struct apr_device *adev, const struct apr_resp_pkt *data)
{
	const struct aprv2_ibasic_rsp_result_t *result = data->payload;
	int port_idx, copp_idx;
	const struct apr_hdr *hdr = &data->hdr;
	struct q6copp *copp;
	struct q6adm *adm = dev_get_drvdata(&adev->dev);
	u8 gen;

	if (data->payload_size < sizeof(u32))
		return 0;

	copp_idx = (hdr->token) & 0XFF;
	port_idx = ((hdr->token) >> 16) & 0xFF;
	gen = ((hdr->token) >> 8) & 0xFF;
	if (port_idx < 0 || port_idx >= AFE_MAX_PORTS) {
		dev_err(&adev->dev, "Invalid port idx %d token %d\n",
		       port_idx, hdr->token);
		return 0;
	}
	if (copp_idx < 0 || copp_idx >= MAX_COPPS_PER_PORT) {
		dev_err(&adev->dev, "Invalid copp idx %d token %d\n",
			copp_idx, hdr->token);
		return 0;
	}

	switch (hdr->opcode) {
	case APR_BASIC_RSP_RESULT: {
		unsigned long flags;

		if (data->payload_size < sizeof(*result))
			return -EPROTO;
		if (result->status != 0) {
			dev_err(&adev->dev, "cmd = 0x%x return error = 0x%x\n",
				result->opcode, result->status);
		}
		switch (result->opcode) {
		case ADM_CMD_DEVICE_OPEN_V5:
		case ADM_CMD_DEVICE_CLOSE_V5:
			/*
			 * The opener or the kref destructor is sleeping on
			 * copp->wait; it keeps the object alive.  Publish the
			 * result and wake while the list lock is held so a
			 * timed-out waiter cannot free it in between.
			 */
			spin_lock_irqsave(&adm->copps_list_lock, flags);
			copp = q6adm_peek_copp(adm, port_idx, copp_idx, gen);
			if (copp && copp->pending_opcode == result->opcode) {
				copp->result = *result;
				if (result->opcode == ADM_CMD_DEVICE_OPEN_V5 &&
				    !result->status)
					copp->acked = true;
				wake_up(&copp->wait);
			} else if (result->opcode == ADM_CMD_DEVICE_CLOSE_V5) {
				/*
				 * The COPP itself is already off the list; the
				 * close request outlives it.
				 */
				q6adm_close_ack_locked(adm, port_idx, copp_idx,
						       gen, result->status);
			}
			spin_unlock_irqrestore(&adm->copps_list_lock, flags);
			break;
		case ADM_CMD_MATRIX_MAP_ROUTINGS_V5: {
			unsigned long flags;

			/*
			 * Only a response that arrived after the current map
			 * was armed may complete it; a late response for a
			 * timed-out map is dropped.
			 */
			spin_lock_irqsave(&adm->result_lock, flags);
			if (adm->map_pending &&
			    (!adm->token_gen ||
			     hdr->token == (adm->map_gen << 8))) {
				adm->result = *result;
				adm->map_pending = false;
				wake_up(&adm->matrix_map_wait);
			}
			spin_unlock_irqrestore(&adm->result_lock, flags);
			break;
		}

		default:
			dev_err(&adev->dev, "Unknown Cmd: 0x%x\n",
				result->opcode);
			break;
		}
		return 0;
	}
	case ADM_CMDRSP_DEVICE_OPEN_V5: {
		struct adm_cmd_rsp_device_open_v5 {
			u32 status;
			u16 copp_id;
			u16 reserved;
		} __packed *open = data->payload;
		unsigned long flags;

		if (data->payload_size < sizeof(*open))
			return -EPROTO;

		spin_lock_irqsave(&adm->copps_list_lock, flags);
		copp = q6adm_peek_copp(adm, port_idx, copp_idx, gen);
		if (!copp || copp->pending_opcode != ADM_CMD_DEVICE_OPEN_V5) {
			dev_warn_ratelimited(&adev->dev,
					     "unexpected DEVICE_OPEN rsp for COPP %d:%d\n",
					     port_idx, copp_idx);
			spin_unlock_irqrestore(&adm->copps_list_lock, flags);
			return 0;
		}
		if (open->status || open->copp_id == INVALID_COPP_ID) {
			dev_err(&adev->dev, "Invalid copp open rsp: status %u id %u\n",
				open->status, open->copp_id);
			copp->result.status = open->status ?
				open->status : ADSP_EBADPARAM;
		} else {
			copp->result.opcode = hdr->opcode;
			copp->id = open->copp_id;
			copp->id_valid = true;
		}
		wake_up(&copp->wait);
		spin_unlock_irqrestore(&adm->copps_list_lock, flags);
	}
	break;
	default:
		dev_err(&adev->dev, "Unknown cmd:0x%x\n",
		       hdr->opcode);
		break;
	}

	return 0;
}

static struct q6copp *q6adm_alloc_copp(struct q6adm *adm, int port_idx)
{
	struct q6copp *c;
	int idx;

	idx = find_first_zero_bit(&adm->copp_bitmap[port_idx],
				  MAX_COPPS_PER_PORT);

	if (idx >= MAX_COPPS_PER_PORT)
		return ERR_PTR(-EBUSY);

	c = kzalloc_obj(*c, GFP_ATOMIC);
	if (!c)
		return ERR_PTR(-ENOMEM);

	set_bit(idx, &adm->copp_bitmap[port_idx]);
	c->copp_idx = idx;
	c->afe_port = port_idx;
	c->gen = adm->copp_gen++;
	c->adm = adm;

	init_waitqueue_head(&c->wait);

	return c;
}

static struct q6copp *q6adm_find_matching_copp(struct q6adm *adm,
					       int port_id, int topology,
					       int mode, int rate,
					       int channel_mode, int bit_width,
					       int app_type)
{
	struct q6copp *c;
	struct q6copp *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&adm->copps_list_lock, flags);

	list_for_each_entry(c, &adm->copps_list, node) {
		if (!c->id_valid)
			continue;
		if ((port_id == c->afe_port) && (topology == c->topology) &&
		    (mode == c->mode) && (rate == c->rate) &&
		    (channel_mode == c->channels) &&
		    (bit_width == c->bit_width) && (app_type == c->app_type)) {
			if (kref_get_unless_zero(&c->refcount))
				ret = c;
			break;
		}
	}
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	return ret;
}

static int q6adm_device_open(struct q6adm *adm, struct q6copp *copp,
			     int port_id, int path, int topology,
			     int channel_mode, int bit_width, int rate)
{
	struct q6adm_cmd_device_open_v5 *open;
	int afe_port = q6afe_get_port_id(port_id);
	struct apr_pkt *pkt;
	int ret, pkt_size = APR_HDR_SIZE + sizeof(*open);

	void *p __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	pkt = p;
	open = p + APR_HDR_SIZE;
	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.src_port = afe_port;
	pkt->hdr.dest_port = afe_port;
	pkt->hdr.token = port_id << 16 | copp->copp_idx;
	if (adm->token_gen)
		pkt->hdr.token |= copp->gen << 8;
	pkt->hdr.opcode = ADM_CMD_DEVICE_OPEN_V5;
	open->flags = ADM_LEGACY_DEVICE_SESSION;
	open->mode_of_operation = path;
	open->endpoint_id_1 = afe_port;
	open->topology_id = topology;
	open->dev_num_channel = channel_mode & 0x00FF;
	open->bit_width = bit_width;
	open->sample_rate = rate;

	ret = q6dsp_map_channels(&open->dev_channel_mapping[0],
				 channel_mode);
	if (ret)
		return ret;

	return q6adm_apr_send_copp_pkt(adm, copp, pkt);
}

/**
 * q6adm_open() - open adm and grab a free copp
 *
 * @dev: Pointer to adm child device.
 * @port_id: port id
 * @path: playback or capture path.
 * @rate: rate at which copp is required.
 * @channel_mode: channel mode
 * @topology: adm topology id
 * @perf_mode: performace mode.
 * @bit_width: audio sample bit width
 * @app_type: Application type.
 * @acdb_id: ACDB id
 *
 * Return: Will be an negative on error or a valid copp pointer on success.
 */
struct q6copp *q6adm_open(struct device *dev, int port_id, int path, int rate,
	       int channel_mode, int topology, int perf_mode,
	       uint16_t bit_width, int app_type, int acdb_id)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6copp *copp;
	unsigned long flags;
	int ret = 0;

	if (port_id < 0) {
		dev_err(dev, "Invalid port_id %d\n", port_id);
		return ERR_PTR(-EINVAL);
	}

	copp = q6adm_find_matching_copp(adm, port_id, topology, perf_mode,
				      rate, channel_mode, bit_width, app_type);
	if (copp) {
		dev_err(dev, "Found Matching Copp 0x%x\n", copp->copp_idx);
		return copp;
	}

	spin_lock_irqsave(&adm->copps_list_lock, flags);
	copp = q6adm_alloc_copp(adm, port_id);
	if (IS_ERR(copp)) {
		spin_unlock_irqrestore(&adm->copps_list_lock, flags);
		return ERR_CAST(copp);
	}

	/*
	 * Publish a fully initialised object: a concurrent lookup must never
	 * see a zero refcount or half-written match keys.
	 */
	kref_init(&copp->refcount);
	copp->topology = topology;
	copp->mode = perf_mode;
	copp->rate = rate;
	copp->channels = channel_mode;
	copp->bit_width = bit_width;
	copp->app_type = app_type;
	/*
	 * Publish the COPP before the DEVICE_OPEN round trip: the APR RX
	 * worker must be able to find it by token when the response arrives.
	 */
	list_add_tail(&copp->node, &adm->copps_list);
	spin_unlock_irqrestore(&adm->copps_list_lock, flags);

	ret = q6adm_device_open(adm, copp, port_id, path, topology,
				channel_mode, bit_width, rate);
	if (ret < 0) {
		kref_put(&copp->refcount, q6adm_free_copp);
		return ERR_PTR(ret);
	}

	return copp;
}
EXPORT_SYMBOL_GPL(q6adm_open);

/**
 * q6adm_get_copp_id() - get copp index
 *
 * @copp: Pointer to valid copp
 *
 * Return: Will be an negative on error or a valid copp index on success.
 **/
int q6adm_get_copp_id(struct q6copp *copp)
{
	if (!copp)
		return -EINVAL;

	return copp->copp_idx;
}
EXPORT_SYMBOL_GPL(q6adm_get_copp_id);

/**
 * q6adm_matrix_map() - Map asm streams and afe ports using payload
 *
 * @dev: Pointer to adm child device.
 * @path: playback or capture path.
 * @payload_map: map between session id and afe ports.
 * @perf_mode: Performace mode.
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_matrix_map(struct device *dev, int path,
		     struct route_payload payload_map, int perf_mode)
{
	struct q6adm *adm = dev_get_drvdata(dev->parent);
	struct q6adm_cmd_matrix_map_routings_v5 *route;
	struct q6adm_session_map_node_v5 *node;
	struct apr_pkt *pkt;
	uint16_t *copps_list;
	unsigned long flags;
	u32 status;
	int ret, i, copp_idx;
	/* Assumes port_ids have already been validated during adm_open */
	struct q6copp *copp;
	int pkt_size = (APR_HDR_SIZE + sizeof(*route) +  sizeof(*node) +
		    (sizeof(uint32_t) * payload_map.num_copps));

	void *matrix_map __free(kfree) = kzalloc(pkt_size, GFP_KERNEL);
	if (!matrix_map)
		return -ENOMEM;

	pkt = matrix_map;
	route = matrix_map + APR_HDR_SIZE;
	node = matrix_map + APR_HDR_SIZE + sizeof(*route);
	copps_list = matrix_map + APR_HDR_SIZE + sizeof(*route) + sizeof(*node);

	pkt->hdr.hdr_field = APR_HDR_FIELD(APR_MSG_TYPE_SEQ_CMD,
					   APR_HDR_LEN(APR_HDR_SIZE),
					   APR_PKT_VER);
	pkt->hdr.pkt_size = pkt_size;
	pkt->hdr.token = 0;
	pkt->hdr.opcode = ADM_CMD_MATRIX_MAP_ROUTINGS_V5;
	route->num_sessions = 1;

	switch (path) {
	case ADM_PATH_PLAYBACK:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_RX;
		break;
	case ADM_PATH_LIVE_REC:
		route->matrix_id = ADM_MATRIX_ID_AUDIO_TX;
		break;
	default:
		dev_err(dev, "Wrong path set[%d]\n", path);
		break;
	}

	node->session_id = payload_map.session_id;
	node->num_copps = payload_map.num_copps;

	for (i = 0; i < payload_map.num_copps; i++) {
		int port_idx = payload_map.port_id[i];

		if (port_idx < 0) {
			dev_err(dev, "Invalid port_id %d\n",
				payload_map.port_id[i]);
			return -EINVAL;
		}
		copp_idx = payload_map.copp_idx[i];

		copp = q6adm_find_copp(adm, port_idx, copp_idx);
		if (!copp)
			return -EINVAL;

		if (!READ_ONCE(copp->id_valid)) {
			/* The DEVICE_OPEN CMDRSP has not arrived yet. */
			kref_put(&copp->refcount, q6adm_free_copp);
			return -EAGAIN;
		}

		copps_list[i] = READ_ONCE(copp->id);
		kref_put(&copp->refcount, q6adm_free_copp);
	}

	mutex_lock(&adm->lock);
	spin_lock_irqsave(&adm->result_lock, flags);
	adm->result.status = 0;
	adm->result.opcode = 0;
	/* The generation lives in token bits 8-15; keep the port byte 0. */
	adm->map_gen = (adm->map_gen + 1) & 0xFF;
	if (!adm->map_gen)
		adm->map_gen = 1;
	pkt->hdr.token = adm->token_gen ? adm->map_gen << 8 : 0;
	adm->map_pending = true;
	spin_unlock_irqrestore(&adm->result_lock, flags);

	ret = apr_send_pkt(adm->apr, pkt);
	if (ret < 0) {
		dev_err(dev, "routing for stream %d failed ret %d\n",
		       payload_map.session_id, ret);
		spin_lock_irqsave(&adm->result_lock, flags);
		adm->map_pending = false;
		spin_unlock_irqrestore(&adm->result_lock, flags);
		goto unlock;
	}
	wait_event_timeout(adm->matrix_map_wait, !READ_ONCE(adm->map_pending),
			   msecs_to_jiffies(TIMEOUT_MS));

	spin_lock_irqsave(&adm->result_lock, flags);
	if (adm->map_pending) {
		adm->map_pending = false;
		ret = -ETIMEDOUT;
	} else if (adm->result.status > 0) {
		ret = -EINVAL;
	} else {
		ret = 0;
	}
	status = adm->result.status;
	spin_unlock_irqrestore(&adm->result_lock, flags);

	if (ret == -ETIMEDOUT)
		dev_err(dev, "routing for stream %d failed\n",
		       payload_map.session_id);
	else if (ret == -EINVAL)
		dev_err(dev, "DSP returned error[%u]\n", status);

unlock:
	mutex_unlock(&adm->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6adm_matrix_map);

/**
 * q6adm_close() - Close adm copp
 *
 * @dev: Pointer to adm child device.
 * @copp: pointer to previously opened copp
 *
 * Return: Will be an negative on error or a zero on success.
 */
int q6adm_close(struct device *dev, struct q6copp *copp)
{
	kref_put(&copp->refcount, q6adm_free_copp);

	return 0;
}
EXPORT_SYMBOL_GPL(q6adm_close);

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
static ssize_t inject_fault_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct q6adm *adm = dev_get_drvdata(dev);
	const char *name = "none";

	switch (atomic_read(&adm->fault)) {
	case Q6ADM_FAULT_CLOSE_FAIL:
		name = "close:fail";
		break;
	case Q6ADM_FAULT_CLOSE_REJECT:
		name = "close:reject";
		break;
	case Q6ADM_FAULT_CLOSE_TIMEOUT:
		name = "close:timeout";
		break;
	}

	return sysfs_emit(buf, "%s\n", name);
}

static ssize_t inject_fault_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct q6adm *adm = dev_get_drvdata(dev);
	unsigned int fault;

	if (sysfs_streq(buf, "clear") || sysfs_streq(buf, "none"))
		fault = Q6ADM_FAULT_NONE;
	else if (sysfs_streq(buf, "close:fail"))
		fault = Q6ADM_FAULT_CLOSE_FAIL;
	else if (sysfs_streq(buf, "close:reject"))
		fault = Q6ADM_FAULT_CLOSE_REJECT;
	else if (sysfs_streq(buf, "close:timeout"))
		fault = Q6ADM_FAULT_CLOSE_TIMEOUT;
	else
		return -EINVAL;

	atomic_set(&adm->fault, fault);
	return count;
}
static DEVICE_ATTR_RW(inject_fault);

static void q6adm_remove_sysfs(void *data)
{
	device_remove_file(data, &dev_attr_inject_fault);
}
#endif /* CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT */

static int q6adm_probe(struct apr_device *adev)
{
	struct device *dev = &adev->dev;
	struct q6adm *adm;
#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	int ret;
#endif

	adm = devm_kzalloc(dev, sizeof(*adm), GFP_KERNEL);
	if (!adm)
		return -ENOMEM;

	adm->apr = adev;
	dev_set_drvdata(dev, adm);
	adm->dev = dev;
	adm->token_gen = IS_ENABLED(CONFIG_OF) &&
			 of_machine_is_compatible("lenovo,j716f");
	q6core_get_svc_api_info(adev->svc_id, &adm->ainfo);
	mutex_init(&adm->lock);
	spin_lock_init(&adm->result_lock);
	init_waitqueue_head(&adm->matrix_map_wait);

	INIT_LIST_HEAD(&adm->copps_list);
	INIT_LIST_HEAD(&adm->closing);
	spin_lock_init(&adm->copps_list_lock);

	/*
	 * Ordered so the closes leave in the order the COPPs were released
	 * and one of them cannot be mistaken for another by the DSP.  The
	 * devres release destroys (and thereby flushes) the queue before adm
	 * is freed, so a pending worker never dereferences a dead adm.
	 */
	adm->close_wq = devm_alloc_ordered_workqueue(dev, "q6adm_close",
						     WQ_MEM_RECLAIM);
	if (!adm->close_wq)
		return -ENOMEM;

#ifdef CONFIG_SND_SOC_QDSP6_ASM_FAULT_INJECT
	atomic_set(&adm->fault, Q6ADM_FAULT_NONE);
	ret = device_create_file(dev, &dev_attr_inject_fault);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, q6adm_remove_sysfs, dev);
	if (ret)
		return ret;
#endif

	return devm_of_platform_populate(dev);
}

#ifdef CONFIG_OF
static const struct of_device_id q6adm_device_id[]  = {
	{ .compatible = "qcom,q6adm" },
	{},
};
MODULE_DEVICE_TABLE(of, q6adm_device_id);
#endif

static struct apr_driver qcom_q6adm_driver = {
	.probe = q6adm_probe,
	.callback = q6adm_callback,
	.driver = {
		.name = "qcom-q6adm",
		.of_match_table = of_match_ptr(q6adm_device_id),
	},
};

module_apr_driver(qcom_q6adm_driver);
MODULE_DESCRIPTION("Q6 Audio Device Manager");
MODULE_LICENSE("GPL v2");
