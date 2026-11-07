# Phase M8 补丁（v42）

v42 只改两个驱动路径；其余 v41 的模块/DTB/白点/音频/i2c-hid 不动。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-f-ncm-tx-immediate-for-netconsole.patch` | `drivers/usb/gadget/function/f_ncm.c` | 新增 `tx_immediate`（默认 true）：每个 Ethernet 帧单独成一个 NTB，在 `ncm_wrap_ntb()` 里同步 `package_for_tx()`，让 netpoll 的 `ndo_start_xmit` 立即拿到 skb 并 `usb_ep_queue`。原路径把帧缓冲进 NTB、等 300 µs soft hrtimer；panic 时 hrtimer 不会跑，netconsole 永远发不出。代价是 NCM 不再聚合，SSH/SCP 可能略慢 |
| `0002-hci-qca-hold-bt-off-on-retry.patch` | `drivers/bluetooth/hci_qca.c` | QCA6390 power-on 失败的重试路径里，`qca_power_off()` 之后 `msleep(200)` 再下一轮上电。pwrseq 会把 BT_EN 拉低，但原来几乎立刻拉高，芯片没有真正掉电复位；`systemctl restart bluetooth` 会卡在 `0xfc00 tx timeout (-110)` |

## 应用顺序

源码树已是“v41 累积改动 + v42 补丁”的状态；补丁是相对上游 7.3.1
原始文件的记录。

```sh
cd sources/linux-7.3.1
for p in /path/to/patches/phase-m8/000*.patch; do patch -p1 < "$p"; done
```

## 与上游的关系

- `0001` 是 NCM gadget 的 netpoll 兼容性改动，可提交上游讨论；默认
  `tx_immediate=true` 改变了聚合行为，上游可能更愿意做成 netconsole
  专用开关（例如只在 `dev->npinfo` 存在时同步 flush）。
- `0002` 是 QCA6390 的掉电时序；上游 `pwrseq-qcom-wcn` 的
  `bt_disable` 没有掉电保持时间，可讨论在 pwrseq 层加 delay，而不是
  在 hci_qca 重试路径加。
