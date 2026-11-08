# Phase M13 补丁（v47 面包屑）

v47 没有引入上游提交，全部是本机诊断脚手架：

| 文件 | 内容 |
| --- | --- |
| `include/soc/qcom/j716f-diag.h`（新增） | 56 个 site 枚举 + `j716f_bc()` / `j716f_diag_dump()` |
| `drivers/soc/qcom/j716f-diag.c`（新增） | per-CPU 16 条面包屑环；panic notifier；`/proc/j716f_diag`；CT/GICR 汇总；RSC 读前 `console_flush_on_panic()` |
| `drivers/soc/qcom/Kconfig`、`Makefile` | `CONFIG_QCOM_J716F_DIAG`（bool，depends on ARCH_QCOM） |
| `drivers/irqchip/irq-gic-v3.c` | `gicv3_j716f_dump_redist()`：`GICR_WAKER` / pending / active / enabled |
| `drivers/soc/qcom/rpmh.c` | `__rpmh_write_wait()` / `rpmh_write_batch_active()` 的 send/wait/timeout site |
| `drivers/soc/qcom/rpmh-rsc.c` | send/claim/buffer/trigger、`write_tcs_reg_sync`、`tcs_invalidate`、`tcs_tx_done`、`rpmh_rsc_write_ctrl_data` site；`rpmh_rsc_panic_dump()`；`j716f_rsc.panic_dump` 参数 |
| `drivers/soc/qcom/rpmh-internal.h` | `rsc_drv.node` 与 `rpmh_rsc_panic_dump()` 声明 |
| `drivers/interconnect/qcom/bcm-voter.c` | commit 入口、`rpmh_invalidate()`、active batch、出口 |
| `drivers/interconnect/qcom/osm-l3.c` | OSM L3 perf-state 写 |
| `drivers/cpufreq/qcom-cpufreq-hw.c` | `target_index` 与 `qcom_cpufreq_set_bw()`（OPP/ICC） |
| `drivers/pmdomain/qcom/rpmhpd.c` | power on/off 与 `rpmhpd_aggregate_corner()` |
| `drivers/clk/qcom/clk-rpmh.c` | `clk_rpmh_send_aggregate_command()` |
| `drivers/firmware/qcom/qcom_scm.c` | `qcom_scm_call()`（覆盖所有 EL3 SMC） |
| `drivers/firmware/psci/psci.c` | `psci_cpu_suspend_enter()` |
| `drivers/spmi/spmi-pmic-arb.c` | SPMI 读/写事务 |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c` | `dpu_crtc_atomic_flush()` |
| `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` | `a6xx_gmu_resume()` / `a6xx_gmu_stop()` |
| `arch/arm64/include/asm/tlbflush.h` | `flush_tlb_mm()` / `arch_tlbbatch_flush()` |

## 应用

基线是 v46 源码（`j716f-m11` tag + `patches/phase-m12/0001-…patch`）：

```sh
cd <v46 source tree>
patch -p1 < patches/phase-m12/0001-rpmh-uaf-fix-rsc-debug-active-only-tags.patch
patch -p1 < patches/phase-m13/0001-j716f-diag-breadcrumbs.patch
```

## 验证边界

- 主机侧只验证编译、`Image` 内诊断字符串、`CONFIG_QCOM_J716F_DIAG=y`
  和包哈希；运行时行为（`/proc/j716f_diag`、panic dump、GICR/RSC 读）
  必须真机冷启动验证。
- panic dump 会最后读 RSC 寄存器；如果 RSC 已经 wedged，panic CPU
  可能跟着停住。所有内存态在此之前打印并 `console_flush_on_panic()`。
- `j716f_rsc.panic_dump=0` 可关闭 RSC 寄存器读取。
- 这批 site 是诊断脚手架：根因定位后整包回退（TD-077–TD-079）。
