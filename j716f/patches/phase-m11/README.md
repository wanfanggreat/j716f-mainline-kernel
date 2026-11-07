# Phase M11 补丁（v45 冻结诊断）

v45 是**诊断内核**，不是产品修复。它给 GPU/GMU/DPU/DSI 路径加现场
打印，并加三个 `msm.*` 排他开关，用来定位“GPU 冷启动 + 前台 WebGL →
CPU1 hard LOCKUP”。根因确定后这些改动应删除或降到 `dev_dbg`
（TD-071/TD-072）。

| 补丁 | 文件 | 作用 |
| --- | --- | --- |
| `0001-j716f-freeze-instrumentation-and-switches.patch` | `adreno_gpu.c`、`adreno_device.c`、`a6xx_gmu.c`、`a6xx_gpu.c`、`msm_gpu.c`、`dpu_plane.c`、`dpu_hw_interrupts.c`、`dsi_host.c` | GPU hw_init/SCM aperture、GMU resume 各步、GPU fault IRQ、DPU/DSI IRQ 的 `J716F ...` 打印；`msm.skip_scm_aperture`、`msm.no_gpu_rpm`、`msm.force_linear` 三个诊断参数 |

## 应用顺序

源码树已是“v44 累积改动 + v45 诊断补丁”的状态。

```sh
cd sources/linux-7.3.1
patch -p1 < /path/to/patches/phase-m11/0001-j716f-freeze-instrumentation-and-switches.patch
```

## 与上游的关系

这些都是本机诊断代码，不应提交上游。`force_linear` 只影响 DPU
advertise 的 modifier；`no_gpu_rpm` 用 `pm_runtime_forbid` 保持 GPU
常供电；`skip_scm_aperture` 跳过 `qcom_scm_set_gpu_smmu_aperture(0)`。
三者都只用于把冻结的嫌疑一条条排除。
