# Phase M14 补丁（v48 心跳快照）

v48 全部是本机诊断改动，没有上游提交。相对 v47：

| 文件 | 改动 |
| --- | --- |
| `drivers/soc/qcom/j716f-diag.c` | 每 CPU 1 Hz pinned hard hrtimer 心跳；每秒轮换一个 CPU 打印 `J716F SNAP`（site/age/STALE）；心跳停滞 2/5/10/20/40/80 s 时打印 `SNAP-ALERT`（含 CT/GICR）；`/proc/j716f_diag` 增加 `beat=`/`STALE`；CPU hotplug 启停定时器；panic dump 开头也打一行 SNAP |
| `drivers/irqchip/irq-gic-v3.c` | 把 GICR 读取拆成 `gicv3_j716f_redist_state()`（供 SNAP-ALERT 使用），保留 `gicv3_j716f_dump_redist()` |

## 应用

基线是 v47 源码（`j716f-m11` tag + M12 patch + M13 patch）：

```sh
cd <v46 source tree>
patch -p1 < patches/phase-m12/0001-rpmh-uaf-fix-rsc-debug-active-only-tags.patch
patch -p1 < patches/phase-m13/0001-j716f-diag-breadcrumbs.patch
patch -p1 < patches/phase-m14/0001-j716f-heartbeat-snapshot.patch
```

## 验证边界

- 主机侧只验证编译和 `Image` 内 `J716F SNAP t=` / `SNAP-ALERT` 字符串；
  1 Hz 打印、心跳停滞判定、GICR 读取必须真机验证。
- 心跳是 **1 Hz pinned hard hrtimer**：CPU 的定时器停转只可能是关中断、
  EL3 或总线 stall。空闲 CPU 的定时器照常运行，所以不会误报。
- 快照从 hardirq 打印到所有 console，每秒约 250 B；同时
  `j716f-snapd.service` 把同样内容 fsync 到 `/var/log/j716f/snap.log`，
  用于 USB 控制台一起死掉时的磁盘兜底。
- 定位根因后连同 M13 的全部 site 一起删除（TD-077–TD-081）。
