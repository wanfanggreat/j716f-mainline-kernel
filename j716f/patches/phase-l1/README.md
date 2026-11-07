# Phase L1 补丁状态（2026-11-07 更新）

| 补丁 | 状态 | 说明 |
| --- | --- | --- |
| `0001-dts-rtc-allow-set-time.patch` | **已被取代** | v22 证明 RTC 写被 SPMI APID 拒绝；v23 放行后整机冻结。Phase L4 删除 `allow-set-time`，改用 fake-hwclock + NTP。 |
| `0002-spmi-allow-rtc-write.patch` | **REJECTED** | 已移到 [`patches/rejected/`](../rejected/0002-spmi-allow-rtc-write.patch)。放行 SPMI 写后 `hwclock -w` 整机挂死，不可用。 |
| `0003-dts-bt-local-bd-address.patch` | **已被取代** | `local-bd-address` 是 LSB-first；原补丁的 `[02 00 00 00 5a ad]` 被解释成组播 `AD:5A:00:00:00:02`。Phase L4 改为 `[ad 5a 00 00 00 02]` = `02:00:00:00:5A:AD`，并加 qcom-pon 修复。 |

当前生效的 DTS 改动见
[`patches/phase-l4/0001-dts-bt-pon-time.patch`](../phase-l4/0001-dts-bt-pon-time.patch)。
决策与证据见
[Phase L4 任务](../../planning/tasks/TASK-20261107-phase-l4-bt-pon-time.md)、
[Phase L1 任务](../../planning/tasks/TASK-20261106-phase-l1-rtc.md)。
