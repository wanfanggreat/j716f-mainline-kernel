# Phase M6 补丁（v40）

三处内核改动，逐条对应 M5 真机报告里的缺陷。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-async-report-stuck-work.patch` | `kernel/async.c` | `async_synchronize_cookie_domain()` 等待超 10 s 打印卡住的 cookie、pending 数量、每个 pending 项的 `%ps` 函数名。**诊断脚手架**（TD-058），根因定位后删除；行为不变（`wait_event` → 等价的 `wait_event_timeout` 循环）。 |
| `0002-dts-ramoops-out-of-framebuffer.patch` | `sm8250-lenovo-j716f.dts` | ramoops `0x9e100000` → `0x9f000000`；`cont_splash_mem`/`framebuffer` 35 MiB → 31 MiB。修 pstore 永远 0 条（TD-053）。 |
| `0003-dpu-pcc-bake-calibrated-anchors.patch` | `dpu_crtc.c` | 白点默认值 hi `950/985/1024` → `949/973/1172`、lo `880/935/1024` → `970/772/1328`（用户 2026-11-07 用 RGB 调色器亲调）。 |

## 应用顺序

```sh
cd sources/linux-7.3.1
for p in /path/to/patches/phase-m6/000*.patch; do patch -p1 < "$p"; done
```

## 配置与 cmdline

见 `v40-config-and-cmdline.txt`。配置增量是在 v39 `.config` 上用
`scripts/config` 应用后 `olddefconfig`。

## 生成方式

- `0001`、`0003`：对 `sources/linux-7.3.1.tar.xz` 解出的原始文件做 `diff -u`。
- `0002`：DTS 是本项目新增文件（不在上游 tarball），补丁以前序
  `patches/phase-m2/0001-ramoops-in-splash-tail.patch` 的上下文为基准手写。
