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

	J716F_BC_MAX,
};

#define J716F_BC_F_IRQOFF	BIT(0)
#define J716F_BC_F_HARDIRQ	BIT(1)
#define J716F_BC_F_NMI		BIT(2)

#if IS_ENABLED(CONFIG_QCOM_J716F_DIAG)
void j716f_bc(enum j716f_bc_site site, unsigned long arg);
void j716f_bc_quiet(enum j716f_bc_site site, unsigned long arg);
void j716f_diag_dump(const char *reason);
#else
static inline void j716f_bc(enum j716f_bc_site site, unsigned long arg) { }
static inline void j716f_bc_quiet(enum j716f_bc_site site, unsigned long arg) { }
static inline void j716f_diag_dump(const char *reason) { }
#endif

#endif /* __SOC_QCOM_J716F_DIAG_H */
