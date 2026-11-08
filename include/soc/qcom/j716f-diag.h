/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * J716F (Lenovo Xiaoxin Pad Pro 2021 / SM8250) freeze breadcrumbs.
 *
 * The v46 freeze evidence shows a CPU stopping dead in normal kernel
 * context: no timer ticks, no pseudo-NMI response, RCU sees the CPU in
 * kernel mode.  A CPU stuck on a bus access that never completes looks
 * exactly like that, so record a small per-CPU breadcrumb before every
 * blocking hardware access on the suspected paths and dump it from the
 * panic notifier.
 */
#ifndef __SOC_QCOM_J716F_DIAG_H
#define __SOC_QCOM_J716F_DIAG_H

#include <linux/types.h>

enum j716f_bc_site {
	J716F_BC_NONE = 0,

	/* drivers/soc/qcom/rpmh.c */
	J716F_BC_RPMH_WRITE_ENTER,
	J716F_BC_RPMH_WRITE_EXIT,
	J716F_BC_RPMH_BATCH_ENTER,
	J716F_BC_RPMH_BATCH_SEND,
	J716F_BC_RPMH_BATCH_WAIT,
	J716F_BC_RPMH_BATCH_TIMEOUT,
	J716F_BC_RPMH_BATCH_EXIT,

	/* drivers/soc/qcom/rpmh-rsc.c */
	J716F_BC_RSC_SEND_ENTER,
	J716F_BC_RSC_SEND_CLAIM,
	J716F_BC_RSC_SEND_BUFFER,
	J716F_BC_RSC_SEND_TRIGGER,
	J716F_BC_RSC_SEND_EXIT,
	J716F_BC_RSC_TRIGGER_ENTER,
	J716F_BC_RSC_TRIGGER_EXIT,
	J716F_BC_RSC_WRSYNC_ENTER,
	J716F_BC_RSC_WRSYNC_DONE,
	J716F_BC_RSC_WRSYNC_FAIL,
	J716F_BC_RSC_INVAL_ENTER,
	J716F_BC_RSC_INVAL_TCS,
	J716F_BC_RSC_INVAL_EXIT,
	J716F_BC_RSC_CTRLDATA_ENTER,
	J716F_BC_RSC_CTRLDATA_EXIT,
	J716F_BC_RSC_IRQ_ENTER,
	J716F_BC_RSC_IRQ_TCS,
	J716F_BC_RSC_IRQ_EXIT,

	/* drivers/interconnect/qcom/bcm-voter.c */
	J716F_BC_ICC_COMMIT_ENTER,
	J716F_BC_ICC_COMMIT_INVAL,
	J716F_BC_ICC_COMMIT_BATCH,
	J716F_BC_ICC_COMMIT_EXIT,

	/* drivers/interconnect/qcom/osm-l3.c */
	J716F_BC_OSM_L3_ENTER,
	J716F_BC_OSM_L3_EXIT,

	/* drivers/cpufreq/qcom-cpufreq-hw.c */
	J716F_BC_CPUFREQ_TARGET_ENTER,
	J716F_BC_CPUFREQ_TARGET_EXIT,
	J716F_BC_CPUFREQ_BW_ENTER,
	J716F_BC_CPUFREQ_BW_EXIT,

	/* drivers/soc/qcom/rpmhpd.c */
	J716F_BC_RPMHPD_ON_ENTER,
	J716F_BC_RPMHPD_ON_EXIT,
	J716F_BC_RPMHPD_PERF_ENTER,
	J716F_BC_RPMHPD_PERF_EXIT,

	/* drivers/clk/qcom/clk-rpmh.c */
	J716F_BC_CLK_RPMH_ENTER,
	J716F_BC_CLK_RPMH_EXIT,

	/* drivers/firmware/qcom/qcom_scm.c */
	J716F_BC_SCM_ENTER,
	J716F_BC_SCM_EXIT,

	/* arch/arm64/kernel/idle.c + drivers/cpuidle */
	J716F_BC_PSCI_SUSPEND_ENTER,
	J716F_BC_PSCI_SUSPEND_EXIT,

	/* drivers/spmi/spmi-pmic-arb.c */
	J716F_BC_SPMI_XFER_ENTER,
	J716F_BC_SPMI_XFER_EXIT,

	/* drivers/gpu/drm/msm */
	J716F_BC_DPU_FLUSH_ENTER,
	J716F_BC_DPU_FLUSH_EXIT,
	J716F_BC_GPU_RESUME_ENTER,
	J716F_BC_GPU_RESUME_EXIT,
	J716F_BC_GPU_STOP_ENTER,
	J716F_BC_GPU_STOP_EXIT,

	/* arch/arm64/include/asm/tlbflush.h */
	J716F_BC_TLB_FLUSH_ENTER,
	J716F_BC_TLB_FLUSH_EXIT,
	/*
	 * Written by a second breadcrumb immediately after TLB_FLUSH_EXIT.
	 * If the CPU stops in the exit breadcrumb's preempt_enable() (a
	 * scheduling point) the RET record is missing and the flush itself
	 * is exonerated.
	 */
	J716F_BC_TLB_FLUSH_RET,

	/* mm teardown, fork/exec and page freeing (M15) */
	J716F_BC_MM_DUP_ENTER,
	J716F_BC_MM_DUP_POST_FLUSH,
	J716F_BC_MM_DUP_UNLOCK_ENTER,
	J716F_BC_MM_DUP_UNLOCK_EXIT,
	J716F_BC_MM_DUP_EXIT,
	J716F_BC_MM_EXIT_ENTER,
	J716F_BC_MM_EXIT_EXIT,
	J716F_BC_MM_UNMAP_REGION_ENTER,
	J716F_BC_MM_UNMAP_REGION_EXIT,
	J716F_BC_MM_UNMAP_VMAS_ENTER,
	J716F_BC_MM_UNMAP_VMAS_EXIT,
	J716F_BC_MM_FREE_PGTABLES_ENTER,
	J716F_BC_MM_FREE_PGTABLES_EXIT,
	J716F_BC_MM_FREE_PGD_RANGE_ENTER,
	J716F_BC_MM_FREE_PGD_RANGE_EXIT,
	J716F_BC_MM_TLB_FINISH_ENTER,
	J716F_BC_MM_TLB_FINISH_EXIT,
	J716F_BC_MM_TLB_FLUSH_MU_ENTER,
	J716F_BC_MM_TLB_FLUSH_MU_EXIT,
	J716F_BC_MM_TLB_PAGES_ENTER,
	J716F_BC_MM_TLB_PAGES_EXIT,
	J716F_BC_MM_TLB_TABLE_ENTER,
	J716F_BC_MM_TLB_TABLE_EXIT,
	J716F_BC_MM_TLB_SYNC_ENTER,
	J716F_BC_MM_TLB_SYNC_EXIT,
	J716F_BC_MM_NOTIFIER_RELEASE_ENTER,
	J716F_BC_MM_NOTIFIER_RELEASE_EXIT,
	J716F_BC_MM_MMPUT_ENTER,
	J716F_BC_MM_MMPUT_EXIT,
	J716F_BC_MM_MMDROP_ENTER,
	J716F_BC_MM_MMDROP_EXIT,
	J716F_BC_MM_EXEC_MMAP_ENTER,
	J716F_BC_MM_EXEC_MMAP_EXIT,
	J716F_BC_FORK_COPY_ENTER,
	J716F_BC_FORK_COPY_EXIT,
	J716F_BC_FORK_WAKE_ENTER,
	J716F_BC_FORK_WAKE_EXIT,
	J716F_BC_PAGE_FREE_LIST_ENTER,
	J716F_BC_PAGE_FREE_LIST_EXIT,
	/*
	 * Written after the exit breadcrumb, but still *inside*
	 * free_unref_folios() and still before folio_batch_reinit().  The
	 * record itself is published before j716f_bc() runs its own
	 * preempt_enable(), so seeing this record only proves the payload
	 * was stored; it does not prove preempt_enable() returned.
	 */
	J716F_BC_PAGE_FREE_PRE_REINIT,
	J716F_BC_PAGE_FREE_PCP_ENTER,
	J716F_BC_PAGE_FREE_PCP_EXIT,
	J716F_BC_TLB_BATCHED_ENTER,
	J716F_BC_TLB_BATCHED_EXIT,

	/* idle loop (M16): a CPU that stops here lost its wakeup */
	J716F_BC_IDLE_ENTER,
	J716F_BC_IDLE_EXIT,

	/* Cortex-A77 1542418 ASID rollover workaround (M17) */
	J716F_BC_A77_ASID_WORKAROUND_ENTER,
	J716F_BC_A77_ASID_WORKAROUND_EXIT,

	/* preemption path (M18): did the exit breadcrumb enter __schedule? */
	J716F_BC_PREEMPT_SCHED_ENTER,
	J716F_BC_PREEMPT_SCHED_EXIT,

	/*
	 * M19: resolve free_unref_folios()'s real return point and its caller.
	 *
	 * All M19 markers use j716f_bc_quiet(), which does not run a
	 * preempt_enable() and therefore adds no scheduling point of its own.
	 * That is what makes them decisive: with a loud record, "last record
	 * present" never proves the record's own preempt_enable() returned.
	 *
	 * PAGE_FREE_PRE_REINIT is written just before folio_batch_reinit()
	 * (it was named PAGE_FREE_LIST_RET in v53, which invited the M18
	 * mistake of reading it as "the function returned"); it is still
	 * inside free_unref_folios().
	 *
	 * PAGE_FREE_BATCH_DONE is written after folio_batch_reinit() and
	 * before the epilogue; the caller marks are written in the caller
	 * immediately after free_unref_folios() returned.  Together they
	 * separate "inside the callee" -> "batch reset done" -> "epilogue"
	 * -> "back in the caller".
	 */
	J716F_BC_PAGE_FREE_BATCH_DONE,
	/*
	 * Caller-side marks.  BCM_LRU is folio_batch_move_lru() (the lru_add
	 * drain path used by release_pages()/folios_put*), REFS is
	 * folios_put_refs(), SHRINK is shrink_folio_list() (both its in-loop
	 * and its tail batch), MOVE_LRU is move_folios_to_lru().  @arg is
	 * _RET_IP_, i.e. the caller of that function, which identifies the
	 * higher-level path (direct reclaim, kswapd, mm teardown, ...).
	 */
	J716F_BC_PAGE_FREE_RET_BCM_LRU,
	J716F_BC_PAGE_FREE_RET_REFS,
	J716F_BC_PAGE_FREE_RET_SHRINK,
	J716F_BC_PAGE_FREE_RET_MOVE_LRU,

	/*
	 * M20-P1: MC0 / RPMh vote provenance.
	 *
	 * VOTE_REQ is written by icc_set_bw(), i.e. at the *client* boundary,
	 * where current->comm still names the real requester (adreno, a UFS
	 * worker, a cpufreq kthread, ...).  The BCM voter aggregates several
	 * clients, so the commit thread is not the requester; this record is
	 * what makes that distinction possible.
	 *
	 * VOTE_AGG is written in qcom_icc_bcm_voter_commit() once the
	 * aggregated MC0 vote is known, i.e. the value that will actually
	 * reach the TCS.  VOTE_TCS is written in rpmh_rsc_send_data() when
	 * the TCS is claimed and triggered.
	 *
	 * All three are quiet records and run in plain process context.
	 */
	J716F_BC_VOTE_REQ,
	J716F_BC_VOTE_AGG,
	J716F_BC_VOTE_TCS,

	/*
	 * M20-P1: RSC completion timeline.  TX_DONE_ENTER/EXIT bracket
	 * tcs_tx_done(); TX_DONE_TCS is the per-TCS bit it is servicing and
	 * TX_DONE_STS the raw RSC_DRV_IRQ_STATUS it read.  This is what
	 * separates "hardware finished" from "Linux reclaimed the TCS".
	 */
	J716F_BC_RSC_TX_DONE_ENTER,
	J716F_BC_RSC_TX_DONE_TCS,
	J716F_BC_RSC_TX_DONE_EXIT,

	J716F_BC_MAX,
};

#define J716F_BC_F_IRQOFF	BIT(0)
#define J716F_BC_F_HARDIRQ	BIT(1)
#define J716F_BC_F_NMI		BIT(2)

/*
 * M20-P1 vote provenance.  Kept out of the per-CPU breadcrumb ring because a
 * request on one CPU and the aggregation/commit on another must be readable
 * in one timeline.
 */
/*
 * What each record actually proves, and - just as important - what it does
 * not prove:
 *
 *   REQ      a client asked for bandwidth.  Not proof that hardware moved.
 *   REQ_MC0  the client that triggered a commit whose batch contained an MC0
 *            command.  It is the TRIGGERING client, not the only contributor:
 *            the aggregated vote also includes every other client's standing
 *            request on the same BCM, set earlier.  It is emitted at AGG time
 *            with the original request's timestamp, so it only appears if the
 *            transaction really reached MC0.
 *   AGG      the BCM voter merged the request into this value.  The thread
 *            here is the commit thread, not the original consumer.
 *   TCS_PRE  the command buffer is programmed; the trigger is about to be
 *            written.  Proves "prepared", nothing more.
 *   TCS_POKED the trigger register store has been issued.  It is a
 *            writel_relaxed() with no readback, so this proves the CPU
 *            executed the store, NOT that the accelerator accepted or acted
 *            on it.  The hardware may be wedged at exactly this point.
 *   DONE     tcs_tx_done() observed the completion bit.  This is the HANDLER
 *            observing an already-set bit; it is not the instant the
 *            accelerator finished, and it is strictly later than it.
 *   RECLAIM  the TCS was released back to the pool (tcs_in_use cleared).
 *
 * The hardware completion instant is only bounded: it is after TCS (trigger)
 * and at or before DONE (handler).  Treat DONE as "no later than", never as
 * "the moment hardware finished".
 *
 * M20-P1d: the DONE/RECLAIM records now carry a batch-mc0 flag (aux bit 0) and
 * only enter the MC0 ring when the exact (txid, rsc, tcs) batch was
 * registered as MC0.  This keeps a completion of a non-MC0 batch from being
 * read as MC0 completion evidence.  It also separates the two B-class
 * observations that v58's prose merged:
 *
 *   RSC DONE=1, no VOTE done   the completion bit was observed in the RSC
 *                              register dump, but no handler entry record
 *                              exists.  Proves the hardware/status bit was
 *                              set; does NOT prove the IRQ was delivered or
 *                              that the handler ran.
 *   VOTE done, no VOTE reclaim the handler DID run and recorded the
 *                              completion; it did not reach the software
 *                              reclaim record.  This is a handler-progress
 *                              failure, not an unserved IRQ.
 *
 * Neither observation excludes a NoC/DDR failure shared with the vote path;
 * a set completion bit only proves the accelerator/status side reached that
 * point.
 *
 * M20-P1c: this enum lives OUTSIDE the CONFIG_QCOM_J716F_DIAG guard.  The
 * call sites pass J716F_VOTE_KIND_* to j716f_vote_record(), and the no-op
 * inline used when the option is off still type-checks its arguments, so a
 * guarded enum made CONFIG_QCOM_J716F_DIAG=n fail to build.
 */
enum j716f_vote_kind {
	J716F_VOTE_KIND_REQ = 1,	/* icc_set_bw(): client request */
	J716F_VOTE_KIND_REQ_MC0,	/* icc_set_bw(): promoted, tx voted MC0 */
	J716F_VOTE_KIND_AGG,		/* bcm commit: aggregated value */
	J716F_VOTE_KIND_TCS_PRE,	/* buffer programmed, trigger not yet */
	J716F_VOTE_KIND_TCS_POKED,	/* trigger register written (no readback) */
	J716F_VOTE_KIND_DONE,		/* tcs_tx_done(): handler saw it done */
	J716F_VOTE_KIND_RECLAIM,	/* tcs_tx_done(): TCS released */
};

#define J716F_VOTE_NO_TCS	0xffffffffu

/*
 * "No MC0 command in this batch".  A TCS can hold up to MAX_CMDS_PER_TCS (16)
 * commands, so 0xff cannot collide with a real index.
 */
#define J716F_VOTE_MC0_NONE	0xffu

/*
 * aux encoding shared by the TCS_PRE / TCS_POKED records.  It says where in
 * the batch MC0 sat, so a dump can tell "MC0 was the trigger command" from
 * "MC0 was command 5 of 9".
 */
#define J716F_VOTE_AUX_NCMDS(a)		((a) & 0xffu)
#define J716F_VOTE_AUX_WAIT(a)		(((a) >> 8) & 1u)
#define J716F_VOTE_AUX_MC0IDX(a)	(((a) >> 16) & 0xffu)

/*
 * DONE/RECLAIM aux bit: the exact (txid, rsc, tcs) batch was registered as
 * MC0 before its trigger.  It is what admits the completion into the MC0 ring;
 * "txid was seen carrying MC0 somewhere" is not enough, because one txid can
 * own several batches and only some are MC0.
 */
#define J716F_VOTE_AUX_MC0_BATCH	0x1u

struct tcs_request;
struct rsc_drv;

#if IS_ENABLED(CONFIG_QCOM_J716F_DIAG)
void j716f_bc(enum j716f_bc_site site, unsigned long arg);
void j716f_bc_quiet(enum j716f_bc_site site, unsigned long arg);
void j716f_diag_dump(const char *reason);

void j716f_vote_set_mc0(u32 addr);
u32 j716f_vote_mc0_addr(void);
void j716f_vote_set_current_txid(u32 txid);
u32 j716f_vote_current_txid(void);
u32 j716f_vote_alloc_txid(void);
void j716f_vote_note_request(u32 txid, u32 avg_bw, u32 peak_bw, u32 node_id,
			     const char *path_name);
void j716f_vote_mc0_commit(u32 txid, u32 idx, u32 addr, u32 data);
void j716f_vote_record(enum j716f_vote_kind kind, u32 txid, u32 addr, u32 data,
		       u32 tcs, u32 aux);
void j716f_vote_record_tcs(enum j716f_vote_kind kind,
			   const struct tcs_request *msg,
			   const struct rsc_drv *rsc, const char *rsc_name,
			   u32 tcs);
u32 j716f_vote_scan_mc0(const struct tcs_request *msg);
void j716f_vote_tx_release(u32 txid, const struct rsc_drv *rsc, u32 tcs);
void j716f_vote_tx_abort(u32 txid, int reason);
void j716f_vote_tx_finish(u32 txid);
bool j716f_vote_tx_batch_is_mc0(u32 txid, const struct rsc_drv *rsc, u32 tcs);
void j716f_vote_dump(void);
/*
 * M20-P1 IRQ accounting.  Cheap enough for the RSC completion handler: one
 * per-CPU counter plus a timestamp, no lock and no printk.
 */
void j716f_irq_enter(int irq);
void j716f_irq_exit(int irq);
void j716f_irq_dump(void);
void j716f_irq_set(int irq);
void j716f_vote_decode(u32 data, bool *valid, bool *commit, u32 *vote_x,
		       u32 *vote_y);
#else
static inline void j716f_bc(enum j716f_bc_site site, unsigned long arg) { }
static inline void j716f_bc_quiet(enum j716f_bc_site site, unsigned long arg) { }
static inline void j716f_diag_dump(const char *reason) { }
static inline void j716f_vote_set_mc0(u32 addr) { }
static inline u32 j716f_vote_mc0_addr(void) { return 0; }
static inline void j716f_vote_set_current_txid(u32 txid) { }
static inline u32 j716f_vote_current_txid(void) { return 0; }
static inline u32 j716f_vote_alloc_txid(void) { return 0; }
static inline void j716f_vote_note_request(u32 txid, u32 avg_bw, u32 peak_bw,
					   u32 node_id, const char *path_name) { }
static inline void j716f_vote_mc0_commit(u32 txid, u32 idx, u32 addr, u32 data) { }
static inline void j716f_vote_record(enum j716f_vote_kind kind, u32 txid,
				     u32 addr, u32 data, u32 tcs, u32 aux) { }
static inline void j716f_vote_record_tcs(enum j716f_vote_kind kind,
					 const struct tcs_request *msg,
					 const struct rsc_drv *rsc,
					 const char *rsc_name, u32 tcs) { }
static inline u32 j716f_vote_scan_mc0(const struct tcs_request *msg)
{ return J716F_VOTE_MC0_NONE; }
static inline void j716f_vote_tx_release(u32 txid, const struct rsc_drv *rsc,
					 u32 tcs) { }
static inline void j716f_vote_tx_abort(u32 txid, int reason) { }
static inline void j716f_vote_tx_finish(u32 txid) { }
static inline bool j716f_vote_tx_batch_is_mc0(u32 txid,
					      const struct rsc_drv *rsc,
					      u32 tcs) { return false; }
static inline void j716f_vote_dump(void) { }
static inline void j716f_irq_enter(int irq) { }
static inline void j716f_irq_exit(int irq) { }
static inline void j716f_irq_dump(void) { }
static inline void j716f_irq_set(int irq) { }
static inline void j716f_vote_decode(u32 data, bool *valid, bool *commit,
				     u32 *vote_x, u32 *vote_y) { }
#endif

#endif /* __SOC_QCOM_J716F_DIAG_H */
