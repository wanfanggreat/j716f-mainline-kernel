// SPDX-License-Identifier: GPL-2.0-only
/*
 * J716F freeze breadcrumbs.
 *
 * Records the last few instrumented call sites per CPU in a small
 * lock-free ring, then dumps them (plus the GIC redistributor state and
 * the RPMh TCS state) from the panic notifier.  A CPU that stops taking
 * timer interrupts and pseudo-NMIs cannot tell us where it is, but the
 * last breadcrumb written before the hang can.
 *
 * This is diagnostic scaffolding for the TB-J716F mainline port; see
 * docs/status/TECH-DEBT.md.
 */

#define pr_fmt(fmt) "j716f-diag: " fmt

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/console.h>
#include <linux/context_tracking_state.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/export.h>
#include <linux/hardirq.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/nmi.h>
#include <linux/kernel.h>
#include <linux/panic_notifier.h>
#include <linux/percpu.h>
#include <linux/suspend.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/smp.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>

#include <asm/memory.h>
#include <asm/mmu_context.h>

#include <soc/qcom/cmd-db.h>
#include <soc/qcom/j716f-diag.h>
#include <soc/qcom/tcs.h>

#define J716F_BC_RING		16
#define J716F_BC_DUMP_DEPTH	6
#define J716F_BC_COMM_LEN	12

/*
 * The generation is written last with release semantics; J716F_BC_GEN_BUSY
 * marks a slot that is being overwritten.  A reader from another CPU (or
 * from the panic notifier while the target is still running) therefore
 * never trusts a half written record.
 */
#define J716F_BC_GEN_BUSY	(~0ULL)

struct j716f_bc_rec {
	u64 ts;
	u64 gen;
	unsigned long arg;
	u32 site;
	u32 flags;
	u32 pid;
	u32 pad;
	char comm[J716F_BC_COMM_LEN];
};

struct j716f_bc_cpu {
	atomic64_t seq;
	u32 alert_stage;
	u32 bt_stage;
	u32 online;
	u32 timer_ready;
	u64 heartbeat;
	struct j716f_bc_rec ring[J716F_BC_RING];
};

static DEFINE_PER_CPU(struct j716f_bc_cpu, j716f_bc_data);
static DEFINE_PER_CPU(struct hrtimer, j716f_snap_timer);

/*
 * M20-P1a vote provenance ring.
 *
 * The BCM voter merges several clients into one MC0 vote, so the thread that
 * calls rpmh_write_batch() is not necessarily the requester.  This ring keeps
 * the client-side request, the aggregated commit, the TCS trigger and the
 * handler's observation of the completion, all with the monotonic timestamp
 * base the per-CPU breadcrumbs use, so a panic dump can answer "who asked
 * for MC0, when, and did the completion ever get serviced".
 *
 * Deliberately NOT per-CPU: a request on CPU4 and the commit on CPU0 must be
 * comparable in one timeline.
 *
 * Locking (M20-P1a): the lock is initialised statically with
 * DEFINE_SPINLOCK.  v55 declared a bare "spinlock_t lock" inside a BSS
 * structure that was never passed to spin_lock_init(); with
 * CONFIG_DEBUG_SPINLOCK off that is not caught at build or boot time and the
 * lock only works because zeroed memory happens to be a valid unlocked
 * spinlock on arm64.  That is not something a diagnostic should rely on.
 *
 * Retention (M20-P1a): a single time-ordered ring loses the interesting
 * records when the rate is high.  dpu_crtc_atomic_flush() calls icc_set_bw()
 * once per CRTC per commit, i.e. ~120 writes/s at 60 Hz on this device, which
 * would wrap 32 entries in well under a second.  Records about the MC0
 * resource (or that cannot be attributed to a resource) therefore go into a
 * separate, slower-wrapping MC0 ring, and both rings count what they
 * overwrote so a dump can say "evidence was lost" instead of silently
 * showing a plausible but truncated history.
 */
#define J716F_VOTE_RING		64	/* general: req/agg/tcs/done */
#define J716F_VOTE_MC0_RING	64	/* MC0 + unattributed only */
#define J716F_VOTE_COMM_LEN	16

/*
 * A transaction id ties the four records of one RPMh round trip together.
 * It is a plain per-boot monotonic counter, allocated by the client request
 * and re-used by the commit, the TCS trigger and the completion observed by
 * the handler.  It is NOT a hardware identifier: the hardware only knows the
 * TCS number, which is recorded separately because it can be re-used long
 * before the software transaction is done.
 */
struct j716f_vote_rec {
	u64 ts;
	u32 kind;		/* enum j716f_vote_kind */
	u32 cpu;
	u32 pid;
	u32 txid;		/* transaction id, see above */
	u32 addr;		/* BCM/RPMh resource address (0 if unknown) */
	u32 data;		/* raw TCS data word (vote_x/vote_y encoded) */
	u32 aux;		/* kind specific extra (see dump) */
	u32 tcs;		/* TCS id when known, else ~0 */
	u32 seq;		/* ring-local sequence, for ordering after wrap */
	char comm[J716F_VOTE_COMM_LEN];
};

struct j716f_vote_ring {
	spinlock_t lock;
	u32 head;
	u32 count;
	u32 overwritten;	/* records lost to wrap since boot */
	/*
	 * Generation is bumped on every push while the lock is held.  The
	 * dump brackets each chunk with it: if it moved across the copy, a
	 * writer got in between and the chunk may mix two time windows.
	 */
	u32 generation;
	struct j716f_vote_rec rec[J716F_VOTE_RING];
};

/*
 * Static initialisation: __SPIN_LOCK_UNLOCKED initialises the lock at
 * compile time, so no early caller can ever take an uninitialised one before
 * j716f_diag_init() runs.  icc_set_bw() can be reached from device probe
 * paths that are not ordered against this driver's initcall, which is exactly
 * the case a bare BSS spinlock fails.
 */
static struct j716f_vote_ring j716f_votes = {
	.lock = __SPIN_LOCK_UNLOCKED(j716f_votes.lock),
};
static struct j716f_vote_ring j716f_votes_mc0 = {
	.lock = __SPIN_LOCK_UNLOCKED(j716f_votes_mc0.lock),
};

/*
 * Transaction id allocator.  A plain atomic counter: ids only have to be
 * unique within a boot, and 32 bits gives far more headroom than any
 * experiment.  Wrapping is harmless because the rings only hold tens of
 * records, so a duplicate id would have to come from 4 billion transactions
 * earlier.
 */
static atomic_t j716f_vote_txid = ATOMIC_INIT(1);

/*
 * The transaction id currently being built by icc_set_bw().
 *
 * M20-P1b: this used to leak.  It was set only when the bandwidth actually
 * changed, never cleared, and read by every RPMh sender - including
 * rpmh_write() from regulators and rpmhpd, which never go through
 * icc_set_bw() at all.  Those transfers therefore inherited the id of
 * whatever interconnect transaction happened to run last, and the dump
 * presented them as belonging to that transaction.
 *
 * The contract now:
 *   - icc_set_bw() sets it while it holds icc_bw_lock, and clears it in all
 *     exit paths, including the no-change path and the failure retry;
 *   - only qcom_icc_bcm_voter_commit() reads it (it runs inside that lock);
 *   - rpmh_rsc_send_data() never reads it: the id travels in the request.
 * A reader that finds 0 records "unattributable" rather than guessing.
 */
static u32 j716f_vote_cur_txid;

void j716f_vote_set_current_txid(u32 txid)
{
	WRITE_ONCE(j716f_vote_cur_txid, txid);
}
EXPORT_SYMBOL_GPL(j716f_vote_set_current_txid);

u32 j716f_vote_current_txid(void)
{
	return READ_ONCE(j716f_vote_cur_txid);
}
EXPORT_SYMBOL_GPL(j716f_vote_current_txid);

/*
 * MC0's RPMh address.  Resolved from the Command DB by name at init instead
 * of being hard-coded: 0x50000 is what this firmware reports today, but the
 * whole point of the MC0 ring is to keep the records that matter, so the
 * classifier must not silently stop matching if the address changes.
 * 0 means "not resolved", and j716f_vote_is_important() then admits nothing:
 * keeping everything would let per-frame traffic flush the ring, which is the
 * v56 failure this replaced.
 */
static u32 j716f_mc0_addr;

/* Diagnostics must never become a fault source: allow turning it off. */
static bool vote_trace = true;
module_param(vote_trace, bool, 0644);
MODULE_PARM_DESC(vote_trace, "J716F: record MC0/RPMh vote provenance (diagnostic)");

/*
 * A CPU whose snapshot hrtimer has not run for this long is not merely
 * idle: its interrupts are off, it is inside EL3, or the core is stuck on
 * a bus access.  That is exactly the freeze signature.
 */
#define J716F_STALE_NS		(2ULL * NSEC_PER_SEC)
#define J716F_SNAP_PERIOD_NS	NSEC_PER_SEC

static const u64 j716f_alert_ms[] = { 2000, 5000, 10000, 20000, 40000, 80000 };

/*
 * At bt_stage (5 s) ask the wedged CPU for a pseudo-NMI backtrace while the
 * rest of the system is still healthy and the console is free; at panic_stage
 * (10 s) panic ourselves instead of waiting for the RCU stall detector, so
 * the breadcrumb/GICR/RSC dump is produced before anything else goes wrong.
 */
static unsigned int bt_stage = 2;
module_param(bt_stage, uint, 0644);
MODULE_PARM_DESC(bt_stage, "J716F: stale-heartbeat stage that requests an NMI backtrace (0=off)");

/*
 * Default 0: the v50 experiment must observe a hang without resetting the
 * device.  A tester enables the self-panic only after the SNAP stream, the
 * ASID counters and the GICR dump have been validated on the device.
 */
static unsigned int panic_stage;
module_param(panic_stage, uint, 0644);
MODULE_PARM_DESC(panic_stage, "J716F: stale-heartbeat stage that panics (0=off, default)");

static unsigned int snap_print = 1;
module_param(snap_print, uint, 0644);
MODULE_PARM_DESC(snap_print, "J716F: print one SNAP line per second (0=off)");

/*
 * All cross-CPU comparisons use the clocksource-based monotonic clock,
 * not sched_clock(): a heartbeat written by another CPU must use the same
 * time base as the printer's "now".  ktime_get_mono_fast_ns() is NMI-safe.
 */
static u64 j716f_now(void)
{
	return ktime_get_mono_fast_ns();
}

/* Ages are computed across CPUs, so clamp the clock skew. */
static u64 j716f_age(u64 now, u64 then)
{
	return then > now ? 0 : now - then;
}

struct j716f_bt_req {
	struct work_struct work;
	int cpu;
};
static DEFINE_PER_CPU(struct j716f_bt_req, j716f_bt_reqs);

/* Ignore stale hearts for a while after a system resume: hrtimers freeze. */
static u64 j716f_snap_guard;
/*
 * Set from PM_SUSPEND_PREPARE until PM_POST_SUSPEND.  With s2idle the
 * hrtimers are frozen but the CPUs stay online, so the first timer after
 * resume would otherwise see every other CPU as stale for the whole sleep
 * duration and panic before the PM_POST_SUSPEND notifier runs.
 */
static bool j716f_suspended;

static void j716f_bt_work_fn(struct work_struct *w)
{
	struct j716f_bt_req *req = container_of(w, struct j716f_bt_req, work);

	/*
	 * The CPU may have gone offline while the work was queued.  Sending
	 * an IPI to a dead CPU is pointless and arch code may WARN.
	 */
	if (!cpu_online(req->cpu))
		return;

	pr_info("J716F SNAP: requesting NMI backtrace of cpu%d\n", req->cpu);
	trigger_single_cpu_backtrace(req->cpu);
}

static const char *const j716f_bc_names[J716F_BC_MAX] = {
	[J716F_BC_NONE]			= "none",

	[J716F_BC_RPMH_WRITE_ENTER]	= "rpmh_write_enter",
	[J716F_BC_RPMH_WRITE_EXIT]	= "rpmh_write_exit",
	[J716F_BC_RPMH_BATCH_ENTER]	= "rpmh_batch_enter",
	[J716F_BC_RPMH_BATCH_SEND]	= "rpmh_batch_send",
	[J716F_BC_RPMH_BATCH_WAIT]	= "rpmh_batch_wait",
	[J716F_BC_RPMH_BATCH_TIMEOUT]	= "rpmh_batch_timeout",
	[J716F_BC_RPMH_BATCH_EXIT]	= "rpmh_batch_exit",

	[J716F_BC_RSC_SEND_ENTER]	= "rsc_send_enter",
	[J716F_BC_RSC_SEND_CLAIM]	= "rsc_send_claim",
	[J716F_BC_RSC_SEND_BUFFER]	= "rsc_send_buffer",
	[J716F_BC_RSC_SEND_TRIGGER]	= "rsc_send_trigger",
	[J716F_BC_RSC_SEND_EXIT]	= "rsc_send_exit",
	[J716F_BC_RSC_TRIGGER_ENTER]	= "rsc_trigger_enter",
	[J716F_BC_RSC_TRIGGER_EXIT]	= "rsc_trigger_exit",
	[J716F_BC_RSC_WRSYNC_ENTER]	= "rsc_wrsync_enter",
	[J716F_BC_RSC_WRSYNC_DONE]	= "rsc_wrsync_done",
	[J716F_BC_RSC_WRSYNC_FAIL]	= "rsc_wrsync_fail",
	[J716F_BC_RSC_INVAL_ENTER]	= "rsc_inval_enter",
	[J716F_BC_RSC_INVAL_TCS]	= "rsc_inval_tcs",
	[J716F_BC_RSC_INVAL_EXIT]	= "rsc_inval_exit",
	[J716F_BC_RSC_CTRLDATA_ENTER]	= "rsc_ctrldata_enter",
	[J716F_BC_RSC_CTRLDATA_EXIT]	= "rsc_ctrldata_exit",
	[J716F_BC_RSC_IRQ_ENTER]	= "rsc_irq_enter",
	[J716F_BC_RSC_IRQ_TCS]		= "rsc_irq_tcs",
	[J716F_BC_RSC_IRQ_EXIT]		= "rsc_irq_exit",

	[J716F_BC_ICC_COMMIT_ENTER]	= "icc_commit_enter",
	[J716F_BC_ICC_COMMIT_INVAL]	= "icc_commit_inval",
	[J716F_BC_ICC_COMMIT_BATCH]	= "icc_commit_batch",
	[J716F_BC_ICC_COMMIT_EXIT]	= "icc_commit_exit",

	[J716F_BC_OSM_L3_ENTER]		= "osm_l3_enter",
	[J716F_BC_OSM_L3_EXIT]		= "osm_l3_exit",

	[J716F_BC_CPUFREQ_TARGET_ENTER]	= "cpufreq_target_enter",
	[J716F_BC_CPUFREQ_TARGET_EXIT]	= "cpufreq_target_exit",
	[J716F_BC_CPUFREQ_BW_ENTER]	= "cpufreq_bw_enter",
	[J716F_BC_CPUFREQ_BW_EXIT]	= "cpufreq_bw_exit",

	[J716F_BC_RPMHPD_ON_ENTER]	= "rpmhpd_on_enter",
	[J716F_BC_RPMHPD_ON_EXIT]	= "rpmhpd_on_exit",
	[J716F_BC_RPMHPD_PERF_ENTER]	= "rpmhpd_perf_enter",
	[J716F_BC_RPMHPD_PERF_EXIT]	= "rpmhpd_perf_exit",

	[J716F_BC_CLK_RPMH_ENTER]	= "clk_rpmh_enter",
	[J716F_BC_CLK_RPMH_EXIT]	= "clk_rpmh_exit",

	[J716F_BC_SCM_ENTER]		= "scm_enter",
	[J716F_BC_SCM_EXIT]		= "scm_exit",

	[J716F_BC_PSCI_SUSPEND_ENTER]	= "psci_suspend_enter",
	[J716F_BC_PSCI_SUSPEND_EXIT]	= "psci_suspend_exit",

	[J716F_BC_SPMI_XFER_ENTER]	= "spmi_xfer_enter",
	[J716F_BC_SPMI_XFER_EXIT]	= "spmi_xfer_exit",

	[J716F_BC_DPU_FLUSH_ENTER]	= "dpu_flush_enter",
	[J716F_BC_DPU_FLUSH_EXIT]	= "dpu_flush_exit",
	[J716F_BC_GPU_RESUME_ENTER]	= "gpu_resume_enter",
	[J716F_BC_GPU_RESUME_EXIT]	= "gpu_resume_exit",
	[J716F_BC_GPU_STOP_ENTER]	= "gpu_stop_enter",
	[J716F_BC_GPU_STOP_EXIT]	= "gpu_stop_exit",

	[J716F_BC_TLB_FLUSH_ENTER]	= "tlb_flush_enter",
	[J716F_BC_TLB_FLUSH_EXIT]	= "tlb_flush_exit",
	[J716F_BC_TLB_FLUSH_RET]	= "tlb_flush_ret",

	[J716F_BC_MM_DUP_ENTER]		= "dup_mmap_enter",
	[J716F_BC_MM_DUP_POST_FLUSH]	= "dup_mmap_post_flush",
	[J716F_BC_MM_DUP_UNLOCK_ENTER]	= "dup_mmap_unlock_enter",
	[J716F_BC_MM_DUP_UNLOCK_EXIT]	= "dup_mmap_unlock_exit",
	[J716F_BC_MM_DUP_EXIT]		= "dup_mmap_exit",
	[J716F_BC_MM_EXIT_ENTER]	= "exit_mmap_enter",
	[J716F_BC_MM_EXIT_EXIT]		= "exit_mmap_exit",
	[J716F_BC_MM_UNMAP_REGION_ENTER] = "unmap_region_enter",
	[J716F_BC_MM_UNMAP_REGION_EXIT]	 = "unmap_region_exit",
	[J716F_BC_MM_UNMAP_VMAS_ENTER]	= "unmap_vmas_enter",
	[J716F_BC_MM_UNMAP_VMAS_EXIT]	= "unmap_vmas_exit",
	[J716F_BC_MM_FREE_PGTABLES_ENTER] = "free_pgtables_enter",
	[J716F_BC_MM_FREE_PGTABLES_EXIT]  = "free_pgtables_exit",
	[J716F_BC_MM_FREE_PGD_RANGE_ENTER] = "free_pgd_range_enter",
	[J716F_BC_MM_FREE_PGD_RANGE_EXIT]  = "free_pgd_range_exit",
	[J716F_BC_MM_TLB_FINISH_ENTER]	= "tlb_finish_mmu_enter",
	[J716F_BC_MM_TLB_FINISH_EXIT]	= "tlb_finish_mmu_exit",
	[J716F_BC_MM_TLB_FLUSH_MU_ENTER] = "tlb_flush_mmu_enter",
	[J716F_BC_MM_TLB_FLUSH_MU_EXIT]	= "tlb_flush_mmu_exit",
	[J716F_BC_MM_TLB_PAGES_ENTER]	= "tlb_batch_pages_enter",
	[J716F_BC_MM_TLB_PAGES_EXIT]	= "tlb_batch_pages_exit",
	[J716F_BC_MM_TLB_TABLE_ENTER]	= "tlb_table_flush_enter",
	[J716F_BC_MM_TLB_TABLE_EXIT]	= "tlb_table_flush_exit",
	[J716F_BC_MM_TLB_SYNC_ENTER]	= "tlb_table_sync_enter",
	[J716F_BC_MM_TLB_SYNC_EXIT]	= "tlb_table_sync_exit",
	[J716F_BC_MM_NOTIFIER_RELEASE_ENTER] = "mmu_notifier_release_enter",
	[J716F_BC_MM_NOTIFIER_RELEASE_EXIT]  = "mmu_notifier_release_exit",
	[J716F_BC_MM_MMPUT_ENTER]	= "mmput_enter",
	[J716F_BC_MM_MMPUT_EXIT]	= "mmput_exit",
	[J716F_BC_MM_MMDROP_ENTER]	= "mmdrop_enter",
	[J716F_BC_MM_MMDROP_EXIT]	= "mmdrop_exit",
	[J716F_BC_MM_EXEC_MMAP_ENTER]	= "exec_mmap_enter",
	[J716F_BC_MM_EXEC_MMAP_EXIT]	= "exec_mmap_exit",
	[J716F_BC_FORK_COPY_ENTER]	= "copy_process_enter",
	[J716F_BC_FORK_COPY_EXIT]	= "copy_process_exit",
	[J716F_BC_FORK_WAKE_ENTER]	= "wake_new_task_enter",
	[J716F_BC_FORK_WAKE_EXIT]	= "wake_new_task_exit",
	[J716F_BC_PAGE_FREE_LIST_ENTER]	= "free_unref_folios_enter",
	[J716F_BC_PAGE_FREE_LIST_EXIT]	= "free_unref_folios_exit",
	[J716F_BC_PAGE_FREE_PRE_REINIT]	= "free_unref_pre_reinit",
	[J716F_BC_PAGE_FREE_PCP_ENTER]	= "free_pcppages_bulk_enter",
	[J716F_BC_PAGE_FREE_PCP_EXIT]	= "free_pcppages_bulk_exit",
	[J716F_BC_TLB_BATCHED_ENTER]	= "tlb_batched_enter",
	[J716F_BC_TLB_BATCHED_EXIT]	= "tlb_batched_exit",

	[J716F_BC_IDLE_ENTER]		= "idle_enter",
	[J716F_BC_IDLE_EXIT]		= "idle_exit",
	[J716F_BC_A77_ASID_WORKAROUND_ENTER] = "a77_asid_wa_enter",
	[J716F_BC_A77_ASID_WORKAROUND_EXIT]  = "a77_asid_wa_exit",
	[J716F_BC_PREEMPT_SCHED_ENTER]	= "preempt_sched_enter",
	[J716F_BC_PREEMPT_SCHED_EXIT]	= "preempt_sched_exit",
	[J716F_BC_PAGE_FREE_BATCH_DONE]	= "free_unref_batch_done",
	[J716F_BC_PAGE_FREE_RET_BCM_LRU] = "free_unref_ret_bcm_lru",
	[J716F_BC_PAGE_FREE_RET_REFS]	= "free_unref_ret_refs",
	[J716F_BC_PAGE_FREE_RET_SHRINK]	= "free_unref_ret_shrink",
	[J716F_BC_PAGE_FREE_RET_MOVE_LRU] = "free_unref_ret_move_lru",
};

static const char *j716f_bc_name(u32 site)
{
	if (site >= J716F_BC_MAX || !j716f_bc_names[site])
		return "?";
	return j716f_bc_names[site];
}

/**
 * j716f_bc_quiet - record one breadcrumb without creating a preemption point
 * @site: instrumented call site
 * @arg: site specific value (register, id, address, ...)
 *
 * Identical payload to j716f_bc() but with a bare preempt_disable()/
 * preempt_enable_no_resched_notrace() pair instead of
 * preempt_disable()/preempt_enable().
 *
 * Why this exists (M19):
 *
 *   j716f_bc() publishes the record and *then* calls preempt_enable().  If
 *   the caller's preempt count reaches zero and TIF_NEED_RESCHED is set,
 *   that preempt_enable() calls preempt_schedule() -> __schedule().  A
 *   visible record therefore only proves that the payload and generation
 *   stores completed; the CPU may have left the instrumented function in
 *   the breadcrumb's own scheduling path.
 *
 *   The M18 conclusion ("free_unref_folios_ret was recorded, so the exit
 *   preempt_enable() completed and the scheduler is excluded") depends on
 *   exactly that inference and does not hold.  A quiet record removes the
 *   ambiguity: it cannot enter the scheduler, so the next record (or its
 *   absence) is attributable to the instrumented code alone.
 *
 * Preemption is still disabled while the ring slot is written, so the
 * per-CPU sequence counter can never be re-entered by a migration or by a
 * preemption on this CPU.  IRQ/NMI re-entry is handled as in j716f_bc().
 */
void j716f_bc_quiet(enum j716f_bc_site site, unsigned long arg)
{
	struct j716f_bc_cpu *c;
	struct j716f_bc_rec *r;
	bool nmi = in_nmi();
	u64 seq;

	if (!nmi)
		preempt_disable_notrace();
	c = this_cpu_ptr(&j716f_bc_data);
	seq = atomic64_fetch_inc(&c->seq);
	r = &c->ring[seq & (J716F_BC_RING - 1)];

	WRITE_ONCE(r->gen, J716F_BC_GEN_BUSY);
	smp_wmb();
	r->arg = arg;
	r->flags = (irqs_disabled() ? J716F_BC_F_IRQOFF : 0) |
		   (in_hardirq() ? J716F_BC_F_HARDIRQ : 0) |
		   (in_nmi() ? J716F_BC_F_NMI : 0);
	r->pid = current->pid;
	strscpy(r->comm, current->comm, sizeof(r->comm));
	r->site = site;
	smp_store_release(&r->ts, j716f_now());
	smp_store_release(&r->gen, seq);

	if (!nmi)
		preempt_enable_no_resched_notrace();
}
EXPORT_SYMBOL_GPL(j716f_bc_quiet);

/* Takes the ring lock itself; defined below. */
static void j716f_vote_push(struct j716f_vote_ring *r,
			    const struct j716f_vote_rec *in);

/*
 * MC0 retention (M20-P1c).
 *
 * v57 classified records by address and remembered "this txid touched MC0" in
 * a direct-mapped array indexed by txid & 63.  Two things were wrong:
 *
 *   - a later txid hashing to the same slot silently evicted an earlier one,
 *     so that earlier transaction's DONE/RECLAIM no longer matched and its
 *     completion evidence never reached the MC0 ring.  The comment claiming
 *     collisions could only over-collect was simply false;
 *   - the client REQ record is written before the aggregate exists, so it can
 *     never carry the MC0 address and always stayed in the general ring, where
 *     ~120 commits/s wrap it in well under a second.  The one record that
 *     names the real requester was therefore the one record most likely lost.
 *
 * Both are fixed by one small table of MC0 transactions that are still in
 * flight.  An entry is created when the voter confirms the batch really
 * contains MC0, carries the original client's provenance, and is only freed
 * when tcs_tx_done() reclaims the TCS.  Consequences:
 *
 *   - a transaction that never completes keeps its entry, so its provenance
 *     and its trigger survive any amount of later ring traffic - the case the
 *     whole diagnostic exists for;
 *   - capacity is explicit (J716F_VOTE_MC0_TX_LEN concurrent MC0
 *     transactions).  Nothing in flight is ever evicted.  If the table is
 *     full the transaction is NOT tracked and the overflow counter says so,
 *     rather than a silent hash collision hiding it;
 *   - it is not a hash: lookup compares the full 32-bit txid, so there is no
 *     aliasing at all.
 *
 * Locking: mutations take j716f_mc0_tx_lock.  It is a leaf in both callers -
 * the commit path holds icc_bw_lock/voter->lock mutexes, the completion path
 * holds no RSC lock (RECLAIM is recorded after drv->lock is dropped).  Lookups
 * used for ring classification are lockless: a stale "not found" only means a
 * record stays in the general ring, never that evidence is invented.
 */
#define J716F_VOTE_MC0_TX_LEN	32
#define J716F_VOTE_PATH_LEN	20

struct j716f_mc0_tx {
	u32 txid;		/* 0 = slot free */
	u32 pending;		/* MC0 triggers issued, decremented on RECLAIM */
	u32 promoted;		/* REQ_MC0 already emitted for this txid */
	u32 tcs;		/* last TCS it was triggered on */
	u32 mc0_idx;		/* MC0's index in the batch, NONE if unknown */
	u32 addr;
	u32 data;
	u32 avg_bw;
	u32 peak_bw;
	u32 pid;
	u32 cpu;
	u64 req_ts;		/* when the client called icc_set_bw() */
	u64 trig_ts;		/* when the TCS trigger was written */
	char comm[J716F_VOTE_COMM_LEN];
	char path[J716F_VOTE_PATH_LEN];
};

static struct j716f_mc0_tx j716f_mc0_txs[J716F_VOTE_MC0_TX_LEN];
static DEFINE_SPINLOCK(j716f_mc0_tx_lock);
/*
 * Counters are atomic: the commit path can run on several CPUs through
 * different voters at once, and a diagnostics counter that silently loses
 * increments cannot be used to argue "this never happened".
 */
static atomic_t j716f_mc0_tx_overflow = ATOMIC_INIT(0);	/* no free slot */
static atomic_t j716f_vote_agg_unattr = ATOMIC_INIT(0);	/* MC0 commit, no request */

/*
 * The client request that is currently being committed.
 *
 * icc_set_bw() writes it while holding icc_bw_lock and the BCM voter reads it
 * inside the same lock, so it always describes the commit that is running -
 * this is a single-slot handoff, not a global "current transaction" that any
 * unrelated RPMh sender could inherit.  rpmh_write() and the wake/sleep
 * flushes never touch it.
 *
 * If a future commit path ever runs outside icc_bw_lock the txid check below
 * fails and the entry is marked "provenance unknown" instead of being filled
 * with somebody else's values.
 */
static struct {
	u32 txid;
	u32 avg_bw;
	u32 peak_bw;
	u32 node_id;
	u32 pid;
	u32 cpu;
	u64 ts;
	char comm[J716F_VOTE_COMM_LEN];
	char path[J716F_VOTE_PATH_LEN];
} j716f_cur_req;

/* Caller holds j716f_mc0_tx_lock. */
static struct j716f_mc0_tx *j716f_mc0_tx_find(u32 txid)
{
	int i;

	if (!txid)
		return NULL;
	for (i = 0; i < J716F_VOTE_MC0_TX_LEN; i++)
		if (j716f_mc0_txs[i].txid == txid)
			return &j716f_mc0_txs[i];
	return NULL;
}

/* Lockless lookup used only to classify a record; see the locking note. */
static bool j716f_vote_txid_touched_mc0(u32 txid)
{
	int i;

	if (!txid)
		return false;
	for (i = 0; i < J716F_VOTE_MC0_TX_LEN; i++)
		if (READ_ONCE(j716f_mc0_txs[i].txid) == txid)
			return true;
	return false;
}

/*
 * Record a client request for later promotion.  Called from icc_set_bw()
 * under icc_bw_lock, before the voter runs.
 */
void j716f_vote_note_request(u32 txid, u32 avg_bw, u32 peak_bw, u32 node_id,
			     const char *path_name)
{
	if (!READ_ONCE(vote_trace) || !txid)
		return;

	j716f_cur_req.txid = txid;
	j716f_cur_req.avg_bw = avg_bw;
	j716f_cur_req.peak_bw = peak_bw;
	j716f_cur_req.node_id = node_id;
	j716f_cur_req.pid = current->pid;
	j716f_cur_req.cpu = raw_smp_processor_id();
	j716f_cur_req.ts = j716f_now();
	strscpy(j716f_cur_req.comm, current->comm, sizeof(j716f_cur_req.comm));
	strscpy(j716f_cur_req.path, path_name ? path_name : "-",
		sizeof(j716f_cur_req.path));
}
EXPORT_SYMBOL_GPL(j716f_vote_note_request);

/**
 * j716f_vote_mc0_commit - the batch that is about to be sent contains MC0
 * @txid: transaction id of the icc_set_bw() call that caused this commit
 * @idx:  command index carrying MC0 within the batch
 * @addr: MC0's resource address as it appeared in the batch
 * @data: the raw vote word the voter produced for MC0
 *
 * Called once per commit from qcom_icc_bcm_voter_commit() after it has looked
 * at every command.  Creates the in-flight entry and emits the promoted
 * request record, which is the only record that carries the original client's
 * pid/comm/cpu/bandwidth/timestamp.
 */
void j716f_vote_mc0_commit(u32 txid, u32 idx, u32 addr, u32 data)
{
	struct j716f_mc0_tx *t;
	struct j716f_mc0_tx snap;
	unsigned long flags;
	bool have_prov, emit;

	if (!READ_ONCE(vote_trace))
		return;

	if (!txid) {
		/*
		 * A commit with no client request behind it (a set that did not
		 * change any bandwidth still re-runs the voter).  These arrive
		 * once per frame per CRTC and would flush the MC0 ring, so they
		 * are counted instead of stored.
		 */
		atomic_inc(&j716f_vote_agg_unattr);
		return;
	}

	spin_lock_irqsave(&j716f_mc0_tx_lock, flags);
	t = j716f_mc0_tx_find(txid);
	/*
	 * One icc_set_bw() can commit more than once (apply_constraints() runs
	 * per provider), so an existing entry must keep its outstanding-trigger
	 * count.  Only the provenance fields are refreshed, and only when the
	 * handoff still belongs to this transaction; otherwise they are left
	 * as they are rather than being replaced by another client's values.
	 */
	have_prov = j716f_cur_req.txid == txid;
	if (!t) {
		int i;

		for (i = 0; i < J716F_VOTE_MC0_TX_LEN; i++) {
			if (!j716f_mc0_txs[i].txid) {
				t = &j716f_mc0_txs[i];
				break;
			}
		}
		if (t) {
			memset(t, 0, sizeof(*t));
			t->txid = txid;
			t->mc0_idx = J716F_VOTE_MC0_NONE;
			if (have_prov) {
				t->avg_bw = j716f_cur_req.avg_bw;
				t->peak_bw = j716f_cur_req.peak_bw;
				t->pid = j716f_cur_req.pid;
				t->cpu = j716f_cur_req.cpu;
				t->req_ts = j716f_cur_req.ts;
				memcpy(t->comm, j716f_cur_req.comm, sizeof(t->comm));
				memcpy(t->path, j716f_cur_req.path, sizeof(t->path));
			}
		}
	}
	if (!t) {
		/*
		 * Every slot is held by a transaction that has not completed.
		 * Refusing to track this one is the honest outcome; the
		 * alternative would be evicting live evidence.
		 */
		atomic_inc(&j716f_mc0_tx_overflow);
		spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);
		return;
	}

	t->mc0_idx = idx;
	t->addr = addr;
	t->data = data;
	if (have_prov && !t->pid) {
		t->avg_bw = j716f_cur_req.avg_bw;
		t->peak_bw = j716f_cur_req.peak_bw;
		t->pid = j716f_cur_req.pid;
		t->cpu = j716f_cur_req.cpu;
		t->req_ts = j716f_cur_req.ts;
		memcpy(t->comm, j716f_cur_req.comm, sizeof(t->comm));
		memcpy(t->path, j716f_cur_req.path, sizeof(t->path));
	}

	/*
	 * Emit the promoted request exactly once per transaction.  A wide
	 * icc_set_bw() commits per provider, so without this guard the ring
	 * would show the same client request several times and a reader would
	 * count them as separate MC0 events.
	 */
	emit = !t->promoted;
	t->promoted = 1;
	snap = *t;
	spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);

	if (!emit)
		return;

	/*
	 * Emit the promoted request outside the table lock: it only needs the
	 * ring's own lock, and keeping the two apart means neither is ever held
	 * while the other is taken.
	 */
	{
		struct j716f_vote_rec rec;

		memset(&rec, 0, sizeof(rec));
		rec.ts = have_prov ? snap.req_ts : j716f_now();
		rec.kind = J716F_VOTE_KIND_REQ_MC0;
		rec.cpu = have_prov ? snap.cpu : raw_smp_processor_id();
		rec.pid = snap.pid;
		rec.txid = txid;
		rec.addr = addr;
		rec.data = data;
		rec.aux = snap.peak_bw;
		rec.tcs = J716F_VOTE_NO_TCS;
		memcpy(rec.comm, snap.comm, sizeof(rec.comm));
		j716f_vote_push(&j716f_votes_mc0, &rec);
	}
}
EXPORT_SYMBOL_GPL(j716f_vote_mc0_commit);

/*
 * Ring classification.  Only records that can be tied to MC0 by resource or
 * by a tracked transaction enter the retention ring.  The MC0 address being
 * unresolved admits nothing, so a failed Command DB lookup cannot silently
 * turn this back into "keep everything".
 */
static bool j716f_vote_is_important(enum j716f_vote_kind kind, u32 txid,
				    u32 addr)
{
	u32 mc0 = READ_ONCE(j716f_mc0_addr);

	if (!mc0)
		return false;
	if (addr == mc0)
		return txid != 0;
	/*
	 * DONE/RECLAIM belong to a transaction that was seen carrying MC0;
	 * REQ never can (it predates the aggregate) and is only promoted
	 * through REQ_MC0.
	 */
	if (kind == J716F_VOTE_KIND_DONE || kind == J716F_VOTE_KIND_RECLAIM)
		return j716f_vote_txid_touched_mc0(txid);
	return false;
}

/* Copy one record and update the ring bookkeeping; caller holds the lock. */
static void j716f_vote_push_locked(struct j716f_vote_ring *r,
				   const struct j716f_vote_rec *in)
{
	u32 slot = r->head;

	r->rec[slot] = *in;
	r->rec[slot].seq = r->count + r->overwritten;
	r->head = (slot + 1) & (J716F_VOTE_RING - 1);
	if (r->count < J716F_VOTE_RING)
		r->count++;
	else
		r->overwritten++;
	r->generation++;
}

/* Take the ring lock, push, release.  Never called with another lock held. */
static void j716f_vote_push(struct j716f_vote_ring *r,
			    const struct j716f_vote_rec *in)
{
	unsigned long flags;

	spin_lock_irqsave(&r->lock, flags);
	j716f_vote_push_locked(r, in);
	spin_unlock_irqrestore(&r->lock, flags);
}

/**
 * j716f_vote_set_mc0 - remember MC0's RPMh address for ring classification
 * @addr: address from cmd_db_read_addr("MC0"), 0 if unavailable
 *
 * Called from init after the Command DB is ready.  Kept as a setter rather
 * than a hard-coded constant so a firmware that moves the resource cannot
 * silently disable the MC0 retention ring.
 */
void j716f_vote_set_mc0(u32 addr)
{
	WRITE_ONCE(j716f_mc0_addr, addr);
}
EXPORT_SYMBOL_GPL(j716f_vote_set_mc0);

u32 j716f_vote_mc0_addr(void)
{
	return READ_ONCE(j716f_mc0_addr);
}
EXPORT_SYMBOL_GPL(j716f_vote_mc0_addr);

/**
 * j716f_vote_alloc_txid - allocate a transaction id for one RPMh round trip
 *
 * Called by icc_set_bw() before it records the client request.  The id is
 * carried through the BCM commit, the TCS trigger and the completion so a
 * dump can line the four up even when other transactions interleave.
 */
u32 j716f_vote_alloc_txid(void)
{
	if (!READ_ONCE(vote_trace))
		return 0;
	return (u32)atomic_inc_return(&j716f_vote_txid);
}
EXPORT_SYMBOL_GPL(j716f_vote_alloc_txid);

/**
 * j716f_vote_record - remember one MC0/RPMh vote event
 * @kind: which boundary produced the event
 * @txid: transaction id from j716f_vote_alloc_txid(), 0 if unknown
 * @addr: BCM/RPMh resource address (0 for a client request)
 * @data: raw TCS data word, i.e. commit/valid/vote_x/vote_y as encoded
 * @tcs:  TCS id when the event came from the RSC layer, else J716F_VOTE_NO_TCS
 * @aux:  kind specific extra value
 *
 * Called from icc_set_bw() (client boundary), the BCM commit path,
 * rpmh_rsc_send_data() and tcs_tx_done().  The first three run in plain
 * process context; the completion is recorded from hardirq context, which is
 * why the critical section contains only stores and never strscpy().
 *
 * No allocation, no printk, no sleeping call.  Bounded rings: old entries are
 * overwritten and counted, so the dump can admit that history was lost.
 */
void j716f_vote_record(enum j716f_vote_kind kind, u32 txid, u32 addr, u32 data,
		       u32 tcs, u32 aux)
{
	struct j716f_vote_rec rec;

	if (!READ_ONCE(vote_trace))
		return;

	memset(&rec, 0, sizeof(rec));
	rec.ts = j716f_now();
	rec.kind = kind;
	rec.cpu = raw_smp_processor_id();
	rec.pid = current->pid;
	rec.txid = txid;
	rec.addr = addr;
	rec.data = data;
	rec.tcs = tcs;
	rec.aux = aux;
	/*
	 * The completion records come from hardirq context.  strscpy() is
	 * safe there, but it reads current->comm for a name that is always
	 * some interrupt-time task, so skip it and keep the IRQ path to a
	 * handful of stores.
	 */
	if (kind != J716F_VOTE_KIND_DONE && kind != J716F_VOTE_KIND_RECLAIM)
		strscpy(rec.comm, current->comm, sizeof(rec.comm));

	if (j716f_vote_is_important(kind, txid, addr))
		j716f_vote_push(&j716f_votes_mc0, &rec);

	j716f_vote_push(&j716f_votes, &rec);
}
EXPORT_SYMBOL_GPL(j716f_vote_record);

/**
 * j716f_vote_scan_mc0 - find MC0 anywhere in a TCS batch
 * @msg: the request that is about to be triggered
 *
 * Returns the command index that carries the MC0 resource, or
 * J716F_VOTE_MC0_NONE.  v57 only ever looked at cmds[0], so an MC0 vote that
 * the voter packed behind other BCMs produced no trigger record at all and
 * the ring showed a completed transaction with no visible trigger.
 *
 * One pass over at most MAX_RPMH_PAYLOAD (16) entries, no allocation, called
 * once per batch from rpmh_rsc_send_data().
 */
u32 j716f_vote_scan_mc0(const struct tcs_request *msg)
{
	u32 mc0 = READ_ONCE(j716f_mc0_addr);
	u32 i;

	if (!mc0 || !msg || !msg->cmds)
		return J716F_VOTE_MC0_NONE;

	for (i = 0; i < msg->num_cmds && i < MAX_RPMH_PAYLOAD; i++)
		if (msg->cmds[i].addr == mc0)
			return i;
	return J716F_VOTE_MC0_NONE;
}
EXPORT_SYMBOL_GPL(j716f_vote_scan_mc0);

/**
 * j716f_vote_record_tcs - record one TCS trigger boundary for a batch
 * @kind: TCS_PRE or TCS_POKED
 * @msg: the batch being triggered
 * @tcs: the TCS id it was assigned
 *
 * The record is emitted once per batch, not once per command: a batch is one
 * hardware trigger, so one record is the honest granularity.  It carries the
 * MC0 command's index, address and data word, so a batch where MC0 sat in the
 * middle is still recognisable, plus the batch size and whether a response
 * was requested.
 *
 * Also counts the trigger on the in-flight transaction.  It is a counter, not
 * a flag: one icc_set_bw() can commit several times (apply_constraints() runs
 * once per provider along the path, and a wide commit splits into more than
 * one TCS batch), and the entry must not be freed by the first RECLAIM while
 * a second trigger is still outstanding.
 */
void j716f_vote_record_tcs(enum j716f_vote_kind kind,
			   const struct tcs_request *msg, u32 tcs)
{
	struct j716f_mc0_tx *t;
	unsigned long flags;
	u32 idx, aux, addr, data;

	if (!READ_ONCE(vote_trace) || !msg)
		return;

	idx = j716f_vote_scan_mc0(msg);
	/*
	 * Batches without MC0 are not recorded at all.  The DPU commits ~120
	 * batches/s; one trigger record each would wrap the general ring in
	 * half a second and bury the MC0 evidence this diagnostic exists for.
	 * Their TCS registers are still printed by the RSC dump.
	 */
	if (idx == J716F_VOTE_MC0_NONE)
		return;

	addr = msg->cmds[idx].addr;
	data = msg->cmds[idx].data;
	aux = (msg->num_cmds & 0xffu) |
	      ((msg->wait_for_compl ? 1u : 0u) << 8) |
	      (idx << 16);

	j716f_vote_record(kind, msg->j716f_txid, addr, data, tcs, aux);

	if (!msg->j716f_txid)
		return;

	spin_lock_irqsave(&j716f_mc0_tx_lock, flags);
	t = j716f_mc0_tx_find(msg->j716f_txid);
	if (t) {
		t->tcs = tcs;
		t->mc0_idx = idx;
		t->addr = addr;
		t->data = data;
		if (kind == J716F_VOTE_KIND_TCS_POKED) {
			if (t->pending != U32_MAX)
				t->pending++;
			t->trig_ts = j716f_now();
		}
	}
	spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);
}
EXPORT_SYMBOL_GPL(j716f_vote_record_tcs);

/**
 * j716f_vote_tx_release - one MC0 trigger of a transaction was reclaimed
 * @txid: transaction id read from drv->txid[] by the handler
 *
 * Decrements the outstanding-trigger count and frees the entry only when the
 * last one is reclaimed.  A transaction that never completes therefore keeps
 * its entry (and its provenance) forever, which is exactly the case a hang
 * dump must show.
 *
 * A RECLAIM for an id that is not tracked is NOT an error and is not counted:
 * the handler runs for every RPMh transaction, and the overwhelming majority
 * never touched MC0.  v57 would have had to guess here; this version simply
 * has nothing to do.
 */
void j716f_vote_tx_release(u32 txid)
{
	struct j716f_mc0_tx *t;
	unsigned long flags;

	if (!READ_ONCE(vote_trace) || !txid)
		return;

	spin_lock_irqsave(&j716f_mc0_tx_lock, flags);
	t = j716f_mc0_tx_find(txid);
	if (t) {
		if (t->pending > 1)
			t->pending--;
		else
			memset(t, 0, sizeof(*t));
	}
	spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);
}
EXPORT_SYMBOL_GPL(j716f_vote_tx_release);

/**
 * j716f_vote_decode - decode one BCM TCS data word
 * @data: raw command data as written into the TCS
 * @valid: out, command valid bit
 * @commit: out, commit bit
 * @vote_x: out, 14-bit vote_x field
 * @vote_y: out, 14-bit vote_y field
 *
 * The encoding is BCM_TCS_CMD() in include/soc/qcom/tcs.h.  Keeping the raw
 * word in the record and decoding only at dump time means a future encoding
 * change cannot retroactively corrupt the captured evidence.
 */
void j716f_vote_decode(u32 data, bool *valid, bool *commit, u32 *vote_x,
		       u32 *vote_y)
{
	if (valid)
		*valid = !!(data & BCM_TCS_CMD_VALID_MASK);
	if (commit)
		*commit = !!(data & BCM_TCS_CMD_COMMIT_MASK);
	if (vote_x)
		*vote_x = (data & BCM_TCS_CMD_VOTE_X_MASK) >> 14;
	if (vote_y)
		*vote_y = data & BCM_TCS_CMD_VOTE_Y_MASK;
}
EXPORT_SYMBOL_GPL(j716f_vote_decode);

/**
 * j716f_vote_dump - print the vote provenance ring
 *
 * Called from the panic notifier and from /proc/j716f_diag.
 *
 * The lock is taken with trylock, not spin_lock: if a CPU wedged while
 * holding it (exactly the kind of hang this is meant to diagnose), blocking
 * here would turn the diagnostic into a second, permanent stall and lose
 * every dump line after it.  On contention the ring cannot be read
 * consistently, so the dump says so and skips it rather than printing
 * half-written records.
 */
static const char *j716f_vote_kind_name(u32 kind)
{
	switch (kind) {
	case J716F_VOTE_KIND_REQ:	return "req";
	case J716F_VOTE_KIND_REQ_MC0:	return "req-mc0";
	case J716F_VOTE_KIND_AGG:	return "agg";
	case J716F_VOTE_KIND_TCS_PRE:	return "tcs-pre";
	case J716F_VOTE_KIND_TCS_POKED:	return "tcs-poked";
	case J716F_VOTE_KIND_DONE:	return "done";
	case J716F_VOTE_KIND_RECLAIM:	return "reclaim";
	default:			return "?";
	}
}

/*
 * Ring dump (M20-P1c).
 *
 * v57 bracketed the chunks with the ring generation and required
 * "gen_after - gen_before == off + n".  That test cannot work: the generation
 * also advances while the previous chunk is being *printed*, so under any
 * real load it reports "snapshot inconsistent" for a perfectly coherent ring;
 * and if the advancement happened to equal n it would call a mixed window
 * coherent.  It measured the wrong thing.
 *
 * What actually identifies a record is its own sequence number, which the
 * writer assigns from a counter that only ever increases.  Each copied slot
 * must hold exactly the seq we expect for that ring position; a recycled slot
 * holds a strictly larger one.  So:
 *
 *   - a slot whose seq does not match was overwritten -> the printed set is
 *     NOT the window we started from -> "snapshot inconsistent";
 *   - a slot that matches is that record, byte for byte, whenever it was
 *     printed;
 *   - the generation delta is reported separately as "moved", because a ring
 *     that advanced but whose slots all still match is a coherent set from a
 *     sliding window, not a corrupt one.
 *
 * The copy is 8 records at a time under trylock, so the stack stays small and
 * a wedged writer can never block the dump.
 */
static void j716f_vote_dump_ring(struct j716f_vote_ring *r, const char *tag,
				 u64 now)
{
	struct j716f_vote_rec chunk[8];
	unsigned long flags;
	u32 i, n, count, start, lost, off, gen_start, gen_now, moved;
	u32 lost_now;
	bool inconsistent = false;
	bool busy = false;

	if (!spin_trylock_irqsave(&r->lock, flags)) {
		pr_info("J716F VOTE %s: ring lock busy, records skipped (undecidable)\n",
			tag);
		return;
	}
	count = r->count;
	lost = r->overwritten;
	gen_start = r->generation;
	/*
	 * The writer assigns seq = count + overwritten, i.e. a global push
	 * counter, and stores at slot seq & (RING-1).  The oldest record still
	 * present is therefore seq == overwritten; using the global counter
	 * (not the masked head) is what makes the per-slot check below exact.
	 */
	start = lost;
	spin_unlock_irqrestore(&r->lock, flags);

	for (off = 0; off < count; off += n) {
		n = min_t(u32, ARRAY_SIZE(chunk), count - off);

		if (!spin_trylock_irqsave(&r->lock, flags)) {
			pr_info("J716F VOTE %s: lock busy after %u of %u records (undecidable)\n",
				tag, off, count);
			busy = true;
			inconsistent = true;
			break;
		}
		for (i = 0; i < n; i++) {
			u32 pos = (start + off + i) & (J716F_VOTE_RING - 1);

			chunk[i] = r->rec[pos];
			/*
			 * The slot must still hold the record that belongs at
			 * this sequence number.  Anything else means it was
			 * recycled between the header read and this copy.
			 */
			if (chunk[i].seq != start + off + i)
				inconsistent = true;
		}
		spin_unlock_irqrestore(&r->lock, flags);

		for (i = 0; i < n; i++) {
			const struct j716f_vote_rec *v = &chunk[i];
			const char *cls = !v->txid ? "unattr" : "tx";
			u32 aux = v->aux;

			/*
			 * TCS records pack the batch layout into aux; spell it
			 * out so "MC0 was command 5 of 9" cannot be misread as
			 * "MC0 was the trigger command".
			 */
			if (v->kind == J716F_VOTE_KIND_TCS_PRE ||
			    v->kind == J716F_VOTE_KIND_TCS_POKED)
				pr_info("J716F VOTE %s %-9s tx=%u seq=%u %s cpu%u pid=%u comm=%s addr=0x%x tcs=%d data=0x%08x cmds=%u mc0=%d wait=%u age=%llums\n",
					tag, j716f_vote_kind_name(v->kind), v->txid,
					v->seq, cls, v->cpu, v->pid, v->comm,
					v->addr,
					v->tcs == J716F_VOTE_NO_TCS ? -1 : (int)v->tcs,
					v->data,
					J716F_VOTE_AUX_NCMDS(aux),
					J716F_VOTE_AUX_MC0IDX(aux) == J716F_VOTE_MC0_NONE ?
						-1 : (int)J716F_VOTE_AUX_MC0IDX(aux),
					J716F_VOTE_AUX_WAIT(aux),
					j716f_age(now, v->ts) / NSEC_PER_MSEC);
			else
				pr_info("J716F VOTE %s %-9s tx=%u seq=%u %s cpu%u pid=%u comm=%s addr=0x%x tcs=%d data=0x%08x aux=0x%x age=%llums\n",
					tag, j716f_vote_kind_name(v->kind), v->txid,
					v->seq, cls, v->cpu, v->pid, v->comm,
					v->addr,
					v->tcs == J716F_VOTE_NO_TCS ? -1 : (int)v->tcs,
					v->data, aux,
					j716f_age(now, v->ts) / NSEC_PER_MSEC);
		}
	}

	if (!spin_trylock_irqsave(&r->lock, flags)) {
		gen_now = gen_start;
		lost_now = lost;
		moved = 0;
		inconsistent = true;
	} else {
		gen_now = r->generation;
		lost_now = r->overwritten;
		moved = gen_now - gen_start;
		spin_unlock_irqrestore(&r->lock, flags);
	}

	pr_info("J716F VOTE %s: %u/%u records, %u overwritten, gen=%u..%u moved=%u lost_during_dump=%u%s%s%s\n",
		tag, count, J716F_VOTE_RING, lost, gen_start, gen_now, moved,
		lost_now - lost,
		lost ? " (history truncated)" : "",
		moved ? " (window advanced)" : "",
		busy ? " (ring lock busy)" : "");

	/*
	 * Say it loudly and last, so it is not missed between record lines.
	 * A record that failed its seq check is not this window's record, so
	 * the whole printed set is unusable for causal reasoning.
	 */	if (inconsistent)
		pr_info("J716F VOTE %s: snapshot inconsistent (records may span two windows) - undecidable\n",
			tag);
}

/*
 * In-flight MC0 transactions.
 *
 * This is the part that survives a hang.  A transaction whose TCS was never
 * reclaimed keeps its slot, so its client provenance, its trigger time and its
 * TCS id are still there no matter how much later traffic filled the rings.
 *
 * Copied and printed 8 entries at a time: the whole table is ~3 KiB, and this
 * runs in the panic path where a large stack frame is not acceptable.
 */
#define J716F_VOTE_INFLIGHT_CHUNK	8

static void j716f_vote_dump_inflight(u64 now)
{
	struct j716f_mc0_tx chunk[J716F_VOTE_INFLIGHT_CHUNK];
	unsigned long flags;
	u32 i, off, n, live = 0;
	u32 overflow = (u32)atomic_read(&j716f_mc0_tx_overflow);
	u32 unattr = (u32)atomic_read(&j716f_vote_agg_unattr);
	bool busy = false;

	if (!spin_trylock_irqsave(&j716f_mc0_tx_lock, flags)) {
		pr_info("J716F VOTE inflight: table lock busy, entries skipped (undecidable)\n");
		return;
	}
	for (i = 0; i < J716F_VOTE_MC0_TX_LEN; i++)
		if (j716f_mc0_txs[i].txid)
			live++;
	spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);

	pr_info("J716F VOTE inflight: %u/%u slots in use, overflow=%u unattr_commits=%u%s\n",
		live, J716F_VOTE_MC0_TX_LEN, overflow, unattr,
		overflow ? " (MC0 batch not tracked: table full)" : "");

	for (off = 0; off < J716F_VOTE_MC0_TX_LEN; off += n) {
		n = min_t(u32, J716F_VOTE_INFLIGHT_CHUNK,
			  J716F_VOTE_MC0_TX_LEN - off);

		if (!spin_trylock_irqsave(&j716f_mc0_tx_lock, flags)) {
			pr_info("J716F VOTE inflight: lock busy after %u entries (undecidable)\n",
				off);
			busy = true;
			break;
		}
		memcpy(chunk, &j716f_mc0_txs[off], n * sizeof(chunk[0]));
		spin_unlock_irqrestore(&j716f_mc0_tx_lock, flags);

		for (i = 0; i < n; i++) {
			const struct j716f_mc0_tx *t = &chunk[i];

			if (!t->txid)
				continue;
			pr_info("J716F VOTE inflight tx=%u pid=%u comm=%s cpu%u path=%s avg=%u peak=%u tcs=%d triggers=%u mc0=%d addr=0x%x data=0x%08x req_age=%llums trig_age=%llums\n",
				t->txid, t->pid, t->comm, t->cpu, t->path,
				t->avg_bw, t->peak_bw,
				t->pending, t->tcs,
				t->mc0_idx == J716F_VOTE_MC0_NONE ? -1 : (int)t->mc0_idx,
				t->addr, t->data,
				t->req_ts ? j716f_age(now, t->req_ts) / NSEC_PER_MSEC : 0,
				t->trig_ts ? j716f_age(now, t->trig_ts) / NSEC_PER_MSEC : 0);
		}
	}
	if (busy)
		pr_info("J716F VOTE inflight: snapshot incomplete - undecidable\n");
}

/**
 * j716f_vote_dump - print both vote provenance rings
 *
 * The MC0 ring is printed first: it is the one that survives a per-frame
 * vote burst, so it is the set to reason from.  The general ring is printed
 * for context and may be heavily truncated, which the loss count states.
 * The in-flight table comes last because it is the only source that is not
 * subject to ring wrap at all.
 */
void j716f_vote_dump(void)
{
	u64 now = j716f_now();

	pr_info("J716F VOTE dump: mc0_addr=0x%x txid_next=%d\n",
		READ_ONCE(j716f_mc0_addr),
		atomic_read(&j716f_vote_txid));
	j716f_vote_dump_ring(&j716f_votes_mc0, "mc0", now);
	j716f_vote_dump_ring(&j716f_votes, "all", now);
	j716f_vote_dump_inflight(now);
}
EXPORT_SYMBOL_GPL(j716f_vote_dump);

/*
 * M20-P1 IRQ accounting for the RSC completion IRQ.
 *
 * The question this answers: did the completion IRQ get delivered at all, to
 * which CPU, and when relative to the vote and to the CPUs going stale.  A
 * pending GIC line with a zero count here means the line never reached a
 * handler; a non-zero count with a stop time means it did and then stopped.
 *
 * Per-CPU counters only: no lock, no atomic RMW, no printk, safe from
 * hardirq context.  Reading them from another CPU is a plain load, which is
 * adequate for a coarse "how many times" answer.
 */
struct j716f_irq_stats {
	u64 last_enter;
	u64 last_exit;
	u32 in_handler;
	u32 pad;
};

static DEFINE_PER_CPU(struct j716f_irq_stats, j716f_irq_stat);
static unsigned int j716f_irq_nr;	/* the irq we account for */
static u32 j716f_irq_count[NR_CPUS];

void j716f_irq_enter(int irq)
{
	struct j716f_irq_stats *s = this_cpu_ptr(&j716f_irq_stat);

	if (irq != READ_ONCE(j716f_irq_nr))
		return;
	s->last_enter = j716f_now();
	s->in_handler++;
}

void j716f_irq_exit(int irq)
{
	struct j716f_irq_stats *s = this_cpu_ptr(&j716f_irq_stat);
	int cpu = raw_smp_processor_id();

	if (irq != READ_ONCE(j716f_irq_nr))
		return;
	s->last_exit = j716f_now();
	if (s->in_handler)
		s->in_handler--;
	if (cpu < NR_CPUS)
		WRITE_ONCE(j716f_irq_count[cpu],
			   READ_ONCE(j716f_irq_count[cpu]) + 1);
}

/**
 * j716f_irq_set - remember which IRQ to account for
 * @irq: the RSC completion IRQ
 *
 * Called once from the RSC probe, in process context.
 */
void j716f_irq_set(int irq)
{
	WRITE_ONCE(j716f_irq_nr, irq);
}
EXPORT_SYMBOL_GPL(j716f_irq_set);

void j716f_irq_dump(void)
{
	u64 now = j716f_now();
	int cpu;

	pr_info("J716F IRQ dump: accounting irq=%u\n", READ_ONCE(j716f_irq_nr));
	for_each_online_cpu(cpu) {
		struct j716f_irq_stats *s = per_cpu_ptr(&j716f_irq_stat, cpu);

		pr_info("J716F IRQ cpu%d count=%u in_handler=%u last_enter_age=%llums last_exit_age=%llums\n",
			cpu, READ_ONCE(j716f_irq_count[cpu]),
			READ_ONCE(s->in_handler),
			s->last_enter ? j716f_age(now, s->last_enter) / NSEC_PER_MSEC : 0,
			s->last_exit ? j716f_age(now, s->last_exit) / NSEC_PER_MSEC : 0);
	}
}
EXPORT_SYMBOL_GPL(j716f_irq_dump);

/**
 * j716f_bc - record one breadcrumb on the current CPU
 * @site: instrumented call site
 * @arg: site specific value (register, id, address, ...)
 *
 * Safe from hardirq/NMI context.  The record is written before the
 * blocking access so a hang leaves the call site visible.
 *
 * Note: the record is published *before* this function's own
 * preempt_enable(), which is a scheduling point (see j716f_bc_quiet()).
 * Any site whose "did we get past it" semantics matter must use
 * j716f_bc_quiet() instead.
 */
void j716f_bc(enum j716f_bc_site site, unsigned long arg)
{
	struct j716f_bc_cpu *c;
	struct j716f_bc_rec *r;
	bool nmi = in_nmi();
	u64 seq;

	/*
	 * NMI context already has preemption disabled (nmi_enter() adds the
	 * NMI offset); touching the preempt count there is unnecessary.  An
	 * IRQ can also re-enter this function, but its disable/enable pair is
	 * balanced, and the sequence counter is a per-CPU atomic so the two
	 * writers cannot lose each other's generation.
	 */
	if (!nmi)
		preempt_disable();
	c = this_cpu_ptr(&j716f_bc_data);
	seq = atomic64_fetch_inc(&c->seq);
	r = &c->ring[seq & (J716F_BC_RING - 1)];

	/*
	 * Seqlock ordering: publish BUSY first, then make sure no payload
	 * store is visible before it.  Without the write barrier a reader on
	 * another CPU could see a new field while gen still looked like the
	 * previous generation, and accept a mixed record.
	 */
	WRITE_ONCE(r->gen, J716F_BC_GEN_BUSY);
	smp_wmb();
	r->arg = arg;
	r->flags = (irqs_disabled() ? J716F_BC_F_IRQOFF : 0) |
		   (in_hardirq() ? J716F_BC_F_HARDIRQ : 0) |
		   (in_nmi() ? J716F_BC_F_NMI : 0);
	r->pid = current->pid;
	strscpy(r->comm, current->comm, sizeof(r->comm));
	r->site = site;
	/* The release stores publish the payload and then the generation. */
	smp_store_release(&r->ts, j716f_now());
	smp_store_release(&r->gen, seq);

	if (!nmi)
		preempt_enable();
}
EXPORT_SYMBOL_GPL(j716f_bc);

/*
 * Copy one record and prove that the generation did not change while we
 * were reading it.  Returns false for a slot that a writer is recycling,
 * or for a record that was never completed (ts is the second to last
 * store, gen is the last one).
 */
static bool j716f_bc_read(int cpu, u64 seq, struct j716f_bc_rec *out)
{
	struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);
	struct j716f_bc_rec *r = &c->ring[seq & (J716F_BC_RING - 1)];

	if (smp_load_acquire(&r->gen) != seq)
		return false;

	out->arg = READ_ONCE(r->arg);
	out->flags = READ_ONCE(r->flags);
	out->pid = READ_ONCE(r->pid);
	out->site = READ_ONCE(r->site);
	memcpy(out->comm, r->comm, sizeof(out->comm));
	out->comm[sizeof(out->comm) - 1] = '\0';
	out->ts = smp_load_acquire(&r->ts);

	/* A writer may have started recycling the slot while we copied. */
	if (smp_load_acquire(&r->gen) != seq)
		return false;

	return out->ts != 0;
}

static void j716f_bc_dump_cpu(int cpu)
{
	struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);
	u64 seq = atomic64_read(&c->seq);
	u64 now = j716f_now();
	bool newest = true;
	int i;

	if (!seq)
		return;

	/*
	 * If the newest slot is BUSY the target stopped inside j716f_bc()
	 * while filling it.  Do not print its fields (they are not a
	 * consistent record), but do show that the writer was in progress;
	 * the loop below then reports the newest completed record.
	 */
	if (smp_load_acquire(&c->ring[(seq - 1) & (J716F_BC_RING - 1)].gen) ==
	    J716F_BC_GEN_BUSY)
		pr_info("J716F BC cpu%d seq=%llu site=<busy> (writer in progress)\n",
			cpu, (unsigned long long)(seq - 1));

	for (i = 0; i < J716F_BC_DUMP_DEPTH && i < seq; i++) {
		struct j716f_bc_rec r;
		u64 s = seq - 1 - i;
		char fl[4];

		if (!j716f_bc_read(cpu, s, &r))
			continue;

		fl[0] = (r.flags & J716F_BC_F_IRQOFF) ? 'I' : '-';
		fl[1] = (r.flags & J716F_BC_F_HARDIRQ) ? 'H' : '-';
		fl[2] = (r.flags & J716F_BC_F_NMI) ? 'N' : '-';
		fl[3] = '\0';

		pr_info("J716F BC cpu%d seq=%llu site=%s arg=0x%lx fl=%s pid=%u comm=%s age=%llums%s\n",
			cpu, (unsigned long long)s, j716f_bc_name(r.site),
			r.arg, fl, r.pid, r.comm,
			j716f_age(now, r.ts) / NSEC_PER_MSEC,
			newest ? " (current)" : "");
		newest = false;
	}
}

static void j716f_ct_dump_cpu(int cpu)
{
#if IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE)
	pr_info("J716F CT cpu%d watching=0x%x nesting=%ld nmi_nesting=%#lx\n",
		cpu, ct_rcu_watching_cpu(cpu) & 0xffff,
		ct_nesting_cpu(cpu), (unsigned long)ct_nmi_nesting_cpu(cpu));
#else
	pr_info("J716F CT cpu%d: context tracking idle disabled\n", cpu);
#endif
}

/*
 * ASID rollover/workaround evidence.  A long "no freeze" run without a
 * rollover does not exercise the 1542418 path at all, so the experiment
 * must show these counters moving on the A77 CPUs.
 */
static void j716f_asid_dump(const char *tag)
{
	int cpu;

	pr_info("J716F ASID %s: bits=%u generation=%llu rollovers=%llu\n",
		tag, arm64_asid_bits(),
		(unsigned long long)arm64_asid_generation(),
		(unsigned long long)arm64_asid_rollover_count());
	for_each_online_cpu(cpu)
		pr_info("J716F ASID cpu%d workarounds=%llu\n", cpu,
			(unsigned long long)arm64_asid_workaround_count(cpu));
}

#if IS_ENABLED(CONFIG_ARM_GIC_V3)
extern int gicv3_j716f_redist_state(int cpu, u32 *waker, u32 *pending, u32 *active);
extern void gicv3_j716f_dump_redist(int cpu);
#endif

/*
 * Return the newest record that is fully published.  The newest slot may be
 * busy (the target CPU is writing it right now), in which case the previous
 * record is still the last reliable position.
 */
static void j716f_bc_last(int cpu, struct j716f_bc_rec *out, u64 *seq_out)
{
	struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);
	u64 seq = atomic64_read(&c->seq);
	u64 i;

	memset(out, 0, sizeof(*out));
	*seq_out = 0;

	for (i = 0; i < J716F_BC_RING && i < seq; i++) {
		u64 s = seq - 1 - i;

		if (j716f_bc_read(cpu, s, out)) {
			*seq_out = s;
			return;
		}
	}
}

/*
 * M19: a per-boot tag.
 *
 * T is the monotonic clock, which restarts at zero on every boot, and the
 * dmesg/serial/snap.log streams from an experiment are concatenated by
 * hand.  Without a boot discriminator two boots cannot be told apart, and
 * the whole point of the ring is to compare "the last second before the
 * hang" across boots.
 *
 * The value is derived from the boot KASLR seed (which differs per boot and
 * needs no entropy at early boot) plus the kernel build time, then folded to
 * 4 hex digits.  It is printed once at startup and on every SNAP/SNAP-ALERT/
 * DIAG line.  It is deliberately not a real boot ID: it only has to be
 * stable within a boot and different across the boots of one experiment.
 */
static u32 j716f_boot_tag;

#define J716F_BOOT_TAG_FMT	"bt=%04x "
#define J716F_BOOT_TAG_ARG	(j716f_boot_tag & 0xffff)

static void j716f_snap_print(u64 now)
{
	char buf[1024];
	int len = 0, cpu;

	len += scnprintf(buf + len, sizeof(buf) - len,
			 "J716F SNAP " J716F_BOOT_TAG_FMT "t=%llus",
			 J716F_BOOT_TAG_ARG, div_u64(now, NSEC_PER_SEC));

	for_each_online_cpu(cpu) {
		struct j716f_bc_rec r;
		u64 seq;
		u64 beat = READ_ONCE(per_cpu(j716f_bc_data, cpu).heartbeat);

		j716f_bc_last(cpu, &r, &seq);
		if (!beat) {
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " cpu%d=nostart", cpu);
			continue;
		}
		if (j716f_age(now, beat) > J716F_STALE_NS)
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " cpu%d=STALE:%llums:%s", cpu,
					 j716f_age(now, beat) / NSEC_PER_MSEC,
					 j716f_bc_name(r.site));
		else
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " cpu%d=%s/%llums", cpu,
					 j716f_bc_name(r.site),
					 r.ts ? j716f_age(now, r.ts) / NSEC_PER_MSEC : 0);
	}
	len += scnprintf(buf + len, sizeof(buf) - len,
			 " asid=%u/r=%llu", arm64_asid_bits(),
			 (unsigned long long)arm64_asid_rollover_count());
	pr_info("%s\n", buf);
}

static void j716f_snap_alert(u64 now)
{
	unsigned int bt = READ_ONCE(bt_stage);
	unsigned int ps = READ_ONCE(panic_stage);
	int cpu;

	if (READ_ONCE(j716f_suspended) || now < READ_ONCE(j716f_snap_guard))
		return;

	if (bt > ARRAY_SIZE(j716f_alert_ms))
		bt = ARRAY_SIZE(j716f_alert_ms);
	if (ps > ARRAY_SIZE(j716f_alert_ms))
		ps = ARRAY_SIZE(j716f_alert_ms);

	for_each_online_cpu(cpu) {
		struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);
		struct j716f_bc_rec r;
		u64 seq, beat, age, threshold;
		u32 flags, stage = 0;
		int i;

		beat = READ_ONCE(c->heartbeat);
		if (!beat || !READ_ONCE(c->online) || cpu_dying(cpu))
			continue;

		age = j716f_age(now, beat);
		for (i = 0; i < ARRAY_SIZE(j716f_alert_ms); i++)
			if (age >= j716f_alert_ms[i] * NSEC_PER_MSEC)
				stage = i + 1;

		if (!stage) {
			WRITE_ONCE(c->alert_stage, 0);
			WRITE_ONCE(c->bt_stage, 0);
			continue;
		}
		if (stage <= READ_ONCE(c->alert_stage))
			continue;
		WRITE_ONCE(c->alert_stage, stage);

		j716f_bc_last(cpu, &r, &seq);
		flags = r.flags;
#if IS_ENABLED(CONFIG_ARM_GIC_V3)
		{
			u32 waker = 0, pending = 0, active = 0;
			int ret = gicv3_j716f_redist_state(cpu, &waker, &pending, &active);

			pr_info("J716F SNAP-ALERT " J716F_BOOT_TAG_FMT "t=%llus cpu%d stale=%llums stage=%u site=%s arg=0x%lx site_age=%llums fl=%c%c%c pid=%u comm=%s ct=%ld/%#lx gicr=%s waker=%08x pending=%08x active=%08x\n",
				J716F_BOOT_TAG_ARG, div_u64(now, NSEC_PER_SEC), cpu,
				age / NSEC_PER_MSEC, stage,
				j716f_bc_name(r.site), r.arg,
				r.ts ? j716f_age(now, r.ts) / NSEC_PER_MSEC : 0,
				(flags & J716F_BC_F_IRQOFF) ? 'I' : '-',
				(flags & J716F_BC_F_HARDIRQ) ? 'H' : '-',
				(flags & J716F_BC_F_NMI) ? 'N' : '-',
				r.pid, r.comm,
#if IS_ENABLED(CONFIG_CONTEXT_TRACKING_IDLE)
				ct_nesting_cpu(cpu),
				(unsigned long)ct_nmi_nesting_cpu(cpu),
#else
				0L, 0UL,
#endif
				ret ? "nomap" : "ok", waker, pending, active);
		}
#else
		pr_info("J716F SNAP-ALERT " J716F_BOOT_TAG_FMT "t=%llus cpu%d stale=%llums stage=%u site=%s arg=0x%lx site_age=%llums fl=%c%c%c pid=%u comm=%s\n",
			J716F_BOOT_TAG_ARG, div_u64(now, NSEC_PER_SEC), cpu, age / NSEC_PER_MSEC, stage,
			j716f_bc_name(r.site), r.arg,
			r.ts ? j716f_age(now, r.ts) / NSEC_PER_MSEC : 0,
			(flags & J716F_BC_F_IRQOFF) ? 'I' : '-',
			(flags & J716F_BC_F_HARDIRQ) ? 'H' : '-',
			(flags & J716F_BC_F_NMI) ? 'N' : '-',
			r.pid, r.comm);
#endif
		if (bt && stage >= bt &&
		    READ_ONCE(c->bt_stage) < stage) {
			WRITE_ONCE(c->bt_stage, stage);
			schedule_work(&per_cpu(j716f_bt_reqs, cpu).work);
		}

		if (ps && stage >= ps) {
			/*
			 * Re-read the heartbeat before resetting the box: a CPU
			 * that recovered between the read above and here must
			 * not trigger a panic.
			 */
			beat = READ_ONCE(c->heartbeat);
			threshold = j716f_alert_ms[ps - 1] * NSEC_PER_MSEC;
			if (!beat || j716f_age(j716f_now(), beat) < threshold)
				continue;
			panic("j716f: cpu%d heartbeat stopped for %llums (last site %s, comm %s)",
			      cpu, j716f_age(now, beat) / NSEC_PER_MSEC,
			      j716f_bc_name(r.site), r.comm);
		}
	}
}

static enum hrtimer_restart j716f_snap_fn(struct hrtimer *h)
{
	u64 now = j716f_now();
	int cpu = raw_smp_processor_id();
	unsigned int online = num_online_cpus();
	u64 tick = div_u64(now, NSEC_PER_SEC);
	struct j716f_bc_cpu *c = this_cpu_ptr(&j716f_bc_data);

	WRITE_ONCE(c->heartbeat, now);

	/* One printer per second, rotating so a stuck CPU cannot blind us. */
	if (online && (unsigned int)(tick % online) == (unsigned int)cpu) {
		if (snap_print)
			j716f_snap_print(now);
		j716f_snap_alert(now);
	}

	hrtimer_forward_now(h, ns_to_ktime(J716F_SNAP_PERIOD_NS));
	return HRTIMER_RESTART;
}

static void j716f_snap_start_cpu(void *unused)
{
	int cpu = raw_smp_processor_id();
	struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);
	struct hrtimer *h = per_cpu_ptr(&j716f_snap_timer, cpu);

	/*
	 * The timer is set up once per CPU.  On a hotplug round trip it was
	 * cancelled by the offline callback, so re-initialising it here would
	 * reinitialise a live object for no reason.
	 */
	if (!READ_ONCE(c->timer_ready)) {
		hrtimer_setup(h, j716f_snap_fn, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_PINNED_HARD);
		WRITE_ONCE(c->timer_ready, 1);
	}

	WRITE_ONCE(c->heartbeat, j716f_now());
	WRITE_ONCE(c->alert_stage, 0);
	WRITE_ONCE(c->bt_stage, 0);
	WRITE_ONCE(c->online, 1);
	hrtimer_start(h, ns_to_ktime(J716F_SNAP_PERIOD_NS),
		      HRTIMER_MODE_REL_PINNED_HARD);
}

static int j716f_snap_cpu_online(unsigned int cpu)
{
	if (cpu == raw_smp_processor_id())
		j716f_snap_start_cpu(NULL);
	else
		smp_call_function_single(cpu, j716f_snap_start_cpu, NULL, 1);
	return 0;
}

static int j716f_snap_cpu_offline(unsigned int cpu)
{
	struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);

	hrtimer_cancel(per_cpu_ptr(&j716f_snap_timer, cpu));

	/*
	 * The CPU is still in cpu_online_mask while the hotplug callbacks
	 * run.  Clear our own liveness state so the printer does not report
	 * a CPU that is deliberately going down as wedged.
	 */
	WRITE_ONCE(c->online, 0);
	WRITE_ONCE(c->heartbeat, 0);
	WRITE_ONCE(c->alert_stage, 0);
	WRITE_ONCE(c->bt_stage, 0);
	return 0;
}

static int j716f_snap_pm_notify(struct notifier_block *nb,
				unsigned long mode, void *data)
{
	int cpu;
	u64 now;

	switch (mode) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
	case PM_RESTORE_PREPARE:
		/*
		 * s2idle keeps the CPUs online and freezes hrtimers; without
		 * this the first timer after resume would panic on every
		 * sibling whose heartbeat is as old as the sleep.
		 */
		WRITE_ONCE(j716f_suspended, true);
		return NOTIFY_DONE;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
	case PM_POST_RESTORE:
		break;
	default:
		return NOTIFY_DONE;
	}

	now = j716f_now();
	WRITE_ONCE(j716f_snap_guard, now + 10ULL * NSEC_PER_SEC);
	for_each_online_cpu(cpu) {
		struct j716f_bc_cpu *c = per_cpu_ptr(&j716f_bc_data, cpu);

		if (!READ_ONCE(c->online))
			continue;
		WRITE_ONCE(c->heartbeat, now);
		WRITE_ONCE(c->alert_stage, 0);
		WRITE_ONCE(c->bt_stage, 0);
	}
	WRITE_ONCE(j716f_suspended, false);
	return NOTIFY_DONE;
}

static struct notifier_block j716f_snap_pm_nb = {
	.notifier_call = j716f_snap_pm_notify,
	.priority = 0,
};

#if IS_ENABLED(CONFIG_QCOM_RPMH)
extern void rpmh_rsc_panic_dump(void);
#endif

/**
 * j716f_diag_dump - print the per-CPU breadcrumbs and hang related state
 * @reason: short tag printed with the dump
 *
 * Called from the panic notifier (on the panicking CPU, interrupts off)
 * and from /proc/j716f_diag.  Everything that can block or wedge the
 * reading CPU is printed last.
 */
void j716f_diag_dump(const char *reason)
{
	int cpu;

	pr_info("J716F DIAG dump " J716F_BOOT_TAG_FMT "reason=%s cpu=%d\n",
		J716F_BOOT_TAG_ARG, reason, raw_smp_processor_id());

	j716f_snap_print(j716f_now());

	for_each_online_cpu(cpu)
		j716f_bc_dump_cpu(cpu);

	for_each_online_cpu(cpu)
		j716f_ct_dump_cpu(cpu);

	j716f_asid_dump("dump");

	/*
	 * M20-P1: vote provenance before any RSC register read.  The ring is
	 * ordinary memory, so it is safe to print even if the RSC or NoC is
	 * wedged; it must appear before the register dumps below for exactly
	 * that reason.
	 */
	j716f_vote_dump();
	j716f_irq_dump();

	/*
	 * Flush the breadcrumbs before reading another CPU's redistributor:
	 * the GICR frame of a CPU stuck in EL3 or in a collapsed power domain
	 * is exactly the kind of device read that can wedge the reader too.
	 * console_flush_on_panic() ignores the console lock so the evidence
	 * cannot be lost behind a stuck printk owner.
	 */
	if (oops_in_progress)
		console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	/* GIC redistributor state: is the CPU's IRQ interface still awake? */
#if IS_ENABLED(CONFIG_ARM_GIC_V3)
	for_each_online_cpu(cpu)
		gicv3_j716f_dump_redist(cpu);
#endif

	if (oops_in_progress)
		console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	/* RPMh TCS state; this touches the (possibly wedged) RSC last. */
#if IS_ENABLED(CONFIG_QCOM_RPMH)
	rpmh_rsc_panic_dump();
#endif

	pr_info("J716F DIAG dump end\n");
}
EXPORT_SYMBOL_GPL(j716f_diag_dump);

static int j716f_panic_notify(struct notifier_block *nb,
			      unsigned long action, void *data)
{
	j716f_diag_dump("panic");
	return NOTIFY_DONE;
}

static struct notifier_block j716f_panic_nb = {
	.notifier_call = j716f_panic_notify,
	.priority = 0,
};

static int j716f_diag_show(struct seq_file *m, void *v)
{
	int cpu;

	seq_printf(m, "j716f diag: %d sites, %d records/cpu\n",
		   J716F_BC_MAX, J716F_BC_RING);
	seq_printf(m, "asid bits=%u generation=%llu rollovers=%llu\n",
		   arm64_asid_bits(),
		   (unsigned long long)arm64_asid_generation(),
		   (unsigned long long)arm64_asid_rollover_count());
	for_each_online_cpu(cpu)
		seq_printf(m, "asid cpu%d workarounds=%llu\n", cpu,
			   (unsigned long long)arm64_asid_workaround_count(cpu));
	for_each_online_cpu(cpu) {
		struct j716f_bc_rec r;
		u64 now = j716f_now();
		u64 beat = READ_ONCE(per_cpu(j716f_bc_data, cpu).heartbeat);
		u64 seq;

		j716f_bc_last(cpu, &r, &seq);
		if (!seq)
			continue;
		seq_printf(m, "cpu%d seq=%llu beat=%llums%s site=%s arg=0x%lx fl=%c%c%c pid=%u comm=%s age=%llums\n",
			   cpu, (unsigned long long)seq,
			   beat ? j716f_age(now, beat) / NSEC_PER_MSEC : 0,
			   (!beat || j716f_age(now, beat) > J716F_STALE_NS) ? " STALE" : "",
			   j716f_bc_name(r.site), r.arg,
			   (r.flags & J716F_BC_F_IRQOFF) ? 'I' : '-',
			   (r.flags & J716F_BC_F_HARDIRQ) ? 'H' : '-',
			   (r.flags & J716F_BC_F_NMI) ? 'N' : '-',
			   r.pid, r.comm,
			   r.ts ? j716f_age(now, r.ts) / NSEC_PER_MSEC : 0);
	}

	return 0;
}

static int j716f_asid_show(struct seq_file *m, void *v)
{
	int cpu;

	seq_printf(m, "asid_bits=%u generation=%llu rollovers=%llu\n",
		   arm64_asid_bits(),
		   (unsigned long long)arm64_asid_generation(),
		   (unsigned long long)arm64_asid_rollover_count());
	for_each_online_cpu(cpu)
		seq_printf(m, "cpu%d workarounds=%llu\n", cpu,
			   (unsigned long long)arm64_asid_workaround_count(cpu));
	return 0;
}

static int j716f_asid_open(struct inode *inode, struct file *file)
{
	return single_open(file, j716f_asid_show, NULL);
}

static const struct proc_ops j716f_asid_proc_ops = {
	.proc_open	= j716f_asid_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int j716f_diag_open(struct inode *inode, struct file *file)
{
	return single_open(file, j716f_diag_show, NULL);
}

static ssize_t j716f_diag_write(struct file *file, const char __user *ubuf,
				size_t count, loff_t *ppos)
{
	char buf[16];

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (sysfs_streq(buf, "dump") || sysfs_streq(buf, "1"))
		j716f_diag_dump("user");
	return count;
}

static const struct proc_ops j716f_diag_proc_ops = {
	.proc_open	= j716f_diag_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
	.proc_write	= j716f_diag_write,
};

static int __init j716f_diag_init(void)
{
	struct proc_dir_entry *entry;
	int cpu, ret;

	/*
	 * M19: derive the per-boot tag before anything can print.  The KASLR
	 * offset differs per boot, and the monotonic clock at this point mixes
	 * in how long the early boot took.  Both are available without
	 * entropy; the tag only has to be stable within a boot and different
	 * across the boots of one experiment.
	 */
	j716f_boot_tag = (u32)((kaslr_offset() >> 21) ^ (u32)(j716f_now() >> 12));

	/*
	 * M20-P1a: resolve MC0 from the Command DB so the retention ring keeps
	 * the right records even if firmware moves the resource.  Best effort:
	 * an unavailable Command DB leaves the address at 0, which makes every
	 * record "important" rather than silently classifying none.
	 */
	j716f_vote_set_mc0(cmd_db_read_addr("MC0"));
	pr_info("j716f-diag: MC0 rpmh addr=0x%x\n", READ_ONCE(j716f_mc0_addr));

	entry = proc_create("j716f_diag", 0600, NULL, &j716f_diag_proc_ops);
	if (!entry) {
		pr_err("failed to create /proc/j716f_diag\n");
		return -ENOMEM;
	}

	entry = proc_create("j716f_asid", 0600, NULL, &j716f_asid_proc_ops);
	if (!entry)
		pr_warn("failed to create /proc/j716f_asid\n");

	atomic_notifier_chain_register(&panic_notifier_list, &j716f_panic_nb);

	/*
	 * The backtrace work is per-CPU storage but is queued on whichever CPU
	 * is printing.  Initialise it for every possible CPU here: a CPU that
	 * is hot-added later must not schedule an uninitialised work item.
	 */
	for_each_possible_cpu(cpu) {
		struct j716f_bt_req *req = per_cpu_ptr(&j716f_bt_reqs, cpu);

		req->cpu = cpu;
		INIT_WORK(&req->work, j716f_bt_work_fn);
	}

	register_pm_notifier(&j716f_snap_pm_nb);

	/*
	 * A 1 Hz pinned hard hrtimer per CPU is the liveness heartbeat: if a
	 * CPU's timer stops, its interrupts are off / it is in EL3 / the core
	 * is wedged, and the last breadcrumb names the site.  The printer is
	 * elected per second so a single stuck CPU cannot blind the stream.
	 *
	 * cpuhp_setup_state() runs the online callback for every CPU that is
	 * already online and takes the hotplug lock, so a CPU cannot go down
	 * between the callback registration and the timer start.
	 */
	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"soc/qcom/j716f-diag:online",
				j716f_snap_cpu_online,
				j716f_snap_cpu_offline);
	if (ret < 0) {
		pr_warn("could not register the CPU online callback: %d\n", ret);
		for_each_online_cpu(cpu)
			smp_call_function_single(cpu, j716f_snap_start_cpu,
						 NULL, 1);
	}

	pr_info("breadcrumbs enabled: %d sites, %d records per CPU, %llu ms heartbeat\n",
		J716F_BC_MAX, J716F_BC_RING,
		(unsigned long long)(J716F_SNAP_PERIOD_NS / NSEC_PER_MSEC));
	return 0;
}
device_initcall(j716f_diag_init);
