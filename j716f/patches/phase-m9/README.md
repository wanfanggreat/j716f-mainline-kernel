# Phase M9 补丁（v43）

v43 只做两件事：USB ACM 串口控制台（panic 通道）和 QCA6390 的
no-ibs-sleep（挂起后蓝牙不掉）。其余 v41/v42 修复不动。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-u-serial-console-panic-direct.patch` | `drivers/usb/gadget/function/u_serial.c` | console 请求从 64 B 改成 8 KiB；`gs_console_write()` 在 `oops_in_progress` 时直接 `usb_ep_queue()`（关中断，不 schedule_work），并一次 `kfifo_reset()` 旧普通日志，让第一个 8 KiB 请求带 panic 头和回溯；正常路径仍走 workqueue |
| `0002-hci-qca-no-ibs-sleep.patch` | `drivers/bluetooth/hci_qca.c` | DT `qcom,no-ibs-sleep` 时：`tx_idle_delay=24h`、`qca_suspend()` 直接返回不发送 `HCI_IBS_SLEEP_IND`，QCA6390 在 s2idle 期间保持供电 |
| `0003-dts-no-ibs-sleep.patch` | `sm8250-lenovo-j716f.dts` | `bluetooth` 节点加 `qcom,no-ibs-sleep;` |

配置/命令行：见 `v43-config-and-cmdline.txt`（`CONFIG_U_SERIAL_CONSOLE=y`、
cmdline 加 `console=ttyGS0`）。

## 应用顺序

源码树已是“v42 累积改动 + v43 补丁”的状态。

```sh
cd sources/linux-7.3.1
for p in /path/to/patches/phase-m9/000*.patch; do patch -p1 < "$p"; done
```

## 与上游的关系

- `0001` 是 ttyGS console 的 panic 兼容性改动；上游可能更愿意做
  `oops_in_progress` 分支 + 更大请求，值得提交讨论。
- `0002` 是 QCA6390 的板级 quirk；更干净的位置可能是
  `pwrseq-qcom-wcn` 的 BT 掉电保持，或在 `qca_suspend()` 里完整
  power cycle。当前 DT 属性可逆、只影响本机。
