/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J716F MC0 in-flight transaction state machine (M20-P1d).
 *
 * This is the part of the MC0 diagnostic that has to be right even when the
 * evidence it produces is unpleasant: which TCS batches belong to a tracked
 * transaction, whether a completion is allowed to release one, and when a
 * transaction entry may be freed.  It is deliberately free of locking,
 * allocation, printing and time: the caller serialises every transition with
 * j716f_mc0_tx_lock and supplies the timestamp.  Keeping it that small is why
 * it can be compiled and exercised on the host (tests/host/
 * test-j716f-vote-sm.c) instead of being trusted because it built.
 *
 * Vocabulary
 *
 *   transaction (tx)  one icc_set_bw() call that the voter saw carrying MC0.
 *                     Identified by the 32-bit txid, never by a hash.
 *   batch             one actual TCS trigger.  A single txid can produce
 *                     several batches (apply_constraints() per provider,
 *                     tcs_list_gen() splitting at MAX_RPMH_PAYLOAD), and only
 *                     some of them need contain MC0.  A batch is therefore
 *                     keyed by (txid, rsc, tcs), not by txid alone.  The TCS
 *                     id is re-used across transactions, so it is only
 *                     meaningful together with the RSC instance and while the
 *                     TCS is in use.
 *
 *                     A duplicate completion of an already-released batch
 *                     finds no key and is a no-op.  The one case the key
 *                     cannot distinguish is a spurious second completion
 *                     arriving after the same TCS was re-armed for the same
 *                     txid.  The RSC clears the IRQ status bit and tcs_in_use
 *                     before the TCS can be claimed again, so that requires
 *                     the hardware to report a completion that was never
 *                     triggered; this diagnostic assumes it does not, and
 *                     says so here instead of pretending the case is covered.
 *   open              the transaction is still inside icc_set_bw(), i.e. more
 *                     commits/batches for the same txid may still arrive.
 *                     Completion must not free an open entry even when the
 *                     batches seen so far have all completed.
 *
 * States
 *
 *   FREE --prepare--> PREPARED(open=1)
 *   PREPARED --arm--> ARMED(outstanding=1)
 *   ARMED --arm--> ARMED(outstanding++)
 *   ARMED/FAILED --release--> ARMED/FAILED(outstanding--)
 *        (free when outstanding==0 && open==0)
 *   PREPARED/ARMED --abort--> FREE if outstanding==0, else FAILED(open=0)
 *   PREPARED/ARMED/FAILED --finish--> free if outstanding==0, else keep
 *   any outstanding==0 --gc--> FREE
 *
 * ARMED with outstanding==0 is not a state the machine leaves behind; the
 * last release either frees the entry or returns it to PREPARED while the
 * transaction is still open.
 *
 * The batch state is smaller: a batch exists from the moment it is registered
 * immediately before the trigger store (arm) and is removed by the matching
 * completion (release).  POKED only annotates that the trigger store was seen
 * after the fact; a completion can win that race, which is exactly why the
 * pending count is registered before the trigger and not after it.
 */
#ifndef __SOC_QCOM_J716F_VOTE_SM_H
#define __SOC_QCOM_J716F_VOTE_SM_H

#ifdef J716F_VOTE_SM_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t j716f_sm_u32;
typedef uint64_t j716f_sm_u64;

#define j716f_sm_read_once(x)	(*(const volatile __typeof__(x) *)&(x))
#define j716f_sm_write_once(x, v)					\
	do { *(volatile __typeof__(x) *)&(x) = (v); } while (0)
#define j716f_sm_store_release(p, v)					\
	__atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define j716f_sm_load_acquire(p)					\
	__atomic_load_n((p), __ATOMIC_ACQUIRE)
#else
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/compiler.h>
#include <asm/barrier.h>

typedef u32 j716f_sm_u32;
typedef u64 j716f_sm_u64;

#define j716f_sm_read_once(x)		READ_ONCE(x)
#define j716f_sm_write_once(x, v)	WRITE_ONCE(x, v)
#define j716f_sm_store_release(p, v)	smp_store_release(p, v)
#define j716f_sm_load_acquire(p)	smp_load_acquire(p)
#endif

#define J716F_VOTE_SM_TX_LEN		32
#define J716F_VOTE_SM_BATCH_LEN		64
#define J716F_VOTE_SM_COMM_LEN		16
#define J716F_VOTE_SM_PATH_LEN		20
#define J716F_VOTE_SM_MC0_NONE		0xffu

enum j716f_vote_sm_state {
	J716F_VOTE_SM_FREE = 0,
	J716F_VOTE_SM_PREPARED,	/* voter saw MC0, no batch registered yet */
	J716F_VOTE_SM_ARMED,	/* >=1 batch registered and not yet reclaimed */
	J716F_VOTE_SM_FAILED,	/* submission failed with >=1 batch in flight */
};

struct j716f_vote_sm_req {
	j716f_sm_u32 avg_bw;
	j716f_sm_u32 peak_bw;
	j716f_sm_u32 pid;
	j716f_sm_u32 cpu;
	j716f_sm_u64 ts;
	char comm[J716F_VOTE_SM_COMM_LEN];
	char path[J716F_VOTE_SM_PATH_LEN];
};

struct j716f_vote_sm_tx {
	j716f_sm_u32 txid;		/* 0 = slot free */
	j716f_sm_u32 state;		/* enum j716f_vote_sm_state */
	j716f_sm_u32 open;		/* still inside icc_set_bw() */
	j716f_sm_u32 outstanding;	/* registered MC0 batches not reclaimed */
	j716f_sm_u32 promoted;		/* req-mc0 already emitted */
	j716f_sm_u32 mc0_idx;		/* last committed batch's MC0 index */
	j716f_sm_u32 addr;
	j716f_sm_u32 data;
	j716f_sm_u32 avg_bw;
	j716f_sm_u32 peak_bw;
	j716f_sm_u32 pid;
	j716f_sm_u32 cpu;
	j716f_sm_u64 req_ts;		/* when the client called icc_set_bw() */
	j716f_sm_u64 first_arm_ts;	/* first registered batch's arm time */
	j716f_sm_u32 gen;		/* mutation sequence, for dump snapshots */
	int fail_reason;		/* first submission error, 0 if none */
	char comm[J716F_VOTE_SM_COMM_LEN];
	char path[J716F_VOTE_SM_PATH_LEN];
};

struct j716f_vote_sm_batch {
	/*
	 * Publication word.  rsc/tcs are written before it and it is cleared
	 * before the rest on free, so the lockless classifier can read it with
	 * acquire and will never match a half-written key.
	 */
	j716f_sm_u32 txid;		/* 0 = slot free */
	j716f_sm_u32 tcs;
	const void *rsc;
	const char *rsc_name;		/* stable name, dump only */
	j716f_sm_u32 mc0_idx;
	j716f_sm_u32 data;
	j716f_sm_u64 arm_ts;		/* registered, immediately before trigger */
	j716f_sm_u64 poked_ts;		/* trigger store observed, 0 if raced */
	j716f_sm_u32 poked;
	j716f_sm_u32 gen;
};

struct j716f_vote_sm {
	struct j716f_vote_sm_tx tx[J716F_VOTE_SM_TX_LEN];
	struct j716f_vote_sm_batch batch[J716F_VOTE_SM_BATCH_LEN];
	/*
	 * Advanced under the caller's lock on every mutation.  The dump compares
	 * it before and after the chunked copy: a difference means the printed
	 * set may mix two windows and must not be used for A/B/C.
	 */
	j716f_sm_u32 seq;
	/* Evidence-loss counters.  All updated under the caller's lock. */
	j716f_sm_u32 overflow;		/* tx table full at prepare */
	j716f_sm_u32 batch_overflow;	/* batch table full at arm */
	j716f_sm_u32 abort_clean;	/* abort freed a transaction with no batch */
	j716f_sm_u32 abort_partial;	/* abort kept a transaction with batches */
	j716f_sm_u32 orphan_release;	/* release matched a batch without a tx */
	j716f_sm_u32 gc_stale;		/* outstanding==0 entries swept by gc */
	j716f_sm_u32 dup_arm;		/* arm found the same batch key twice */
};

static inline void j716f_vote_sm_copy_name(char *dst, const char *src,
					   j716f_sm_u32 n)
{
	j716f_sm_u32 i;

	if (!n)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	for (i = 0; i + 1 < n && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static inline void j716f_vote_sm_tx_clear(struct j716f_vote_sm_tx *t)
{
	memset(t, 0, sizeof(*t));
}

static inline void j716f_vote_sm_batch_clear(struct j716f_vote_sm_batch *b)
{
	/*
	 * Stop lockless matching first, then wipe.  A reader that already
	 * loaded the old txid can still match a free slot for a few cycles;
	 * that only lets a duplicate completion be classified as MC0, never
	 * changes a count (release always takes the lock and finds nothing).
	 */
	j716f_sm_store_release(&b->txid, 0);
	memset(b, 0, sizeof(*b));
}

static inline struct j716f_vote_sm_tx *
j716f_vote_sm_tx_find(struct j716f_vote_sm *sm, j716f_sm_u32 txid)
{
	int i;

	if (!txid)
		return NULL;
	for (i = 0; i < J716F_VOTE_SM_TX_LEN; i++)
		if (sm->tx[i].txid == txid)
			return &sm->tx[i];
	return NULL;
}

static inline struct j716f_vote_sm_batch *
j716f_vote_sm_batch_find(struct j716f_vote_sm *sm, j716f_sm_u32 txid,
			 const void *rsc, j716f_sm_u32 tcs)
{
	int i;

	if (!txid)
		return NULL;
	for (i = 0; i < J716F_VOTE_SM_BATCH_LEN; i++) {
		struct j716f_vote_sm_batch *b = &sm->batch[i];

		if (b->txid == txid && b->rsc == rsc && b->tcs == tcs)
			return b;
	}
	return NULL;
}

/*
 * Lockless "is this exact completion a registered MC0 batch?".
 *
 * Only classification uses this.  A false negative leaves the record in the
 * general ring (the safe direction); a false positive can only come from a
 * duplicate completion racing the free, and cannot change outstanding because
 * the release path is exact and locked.
 */
static inline bool j716f_vote_sm_batch_is_mc0(struct j716f_vote_sm *sm,
					      j716f_sm_u32 txid,
					      const void *rsc,
					      j716f_sm_u32 tcs)
{
	int i;

	if (!txid)
		return false;
	for (i = 0; i < J716F_VOTE_SM_BATCH_LEN; i++) {
		struct j716f_vote_sm_batch *b = &sm->batch[i];
		j716f_sm_u32 btxid = j716f_sm_load_acquire(&b->txid);

		if (btxid == txid && j716f_sm_read_once(b->rsc) == rsc &&
		    j716f_sm_read_once(b->tcs) == tcs)
			return true;
	}
	return false;
}

/*
 * Prepare/refresh the transaction entry for a batch the voter has confirmed
 * contains MC0.  Called before submission.  *emit is true exactly once per
 * transaction, for the promoted req-mc0 record.
 *
 * Returns NULL only when the tx table is full (overflow is incremented); the
 * caller then has no entry to track and must not pretend otherwise.
 */
static inline struct j716f_vote_sm_tx *
j716f_vote_sm_prepare(struct j716f_vote_sm *sm, j716f_sm_u32 txid,
		      j716f_sm_u32 idx, j716f_sm_u32 addr, j716f_sm_u32 data,
		      const struct j716f_vote_sm_req *req, bool have_req,
		      bool *emit)
{
	struct j716f_vote_sm_tx *t;
	int i;

	if (emit)
		*emit = false;
	if (!txid)
		return NULL;

	t = j716f_vote_sm_tx_find(sm, txid);
	if (!t) {
		for (i = 0; i < J716F_VOTE_SM_TX_LEN; i++) {
			if (!sm->tx[i].txid) {
				t = &sm->tx[i];
				break;
			}
		}
		if (!t) {
			sm->overflow++;
			return NULL;
		}
		j716f_vote_sm_tx_clear(t);
		t->txid = txid;
		t->mc0_idx = J716F_VOTE_SM_MC0_NONE;
		t->state = J716F_VOTE_SM_PREPARED;
		t->gen = ++sm->seq;
	}

	/*
	 * One icc_set_bw() can commit more than once (apply_constraints()
	 * runs per provider).  The first provenance wins: it is the request
	 * that actually changed the bandwidth.  A later commit that cannot
	 * see the handoff must not replace it with another client's values.
	 */
	if (have_req && !t->pid) {
		t->avg_bw = req ? req->avg_bw : 0;
		t->peak_bw = req ? req->peak_bw : 0;
		t->pid = req ? req->pid : 0;
		t->cpu = req ? req->cpu : 0;
		t->req_ts = req ? req->ts : 0;
		if (req) {
			j716f_vote_sm_copy_name(t->comm, req->comm,
						sizeof(t->comm));
			j716f_vote_sm_copy_name(t->path, req->path,
						sizeof(t->path));
		}
	}

	t->mc0_idx = idx;
	t->addr = addr;
	t->data = data;
	t->open = 1;
	if (t->outstanding == 0 && t->state != J716F_VOTE_SM_FAILED)
		t->state = J716F_VOTE_SM_PREPARED;
	t->gen = ++sm->seq;

	if (emit) {
		*emit = !t->promoted;
		t->promoted = 1;
		t->gen = ++sm->seq;
	}
	return t;
}

/*
 * Register one actual MC0 batch.  MUST be called before the trigger store is
 * issued, so a completion running on another CPU cannot decrement a count
 * that was not taken yet.  Returns false when the batch could not be tracked
 * (no transaction entry, or batch table full); the caller must not count it
 * as registered.
 */
static inline bool j716f_vote_sm_arm(struct j716f_vote_sm *sm,
				     j716f_sm_u32 txid, const void *rsc,
				     const char *rsc_name, j716f_sm_u32 tcs,
				     j716f_sm_u32 idx, j716f_sm_u32 data,
				     j716f_sm_u64 now)
{
	struct j716f_vote_sm_tx *t;
	struct j716f_vote_sm_batch *b;
	int i;

	if (!txid)
		return false;
	t = j716f_vote_sm_tx_find(sm, txid);
	if (!t)
		return false;

	/*
	 * TCS ids are re-used, but a completed batch is removed before the
	 * TCS can be claimed again.  Finding the same key therefore means a
	 * duplicate registration, not a second trigger; counting it twice
	 * would keep the transaction alive forever.
	 */
	b = j716f_vote_sm_batch_find(sm, txid, rsc, tcs);
	if (b) {
		sm->dup_arm++;
		return true;
	}

	for (i = 0; i < J716F_VOTE_SM_BATCH_LEN; i++) {
		if (!sm->batch[i].txid) {
			b = &sm->batch[i];
			break;
		}
	}
	if (!b) {
		/*
		 * Refuse rather than evict.  If tracked batches for this
		 * transaction later complete, its entry is freed even though
		 * this untracked batch may still be in flight; batch_overflow
		 * is what tells the reader that the set is incomplete.
		 */
		sm->batch_overflow++;
		return false;
	}

	j716f_vote_sm_batch_clear(b);
	b->rsc = rsc;
	b->rsc_name = rsc_name;
	b->tcs = tcs;
	b->mc0_idx = idx;
	b->data = data;
	b->arm_ts = now;
	b->gen = ++sm->seq;
	/* Publish the key last; see the struct comment. */
	j716f_sm_store_release(&b->txid, txid);

	if (t->outstanding++ == 0)
		t->first_arm_ts = now;
	t->state = J716F_VOTE_SM_ARMED;
	t->gen = ++sm->seq;
	return true;
}

/*
 * Annotate the exact batch with "the trigger store was issued".  This is
 * intentionally allowed to fail: the completion may have run and freed the
 * batch first.  The TCS_POKED ring record is emitted independently, so losing
 * this annotation never loses the fact that the store was executed.
 */
static inline bool j716f_vote_sm_poked(struct j716f_vote_sm *sm,
				       j716f_sm_u32 txid, const void *rsc,
				       j716f_sm_u32 tcs, j716f_sm_u64 now)
{
	struct j716f_vote_sm_batch *b = j716f_vote_sm_batch_find(sm, txid,
								 rsc, tcs);
	struct j716f_vote_sm_tx *t;

	if (!b)
		return false;
	if (!b->poked) {
		b->poked = 1;
		b->poked_ts = now;
		b->gen = ++sm->seq;
	}
	t = j716f_vote_sm_tx_find(sm, txid);
	if (t)
		t->gen = ++sm->seq;
	return true;
}

/*
 * Consume the completion of exactly one registered MC0 batch.  A completion
 * of a non-MC0 batch, a duplicate completion, or a completion whose batch was
 * never registered finds no key and changes nothing.
 *
 * The entry is freed only when no registered batch remains AND the
 * transaction is no longer open.  A transaction that is still inside
 * icc_set_bw() may have another commit coming; freeing it here would let the
 * next batch arm into a missing entry and lose its hang evidence.
 */
static inline bool j716f_vote_sm_release(struct j716f_vote_sm *sm,
					 j716f_sm_u32 txid, const void *rsc,
					 j716f_sm_u32 tcs)
{
	struct j716f_vote_sm_batch *b = j716f_vote_sm_batch_find(sm, txid,
								 rsc, tcs);
	struct j716f_vote_sm_tx *t;

	if (!b)
		return false;

	t = j716f_vote_sm_tx_find(sm, txid);
	j716f_vote_sm_batch_clear(b);
	if (!t) {
		sm->orphan_release++;
		return true;
	}

	if (t->outstanding)
		t->outstanding--;
	else
		sm->orphan_release++;

	if (!t->outstanding) {
		if (!t->open) {
			j716f_vote_sm_tx_clear(t);
		} else if (t->state == J716F_VOTE_SM_ARMED) {
			t->state = J716F_VOTE_SM_PREPARED;
			t->gen = ++sm->seq;
		}
	} else {
		t->gen = ++sm->seq;
	}
	return true;
}

/*
 * Submission failed.  With no registered batch the entry is safe to drop: no
 * hardware request can complete for it.  With batches in flight it is NOT
 * dropped - the failure reason and the original provenance must survive until
 * the last completion arrives, and forever if it never does.
 */
static inline int j716f_vote_sm_abort(struct j716f_vote_sm *sm,
				      j716f_sm_u32 txid, int reason)
{
	struct j716f_vote_sm_tx *t = j716f_vote_sm_tx_find(sm, txid);

	if (!t)
		return 0;
	if (!t->fail_reason)
		t->fail_reason = reason;
	if (!t->outstanding) {
		j716f_vote_sm_tx_clear(t);
		sm->abort_clean++;
		return 1;
	}
	t->state = J716F_VOTE_SM_FAILED;
	t->open = 0;
	t->gen = ++sm->seq;
	sm->abort_partial++;
	return 2;
}

/*
 * End of icc_set_bw().  No more commits for this txid can arrive.  Drop the
 * open reference; if everything already completed, the entry disappears now,
 * otherwise the remaining completions will free it.
 */
static inline bool j716f_vote_sm_finish(struct j716f_vote_sm *sm,
					j716f_sm_u32 txid)
{
	struct j716f_vote_sm_tx *t = j716f_vote_sm_tx_find(sm, txid);

	if (!t)
		return false;
	t->open = 0;
	t->gen = ++sm->seq;
	if (!t->outstanding)
		j716f_vote_sm_tx_clear(t);
	return true;
}

/*
 * Safety net for a prepared entry whose transaction returned without ever
 * reaching finish() (or without arming a batch).  Called from the next
 * alloc_txid(), which runs under icc_bw_lock: no other icc_set_bw() can be
 * active then, so every entry with outstanding==0 is stale.  Entries with
 * batches in flight are never touched.
 */
static inline j716f_sm_u32 j716f_vote_sm_gc(struct j716f_vote_sm *sm)
{
	j716f_sm_u32 freed = 0;
	int i;

	for (i = 0; i < J716F_VOTE_SM_TX_LEN; i++) {
		struct j716f_vote_sm_tx *t = &sm->tx[i];

		if (t->txid && !t->outstanding) {
			j716f_vote_sm_tx_clear(t);
			sm->gc_stale++;
			freed++;
		}
	}
	return freed;
}

static inline const char *j716f_vote_sm_state_name(j716f_sm_u32 state)
{
	switch (state) {
	case J716F_VOTE_SM_FREE:	return "free";
	case J716F_VOTE_SM_PREPARED:	return "prepared";
	case J716F_VOTE_SM_ARMED:	return "armed";
	case J716F_VOTE_SM_FAILED:	return "failed";
	default:			return "?";
	}
}

#endif /* __SOC_QCOM_J716F_VOTE_SM_H */
