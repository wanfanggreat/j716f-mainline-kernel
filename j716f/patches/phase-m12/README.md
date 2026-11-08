# Phase M12 补丁（v46 RPMh）

v46 回移三类上游修复，并加少量诊断：

| 补丁/来源 | 内容 |
| --- | --- |
| Abel Vesa, 2026-11-02, `soc: qcom: rpmh: Fix use-after-free on timed out blocking requests` | `rpmh_request` 的 completion 内嵌、`refcount_t refcnt`、`rpmh_alloc_req()`/`rpmh_put_req()`/`__rpmh_write_wait()`/`rpmh_write_batch_active()`；超时后 request/completion 不再被释放 |
| Maulik Shah, 2026-10-05, `Output debug information from RSC` v5 1-3 | `cmd_db_hw_type_str()`、`cmd_db_read_name()`、`rpmh_rsc_debug()`；超时打印 GIC pending/completion/TCS/命令状态 |
| Konrad Dybcio, 2026-10-20, `arm64: dts: qcom: sm8250: Set active-only tags for CPU interconnects` | 8 个 CPU 的 CPU→memory `interconnects` 加 `QCOM_ICC_TAG_ACTIVE_ONLY` |
| 本机修复 | `u_serial.c` 的 panic 请求池 `req->complete = NULL` → no-op 回调，修 M10 的 UDC giveback 空指针 Oops |
| 本机诊断 | `rpmh-rsc.c` probe 打印 IRQ/hwirq/affinity；`rpmh_rsc.enable_wake` 参数；`rpmh_rsc_debug()` 把 GIC/completion 检查放在 TCS 寄存器读之前 |

## 与上游的差异

- `rpmh_rsc_debug()` 的 GIC pending/completion 检查被提前到 TCS 寄存器
  读之前。原因：如果 RSC 总线已经卡死，先读 TCS 可能让超时线程也卡住，
  关键结论就永远打不出来。
- 增加了 probe 的 `J716F RSC ... irq=.. hwirq=.. aff=..` 和
  `enable_wake` 诊断参数；这些不是上游内容，根因定位后应删除
  （TD-071/TD-074/TD-075）。
- 合并成一个 patch 文件，便于在 v45 源码树上一次性回移。

## 应用顺序

源码树已是“v45 累积改动 + v46 补丁”的状态。

```sh
cd sources/linux-7.3.1
patch -p1 < /path/to/patches/phase-m12/0001-rpmh-uaf-fix-rsc-debug-active-only-tags.patch
```

## 验证边界

- 主机侧只验证编译、DTB tag、字符串、包哈希；UAF 修复的运行时行为
  必须真机冷启动回归。
- `rpmh_rsc_debug()` 在坏 RSC 上仍可能卡在寄存器读；GIC 检查提前是
  为了先留下结论。
