# Phase M15 补丁（v49 mm teardown 面包屑）

v49 全部是本机诊断改动，没有上游提交。相对 v48：

| 文件 | 改动 |
| --- | --- |
| `include/soc/qcom/j716f-diag.h` | 新增 40 个 site（dup_mmap/exit_mmap/unmap_*/free_pgtables/free_pgd_range/tlb_*/mmu_notifier_release/mmput/mmdrop/exec_mmap/copy_process/wake_up_new_task/flush_tlb_batched_pending） |
| `drivers/soc/qcom/j716f-diag.c` | 年龄 clamp（修 v48 的 u64 下溢）；stage 2 调度 `trigger_single_cpu_backtrace()`；stage 3 主动 `panic()`；PM resume guard；`bt_stage`/`panic_stage`/`snap_print` 参数 |
| `mm/mmap.c` | `dup_mmap` 入口/`flush_tlb_mm` 后/`mmap_write_unlock` 前后/出口；`exit_mmap` 入口/出口 |
| `mm/vma.c` | `unmap_region` 入口/出口 |
| `mm/memory.c` | `unmap_vmas`、`free_pgtables` 入口/出口；`free_pgd_range` 入口 |
| `mm/mmu_gather.c` | `tlb_batch_pages_flush`、`tlb_table_flush`、`tlb_flush_mmu`、`tlb_finish_mmu`、`tlb_remove_table_sync_one` |
| `mm/mmu_notifier.c` | `__mmu_notifier_release` |
| `mm/rmap.c` | `flush_tlb_batched_pending` |
| `kernel/fork.c` | `copy_process`、`wake_up_new_task`、`__mmput`、`__mmdrop` |
| `fs/exec.c` | `exec_mmap` |

## 应用

基线是 v48 源码（`j716f-m11` tag + M12 + M13 + M14 补丁）：

```sh
patch -p1 < patches/phase-m12/0001-rpmh-uaf-fix-rsc-debug-active-only-tags.patch
patch -p1 < patches/phase-m13/0001-j716f-diag-breadcrumbs.patch
patch -p1 < patches/phase-m14/0001-j716f-heartbeat-snapshot.patch
patch -p1 < patches/phase-m15/0001-j716f-mmteardown-breadcrumbs.patch
```

## 验证边界

- 主机侧只验证编译和 Image 内 `heartbeat stopped for` /
  `requesting NMI backtrace` 字符串；回溯、主动 panic、年龄 clamp
  必须真机验证。
- 主动 panic 默认 10 s（`j716f_diag.panic_stage=3`），是诊断行为；
  产品内核不得保留。
- 5 s 的 NMI 回溯 work 最多阻塞 10 s，只在已失联时发生。
