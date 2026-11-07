# Phase M7 补丁（v41）

v41 不再改显示/音频/白点主路径；补丁只针对 M6 真机确认的缺陷和取证链。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-dts-fix-31mib-splash-and-ramoops.patch` | `sm8250-lenovo-j716f.dts` | `framebuffer`/`cont_splash_mem` `0x1f000000`（496 MiB，笔误）→ `0x1f00000`（31 MiB）；ramoops `0x9f000000+2 MiB` 真正落在 31 MiB 之外；可用内存回约 465 MiB |
| `0002-qcom-wdt-honour-nowayout.patch` | `qcom-wdt.c` | `wdt->wdd.status = WATCHDOG_NOWAYOUT_INIT_STATUS`；v40 的 `CONFIG_WATCHDOG_NOWAYOUT=y` 对这颗驱动无效（sysfs `nowayout=0`），关机时 systemd 仍能 `close()` 关掉看门狗 |
| `0003-q6dsp-clocks-skip-refused-devote.patch` | `q6dsp-lpass-clocks.c` | ADSP 拒绝 LPASS core vote 且 `clk->handle==0` 时跳过 devote，消除 `cmd = 0x100f6 ... Unknown cmd` |
| `0004-va-macro-current-limit-is-optional.patch` | `lpass-va-macro.c` | `va-vdd-micb` 是固定 SMPS，`regulator_set_current_limit()` 返回 `-EINVAL` 是正常的；`dev_err` → `dev_dbg` |
| `0005-i2c-hid-retry-power-on-after-resume.patch` | `i2c-hid-core.c` | s2idle resume 时磁吸键盘/触控板 `-ENXIO`；在原 400 µs 重试之外再加 10×50 ms，只报一次错 |
| `0006-v41-config.diff` | `.config`（v40 → v41） | `ARM64_PSEUDO_NMI=y`、buddy 硬锁死、`BOOTPARAM_HARDLOCKUP_PANIC=y`、`NETCONSOLE=y/DYNAMIC/EXTENDED_LOG` |

## 应用顺序

源码树 `sources/linux-7.3.1` 已是“v40 累积改动 + v41 补丁”的状态；
这些文件是相对 v40 状态的记录，不用于从原始 tarball 直接全量应用。

```sh
cd sources/linux-7.3.1
for p in /path/to/patches/phase-m7/000[1-5]-*.patch; do patch -p1 < "$p"; done
```

`0006-v41-config.diff` 是配置审计记录，不直接 `patch`；用
`scripts/config` + `olddefconfig` 应用（见 Phase M7 清单）。

## 与上游的关系

- `0002` 是上游 `qcom-wdt` 的缺口：驱动没有 `nowayout` 模块参数，也不引用
  `WATCHDOG_NOWAYOUT_INIT_STATUS`（2026-11-07 查 torvalds 树确认）。可提交
  上游。
- `0005` 对应上游 `i2c-hid` 的 resume 只有 400 µs 重试；本机设备需要更久。
  可先作为设备 quirk 提交，或等上游接受更长的重试。
- `0001` 是本项目 DTS 的自有笔误，无需上游。
- `0003`/`0004` 是本项目音频移植的清理，保留设备注释。
