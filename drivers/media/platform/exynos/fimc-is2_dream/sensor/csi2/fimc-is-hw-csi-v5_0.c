/*
 * Samsung Exynos SoC series FIMC-IS2 driver
 *
 * exynos fimc-is2_dream hw csi control functions
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#if IS_ENABLED(CONFIG_EXYNOS_OTP)
#include <linux/exynos_otp.h>
#endif

#include "fimc-is-hw-api-common.h"
#include "fimc-is-config.h"
#include "fimc-is-type.h"
#include "fimc-is-regs.h"
#include "fimc-is-device-csi.h"
#include "fimc-is-hw.h"
#include "fimc-is-hw-csi-v5_0.h"

void csi_hw_phy_otp_config(u32 __iomem *base_reg, u32 instance)
{
#if IS_ENABLED(CONFIG_EXYNOS_OTP)
	int ret;
	int i;
	u16 magic_code;
	u8 type;
	u8 index_count;
	struct tune_bits *data;

	magic_code = OTP_MAGIC_MIPI_CSI0 + instance;

	ret = otp_tune_bits_parsed(magic_code, &type, &index_count, &data);
	if (ret) {
		err("otp_tune_bits_parsed is fail(%d)", ret);
		goto p_err;
	}

	for (i = 0; i < index_count; i++){
		writel(data[i].value, base_reg + (data[i].index * 4));
		info("[CSI%d]set OTP(index = 0x%X, value = 0x%X)\n", instance, data[i].index, data[i].value);
	}

p_err:
	return;
#endif
}

int csi_hw_reset(u32 __iomem *base_reg)
{
	int ret = 0;
	u32 retry = 10;
	u32 i;

	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_SW_RESET], 1);

	while (--retry) {
		udelay(10);
		if (fimc_is_hw_get_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_SW_RESET]) != 1)
			break;
	}

	if (!retry) {
		err("reset is fail(%d)", retry);
		ret = -EINVAL;
		goto p_err;
	}

	/* disable all virtual ch dma */
	for (i = 0; i < CSI_VIRTUAL_CH_MAX; i++)
		csi_hw_s_output_dma(base_reg, i, false);

p_err:
	return ret;
}

int csi_hw_s_settle(u32 __iomem *base_reg,
	u32 settle)
{
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_HSSETTLE], settle);

	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_S_CLKSETTLECTL], 1);
	return 0;
}

int csi_hw_s_phy_bctrl_n(u32 __iomem *base_reg,
	u32 ctrl, u32 n)
{
	if (n > 11) {
		err("invalid bctrl number(%d).\n", n);
		return -EINVAL;
	}

	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_BCTRL_0 + n],
			&csi_fields[CSIS_F_B_PHYCTRL], ctrl);

	return 0;
}

int csi_hw_s_phy_sctrl_n(u32 __iomem *base_reg,
	u32 ctrl, u32 n)
{
	if (n > 11) {
		err("invalid bctrl number(%d).\n", n);
		return -EINVAL;
	}

	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_SCTRL_0 + n],
			&csi_fields[CSIS_F_S_PHYCTRL], ctrl);

	return 0;
}

int csi_hw_s_phy_default_value(u32 __iomem *base_reg, u32 instance)
{
#ifdef CONFIG_SOC_EXYNOS8895
	csi_hw_s_phy_bctrl_n(base_reg, 0x1F4, 0);
	csi_hw_s_phy_bctrl_n(base_reg, 0x800, 1);
	csi_hw_s_phy_bctrl_n(base_reg, 0x10001249, 2);
	csi_hw_s_phy_bctrl_n(base_reg, 0x500002, 3);

	csi_hw_s_phy_sctrl_n(base_reg, 0x9E003E00, 0);
	csi_hw_s_phy_sctrl_n(base_reg, 0x46, 1);
	csi_hw_s_phy_sctrl_n(base_reg, 0xC000002C, 2);

	if (instance == CSI_ID_A || instance == CSI_ID_C) {
		csi_hw_s_phy_sctrl_n(base_reg, 0x3FEA, 4);
		csi_hw_s_phy_sctrl_n(base_reg, 0xC0000, 5);
		csi_hw_s_phy_sctrl_n(base_reg, 0x140, 6);
	} else if (instance == CSI_ID_B || instance == CSI_ID_D) {
		csi_hw_s_phy_sctrl_n(base_reg, 0xC000, 3);
	}
#endif
	return 0;
}

int csi_hw_s_lane(u32 __iomem *base_reg,
	struct fimc_is_image *image, u32 lanes, u32 mipi_speed)
{
	bool deskew = false;
	u32 calc_lane_speed = 0;
	u32 bit_width = 0;
	u32 width, height, fps, pixelformat;
	u32 val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL]);
	u32 lane;

	width = image->window.width;
	height = image->window.height;
	bit_width = image->format.bitwidth;
	fps = image->framerate;
	pixelformat = image->format.pixelformat;

	/* lane number */
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_LANE_NUMBER], lanes);

#ifdef USE_SENSOR_IF_DPHY
	/* deskew enable (only over than 1.5Gbps) */
	if (mipi_speed > 1500) {
		u32 phy_val = 0;
		deskew = true;

		/*
		 * 1. D-phy Slave S_DPHYCTL[13:0] setting
		 *   [0]     = 0b1	/ Skew calibration enable (default disabled)
		 *   [13:12] = 0bxx	/ Coarse delay selection for skew calibration
		 *   		 00: for test
		 *   		 01:   3Gbps ~ 4.5Gbps
		 *   		 10:   2Gbps ~   3Gbps
		 *   		 11: 1.5Gbps ~   2Gbps (default setting)
		 */
		phy_val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_PHY_SCTRL_0]);
		phy_val |= (1 << 0);
		if (mipi_speed > 3000)
			phy_val &= ~(1 << 13);
		else if (mipi_speed > 2000)
			phy_val &= ~(1 << 12);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_PHY_SCTRL_0], phy_val);

		/*
		 * 2. D-phy Slave byte clock control register enable
		 */
		phy_val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL]);
		phy_val = fimc_is_hw_set_field_value(phy_val, &csi_fields[CSIS_F_S_BYTE_CLK_ENABLE], 1);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL], phy_val);
	}

	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DESKEW_ENABLE], 1);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL], val);

	switch (lanes) {
	case CSI_DATA_LANES_1:
		/* lane 0 */
		lane = (0x1);
		break;
	case CSI_DATA_LANES_2:
		/* lane 0 + lane 1 */
		lane = (0x3);
		break;
	case CSI_DATA_LANES_3:
		/* lane 0 + lane 1 + lane 2 */
		lane = (0x7);
		break;
	case CSI_DATA_LANES_4:
		/* lane 0 + lane 1 + lane 2 + lane 3 */
		lane = (0xF);
		break;
	default:
		err("lanes is invalid(%d)", lanes);
		lane = (0xF);
		break;
	}

	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_ENABLE_DAT], lane);
#endif

	/* just for reference */
	calc_lane_speed = CSI_GET_LANE_SPEED(width, height, fps, bit_width, (lanes + 1), 15);

	info("[CSI] (%uX%u@%u/%ulane:%ubit/deskew(%d) %s%uMbps)\n",
			width, height, fps, lanes + 1, bit_width, deskew,
			deskew ? "" : "aproximately ",
			deskew ? mipi_speed : calc_lane_speed);

	return 0;
}

int csi_hw_s_control(u32 __iomem *base_reg, u32 id, u32 value)
{
	switch(id) {
	case CSIS_CTRL_INTERLEAVE_MODE:
		/* interleave mode */
		fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
				&csi_fields[CSIS_F_INTERLEAVE_MODE], value);
		break;
	case CSIS_CTRL_LINE_RATIO:
		/* line irq ratio */
		fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_LINE_INTR_CH0],
				&csi_fields[CSIS_F_LINE_INTR_CH_N], value);
		break;
	case CSIS_CTRL_DMA_ABORT_REQ:
		/* dma abort req */
		fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_DMA_CMN_CTRL],
				&csi_fields[CSIS_F_DMA_ABORT_REQ], value);
		break;
	case CSIS_CTRL_ENABLE_LINE_IRQ:
		fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_INT_MSK1],
				&csi_fields[CSIS_F_MSK_LINE_END], value);
		break;
	default:
		err("control id is invalid(%d)", id);
		break;
	}

	return 0;
}

int csi_hw_s_config(u32 __iomem *base_reg,
	u32 channel, struct fimc_is_vci_config *config, u32 width, u32 height)
{
	int ret = 0;
	u32 val, parallel;
	u32 otf_format;

	if (channel > CSI_VIRTUAL_CH_3) {
		err("invalid channel(%d)", channel);
		ret = -EINVAL;
		goto p_err;
	}

	if ((config->hwformat == HW_FORMAT_YUV420_8BIT) ||
		(config->hwformat == HW_FORMAT_YUV422_8BIT))
		parallel = 1;
	else
		parallel = 0;

	val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_ISP_CONFIG_CH0 + (channel * 3)]);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_VIRTUAL_CHANNEL], config->map);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DATAFORMAT], config->hwformat);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_PARALLEL], parallel);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_PIXEL_MODE], CSIS_REG_PIXEL_MODE);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_ISP_CONFIG_CH0 + (channel * 3)], val);

	val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_ISP_RESOL_CH0 + (channel * 3)]);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_VRESOL], height);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_HRESOL], width);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_ISP_RESOL_CH0 + (channel * 3)], val);

	if (channel == CSI_VIRTUAL_CH_0) {
		switch (config->hwformat) {
		case HW_FORMAT_RAW10:
			otf_format = 0;
			break;
		case HW_FORMAT_RAW12:
			otf_format = 1;
			break;
		case HW_FORMAT_RAW14:
			otf_format = 2;
			break;
		case HW_FORMAT_RAW8:
			otf_format = 3;
			break;
		default:
			err("invalid data format (%02X)", config->hwformat);
			ret = -EINVAL;
			goto p_err;
			break;
		}

		val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_OTF_FORMAT]);
		val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_OTF_FORMAT], otf_format);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_OTF_FORMAT], val);
	}

p_err:
	return ret;
}

#ifdef CONFIG_USE_SENSOR_GROUP
int csi_hw_s_config_dma(u32 __iomem *base_reg, u32 channel, struct fimc_is_frame_cfg *cfg, u32 hwformat)
#else
int csi_hw_s_config_dma(u32 __iomem *base_reg, u32 channel, struct fimc_is_image *image, u32 hwformat)
#endif
{
	int ret = 0;
	u32 val;
	u32 dma_dim = 0;
	u32 dma_pack12 = 0;
	u32 dma_format = 3; /* reserved value */
	u32 dma_stg_mode;

	if (channel > CSI_VIRTUAL_CH_3) {
		err("invalid channel(%d)", channel);
		ret = -EINVAL;
		goto p_err;
	}

#ifdef CONFIG_USE_SENSOR_GROUP
	if (cfg->format->pixelformat == V4L2_PIX_FMT_SBGGR10 ||
		cfg->format->pixelformat == V4L2_PIX_FMT_SBGGR12)
#else
	if (image->format.pixelformat == V4L2_PIX_FMT_SBGGR10 ||
		image->format.pixelformat == V4L2_PIX_FMT_SBGGR12)
#endif
		dma_pack12 = CSIS_REG_DMA_PACK12;
	else
		dma_pack12 = CSIS_REG_DMA_NORMAL;

#ifdef CONFIG_USE_SENSOR_GROUP
	switch (cfg->format->pixelformat)
#else
	switch (image->format.pixelformat)
#endif
	{
	case V4L2_PIX_FMT_SGRBG8:
	case V4L2_PIX_FMT_SBGGR8:
		dma_dim = CSIS_REG_DMA_1D_DMA;
		break;
	default:
		dma_dim = CSIS_REG_DMA_2D_DMA;
		break;
	}

	switch (hwformat) {
	case HW_FORMAT_RAW10:
		dma_format = 0;
		dma_stg_mode = CSIS_REG_DMA_STG_LEGACY_MODE;
		break;
	case HW_FORMAT_RAW12:
		dma_format = 1;
		dma_stg_mode = CSIS_REG_DMA_STG_LEGACY_MODE;
		break;
	case HW_FORMAT_RAW14:
		dma_format = 2;
		dma_stg_mode = CSIS_REG_DMA_STG_LEGACY_MODE;
		break;
	case HW_FORMAT_USER:
	case HW_FORMAT_EMBEDDED_8BIT:
	case HW_FORMAT_YUV420_8BIT:
	case HW_FORMAT_YUV420_10BIT:
	case HW_FORMAT_YUV422_8BIT:
	case HW_FORMAT_YUV422_10BIT:
	case HW_FORMAT_RGB565:
	case HW_FORMAT_RGB666:
	case HW_FORMAT_RGB888:
	case HW_FORMAT_RAW6:
	case HW_FORMAT_RAW7:
	case HW_FORMAT_RAW8:
		dma_stg_mode = CSIS_REG_DMA_STG_PACKET_MODE;
		break;
	default:
		err("invalid data format (%02X)", hwformat);
		ret = -EINVAL;
		goto p_err;
		break;
	}

	val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_FMT + (channel * 17)]);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_DIM], dma_dim);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_PACK12], dma_pack12);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_STORAGE_MODE], dma_stg_mode);

	if (dma_stg_mode == CSIS_REG_DMA_STG_LEGACY_MODE) {
		/* below setting is only valid for storage legacy mode */
		val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_DATAFORMAT], dma_format);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_FMT + (channel * 17)], val);

		val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_RESOL + (channel * 17)]);
#ifdef CONFIG_USE_SENSOR_GROUP
		val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_RESOL], cfg->width);
#else
		val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_RESOL], image->window.width);
#endif
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_RESOL + (channel * 17)], val);
	} else if (dma_stg_mode == CSIS_REG_DMA_STG_PACKET_MODE) {
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_FMT + (channel * 17)], val);
	}

	val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA_DATA_CTRL]);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_INPUT_PATH], CSIS_DMA_INPUT_PATH);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA_DATA_CTRL], val);

p_err:
	return ret;
}

int csi_hw_s_irq_msk(u32 __iomem *base_reg, bool on)
{
	u32 otf_msk, dma_msk;

	/* default setting */
	if (on) {
		/* base interrupt setting */
		otf_msk = CSIS_IRQ_MASK0;
		dma_msk = CSIS_DMA_IRQ_MASK;
#if defined(SUPPORTED_EARLYBUF_DONE_SW) || defined(SUPPORTED_EARLYBUF_DONE_HW)
#if defined(SUPPORTED_EARLYBUF_DONE_HW)
		dma_msk = fimc_is_hw_set_field_value(dma_msk, &csi_fields[CSIS_F_MSK_LINE_END], 0x1);
#endif
		otf_msk = fimc_is_hw_set_field_value(otf_msk, &csi_fields[CSIS_F_FRAMEEND], 0x0);
#endif
	} else {
		otf_msk = 0;
		dma_msk = 0;
	}

	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_CSIS_INT_MSK0], otf_msk);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA_INT_MASK], dma_msk);

	return 0;
}

static void csi_hw_g_err_types_from_err(u32 err_src0, u32 err_src1, u32 *err_id)
{
	int i = 0;
	u32 sot_hs_err = 0;
	u32 lost_fs_err = 0;
	u32 lost_fe_err = 0;
	u32 ovf_err = 0;
	u32 wrong_cfg_err = 0;
	u32 err_ecc_err = 0;
	u32 crc_err = 0;
	u32 err_id_err = 0;
#ifdef USE_SENSOR_IF_CPHY
	u32 mal_crc_err = 0;
	u32 inval_code_hs = 0;
	u32 sot_sync_hs = 0
#endif

	sot_hs_err    = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_SOT_HS   ]);
	lost_fs_err   = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_LOST_FS  ]);
	lost_fe_err   = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_LOST_FE  ]);
	ovf_err       = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_OVER     ]);
	wrong_cfg_err = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_WRONG_CFG]);
	err_ecc_err   = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_ECC      ]);
	crc_err       = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_CRC_DPHY ]);
	err_id_err    = fimc_is_hw_get_field_value(err_src0, &csi_fields[CSIS_F_ERR_ID       ]);

#ifdef USE_SENSOR_IF_CPHY
	inval_code_hs = fimc_is_hw_get_field_value(err_src1, &csi_fields[CSIS_F_RXINVALIDCODEHS]);
	sot_sync_hs   = fimc_is_hw_get_field_value(err_src1, &csi_fields[CSIS_F_ERRSOTSYNCHS   ]);
	mal_crc_err   = fimc_is_hw_get_field_value(err_src1, &csi_fields[CSIS_F_MAL_CRC        ]);
	crc_err      |= fimc_is_hw_get_field_value(err_src1, &csi_fields[CSIS_F_ERR_CRC_CPHY   ]);
#endif

	for (i = 0; i < CSI_VIRTUAL_CH_MAX; i++) {
		err_id[i] |= ((sot_hs_err    & (1 << i)) ? (1 << CSIS_ERR_SOT_VC) : 0);
		err_id[i] |= ((lost_fs_err   & (1 << i)) ? (1 << CSIS_ERR_LOST_FS_VC) : 0);
		err_id[i] |= ((lost_fe_err   & (1 << i)) ? (1 << CSIS_ERR_LOST_FE_VC) : 0);
		err_id[i] |= ((ovf_err       & (1 << i)) ? (1 << CSIS_ERR_OVERFLOW_VC) : 0);
		err_id[i] |= ((wrong_cfg_err & (1 << i)) ? (1 << CSIS_ERR_WRONG_CFG) : 0);
		err_id[i] |= ((err_ecc_err   & (1 << i)) ? (1 << CSIS_ERR_ECC) : 0);
		err_id[i] |= ((crc_err       & (1 << i)) ? (1 << CSIS_ERR_CRC) : 0);
		err_id[i] |= ((err_id_err    & (1 << i)) ? (1 << CSIS_ERR_ID) : 0);
#ifdef USE_SENSOR_IF_CPHY
		err_id[i] |= ((inval_code_hs & (1 << i)) ? (1 << CSIS_ERR_INVALID_CODE_HS) : 0);
		err_id[i] |= ((sot_sync_hs & (1 << i)) ? (1 << CSIS_ERR_SOT_SYNC_HS) : 0);
		err_id[i] |= ((mal_crc_err & (1 << i)) ? (1 << CSIS_ERR_MAL_CRC) : 0);
#endif
	}
}

static void csi_hw_g_err_types_from_err_dma(u32 __iomem *base_reg, u32 val, u32 *err_id)
{
	int i;
	u32 dma_otf_overlap = 0;
	u32 dmafifo_full_err = 0;
	u32 dma_abort_done = 0;

	dma_otf_overlap = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_DMA_OTF_OVERLAP]);
	dma_abort_done = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_DMA_ABORT_DONE]);
	dmafifo_full_err = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_DMA_ERROR]);

	for (i = 0; i < CSI_VIRTUAL_CH_MAX; i++) {
		err_id[i] |= ((dma_otf_overlap   & (1 << i)) ? (1 << CSIS_ERR_OTF_OVERLAP) : 0);
		err_id[i] |= ((dmafifo_full_err  & 1) ? (1 << CSIS_ERR_DMA_ERR_DMAFIFO_FULL) : 0);
		err_id[i] |= ((dma_abort_done  & 1) ? (1 << CSIS_ERR_DMA_ABORT_DONE) : 0);
	}
}

static bool csi_hw_g_value_of_err(u32 __iomem *base_reg, u32 otf_src0, u32 otf_src1, u32 dma_src, u32 *err_id)
{
	u32 err_src0 = (otf_src0 & CSIS_ERR_MASK0);
	u32 err_src1 = (otf_src1 & CSIS_ERR_MASK1);
	u32 err_dma_src;
	bool err_flag = false;

	if (err_src0 || err_src1) {
		csi_hw_g_err_types_from_err(err_src0, err_src1, err_id);
		err_flag = true;
	}

	err_dma_src = (dma_src & CSIS_DMA_ERR_MASK);
	if (err_dma_src) {
		csi_hw_g_err_types_from_err_dma(base_reg, err_dma_src, err_id);
		err_flag = true;
	}

	return err_flag;
}

int csi_hw_g_irq_src(u32 __iomem *base_reg, struct csis_irq_src *src, bool clear)
{
	u32 otf_src0;
	u32 otf_src1;
	u32 dma_src;

	otf_src0 = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_CSIS_INT_SRC0]);
	otf_src1 = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_CSIS_INT_SRC1]);
	dma_src = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA_INT_SRC]);

	if (clear) {
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_CSIS_INT_SRC0], otf_src0);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_CSIS_INT_SRC1], otf_src1);
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA_INT_SRC], dma_src);
	}

	src->dma_start = fimc_is_hw_get_field_value(dma_src, &csi_fields[CSIS_F_MSK_DMA_FRM_START]);
	src->dma_end = fimc_is_hw_get_field_value(dma_src, &csi_fields[CSIS_F_MSK_DMA_FRM_END]);
#ifdef CONFIG_CSIS_V4_0
	/* HACK: For dual scanario in EVT0, we should use only DMA interrupt */
	src->otf_start = src->dma_start;
	src->otf_end = src->dma_end;
#else
	src->otf_start = fimc_is_hw_get_field_value(otf_src0, &csi_fields[CSIS_F_FRAMESTART]);
	src->otf_end = fimc_is_hw_get_field_value(otf_src0, &csi_fields[CSIS_F_FRAMEEND]);
#endif
	src->line_end = fimc_is_hw_get_field_value(otf_src1, &csi_fields[CSIS_F_MSK_LINE_END]);
	src->err_flag = csi_hw_g_value_of_err(base_reg, otf_src0, otf_src1, dma_src, (u32 *)src->err_id);

	return 0;
}

void csi_hw_s_frameptr(u32 __iomem *base_reg, u32 vc, u32 number, bool clear)
{
	u32 frame_ptr = 0;
	u32 val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_CTRL + (vc * 17)]);
	frame_ptr = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_DMA_N_UPDT_FRAMEPTR]);
	if (clear)
		frame_ptr |= (1 << number);
	else
		frame_ptr &= ~(1 << number);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_UPDT_FRAMEPTR], frame_ptr);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_CTRL + (vc * 17)], val);
}

u32 csi_hw_g_frameptr(u32 __iomem *base_reg, u32 vc)
{
	u32 frame_ptr = 0;
	u32 val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_ACT_CTRL + (vc * 17)]);
	frame_ptr = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_ACTIVE_DMA_N_FRAMEPTR]);

	return frame_ptr;
}

void csi_hw_s_dma_addr(u32 __iomem *base_reg, u32 vc, u32 number, u32 addr)
{
	u32 i = 0;

	csi_hw_s_frameptr(base_reg, vc, number, false);

	/*
	 * SW W/R for CSIS frameptr reset problem.
	 * If CSIS interrupt hanler's action delayed due to busy system,
	 * DMA enable state still applied in CSIS. But the frameptr can't be updated..
	 * So CSIS H/W automatically will increase the frameprt from 0 to 1.
	 * 1's dma address was zero..so page fault problam can be happened.
	 */
	for (i = 0; i < 8; i++)
		fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_ADDR1 + (vc * 17) + i], addr);
}

void csi_hw_s_multibuf_dma_addr(u32 __iomem *base_reg, u32 vc, u32 number, u32 addr)
{
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_ADDR1 + (vc * 17) + number], addr);
}

void csi_hw_s_output_dma(u32 __iomem *base_reg, u32 vc, bool enable)
{
	u32 val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_CTRL + (vc * 17)]);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_DISABLE], !enable);
	val = fimc_is_hw_set_field_value(val, &csi_fields[CSIS_F_DMA_N_UPDT_PTR_EN], enable);
	fimc_is_hw_set_reg(base_reg, &csi_regs[CSIS_R_DMA0_CTRL + (vc * 17)], val);
}

bool csi_hw_g_output_dma_enable(u32 __iomem *base_reg, u32 vc)
{
	/* if DMA_DISABLE field value is 1, this means dma output is disabled */
	if (fimc_is_hw_get_field(base_reg, &csi_regs[CSIS_R_DMA0_CTRL + (vc * 17)],
			&csi_fields[CSIS_F_DMA_N_DISABLE]))
		return false;
	else
		return true;
}

bool csi_hw_g_output_cur_dma_enable(u32 __iomem *base_reg, u32 vc)
{
	u32 val = fimc_is_hw_get_reg(base_reg, &csi_regs[CSIS_R_DMA0_ACT_CTRL + (vc * 17)]);
	/* if DMA_ENABLE field value is 1, this means dma output is enabled */
	bool dma_enable = fimc_is_hw_get_field_value(val, &csi_fields[CSIS_F_ACTIVE_DMA_N_ENABLE]);

	return dma_enable;
}

void csi_hw_set_start_addr(u32 __iomem *base_reg, u32 number, u32 addr)
{
	u32 __iomem *target_reg;

	if (number == 0) {
		target_reg = base_reg + TO_WORD_OFFSET(0x30);
	} else {
		number--;
		target_reg = base_reg + TO_WORD_OFFSET(0x200 + (0x4*number));
	}

	writel(addr, target_reg);
}

int csi_hw_dma_common_reset(void)
{
	return 0;
}

int csi_hw_s_dma_common(u32 __iomem *base_reg)
{
	u32 val;

	if (!base_reg)
		return 0;

	/* Common DMA Arbitration Priority register */
	/* CSIS_DMA_F_DMA_ARB_PRI_1 : 1 = CSIS2 DMA has a high priority */
	/* CSIS_DMA_F_DMA_ARB_PRI_1 : 2 = CSIS3 DMA has a high priority */
	/* CSIS_DMA_F_DMA_ARB_PRI_0 : 1 = CSIS0 DMA has a high priority */
	/* CSIS_DMA_F_DMA_ARB_PRI_0 : 2 = CSIS1 DMA has a high priority */
	val = fimc_is_hw_get_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_ARB_PRI]);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_DMA_ARB_PRI_1], 0x1);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_DMA_ARB_PRI_0], 0x2);
	fimc_is_hw_set_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_ARB_PRI], val);

	/* Common DMA Control register */
	/* CSIS_DMA_F_IP_PROCESSING : 1 = Q-channel clock enable  */
	/* CSIS_DMA_F_IP_PROCESSING : 0 = Q-channel clock disable */
	val = fimc_is_hw_get_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_CTRL]);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_IP_PROCESSING], 0x1);
	fimc_is_hw_set_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_CTRL], val);

	/* Common DMA SRAM split register */
	/* CSIS_DMA_F_DMA_SRAM1_SPLIT : internal SRAM1 is 10KB (640 * 16 bytes) */
	/* CSIS_DMA_F_DMA_SRAM0_SPLIT : internal SRAM0 is 10KB (640 * 16 bytes) */
	/* This register can be set between 0 to 640 */
	val = fimc_is_hw_get_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_SRAM_SPLIT]);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_DMA_SRAM1_SPLIT], 0x140);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_DMA_SRAM0_SPLIT], 0x140);
	fimc_is_hw_set_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_SRAM_SPLIT], val);

	/* Common DMA Martix register */
	/* CSIS_DMA_F_DMA_MATRIX : Under Table see */
	/*       CSIS0      CSIS1      CSIS2      CSIS3  */
	/* 0  : SRAM0_0    SRAM0_0    SRAM1_0    SRAM1_1 */
	/* 2  : SRAM0_0    SRAM1_0    SRAM0_1    SRAM1_1 */
	/* 5  : SRAM0_0    SRAM1_1    SRAM1_0    SRAM0_1 */
	/* 14 : SRAM1_0    SRAM0_1    SRAM0_0    SRAM1_1 */
	/* 16 : SRAM1_0    SRAM1_1    SRAM0_0    SRAM0_1 */
	/* 17 : SRAM1_0    SRAM1_1    SRAM0_1    SRAM0_0 */
	/* 22 : SRAM1_1    SRAM1_0    SRAM0_0    SRAM0_1 */
	/* 23 : SRAM1_1    SRAM1_0    SRAM0_1    SRAM0_0 */
	val = fimc_is_hw_get_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_MATRIX]);
	val = fimc_is_hw_set_field_value(val, &csi_dma_fields[CSIS_DMA_F_DMA_MATRIX], 0x0);
	fimc_is_hw_set_reg(base_reg, &csi_dma_regs[CSIS_DMA_R_COMMON_DMA_MATRIX], val);

	return 0;
}

int csi_hw_enable(u32 __iomem *base_reg)
{
	/* update shadow */
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_UPDATE_SHADOW], 0xF);

	/* PHY selection */
#ifdef USE_SENSOR_IF_CPHY
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_PHY_SEL], 1);
#else
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_PHY_SEL], 0);
#endif

	/* PHY on */
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_ENABLE_CLK], 1);

	/* csi enable */
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_CSI_EN], 1);

	return 0;
}

int csi_hw_disable(u32 __iomem *base_reg)
{
	/* PHY off */
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_ENABLE_CLK], 0);
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_PHY_CMN_CTRL],
			&csi_fields[CSIS_F_ENABLE_DAT], 0);

	/* csi disable */
	fimc_is_hw_set_field(base_reg, &csi_regs[CSIS_R_CSIS_CMN_CTRL],
			&csi_fields[CSIS_F_CSI_EN], 0);

	return 0;
}

int csi_hw_dump(u32 __iomem *base_reg)
{
	info("CSI 5.0 DUMP\n");
	fimc_is_hw_dump_regs(base_reg, csi_regs, CSIS_REG_CNT);

	return 0;
}
