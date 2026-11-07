# Phase M10 补丁（v44）

v44 修两件事：**蓝牙 UART IRQ 在 suspend 窗口被 spurious 检测器永久
关闭**（真实 s2idle 后蓝牙掉线的真因），以及 **panic 只能发出第一行**。
v43 的 `qcom,no-ibs-sleep`、模块集、DTB 和其余修复不动。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-qcom-geni-serial-clear-irq-when-suspended.patch` | `drivers/tty/serial/qcom_geni_serial.c` | `qcom_geni_serial_isr()` 在 `uport->suspended` 且设备仍 runtime-active 时，读并写清 `SE_GENI_S_IRQ_STATUS`、`SE_GENI_M_IRQ_STATUS`、`SE_DMA_RX_IRQ_STAT`、`SE_DMA_TX_IRQ_STAT`，返回 `IRQ_HANDLED`。不再让 `note_interrupt()` 把 `998000.serial`（蓝牙 UART，IRQ 163）当成 stuck IRQ 永久 disable。 |
| `0002-u-serial-queue-the-whole-panic-from-a-request-pool.patch` | `drivers/usb/gadget/function/u_serial.c` | `gs_console` 在 init 预分配 40 × 8 KiB 缓冲，在 connect 绑定 40 个 `usb_request`；panic 时第一条记录立即排队，回溯合并成约 7 KiB/请求，检测到 `Rebooting in`/`end Kernel panic` 时刷尾。正常路径仍是单请求 + workqueue。 |

## 应用顺序

源码树已是“v43 累积改动 + v44 补丁”的状态。

```sh
cd sources/linux-7.3.1
for p in /path/to/patches/phase-m10/000*.patch; do patch -p1 < "$p"; done
```

## 与上游的关系

- `0001` 是 geni serial 的通用 suspend 竞态：更干净的上游修法可能是
  `serial_core` 在 `ops->shutdown()` 之后再置 `suspended`，或让 ISR 在
  suspended 时清状态。当前只影响本机，等真机确认后可提上游讨论。
- `0002` 是 ttyGS console 在 panic（IRQ 关闭）下的多请求回退；上游
  可能更愿意做 “panic console” 框架级支持。请求池只在 ACM 调试功能
  存在时分配（TD-069）。
