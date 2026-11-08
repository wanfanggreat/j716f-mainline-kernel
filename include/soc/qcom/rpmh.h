/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#ifndef __SOC_QCOM_RPMH_H__
#define __SOC_QCOM_RPMH_H__

#include <soc/qcom/tcs.h>
#include <linux/platform_device.h>


#if IS_ENABLED(CONFIG_QCOM_RPMH)
int rpmh_read(const struct device *dev, struct tcs_cmd *cmd);

int rpmh_write(const struct device *dev, enum rpmh_state state,
	       const struct tcs_cmd *cmd, u32 n);

int rpmh_write_async(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 n);

int rpmh_write_batch(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 *n);
/*
 * M20-P1c: always a real symbol.  rpmh.c defines it unconditionally (a plain
 * wrapper that stamps a software-only field), so the previous "static inline
 * when the diagnostic is off" version collided with that definition and
 * CONFIG_QCOM_J716F_DIAG=n failed to build.  Callers with no transaction
 * context pass 0.
 */
int rpmh_write_batch_txid(const struct device *dev, enum rpmh_state state,
			  const struct tcs_cmd *cmd, u32 *n, u32 txid);

void rpmh_invalidate(const struct device *dev);

#else

static inline int rpmh_read(const struct device *dev, struct tcs_cmd *cmd)
{ return -ENODEV; }

static inline int rpmh_write(const struct device *dev, enum rpmh_state state,
			     const struct tcs_cmd *cmd, u32 n)
{ return -ENODEV; }

static inline int rpmh_write_async(const struct device *dev,
				   enum rpmh_state state,
				   const struct tcs_cmd *cmd, u32 n)
{ return -ENODEV; }

static inline int rpmh_write_batch(const struct device *dev,
				   enum rpmh_state state,
				   const struct tcs_cmd *cmd, u32 *n)
{ return -ENODEV; }

static inline void rpmh_invalidate(const struct device *dev)
{
}

#endif /* CONFIG_QCOM_RPMH */

#endif /* __SOC_QCOM_RPMH_H__ */
