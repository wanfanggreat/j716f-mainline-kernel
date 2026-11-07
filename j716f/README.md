# TB-J716F 主线内核源码仓

本仓是 Lenovo Xiaoxin Pad Pro 2021（TB-J716F，`p11_pro_plus`，
Snapdragon 870 / SM8250-AC）主线 Linux 移植的**完整内核源码**，
包含截至 Phase M10 的全部本地补丁和未提交前的工作树改动。

主仓（构建脚本、补丁说明、测试证据、清单）：
<https://github.com/wanfanggreat/j716f-mainline-linux>

## 上游来源

| 项 | 值 |
| --- | --- |
| 上游 | kernel.org `linux-7.3.1.tar.xz` |
| tarball SHA-256 | `fa595aad93734f829c963c2a90ef7c3673224fd7fdae7b49406e397cde2de231` |
| 上游基线 tag | `upstream-7.3.1` |
| 基线提交 | `788e48f`（chore(kernel): import Linux 7.3.1 from kernel.org tarball） |
| 本仓最终 tag | `j716f-m10` |
| 许可 | GPL-2.0（见 `COPYING`） |

上游 tarball 不携带 git 历史，因此本仓把**未修改的 7.3.1 树**作为单个
基线提交导入；`git diff upstream-7.3.1..j716f-m10` 就是本项目的全部本地改动。

## 本地改动范围

相对 7.3.1 共 40 个文件：38 个修改、2 个新增
（`sm8250-lenovo-j716f.dts`、`es7210.c`），合计
2820 insertions / 175 deletions。逐文件统计见
[`CHANGES-vs-7.3.1.md`](CHANGES-vs-7.3.1.md)，按阶段拆分的补丁原文在
[`patches/`](patches/)。

改动集中在：

- Qualcomm 显示链路：DPU、DSI host/PHY、RM69380 面板、PCC 白点校准；
- 触控：Goodix GT738x；
- 音频：LPASS VA/TX macro、q6asm/q6afe/q6routing、SM8250 声卡、
  CS35L41、ES7210、ASoC DAPM/PCM；
- 无线：`hci_qca`（QCA6390 上电/休眠/重试）；
- USB gadget：NCM netconsole（`tx_immediate`）、CDC-ACM 串口控制台
  （panic 请求池）；
- 调试/可靠性：GENI serial、GENI I2C、i2c-hid、`qcom-wdt` nowayout、
  `kernel/async`、`drivers/base/dd.c`、RPMh regulator、PD mapper、UBWC；
- 设备树：`arch/arm64/boot/dts/qcom/sm8250-lenovo-j716f.dts`。

本仓只做资料归档，未在本轮修改任何功能逻辑。

## 构建

使用主仓的固定入口脚本（固定 ccache、OUT、构建身份）：

```sh
TB_J716F_CLANG_BIN=/path/to/clang-r584948b/bin \
  /path/to/j716f-mainline-linux/tools/build-kernel.sh \
  --src /path/to/j716f-mainline-kernel \
  --out /path/to/out/linux-7.3.1-phasei \
  --config j716f/config/phase-m10.config \
  --targets "Image dtbs modules" \
  --name phase-m10 --jobs 20
```

Phase M10 的实际构建身份、编译器版本和产物哈希在
`j716f/config/build-identity-phase-m10.txt`。构建产物不进入 git：

| 产物 | 位置 |
| --- | --- |
| `Image`、`System.map`、`vmlinux`（含调试符号）、DTB | 主仓 release `kernel-m10-20261108` |
| 595 个 `=m` 模块 tar 包、模块清单/vermagic | 同上 |
| `.config`、DTS/DTSI、构建日志 | 主仓 `builds/kernel-m10/` |

## 归档说明

- 工作树中的 `*.rej` 是历史补丁被拒后的残留片段，不属于可构建源码，
  未纳入本仓；原始文件仍在主仓工作区。
- 构建输出目录 `out/` 不属于源码，未纳入本仓。
- 设备序列号、MAC、SSID 等唯一信息不进入本仓；测试证据中的脱敏副本在
  主仓 `diagnostics/`。
