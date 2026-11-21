// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 */

#include <drm/drm_managed.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>

#include "dpu_hwio.h"
#include "dpu_hw_catalog.h"
#include "dpu_hw_lm.h"
#include "dpu_hw_dspp.h"
#include "dpu_kms.h"


/* DSPP_PCC */
#define PCC_EN BIT(0)
#define PCC_DIS 0
#define PCC_RED_R_OFF 0x10
#define PCC_RED_G_OFF 0x1C
#define PCC_RED_B_OFF 0x28
#define PCC_GREEN_R_OFF 0x14
#define PCC_GREEN_G_OFF 0x20
#define PCC_GREEN_B_OFF 0x2C
#define PCC_BLUE_R_OFF 0x18
#define PCC_BLUE_G_OFF 0x24
#define PCC_BLUE_B_OFF 0x30

/*
 * PCC v4 (SM8250, DSPP PCC version 0x40000) coefficient groups.  Each group
 * holds one coefficient for the three output components, four bytes apart:
 * output R at the group offset, output G at +0x4 and output B at +0x8.
 *
 * Taken from the vendor display driver, sde_hw_color_proc_common_v4.h
 * (PCC_C_OFF, PCC_R_OFF, ..., PCC_BB_OFF) and cross-checked against the
 * register packing in sde_hw_reg_dma_v1_color_proc.c, which writes 33
 * consecutive words starting at PCC_C_OFF.  The last coefficient is
 * PCC_BB_OFF + 0x8 = 0x84, inside the 0x90 byte PCC block.
 */
#define PCC_C_OFF 0x04
#define PCC_R_OFF 0x10
#define PCC_G_OFF 0x1c
#define PCC_B_OFF 0x28
#define PCC_RG_OFF 0x34
#define PCC_RB_OFF 0x40
#define PCC_GB_OFF 0x4c
#define PCC_RGB_OFF 0x58
#define PCC_RR_OFF 0x64
#define PCC_GG_OFF 0x70
#define PCC_BB_OFF 0x7c
#define PCC_LANE_STRIDE 0x04

/* DSPP_GC */
#define GC_EN BIT(0)
#define GC_DIS 0
#define GC_8B_ROUND_EN BIT(1)
#define GC_LUT_SWAP_OFF 0x1c
#define GC_C0_OFF 0x4
#define GC_C1_OFF 0xc
#define GC_C2_OFF 0x14
#define GC_C0_INDEX_OFF 0x8
#define GC_C1_INDEX_OFF 0x10
#define GC_C2_INDEX_OFF 0x18

/*
 * DSPP_IGC v3 (SM8250).  The IGC opmode/enable register is per DSPP at +0x0;
 * the LUT data ports live in the shared DSPP Top block and are selected by
 * one cleared bit in the top nibble.  The vendor writer clears the bit for
 * ctx->idx - 1, i.e. DSPP_0 uses 0xE, DSPP_1 0xD, and so on.
 */
#define IGC_OPMODE_OFF 0x0
#define IGC_C0_OFF 0x0
#define IGC_C1_OFF 0x4
#define IGC_C2_OFF 0x8
#define IGC_DATA_MASK (BIT(12) - 1)
#define IGC_DSPP_SEL_MASK_MAX (BIT(4) - 1)
#define IGC_DSPP_SEL_MASK(n) ((IGC_DSPP_SEL_MASK_MAX & ~(1 << (n))) << 28)
#define IGC_INDEX_UPDATE BIT(25)
#define IGC_EN BIT(0)

/* Serialise the shared, auto-incrementing IGC data ports across commits. */
static DEFINE_MUTEX(dpu_igc_lut_lock);

/*
 * DSPP 3D gamut (v4.2).  The register layout and the 0x9a70 payload layout
 * are the vendor SDM ones: four interleaved banks, table selector in
 * bits[12..15], LOWER=(G<<16)|B at +0xc, UPPER=R at +8, three 16-entry 1D
 * scale tables at +0x10/+0x50/+0x90 when the payload enables them.
 */
#define GAMUT_STATUS_OFF 0x000
#define GAMUT_TABLE_SEL_OFF 0x004
#define GAMUT_UPPER_COLOR_OFF 0x008
#define GAMUT_LOWER_COLOR_OFF 0x00c
#define GAMUT_1D_OFF 0x010
#define GAMUT_1D_STRIDE 0x040

#define GAMUT_PAYLOAD_MODE_OFF 0x008
#define GAMUT_PAYLOAD_1D_OFF 0x00c
#define GAMUT_PAYLOAD_LOWER_OFF 0x0cc
#define GAMUT_PAYLOAD_UPPER_OFF 0x0d0
#define GAMUT_BANK_BYTES 0x2668
#define GAMUT_NODES_PER_BANK 1229

static void dpu_setup_dspp_pcc(struct dpu_hw_dspp *ctx,
		struct dpu_hw_pcc_cfg *cfg)
{

	u32 base;

	if (!ctx) {
		DRM_ERROR("invalid ctx %p\n", ctx);
		return;
	}

	base = ctx->cap->sblk->pcc.base;

	if (!base) {
		DRM_ERROR("invalid ctx %p pcc base 0x%x\n", ctx, base);
		return;
	}

	if (!cfg) {
		DRM_DEBUG_DRIVER("disable pcc feature\n");
		DPU_REG_WRITE(&ctx->hw, base, PCC_DIS);
		return;
	}

	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_R_OFF, cfg->r.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_G_OFF, cfg->r.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_RED_B_OFF, cfg->r.b);

	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_R_OFF, cfg->g.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_G_OFF, cfg->g.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_GREEN_B_OFF, cfg->g.b);

	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_R_OFF, cfg->b.r);
	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_G_OFF, cfg->b.g);
	DPU_REG_WRITE(&ctx->hw, base + PCC_BLUE_B_OFF, cfg->b.b);

	DPU_REG_WRITE(&ctx->hw, base, PCC_EN);
}

/*
 * Program one output component (lane 0 = R, 1 = G, 2 = B) of the PCC v4
 * coefficient block.  The order below is ascending by address; the vendor
 * AHB writer programs the second order terms first, but both paths end with
 * the same 33 values before the enable bit is set.
 */
static void dpu_setup_dspp_pcc_v4_lane(struct dpu_hw_dspp *ctx, u32 base,
				       u32 lane,
				       const struct dpu_hw_pcc_coeff *coeff)
{
	u32 off = base + lane * PCC_LANE_STRIDE;

	DPU_REG_WRITE(&ctx->hw, off + PCC_C_OFF, coeff->c);
	DPU_REG_WRITE(&ctx->hw, off + PCC_R_OFF, coeff->r);
	DPU_REG_WRITE(&ctx->hw, off + PCC_G_OFF, coeff->g);
	DPU_REG_WRITE(&ctx->hw, off + PCC_B_OFF, coeff->b);
	DPU_REG_WRITE(&ctx->hw, off + PCC_RG_OFF, coeff->rg);
	DPU_REG_WRITE(&ctx->hw, off + PCC_RB_OFF, coeff->rb);
	DPU_REG_WRITE(&ctx->hw, off + PCC_GB_OFF, coeff->gb);
	DPU_REG_WRITE(&ctx->hw, off + PCC_RGB_OFF, coeff->rgb);
	DPU_REG_WRITE(&ctx->hw, off + PCC_RR_OFF, coeff->rr);
	DPU_REG_WRITE(&ctx->hw, off + PCC_GG_OFF, coeff->gg);
	DPU_REG_WRITE(&ctx->hw, off + PCC_BB_OFF, coeff->bb);
}

static void dpu_setup_dspp_pcc_v4(struct dpu_hw_dspp *ctx,
				  struct dpu_hw_pcc_cfg *cfg)
{
	u32 base;

	if (!ctx) {
		DRM_ERROR("invalid ctx %p\n", ctx);
		return;
	}

	base = ctx->cap->sblk->pcc.base;

	if (!base) {
		DRM_ERROR("invalid ctx %p pcc base 0x%x\n", ctx, base);
		return;
	}

	if (!cfg) {
		DRM_DEBUG_DRIVER("disable pcc feature\n");
		DPU_REG_WRITE(&ctx->hw, base, PCC_DIS);
		return;
	}

	/*
	 * Always write all 33 coefficients: the block has no per-term update
	 * mask, so a partial update would leave the previous commit's cross
	 * and second order terms in place.
	 */
	dpu_setup_dspp_pcc_v4_lane(ctx, base, 0, &cfg->r);
	dpu_setup_dspp_pcc_v4_lane(ctx, base, 1, &cfg->g);
	dpu_setup_dspp_pcc_v4_lane(ctx, base, 2, &cfg->b);

	DPU_REG_WRITE(&ctx->hw, base, PCC_EN);
}

static void dpu_setup_dspp_gc(struct dpu_hw_dspp *ctx,
		struct dpu_hw_gc_lut *gc_lut)
{
	int i = 0;
	u32 base, reg;

	if (!ctx) {
		DRM_ERROR("invalid ctx\n");
		return;
	}

	base = ctx->cap->sblk->gc.base;

	if (!base) {
		DRM_ERROR("invalid ctx %p gc base\n", ctx);
		return;
	}

	if (!gc_lut) {
		DRM_DEBUG_DRIVER("disable gc feature\n");
		DPU_REG_WRITE(&ctx->hw, base, GC_DIS);
		return;
	}

	DPU_REG_WRITE(&ctx->hw, base + GC_C0_INDEX_OFF, 0);
	DPU_REG_WRITE(&ctx->hw, base + GC_C1_INDEX_OFF, 0);
	DPU_REG_WRITE(&ctx->hw, base + GC_C2_INDEX_OFF, 0);

	for (i = 0; i < PGC_TBL_LEN; i++) {
		DPU_REG_WRITE(&ctx->hw, base + GC_C0_OFF, gc_lut->c0[i]);
		DPU_REG_WRITE(&ctx->hw, base + GC_C1_OFF, gc_lut->c1[i]);
		DPU_REG_WRITE(&ctx->hw, base + GC_C2_OFF, gc_lut->c2[i]);
	}

	DPU_REG_WRITE(&ctx->hw, base + GC_LUT_SWAP_OFF, BIT(0));

	reg = GC_EN | ((gc_lut->flags & PGC_8B_ROUND) ? GC_8B_ROUND_EN : 0);
	DPU_REG_WRITE(&ctx->hw, base, reg);
}

static void dpu_setup_dspp_igc(struct dpu_hw_dspp *ctx,
		struct dpu_hw_igc_lut *igc_lut)
{
	u32 sel, val;
	int i;

	if (!ctx)
		return;

	if (!igc_lut) {
		DPU_REG_WRITE(&ctx->hw, IGC_OPMODE_OFF, 0);
		return;
	}

	sel = IGC_DSPP_SEL_MASK(ctx->idx - DSPP_0);

	/*
	 * The table ports auto-increment; one index-update per channel resets
	 * the port to entry 0.  Hold the lock until the enable write, otherwise
	 * a second CRTC could interleave its table into this one.
	 */
	mutex_lock(&dpu_igc_lut_lock);
	for (i = 0; i < DPU_IGC_LUT_SIZE; i++) {
		val = (igc_lut->c0[i] & IGC_DATA_MASK) | sel;
		if (i == 0)
			val |= IGC_INDEX_UPDATE;
		DPU_REG_WRITE(&ctx->hw_top, IGC_C0_OFF, val);
	}
	for (i = 0; i < DPU_IGC_LUT_SIZE; i++) {
		val = (igc_lut->c1[i] & IGC_DATA_MASK) | sel;
		if (i == 0)
			val |= IGC_INDEX_UPDATE;
		DPU_REG_WRITE(&ctx->hw_top, IGC_C1_OFF, val);
	}
	for (i = 0; i < DPU_IGC_LUT_SIZE; i++) {
		val = (igc_lut->c2[i] & IGC_DATA_MASK) | sel;
		if (i == 0)
			val |= IGC_INDEX_UPDATE;
		DPU_REG_WRITE(&ctx->hw_top, IGC_C2_OFF, val);
	}
	DPU_REG_WRITE(&ctx->hw, IGC_OPMODE_OFF, IGC_EN);
	/* Complete the relaxed MMIO writes before another CPU uses the ports. */
	wmb();
	mutex_unlock(&dpu_igc_lut_lock);
}

static void dpu_setup_dspp_gamut(struct dpu_hw_dspp *ctx,
				 const struct dpu_hw_gamut_lut *gamut_lut)
{
	const u8 *p;
	u32 base, flags;
	int bank, i;

	if (!ctx)
		return;

	base = ctx->cap->sblk->gamut.base;
	if (!base)
		return;

	if (!gamut_lut || !gamut_lut->data ||
	    gamut_lut->len != DPU_GAMUT_LUT_SIZE) {
		DPU_REG_WRITE(&ctx->hw, base + GAMUT_STATUS_OFF, 0);
		return;
	}

	p = gamut_lut->data;
	if (get_unaligned_le32(p + GAMUT_PAYLOAD_MODE_OFF) != 1) {
		DRM_ERROR("unsupported gamut payload mode %u\n",
			  get_unaligned_le32(p + GAMUT_PAYLOAD_MODE_OFF));
		DPU_REG_WRITE(&ctx->hw, base + GAMUT_STATUS_OFF, 0);
		return;
	}

	flags = get_unaligned_le32(p);

	for (bank = 0; bank < 4; bank++) {
		u32 bank_off = GAMUT_PAYLOAD_LOWER_OFF +
			       bank * GAMUT_BANK_BYTES;

		DPU_REG_WRITE(&ctx->hw, base + GAMUT_TABLE_SEL_OFF,
			      0x1000u << bank);

		for (i = 0; i < GAMUT_NODES_PER_BANK; i++) {
			u32 off = bank_off + i * 8;

			DPU_REG_WRITE(&ctx->hw,
				      base + GAMUT_LOWER_COLOR_OFF,
				      get_unaligned_le32(p + off));
			DPU_REG_WRITE(&ctx->hw,
				      base + GAMUT_UPPER_COLOR_OFF,
				      get_unaligned_le32(p + off + 4));
		}
	}

	if (flags & BIT(0)) {
		for (i = 0; i < 16; i++) {
			u32 off = GAMUT_PAYLOAD_1D_OFF + i * 4;

			DPU_REG_WRITE(&ctx->hw, base + GAMUT_1D_OFF,
				      get_unaligned_le32(p + off));
			DPU_REG_WRITE(&ctx->hw,
				      base + GAMUT_1D_OFF + GAMUT_1D_STRIDE,
				      get_unaligned_le32(p + off + 0x40));
			DPU_REG_WRITE(&ctx->hw,
				      base + GAMUT_1D_OFF + 2 * GAMUT_1D_STRIDE,
				      get_unaligned_le32(p + off + 0x80));
		}
	}

	/* mode 1 has size code 0; bit 1 advertises the 1D tables; bit 0 = on */
	DPU_REG_WRITE(&ctx->hw, base + GAMUT_STATUS_OFF,
		      (flags & BIT(0)) ? 0x3 : 0x1);
}

/**
 * dpu_hw_dspp_init() - Initializes the DSPP hw driver object.
 * should be called once before accessing every DSPP.
 * @dev:  Corresponding device for devres management
 * @cfg:  DSPP catalog entry for which driver object is required
 * @addr: Mapped register io address of MDP
 * Return: pointer to structure or ERR_PTR
 */
struct dpu_hw_dspp *dpu_hw_dspp_init(struct drm_device *dev,
				     const struct dpu_dspp_cfg *cfg,
				     void __iomem *addr)
{
	struct dpu_hw_dspp *c;

	if (!addr)
		return ERR_PTR(-EINVAL);

	c = drmm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);

	c->hw.blk_addr = addr + cfg->base;
	c->hw.log_mask = DPU_DBG_MASK_DSPP;

	/* Assign ops */
	c->idx = cfg->id;
	c->cap = cfg;
	if (c->cap->sblk->pcc.base) {
		if (c->cap->sblk->pcc_v4_full)
			c->ops.setup_pcc = dpu_setup_dspp_pcc_v4;
		else
			c->ops.setup_pcc = dpu_setup_dspp_pcc;
	}
	if (c->cap->sblk->gc.base)
		c->ops.setup_gc = dpu_setup_dspp_gc;
	if (c->cap->sblk->gamut.base)
		c->ops.setup_gamut = dpu_setup_dspp_gamut;
	if (c->cap->sblk->igc_lut_base) {
		c->hw_top.blk_addr = addr + c->cap->sblk->igc_lut_base;
		c->hw_top.log_mask = DPU_DBG_MASK_DSPP;
		c->ops.setup_igc = dpu_setup_dspp_igc;
	}

	return c;
}
