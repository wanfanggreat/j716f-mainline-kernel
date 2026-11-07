# Phase A 源码补丁

本目录只用于 Phase A 单变量构建。所有补丁都在 `sources/linux-7.3.1/`
（Linux 7.3.1 tarball，非 git 工作树）上以 `-p1` 应用，**每个补丁单独应用、
构建后立即反向应用**；源码树必须恢复到下列基线哈希。

## 基线（2026-11-05）

| 文件 | SHA-256 |
| --- | --- |
| `drivers/base/dd.c` | `03dbcf0f223b28da2006cc9fe851c8782c4c17f50e35027c94e2b4ef208b0e52` |
| `arch/arm64/boot/dts/qcom/sm8250-lenovo-j716f.dts` | `def56ebc9267aca7e6ffaa70749571f9ed3cac506d03c93c98e2111a5d10cfbb` |

前置核对（不符立即停止）：

```sh
sha256sum sources/linux-7.3.1/drivers/base/dd.c \
          sources/linux-7.3.1/arch/arm64/boot/dts/qcom/sm8250-lenovo-j716f.dts
```

## 补丁

| 补丁 | 作用 | 用于 | 依赖 |
| --- | --- | --- | --- |
| `0001-dd-probe-instrumentation.patch` | `really_probe_debug()` 打印 probe start；`wait_for_device_probe()` 打印 enter/after flush/count=0/async done | `diag-pstore`、`diag-pstore-pcie-off` | 无 |
| `0002-dts-ramoops.patch` | `/reserved-memory` 增加 `ramoops@b0000000`：1 MiB，record 128 KiB、console 256 KiB、pmsg 128 KiB，无 `no-map` | `diag-pstore`、`diag-pstore-pcie-off` 的 DTB | 无 |
| `0003-dts-pcie-disable.patch` | 只把 `&pcie0`、`&pcie0_phy` 的 `status` 改为 `disabled` | `v5-pcie-dtb` 的 DTB | 无 |
| `0004-dts-pm8009-disable.patch` | 只给 `&pm8009_pon` 增加 `status = "disabled"` | `v8-pm8009-off` 的 DTB | 无 |

`0002`/`0003`/`0004` 都是相对同一份 v5 DTS 的独立补丁，**不要互相叠加**。
Phase A 的每个 DTB 变体各自从干净源码应用一个补丁。

`0001` 的内容来自
`private/step3-20261105/dd-probe-instrumentation.patch`（2026-11-05 第 3 步
诊断构建），只把路径头改为 `a/`、`b/` 以支持 `-p1`；补丁正文未改。

`0002` 的 ramoops 属性以参考 6.15 DTB 的反编译结果为准：
`/mnt/d/TB-J716F-mainline/reference-ubuntu22/analysis/ref-6.15.dts`
（`ramoops@b0000000`，`reg = <0x0 0xb0000000 0x0 0x100000>`，
`record-size = <0x20000>`、`console-size = <0x40000>`、`pmsg-size = <0x20000>`，
**无 `no-map`**）。该地址与 v5 DTB 现有 reserved-memory
（`cont_splash_mem` 0x9c000000–0x9e300000）不重叠。

## 应用 / 反向应用

```sh
# 应用（示例：ramoops）
patch --dry-run -p1 -d sources/linux-7.3.1 < patches/phase-a/0002-dts-ramoops.patch
patch -p1 -d sources/linux-7.3.1 < patches/phase-a/0002-dts-ramoops.patch

# 反向应用
patch -R -p1 -d sources/linux-7.3.1 < patches/phase-a/0002-dts-ramoops.patch
```

重新应用前先确认源码哈希等于基线；如果 `patch` 提示
“Reversed (or previously applied)”，说明源码树不干净，先调查，不要强行应用。

## 自动化

`tools/phase-a-build.sh` 负责应用、构建、反向应用和哈希复核。补丁期间持有
`out/phase-a/.source.lock`（`flock`），退出 trap 会反向应用未撤销的补丁并
再次核对基线哈希。手工操作时也必须遵守同样的顺序。

## 边界

- 这些补丁是诊断用，不是上游提交候选；不进 `docs/` 结论。
- `0001` 只增加 `printk(KERN_DEBUG ...)`，不改 probe 流程。
- 不修改 DTS 的 CPU OPP、PCIe 供电或 PON 驱动代码；Phase A 只做隔离。
