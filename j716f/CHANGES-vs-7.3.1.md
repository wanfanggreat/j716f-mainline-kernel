# 相对 Linux 7.3.1 的本地改动

基线：tag `upstream-7.3.1`（未修改的 kernel.org 7.3.1 树）。
M10 构建源码：tag `j716f-m10`（`e562462a3`）。
M11/v45-diag 构建源码：tag `j716f-m11`（`0a2059ef0`）。
M15/v49-diag 构建源码：tag `j716f-m15`（本提交）。
归档工作树：tag `j716f-worktree-20261108`（`15a8792c9`）。
归档文档 HEAD：tag `j716f-archive-20261108` 及其后续归档提交。
下面的逐文件统计以 M15 构建源码为准；M10 产物对应 `j716f-m10`。

## 新增文件

| 文件 | 说明 |
| --- | --- |
| `arch/arm64/boot/dts/qcom/sm8250-lenovo-j716f.dts` | TB-J716F 设备树（1462 行） |
| `sound/soc/codecs/es7210.c` | ES7210 采集 codec 驱动（704 行） |
| `drivers/soc/qcom/j716f-diag.c` | J716F 面包屑/心跳/panic dump 诊断（691 行） |
| `include/soc/qcom/j716f-diag.h` | 诊断 site 定义（159 行） |

设备树 `Makefile` 与 `scripts/dtc/include-prefixes/arm64/qcom/Makefile`
增加了 `sm8250-lenovo-j716f.dtb` 条目。

## 修改文件

| 文件 | 变更行 |
| --- | ---: |
| `arch/arm64/boot/dts/qcom/Makefile` | +2/-0 |
| `arch/arm64/boot/dts/qcom/sm8250.dtsi` | +17/-8 |
| `arch/arm64/include/asm/tlbflush.h` | +6/-0 |
| `drivers/base/dd.c` | +9/-0 |
| `drivers/bluetooth/hci_qca.c` | +34/-0 |
| `drivers/clk/qcom/clk-rpmh.c` | +4/-0 |
| `drivers/cpufreq/qcom-cpufreq-hw.c` | +6/-0 |
| `drivers/firmware/psci/psci.c` | +4/-0 |
| `drivers/firmware/qcom/qcom_scm.c` | +14/-3 |
| `drivers/gpu/drm/msm/adreno/a6xx_gmu.c` | +19/-0 |
| `drivers/gpu/drm/msm/adreno/a6xx_gpu.c` | +7/-1 |
| `drivers/gpu/drm/msm/adreno/adreno_device.c` | +10/-0 |
| `drivers/gpu/drm/msm/adreno/adreno_gpu.c` | +27/-5 |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c` | +177/-5 |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_hw_interrupts.c` | +9/-0 |
| `drivers/gpu/drm/msm/disp/dpu1/dpu_plane.c` | +19/-1 |
| `drivers/gpu/drm/msm/dsi/dsi_host.c` | +133/-23 |
| `drivers/gpu/drm/msm/dsi/phy/dsi_phy.c` | +15/-7 |
| `drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c` | +49/-2 |
| `drivers/gpu/drm/msm/msm_gpu.c` | +13/-0 |
| `drivers/gpu/drm/panel/panel-raydium-rm69380.c` | +184/-30 |
| `drivers/hid/i2c-hid/i2c-hid-core.c` | +16/-0 |
| `drivers/i2c/busses/i2c-qcom-geni.c` | +9/-1 |
| `drivers/input/touchscreen/goodix.c` | +338/-7 |
| `drivers/input/touchscreen/goodix.h` | +13/-0 |
| `drivers/interconnect/qcom/bcm-voter.c` | +5/-0 |
| `drivers/interconnect/qcom/osm-l3.c` | +3/-0 |
| `drivers/irqchip/irq-gic-v3.c` | +50/-0 |
| `drivers/pmdomain/qcom/rpmhpd.c` | +12/-4 |
| `drivers/regulator/qcom-rpmh-regulator.c` | +44/-1 |
| `drivers/soc/qcom/Kconfig` | +14/-0 |
| `drivers/soc/qcom/Makefile` | +1/-0 |
| `drivers/soc/qcom/cmd-db.c` | +65/-0 |
| `drivers/soc/qcom/qcom_pd_mapper.c` | +7/-0 |
| `drivers/soc/qcom/rpmh-internal.h` | +15/-3 |
| `drivers/soc/qcom/rpmh-rsc.c` | +240/-2 |
| `drivers/soc/qcom/rpmh.c` | +200/-107 |
| `drivers/soc/qcom/ubwc_config.c` | +2/-0 |
| `drivers/spmi/spmi-pmic-arb.c` | +13/-3 |
| `drivers/tty/serial/qcom_geni_serial.c` | +24/-2 |
| `drivers/usb/gadget/function/f_ncm.c` | +29/-0 |
| `drivers/usb/gadget/function/u_serial.c` | +201/-7 |
| `drivers/watchdog/qcom-wdt.c` | +12/-0 |
| `fs/exec.c` | +4/-0 |
| `include/soc/qcom/cmd-db.h` | +11/-0 |
| `include/sound/cs35l41.h` | +5/-0 |
| `kernel/async.c` | +81/-1 |
| `kernel/fork.c` | +9/-0 |
| `mm/memory.c` | +6/-0 |
| `mm/mmap.c` | +8/-0 |
| `mm/mmu_gather.c` | +11/-0 |
| `mm/mmu_notifier.c` | +3/-0 |
| `mm/rmap.c` | +3/-0 |
| `mm/vma.c` | +3/-0 |
| `sound/core/pcm_lib.c` | +0/-3 |
| `sound/soc/codecs/Kconfig` | +4/-0 |
| `sound/soc/codecs/Makefile` | +2/-0 |
| `sound/soc/codecs/cs35l41-lib.c` | +29/-2 |
| `sound/soc/codecs/cs35l41.c` | +27/-0 |
| `sound/soc/codecs/lpass-tx-macro.c` | +129/-12 |
| `sound/soc/codecs/lpass-va-macro.c` | +377/-2 |
| `sound/soc/qcom/common.c` | +16/-3 |
| `sound/soc/qcom/qdsp6/q6afe-dai.c` | +23/-23 |
| `sound/soc/qcom/qdsp6/q6afe.c` | +17/-2 |
| `sound/soc/qcom/qdsp6/q6asm-dai.c` | +286/-30 |
| `sound/soc/qcom/qdsp6/q6asm.c` | +73/-4 |
| `sound/soc/qcom/qdsp6/q6dsp-lpass-clocks.c` | +46/-3 |
| `sound/soc/qcom/qdsp6/q6routing.c` | +7/-2 |
| `sound/soc/qcom/sm8250.c` | +375/-3 |
| `sound/soc/soc-dapm.c` | +24/-0 |
| `sound/soc/soc-pcm.c` | +30/-1 |

合计：71 个修改文件 + 4 个新增文件，6696 insertions(+)，313 deletions(-)
（`git diff --stat upstream-7.3.1..j716f-m10` 的原始输出）：

```text
 arch/arm64/boot/dts/qcom/Makefile                 |    2 +
 arch/arm64/boot/dts/qcom/sm8250-lenovo-j716f.dts  | 1462 +++++++++++++++++++++
 arch/arm64/boot/dts/qcom/sm8250.dtsi              |   25 +-
 arch/arm64/include/asm/tlbflush.h                 |    6 +
 drivers/base/dd.c                                 |    9 +
 drivers/bluetooth/hci_qca.c                       |   34 +
 drivers/clk/qcom/clk-rpmh.c                       |    4 +
 drivers/cpufreq/qcom-cpufreq-hw.c                 |    6 +
 drivers/firmware/psci/psci.c                      |    4 +
 drivers/firmware/qcom/qcom_scm.c                  |   17 +-
 drivers/gpu/drm/msm/adreno/a6xx_gmu.c             |   19 +
 drivers/gpu/drm/msm/adreno/a6xx_gpu.c             |    8 +-
 drivers/gpu/drm/msm/adreno/adreno_device.c        |   10 +
 drivers/gpu/drm/msm/adreno/adreno_gpu.c           |   32 +-
 drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c          |  182 ++-
 drivers/gpu/drm/msm/disp/dpu1/dpu_hw_interrupts.c |    9 +
 drivers/gpu/drm/msm/disp/dpu1/dpu_plane.c         |   20 +-
 drivers/gpu/drm/msm/dsi/dsi_host.c                |  156 ++-
 drivers/gpu/drm/msm/dsi/phy/dsi_phy.c             |   22 +-
 drivers/gpu/drm/msm/dsi/phy/dsi_phy_7nm.c         |   51 +-
 drivers/gpu/drm/msm/msm_gpu.c                     |   13 +
 drivers/gpu/drm/panel/panel-raydium-rm69380.c     |  214 ++-
 drivers/hid/i2c-hid/i2c-hid-core.c                |   16 +
 drivers/i2c/busses/i2c-qcom-geni.c                |   10 +-
 drivers/input/touchscreen/goodix.c                |  345 ++++-
 drivers/input/touchscreen/goodix.h                |   13 +
 drivers/interconnect/qcom/bcm-voter.c             |    5 +
 drivers/interconnect/qcom/osm-l3.c                |    3 +
 drivers/irqchip/irq-gic-v3.c                      |   50 +
 drivers/pmdomain/qcom/rpmhpd.c                    |   16 +-
 drivers/regulator/qcom-rpmh-regulator.c           |   45 +-
 drivers/soc/qcom/Kconfig                          |   14 +
 drivers/soc/qcom/Makefile                         |    1 +
 drivers/soc/qcom/cmd-db.c                         |   65 +
 drivers/soc/qcom/j716f-diag.c                     |  691 ++++++++++
 drivers/soc/qcom/qcom_pd_mapper.c                 |    7 +
 drivers/soc/qcom/rpmh-internal.h                  |   18 +-
 drivers/soc/qcom/rpmh-rsc.c                       |  242 +++-
 drivers/soc/qcom/rpmh.c                           |  307 +++--
 drivers/soc/qcom/ubwc_config.c                    |    2 +
 drivers/spmi/spmi-pmic-arb.c                      |   16 +-
 drivers/tty/serial/qcom_geni_serial.c             |   26 +-
 drivers/usb/gadget/function/f_ncm.c               |   29 +
 drivers/usb/gadget/function/u_serial.c            |  208 ++-
 drivers/watchdog/qcom-wdt.c                       |   12 +
 fs/exec.c                                         |    4 +
 include/soc/qcom/cmd-db.h                         |   11 +
 include/soc/qcom/j716f-diag.h                     |  159 +++
 include/sound/cs35l41.h                           |    5 +
 kernel/async.c                                    |   82 +-
 kernel/fork.c                                     |    9 +
 mm/memory.c                                       |    6 +
 mm/mmap.c                                         |    8 +
 mm/mmu_gather.c                                   |   11 +
 mm/mmu_notifier.c                                 |    3 +
 mm/rmap.c                                         |    3 +
 mm/vma.c                                          |    3 +
 sound/core/pcm_lib.c                              |    3 -
 sound/soc/codecs/Kconfig                          |    4 +
 sound/soc/codecs/Makefile                         |    2 +
 sound/soc/codecs/cs35l41-lib.c                    |   31 +-
 sound/soc/codecs/cs35l41.c                        |   27 +
 sound/soc/codecs/es7210.c                         |  704 ++++++++++
 sound/soc/codecs/lpass-tx-macro.c                 |  141 +-
 sound/soc/codecs/lpass-va-macro.c                 |  379 +++++-
 sound/soc/qcom/common.c                           |   19 +-
 sound/soc/qcom/qdsp6/q6afe-dai.c                  |   46 +-
 sound/soc/qcom/qdsp6/q6afe.c                      |   19 +-
 sound/soc/qcom/qdsp6/q6asm-dai.c                  |  316 ++++-
 sound/soc/qcom/qdsp6/q6asm.c                      |   77 +-
 sound/soc/qcom/qdsp6/q6dsp-lpass-clocks.c         |   49 +-
 sound/soc/qcom/qdsp6/q6routing.c                  |    9 +-
 sound/soc/qcom/sm8250.c                           |  378 +++++-
 sound/soc/soc-dapm.c                              |   24 +
 sound/soc/soc-pcm.c                               |   31 +-
 75 files changed, 6696 insertions(+), 313 deletions(-)
```

## 未纳入本仓的工作树文件

| 文件 | 原因 |
| --- | --- |
| `sound/soc/**/*.rej`（5 个） | 历史补丁被拒后的残留片段，不参与构建 |
| `out/` | 主机侧构建输出，不属于源码 |

补丁原文和时间线说明在主仓 `patches/`；本仓 `j716f/patches/` 是同一份副本。
