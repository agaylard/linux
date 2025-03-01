// SPDX-License-Identifier: GPL-2.0-only
/**
 * (C) Copyright 2019 Sunplus Technology. <http://www.sunplus.com/>
 *
 * Sunplus SD host controller v3.0
 *
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/of_device.h>
#include <linux/reset.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/clk.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>
#include "sunplus_sd3.h"

enum loglevel {
	SPSDC_LOG_OFF,
	SPSDC_LOG_ERROR,
	SPSDC_LOG_WARNING,
	SPSDC_LOG_INFO,
	SPSDC_LOG_DEBUG,
	SPSDC_LOG_VERBOSE,
	SPSDC_LOG_MAX
};
static int loglevel = SPSDC_LOG_WARNING;

/**
 * we do not need `SPSDC_LOG_' prefix here, when specify @level.
 */
#define spsdc_pr(level, fmt, ...)	\
	do {	\
		if (unlikely(SPSDC_LOG_##level <= loglevel))	\
			pr_info("SPSDC [" #level "] " fmt, ##__VA_ARGS__);	\
	} while (0)

/* Produces a mask of set bits covering a range of a 32-bit value */
static inline u32 bitfield_mask(u32 shift, u32 width)
{
	return ((1 << width) - 1) << shift;
}

/* Extract the value of a bitfield found within a given register value */
static inline u32 bitfield_extract(u32 reg_val, u32 shift, u32 width)
{
	return (reg_val & bitfield_mask(shift, width)) >> shift;
}

/* Replace the value of a bitfield found within a given register value */
static inline u32 bitfield_replace(u32 reg_val, u32 shift, u32 width, u32 val)
{
	u32 mask = bitfield_mask(shift, width);

	return (reg_val & ~mask) | (val << shift);
}
/* for register value with mask bits */
#define __bitfield_replace(value, shift, width, new_value)		\
	(bitfield_replace(value, shift, width, new_value) | bitfield_mask(shift+16, width))

#ifdef SPSDC_DEBUG
#define SPSDC_REG_SIZE (sizeof(struct spsdc_regs)) /* register address space size */
#define SPSDC_REG_GRPS (sizeof(struct spsdc_regs) / 128) /* we organize 32 registers as a group */
#define SPSDC_REG_CNT  (sizeof(struct spsdc_regs) / 4) /* total registers */

/**
 * dump a range of registers.
 * @host: host
 * @start_group: dump start from which group, base is 0
 * @start_reg: dump start from which register in @start_group
 * @len: how many registers to dump
 */
static void spsdc_dump_regs(struct spsdc_host *host, int start_group, int start_reg, int len)
{
	u32 *p = (u32 *)host->base;
	u32 *reg_end = p + SPSDC_REG_CNT;
	u32 *end;
	int groups;
	int i, j;

	if (start_group > SPSDC_REG_GRPS || start_reg > 31)
		return;
	p += start_group * 32 + start_reg;
	if (p > reg_end)
		return;
	end = p + len;
	groups = (len + 31) / 32;
	pr_info("groups = %d\n", groups);
	pr_info("### dump sd card controller registers start ###\n");
	for (i = 0; i < groups; i++) {
		for (j =  start_reg; j < 32 && p < end; j++) {
			pr_info("g%02d.%02d = 0x%08x\n", i+start_group, j, readl(p));
			p++;
		}
		start_reg = 0;
	}
	pr_info("### dump sd card controller registers end ###\n");
}
#endif

/**
 * wait for transaction done, return -1 if error.
 */
static inline int spsdc_wait_finish(struct spsdc_host *host)
{
	/* Wait for transaction finish */
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);

	while (!time_after(jiffies, timeout)) {
		if (readl(&host->base->sd_state) & SPSDC_SDSTATE_FINISH)
			return 0;
		if (readl(&host->base->sd_state) & SPSDC_SDSTATE_ERROR)
			return -1;
	}
	return -1;
}

static inline int spsdc_wait_sdstatus(struct spsdc_host *host, unsigned int status_bit)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);

	while (!time_after(jiffies, timeout)) {
		if (readl(&host->base->sd_status) & status_bit)
			return 0;
		if (readl(&host->base->sd_state) & SPSDC_SDSTATE_ERROR)
			return -1;
	}
	return -1;
}

#define spsdc_wait_rspbuf_full(host) spsdc_wait_sdstatus(host, SPSDC_SDSTATUS_RSP_BUF_FULL)
#define spsdc_wait_rxbuf_full(host) spsdc_wait_sdstatus(host, SPSDC_SDSTATUS_RX_DATA_BUF_FULL)
#define spsdc_wait_txbuf_empty(host) spsdc_wait_sdstatus(host, SPSDC_SDSTATUS_TX_DATA_BUF_EMPTY)

static inline __maybe_unused void spsdc_txdummy(struct spsdc_host *host, int count)
{
	u32 value;

	count &= 0x1ff;
	value = readl(&host->base->sd_config1);
	value = bitfield_replace(value, SPSDC_TX_DUMMY_NUM_w09, 9, count);
	writel(value, &host->base->sd_config1);
	value = readl(&host->base->sd_ctrl);
	value = bitfield_replace(value, SPSDC_sdctrl1_w01, 1, 1); /* trigger tx dummy */
	writel(value, &host->base->sd_ctrl);
}

static void spsdc_get_rsp(struct spsdc_host *host, struct mmc_command *cmd)
{
	u32 value0_3, value4_5;

	if (unlikely(!(cmd->flags & MMC_RSP_PRESENT)))
		return;
	if (unlikely(cmd->flags & MMC_RSP_136)) {
		if (spsdc_wait_rspbuf_full(host))
			return;
		value0_3 = readl(&host->base->sd_rspbuf0_3);
		value4_5 = readl(&host->base->sd_rspbuf4_5) & 0xffff;
		cmd->resp[0] = (value0_3 << 8) | (value4_5 >> 8);
		cmd->resp[1] = value4_5 << 24;
		value0_3 = readl(&host->base->sd_rspbuf0_3);
		value4_5 = readl(&host->base->sd_rspbuf4_5) & 0xffff;
		cmd->resp[1] |= value0_3 >> 8;
		cmd->resp[2] = value0_3 << 24;
		cmd->resp[2] |= value4_5 << 8;
		value0_3 = readl(&host->base->sd_rspbuf0_3);
		value4_5 = readl(&host->base->sd_rspbuf4_5) & 0xffff;
		cmd->resp[2] |= value0_3 >> 24;
		cmd->resp[3] = value0_3 << 8;
		cmd->resp[3] |= value4_5 >> 8;
	} else {
		if (spsdc_wait_rspbuf_full(host))
			return;
		value0_3 = readl(&host->base->sd_rspbuf0_3);
		value4_5 = readl(&host->base->sd_rspbuf4_5) & 0xffff;
		cmd->resp[0] = (value0_3 << 8) | (value4_5 >> 8);
		cmd->resp[1] = value4_5 << 24;
	}
}

static void spsdc_set_bus_clk(struct spsdc_host *host, int clk)
{
	unsigned int clkdiv;
	int f_min = host->mmc->f_min;
	int f_max = host->mmc->f_max;
	int soc_clk  = clk_get_rate(host->clk);
	u32 value = readl(&host->base->sd_config0);

	#ifdef CONFIG_SOC_SP7350 // Added temporarily. Remove when CCF is ready.
	soc_clk = SPMMC_SYS_CLK;
	#endif

	if (clk < f_min)
		clk = f_min;
	if (clk > f_max)
		clk = f_max;
	if (host->soc_clk != soc_clk)
		spsdc_pr(ERROR, "CCF clock error CCF_clk : %d source_clk : %d", soc_clk, host->soc_clk);

	clkdiv = (soc_clk/clk)-1;

	if ((clk_get_rate(host->clk) % clk) > (clk/10)) {
		clkdiv++;
		spsdc_pr(INFO, "clk down to %d,SYS_CLK %d,clkdiv %d real_clk %d\n", clk,
			soc_clk, clkdiv, (soc_clk / (clkdiv+1)));
	} else {
		spsdc_pr(INFO, "clk to %d,SYS_CLK %d,clkdiv %d real_clk %d\n", clk,
			soc_clk, clkdiv, (soc_clk / (clkdiv+1)));
	}

	if (clkdiv > 0xfff) {
		spsdc_pr(WARNING, "clock %d is too low to be set!\n", clk);
		clkdiv = 0xfff;
	}
	value = bitfield_replace(value, SPSDC_sdfqsel_w12, 12, clkdiv);
	writel(value, &host->base->sd_config0);

	/* In order to reduce the frequency of context switch,
	 * if it is high speed or upper, we do not use interrupt
	 * when send a command that without data transfering.
	 */
	if (clk > 25000000)
		host->use_int = 0;
	else
		host->use_int = 1;
}

static void spsdc_set_bus_timing(struct spsdc_host *host, unsigned int timing)
{
	u32 value = readl(&host->base->sd_config1);
	int clkdiv = readl(&host->base->sd_config0) >> SPSDC_sdfqsel_w12;
	int wr_delay = clkdiv < 7 ? 1 : 7;
	int rd_delay = clkdiv < 7 ? clkdiv+1 : 7;
	int hs_en = 1;
	char *timing_name;

	host->ddr_enabled = 0;

	switch (timing) {
	case MMC_TIMING_LEGACY:
		hs_en = 0;
		timing_name = "legacy";
		break;
	case MMC_TIMING_MMC_HS:
		timing_name = "mmc high-speed";
		break;
	case MMC_TIMING_SD_HS:
		wr_delay = 1;
		rd_delay = 4;
		timing_name = "sd high-speed";
		break;
	case MMC_TIMING_UHS_SDR50:
		wr_delay = 1;
		rd_delay = 3;
		timing_name = "sd uhs SDR50";
		break;
	case MMC_TIMING_UHS_SDR104:
		wr_delay = 1;
		rd_delay = 3;
		timing_name = "sd uhs SDR104";
		break;
	case MMC_TIMING_UHS_DDR50:
		host->ddr_enabled = 1;
		wr_delay = 2;
		rd_delay = 4;
		timing_name = "sd uhs DDR50";
		break;
	case MMC_TIMING_MMC_DDR52:
		host->ddr_enabled = 1;
		timing_name = "mmc DDR52";
		break;
	case MMC_TIMING_MMC_HS200:
		timing_name = "mmc HS200";
		break;
	default:
		timing_name = "invalid";
		hs_en = 0;
		break;
	}

	if (hs_en) {
		value = bitfield_replace(value, SPSDC_sdhigh_speed_en_w01, 1, 1); /* sd_high_speed_en */
		writel(value, &host->base->sd_config1);
		value = readl(&host->base->sd_timing_config0);
		value = bitfield_replace(value, SPSDC_sd_wr_dat_dly_sel_w03, 3, wr_delay); /* sd_wr_dat_dly_sel */
		value = bitfield_replace(value, SPSDC_sd_wr_cmd_dly_sel_w03, 3, wr_delay); /* sd_wr_cmd_dly_sel */
		value = bitfield_replace(value, SPSDC_sd_rd_dat_dly_sel_w03, 3, rd_delay); /* sd_wr_dat_dly_sel */
		value = bitfield_replace(value, SPSDC_sd_rd_rsp_dly_sel_w03, 3, rd_delay); /* sd_wr_cmd_dly_sel */
		value = bitfield_replace(value, SPSDC_sd_rd_crc_dly_sel_w03, 3, rd_delay); /* sd_wr_cmd_dly_sel */

		spsdc_pr(VERBOSE, "sd_timing_config0: 0x%08x\n", value);
		writel(value, &host->base->sd_timing_config0);
	} else {
		value = bitfield_replace(value, SPSDC_sdhigh_speed_en_w01, 1, 0);
		writel(value, &host->base->sd_config1);
	}
	if (host->ddr_enabled) {
		value = readl(&host->base->sd_config0);
		value = bitfield_replace(value, SPSDC_sdddrmode_w01, 1, 1); /* sdddrmode */
		writel(value, &host->base->sd_config0);
	} else {
		value = readl(&host->base->sd_config0);
		value = bitfield_replace(value, SPSDC_sdddrmode_w01, 1, 0);
		writel(value, &host->base->sd_config0);
	}

	spsdc_pr(INFO, "set bus timing to %s\n", timing_name);

}

static void spsdc_set_bus_width(struct spsdc_host *host, int width)
{
	u32 value = readl(&host->base->sd_config0);
	int bus_width;

	switch (width) {
	case MMC_BUS_WIDTH_8:
		value = bitfield_replace(value, SPSDC_sddatawd_w01, 1, 0);
		value = bitfield_replace(value, SPSDC_mmc8_en_w01, 1, 1);
		bus_width = 8;
		break;
	case MMC_BUS_WIDTH_4:
		value = bitfield_replace(value, SPSDC_sddatawd_w01, 1, 1);
		value = bitfield_replace(value, SPSDC_mmc8_en_w01, 1, 0);
		bus_width = 4;
		break;
	default:
		value = bitfield_replace(value, SPSDC_sddatawd_w01, 1, 0);
		value = bitfield_replace(value, SPSDC_mmc8_en_w01, 1, 0);
		bus_width = 1;
		break;
	};
	spsdc_pr(INFO, "set bus width to %d bit(s)\n", bus_width);
	writel(value, &host->base->sd_config0);
}
/**
 * select the working mode of controller: sd/sdio/emmc
 */
static void spsdc_select_mode(struct spsdc_host *host, int mode)
{
	u32 value = readl(&host->base->sd_config0);

	host->mode = mode;
	/* set `sdmmcmode', as it will sample data at fall edge
	 * of SD bus clock if `sdmmcmode' is not set when
	 * `sd_high_speed_en' is not set, which is not compliant
	 * with SD specification
	 */
	value = bitfield_replace(value, SPSDC_sdmmcmode_w01, 1, 1);
	switch (mode) {
	case SPSDC_MODE_EMMC:
		value = bitfield_replace(value, SPSDC_sdiomode_w01, 1, 0);
		writel(value, &host->base->sd_config0);
		break;
	case SPSDC_MODE_SDIO:
		value = bitfield_replace(value, SPSDC_sdiomode_w01, 1, 1);
		writel(value, &host->base->sd_config0);
		value = readl(&host->base->sdio_ctrl);
		value = bitfield_replace(value, SPSDC_INT_MULTI_TRIG_w01, 1, 1); /* int_multi_trig */
		writel(value, &host->base->sdio_ctrl);
		break;
	case SPSDC_MODE_SD:
	default:
		value = bitfield_replace(value, SPSDC_sdiomode_w01, 1, 0);
		host->mode = SPSDC_MODE_SD;
		writel(value, &host->base->sd_config0);
		break;
	}
}

static void spsdc_sw_reset(struct spsdc_host *host)
{
	u32 value;

	spsdc_pr(DEBUG, "sw reset\n");
	/* Must reset dma operation first, or it will
	 * be stuck on sd_state == 0x1c00 because of
	 * a controller software reset bug
	 */
	value = readl(&host->base->hw_dma_ctrl);
	value = bitfield_replace(value, SPSDC_dmaidle_w01, 1, 1);
	writel(value, &host->base->hw_dma_ctrl);
	value = bitfield_replace(value, SPSDC_dmaidle_w01, 1, 0);
	writel(value, &host->base->hw_dma_ctrl);
	value = readl(&host->base->hw_dma_ctrl);
	value = bitfield_replace(value, SPSDC_HW_DMA_RST_w01, 1, 1);
	writel(value, &host->base->hw_dma_ctrl);
	writel(0x7, &host->base->sd_rst);
	while (readl(&host->base->sd_hw_state) & BIT(6))
		;
	spsdc_pr(DEBUG, "sw reset done\n");

}

static void spsdc_prepare_cmd(struct spsdc_host *host, struct mmc_command *cmd)
{

	u32 value;

	value = ((cmd->opcode | 0x40) << 24) | (cmd->arg >> 8); /* add start bit, according to spec, command format */

	writel(value, &host->base->sd_cmdbuf0_3);
	writeb(cmd->arg & 0xff, &host->base->sd_cmdbuf4);

	/* disable interrupt if needed */
	value = readl(&host->base->sd_int);
	value = bitfield_replace(value, SPSDC_sd_cmp_clr_w01, 1, 1); /* sd_cmp_clr */
	if (likely(!host->use_int || cmd->flags & MMC_RSP_136))
		value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 0); /* sdcmpen */
	else
		value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 1);
	writel(value, &host->base->sd_int);

	value = readl(&host->base->sd_config0);
	value = bitfield_replace(value, SPSDC_trans_mode_w02, 2, 0); /* sd_trans_mode */
	value = bitfield_replace(value, SPSDC_sdcmddummy_w01, 1, 1); /* sdcmddummy */
	if (likely(cmd->flags & MMC_RSP_PRESENT)) {
		value = bitfield_replace(value, SPSDC_sdautorsp_w01, 1, 1); /* sdautorsp */
	} else {
		value = bitfield_replace(value, SPSDC_sdautorsp_w01, 1, 0);
		writel(value, &host->base->sd_config0);
		return;
	}
	/*
	 * Currently, host is not capable of checking R2's CRC7,
	 * thus, enable crc7 check only for 48 bit response commands
	 */
	if (likely(cmd->flags & MMC_RSP_CRC && !(cmd->flags & MMC_RSP_136)))
		value = bitfield_replace(value, SPSDC_sdrspchk_w01, 1, 1); /* sdrspchk_en */
	else
		value = bitfield_replace(value, SPSDC_sdrspchk_w01, 1, 0);

	if (unlikely(cmd->flags & MMC_RSP_136))
		value = bitfield_replace(value, SPSDC_sdrsptype_w01, 1, 1); /* sdrsptype */
	else
		value = bitfield_replace(value, SPSDC_sdrsptype_w01, 1, 0);
	writel(value, &host->base->sd_config0);
}

static void spsdc_prepare_data(struct spsdc_host *host, struct mmc_data *data)
{
	u32 value, srcdst;

	writel(data->blocks - 1, &host->base->sd_page_num);
	writel(data->blksz - 1, &host->base->sd_blocksize);
	value = readl(&host->base->sd_config0);
	if (data->flags & MMC_DATA_READ) {
		value = bitfield_replace(value, SPSDC_trans_mode_w02, 2, 2); /* sd_trans_mode */
		value = bitfield_replace(value, SPSDC_sdautorsp_w01, 1, 0); /* sdautorsp */
		value = bitfield_replace(value, SPSDC_sdcmddummy_w01, 1, 0); /* sdcmddummy */
		srcdst = readl(&host->base->card_mediatype_srcdst);
		srcdst = bitfield_replace(srcdst, SPSDC_dmasrc_w03, 7, SPSDC_DMA_READ);
		writel(srcdst, &host->base->card_mediatype_srcdst);
	} else {
		value = bitfield_replace(value, SPSDC_trans_mode_w02, 2, 1);
		srcdst = readl(&host->base->card_mediatype_srcdst);
		srcdst = bitfield_replace(srcdst, SPSDC_dmasrc_w03, 7, SPSDC_DMA_WRITE);
		writel(srcdst, &host->base->card_mediatype_srcdst);
	}

	/* to prevent of the responses of CMD18/25 being overrided by CMD12's,
	 * send CMD12 by ourself instead of by controller automatically
	 *
	 *	if ((cmd->opcode == MMC_READ_MULTIPLE_BLOCK) || (cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK))
	 *	value = bitfield_replace(value, SPSDC_sd_len_mode_w01, 1, 0); // sd_len_mode
	 *	else
	 *	value = bitfield_replace(value, SPSDC_sd_len_mode_w01, 1, 1);
	 *
	 */

	value = bitfield_replace(value, SPSDC_sd_len_mode_w01, 1, 1);

	if (likely(host->dmapio_mode == SPSDC_DMA_MODE)) {
		struct scatterlist *sg;
		dma_addr_t dma_addr;
		unsigned int dma_size;
		u32 *reg_addr;
		int dma_direction = data->flags & MMC_DATA_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
		int i, count = 1;

		count = dma_map_sg(host->mmc->parent, data->sg, data->sg_len, dma_direction);
		if (unlikely(!count || count > SPSDC_MAX_DMA_MEMORY_SECTORS)) {
			spsdc_pr(ERROR, "error occured at dma_mapp_sg: count = %d\n", count);
			data->error = -EINVAL;
			return;
		}
		for_each_sg(data->sg, sg, count, i) {
			dma_addr = sg_dma_address(sg);
			dma_size = sg_dma_len(sg) / data->blksz - 1;
			if (i == 0) {
				writel(dma_addr, &host->base->dma_base_addr);
				writel(dma_size, &host->base->sdram_sector_0_size);
			} else {
				reg_addr = &host->base->sdram_sector_1_addr + (i - 1) * 2;
				writel(dma_addr, reg_addr);
				writel(dma_size, reg_addr + 1);
			}
		}
		value = bitfield_replace(value, SPSDC_sdpiomode_w01, 1, 0); /* sdpiomode */
		writel(value, &host->base->sd_config0);
		/* enable interrupt if needed */
		if (!host->use_int && data->blksz * data->blocks > host->dma_int_threshold) {
			host->dma_use_int = 1;
			value = readl(&host->base->sd_int);
			value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 1); /* sdcmpen */
			writel(value, &host->base->sd_int);
		}
	} else {
		value = bitfield_replace(value, SPSDC_sdpiomode_w01, 1, 1);
		value = bitfield_replace(value, SPSDC_rx4_en_w01, 1, 1); /* rx4_en */
		writel(value, &host->base->sd_config0);
	}
}

static inline void spsdc_trigger_transaction(struct spsdc_host *host)
{
	u32 value = readl(&host->base->sd_ctrl);

	value = bitfield_replace(value, SPSDC_sdctrl0_w01, 1, 1); /* trigger transaction */
	writel(value, &host->base->sd_ctrl);
}

static int __send_stop_cmd(struct spsdc_host *host, struct mmc_command *stop)
{
	u32 value;

	spsdc_prepare_cmd(host, stop);
	value = readl(&host->base->sd_int);
	value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 0); /* sdcmpen */
	writel(value, &host->base->sd_int);
	spsdc_trigger_transaction(host);
	if (spsdc_wait_finish(host)) {
		value = readl(&host->base->sd_status);
		if (value & SPSDC_SDSTATUS_RSP_CRC7_ERROR)
			stop->error = -EILSEQ;
		else
			stop->error = -ETIMEDOUT;
		return -1;
	}

	spsdc_get_rsp(host, stop);
	return 0;
}

/*
 * check if error occured during transaction.
 * @host -  host
 * @mrq - the mrq
 * @return 0 if no error otherwise the error number.
 */
static int spsdc_check_error(struct spsdc_host *host, struct mmc_request *mrq)
{
	int ret = 0;
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_data *data = mrq->data;
	u32 value = readl(&host->base->sd_state);
	u32 crc_token = bitfield_extract(value, SPSDC_sdcrdcrc_w03, 3);
	u32 timing_cfg0 = readl(&host->base->sd_timing_config0);
	int clkdiv = readl(&host->base->sd_config0) >> SPSDC_sdfqsel_w12;

	if (unlikely(value & SPSDC_SDSTATE_ERROR)) {
		spsdc_pr(DEBUG, "%s cmd %d with data %08x error!\n", __func__, cmd->opcode, (unsigned int)(long)data);
		spsdc_pr(VERBOSE, "%s sd_state: 0x%08x\n", __func__, value);
		value = readl(&host->base->sd_status);
		spsdc_pr(VERBOSE, "%s sd_status: 0x%08x\n", __func__, value);

		if (host->tuning_info.enable_tuning) {
			timing_cfg0 = readl(&host->base->sd_timing_config0);
			host->tuning_info.rd_crc_dly = bitfield_extract(timing_cfg0, SPSDC_sd_rd_crc_dly_sel_w03, 3);
			host->tuning_info.rd_dat_dly = bitfield_extract(timing_cfg0, SPSDC_sd_rd_dat_dly_sel_w03, 3);
			host->tuning_info.rd_rsp_dly = bitfield_extract(timing_cfg0, SPSDC_sd_rd_rsp_dly_sel_w03, 3);
			host->tuning_info.wr_cmd_dly = bitfield_extract(timing_cfg0, SPSDC_sd_wr_cmd_dly_sel_w03, 3);
			host->tuning_info.wr_dat_dly = bitfield_extract(timing_cfg0, SPSDC_sd_wr_dat_dly_sel_w03, 3);
		}
		if (value & SPSDC_SDSTATUS_RSP_TIMEOUT) {
			spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_RSP_TIMEOUT\n");
			ret = -ETIMEDOUT;
			host->tuning_info.wr_cmd_dly++;
		} else if (value & SPSDC_SDSTATUS_RSP_CRC7_ERROR) {
			spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_RSP_CRC7_ERROR\n");
			ret = -EILSEQ;
			host->tuning_info.rd_rsp_dly++;
		} else if (data) {
			if ((value & SPSDC_SDSTATUS_STB_TIMEOUT)) {
				spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_STB_TIMEOUT\n");
				ret = -ETIMEDOUT;
				host->tuning_info.rd_dat_dly++;
			} else if (value & SPSDC_SDSTATUS_RDATA_CRC16_ERROR) {
				spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_RDATA_CRC16_ERROR\n");
				ret = -EILSEQ;
				host->tuning_info.rd_dat_dly++;
			} else if (value & SPSDC_SDSTATUS_CARD_CRC_CHECK_TIMEOUT) {
				spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_CARD_CRC_CHECK_TIMEOUT\n");
				ret = -ETIMEDOUT;
				host->tuning_info.rd_crc_dly++;
			} else if (value & SPSDC_SDSTATUS_CRC_TOKEN_CHECK_ERROR) {
				spsdc_pr(VERBOSE, "SPSDC_SDSTATUS_CRC_TOKEN_CHECK_ERROR\n");
				ret = -EILSEQ;
				if (crc_token == 0x5)
					host->tuning_info.wr_dat_dly++;
				else
					host->tuning_info.rd_crc_dly++;
			}
			}
		cmd->error = ret;
		if (data) {
			data->error = ret;
			data->bytes_xfered = 0;
		}

		if (!host->tuning_info.need_tuning) {
			if (clkdiv >= 500)
				cmd->retries = 1; /* retry it */
			else
				cmd->retries = SPSDC_MAX_RETRIES; /* retry it */
		}

		spsdc_sw_reset(host);
		mdelay(100);

		if (host->tuning_info.enable_tuning) {
			timing_cfg0 = bitfield_replace(timing_cfg0, SPSDC_sd_rd_crc_dly_sel_w03, 3, host->tuning_info.rd_crc_dly); /* sd_wr_dat_dly_sel */
			timing_cfg0 = bitfield_replace(timing_cfg0, SPSDC_sd_rd_dat_dly_sel_w03, 3, host->tuning_info.rd_dat_dly); /* sd_wr_cmd_dly_sel */
			timing_cfg0 = bitfield_replace(timing_cfg0, SPSDC_sd_rd_rsp_dly_sel_w03, 3, host->tuning_info.rd_rsp_dly); /* sd_wr_dat_dly_sel */
			timing_cfg0 = bitfield_replace(timing_cfg0, SPSDC_sd_wr_cmd_dly_sel_w03, 3, host->tuning_info.wr_cmd_dly); /* sd_wr_cmd_dly_sel */
			timing_cfg0 = bitfield_replace(timing_cfg0, SPSDC_sd_wr_dat_dly_sel_w03, 3, host->tuning_info.wr_dat_dly); /* sd_wr_cmd_dly_sel */
			writel(timing_cfg0, &host->base->sd_timing_config0);
		}

	} else if (data) {
		data->error = 0;
		data->bytes_xfered = data->blocks * data->blksz;
	}
	host->tuning_info.need_tuning = ret;
	return ret;
}

static void spsdc_xfer_data_pio(struct spsdc_host *host, struct mmc_data *data)
{
	u32 *buf; /* tx/rx 4 bytes one time in pio mode */
	int data_left = data->blocks * data->blksz;
	int consumed, remain;
	struct sg_mapping_iter *sg_miter = &host->sg_miter;
	unsigned int flags = 0;

	if (data->flags & MMC_DATA_WRITE)
		flags |= SG_MITER_FROM_SG;
	else
		flags |= SG_MITER_TO_SG;
	sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);
	while (data_left > 0) {
		consumed = 0;
		if (!sg_miter_next(sg_miter))
			break;
		buf = sg_miter->addr;
		remain = sg_miter->length;
		do {
			if (data->flags & MMC_DATA_WRITE) {
				if (spsdc_wait_txbuf_empty(host))
					goto done;
				writel(*buf, &host->base->sd_piodatatx);
			} else {
				if (spsdc_wait_rxbuf_full(host))
					goto done;
				*buf = readl(&host->base->sd_piodatarx);
			}
			buf++;
			consumed += 4; // enable rx4_en +=4  diaable +=2
			remain -= 4;
		} while (remain);
		sg_miter->consumed = consumed;
		data_left -= consumed;
	}
done:
	sg_miter_stop(sg_miter);
}

static void spsdc_controller_init(struct spsdc_host *host)
{
	u32 value;
	int ret = reset_control_assert(host->rstc);

	if (!ret) {
		mdelay(1);
		ret = reset_control_deassert(host->rstc);
	}
	if (ret)
		spsdc_pr(WARNING, "Failed to reset SD controller!\n");
	value = readl(&host->base->card_mediatype_srcdst);
	value = bitfield_replace(value, SPSDC_MediaType_w03, 3, SPSDC_MEDIA_SD);
	writel(value, &host->base->card_mediatype_srcdst);
	host->signal_voltage = MMC_SIGNAL_VOLTAGE_330;


#ifdef SPMMC_SDIO_1V8
	/* Because we do not have a regulator to change the voltage at
	 * runtime, we can only rely on hardware circuit to ensure that
	 * the device pull up voltage is 1.8V(ex: wifi module AP6256) and
	 * use the macro `SPMMC_SDIO_1V8'to indicate that. Set signal
	 * voltage to 1.8V here.
	 */
	if (host->mode == SPSDC_MODE_SDIO) {
		value = readl(&host->base->sd_vol_ctrl);
		value = bitfield_replace(value, SPSDC_sw_set_vol_w01, 1, 1);
		writel(value, &host->base->sd_vol_ctrl);
		mdelay(20);
		spsdc_txdummy(host, 400);
		host->signal_voltage = MMC_SIGNAL_VOLTAGE_180;
		spsdc_pr(INFO, "use signal voltage 1.8V for SDIO\n");
	}
#endif
}

static void spsdc_set_power_mode(struct spsdc_host *host, struct mmc_ios *ios)
{
	if (host->power_state == ios->power_mode)
		return;

	switch (ios->power_mode) {
		/* power off->up->on */
	case MMC_POWER_ON:
		spsdc_pr(DEBUG, "set SD_POWER_ON\n");
		spsdc_controller_init(host);
		pm_runtime_get_sync(host->mmc->parent);
		break;
	case MMC_POWER_UP:
		spsdc_pr(DEBUG, "setSD_POWER_UP\n");
		break;
	case MMC_POWER_OFF:
		spsdc_pr(DEBUG, "set SD_POWER_OFF\n");
		pm_runtime_put(host->mmc->parent);
		break;
	}
	host->power_state = ios->power_mode;
}

/**
 * 1. unmap scatterlist if needed;
 * 2. get response & check error conditions;
 * 3. unlock host->mrq_lock
 * 4. notify mmc layer the request is done
 */
static void spsdc_finish_request(struct spsdc_host *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd;
	struct mmc_data *data;

	if (!mrq)
		return;

	cmd = mrq->cmd;
	data = mrq->data;

	if (data && SPSDC_DMA_MODE == host->dmapio_mode) {
		int dma_direction = data->flags & MMC_DATA_READ ? DMA_FROM_DEVICE : DMA_TO_DEVICE;

		dma_unmap_sg(host->mmc->parent, data->sg, data->sg_len, dma_direction);
		host->dma_use_int = 0;
	}
	spsdc_get_rsp(host, cmd);
	spsdc_check_error(host, mrq);
	if (mrq->stop) {
		if (__send_stop_cmd(host, mrq->stop))
			spsdc_sw_reset(host);
	}
	host->mrq = NULL;
	mutex_unlock(&host->mrq_lock);
		//if((cmd->opcode != 13) && (cmd->opcode != 18) && (cmd->opcode != 25)){
	spsdc_pr(VERBOSE, "request done > error:%d, cmd:%d, resp:0x%08x\n", cmd->error, cmd->opcode, cmd->resp[0]);
		//	}
	mmc_request_done(host->mmc, mrq);
}


/* Interrupt Service Routine */
irqreturn_t spsdc_irq(int irq, void *dev_id)
{
	struct spsdc_host *host = dev_id;
	u32 value = readl(&host->base->sd_int);

	spin_lock(&host->lock);
	if ((value & SPSDC_SDINT_SDCMP) &&
		(value & SPSDC_SDINT_SDCMPEN)) {
		value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 0); /* disable sdcmmp */
		value = bitfield_replace(value, SPSDC_sd_cmp_w01, 1, 1); /* sd_cmp_clr */
		writel(value, &host->base->sd_int);
		/* we may need send stop command to stop data transaction,
		 * which is time consuming, so make use of tasklet to handle this.
		 */
		if (host->mrq && host->mrq->stop)
			tasklet_schedule(&host->tsklet_finish_req);
		else
			spsdc_finish_request(host, host->mrq);

	}
	if ((value & SPSDC_SDINT_SDIO) &&
		(value & SPSDC_SDINT_SDIOEN)) {
		mmc_signal_sdio_irq(host->mmc);
	}
	spin_unlock(&host->lock);
	return IRQ_HANDLED;
}

static void spsdc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct spsdc_host *host = mmc_priv(mmc);
	struct mmc_data *data;
	struct mmc_command *cmd;
	int ret = 0;

	ret = mutex_lock_interruptible(&host->mrq_lock);
	host->mrq = mrq;
	data = mrq->data;
	cmd = mrq->cmd;

//	if((host->mode == SPSDC_MODE_SD) && (cmd->opcode != 13) && (cmd->opcode != 18) && (cmd->opcode != 25)){
	spsdc_pr(VERBOSE, "%s > cmd:%d, arg:0x%08x, data len:%d\n", __func__,
		cmd->opcode, cmd->arg, data ? (data->blocks*data->blksz) : 0);
//}

#ifdef HW_VOLTAGE_1V8
	u32 value;

	value = readl(&host->base->sd_vol_ctrl);
	value = bitfield_replace(value, SPSDC_vol_tmr_w02, 2, 3); /* 1ms timeout for 400K */

	if (cmd->opcode == 11)
		value = bitfield_replace(value, SPSDC_hw_set_vol_w01, 1, 1);
	else
		value = bitfield_replace(value, SPSDC_hw_set_vol_w01, 1, 0);

	//spsdc_pr(WARNING, "base->sd_vol_ctrl!  0x%x\n",readl(&host->base->sd_vol_ctrl));
	writel(value, &host->base->sd_vol_ctrl);

#endif

	spsdc_prepare_cmd(host, cmd);

	/* we need manually read response R2. */
	if (unlikely(cmd->flags & MMC_RSP_136)) {
		spsdc_trigger_transaction(host);
		spsdc_get_rsp(host, cmd);
		spsdc_wait_finish(host);
		spsdc_check_error(host, mrq);
		host->mrq = NULL;
		spsdc_pr(VERBOSE, "request done > error:%d, cmd:%d, resp:%08x %08x %08x %08x\n",
			 cmd->error, cmd->opcode, cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);
		mutex_unlock(&host->mrq_lock);
		mmc_request_done(host->mmc, mrq);
	} else {
		if (data)
			spsdc_prepare_data(host, data);

		if (unlikely(host->dmapio_mode == SPSDC_PIO_MODE && data)) {
			u32 value;
			/* pio data transfer do not use interrupt */
			value = readl(&host->base->sd_int);
			value = bitfield_replace(value, SPSDC_sdcmpen_w01, 1, 0); /* sdcmpen */
			writel(value, &host->base->sd_int);
			spsdc_trigger_transaction(host);
			spsdc_xfer_data_pio(host, data);
			spsdc_wait_finish(host);
			spsdc_finish_request(host, mrq);
		} else {
			if (!(host->use_int || host->dma_use_int)) {
				spsdc_trigger_transaction(host);
				spsdc_wait_finish(host);
				spsdc_finish_request(host, mrq);
			} else {
				spsdc_trigger_transaction(host);
			}
		}
	}
}

static void spsdc_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct spsdc_host *host = (struct spsdc_host *)mmc_priv(mmc);

	mutex_lock(&host->mrq_lock);
	spsdc_set_power_mode(host, ios);
	spsdc_set_bus_clk(host, ios->clock);
	spsdc_set_bus_timing(host, ios->timing);
	spsdc_set_bus_width(host, ios->bus_width);
	/* ensure mode is correct, because we might have hw reset the controller */
	spsdc_select_mode(host, host->mode);
	mutex_unlock(&host->mrq_lock);

}

/**
 * Return values for the get_cd callback should be:
 *   0 for a absent card
 *   1 for a present card
 *   -ENOSYS when not supported (equal to NULL callback)
 *   or a negative errno value when something bad happened
 */
int spsdc_get_cd(struct mmc_host *mmc)
{
	int ret = 0;

	if (mmc_can_gpio_cd(mmc))
		ret = mmc_gpio_get_cd(mmc);
	else
		spsdc_pr(WARNING, "no gpio assigned for card detection\n");

	if (ret < 0) {
		spsdc_pr(ERROR, "Failed to get card presence status\n");
		ret = 0;
	}

    //return 1;  // for zebu test
	return ret;
}

#ifdef SPMMC_SUPPORT_VOLTAGE_1V8
static int spmmc_card_busy(struct mmc_host *mmc)
{
	struct spsdc_host *host = mmc_priv(mmc);

	spsdc_pr(INFO, "card_busy! %d\n", !(readl(&host->base->sd_status) & SPSDC_SDSTATUS_DAT0_PIN_STATUS));
	return !(readl(&host->base->sd_status) & SPSDC_SDSTATUS_DAT0_PIN_STATUS);
}

static int spmmc_start_signal_voltage_switch(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct spsdc_host *host = mmc_priv(mmc);
	u32 value;

	spsdc_pr(INFO, "start_signal_voltage_switch: host->voltage %d ios->voltage %d!\n", host->signal_voltage, ios->signal_voltage);

	if (host->signal_voltage == ios->signal_voltage) {

		spsdc_txdummy(host, 400);
		return 0;

	}

	/* we do not support switch signal voltage for eMMC at runtime at present */
	if (host->mode == SPSDC_MODE_EMMC)
		return -EIO;

	if (ios->signal_voltage != MMC_SIGNAL_VOLTAGE_180) {
		spsdc_pr(INFO, "can not switch voltage, only support 3.3v -> 1.8v switch!\n");
		return -EIO;
	}

#ifdef HW_VOLTAGE_1V8

	mdelay(15);
	value = readl(&host->base->sd_ctrl);
	value = bitfield_replace(value, SPSDC_sdctrl1_w01, 1, 1); /* trigger tx dummy */
	writel(value, &host->base->sd_ctrl);
	mdelay(1);

	/* mmc layer has guaranteed that CMD11 had issued to SD card at
	 * this time, so we can just continue to check the status.
	 */
	value = readl(&host->base->sd_vol_ctrl);
	for (i = 0 ; i <= 10 ;) {
		if (SPSDC_SWITCH_VOLTAGE_1V8_ERROR == (value & SPSDC_SWITCH_VOLTAGE_MASK) >> 4)
			return -EIO;
		if (SPSDC_SWITCH_VOLTAGE_1V8_TIMEOUT == (value & SPSDC_SWITCH_VOLTAGE_MASK) >> 4)
			return -EIO;
		if (SPSDC_SWITCH_VOLTAGE_1V8_FINISH == (value & SPSDC_SWITCH_VOLTAGE_MASK) >> 4)
			break;
		//if (value >> 4 == 0)
			i++;
		spsdc_pr(INFO, "1V8 result %d\n", value >> 4);
	}

		spsdc_pr(INFO, "1V8 result out %d\n", value >> 4);

#else


	value = readl(&host->base->sd_vol_ctrl);
	value = bitfield_replace(value, SPSDC_sw_set_vol_w01, 1, 1);
	writel(value, &host->base->sd_vol_ctrl);

	spsdc_pr(INFO, "base->sd_vol_ctrl!  0x%x\n", readl(&host->base->sd_vol_ctrl));

	mdelay(20);
	spsdc_txdummy(host, 400);
	mdelay(1);

	#endif

	host->signal_voltage = ios->signal_voltage;
	return 0;
}
#endif /* ifdef SPMMC_SUPPORT_VOLTAGE_1V8 */

static void spsdc_enable_sdio_irq(struct mmc_host *mmc, int enable)
{
	struct spsdc_host *host = mmc_priv(mmc);
	u32 value = readl(&host->base->sd_int);

	value = bitfield_replace(value, SPSDC_sdio_init_clr_w01, 1, 1); /* sdio_int_clr */
	if (enable)
		value = bitfield_replace(value, SPSDC_sdio_init_en_w01, 1, 1);
	else
		value = bitfield_replace(value, SPSDC_sdio_init_en_w01, 1, 0);
	writel(value, &host->base->sd_int);
}

static const struct spsdc_compatible sp_sd_645_compat = {
	.mode = SPSDC_MODE_SD,
	.source_clk = SPSDC_CLK_360M,
};

static const struct spsdc_compatible sp_sdio_645_compat = {
	.mode = SPSDC_MODE_SDIO,
	.source_clk = SPSDC_CLK_360M,
};

static const struct spsdc_compatible sp_sd_654_compat = {
	.mode = SPSDC_MODE_SD,
	.source_clk = SPSDC_CLK_360M,
};

static const struct spsdc_compatible sp_sdio_654_compat = {
	.mode = SPSDC_MODE_SDIO,
	.source_clk = SPSDC_CLK_360M,
};

static const struct spsdc_compatible sp_sd_143_compat = {
	.mode = SPSDC_MODE_SD,
	.source_clk = SPSDC_CLK_220M,
};

static const struct spsdc_compatible sp_sdio_143_compat = {
	.mode = SPSDC_MODE_SDIO,
	.source_clk = SPSDC_CLK_220M,
};


static const struct of_device_id spsdc_of_table[] = {
	{
		.compatible = "sunplus,q645-card",
		.data = &sp_sd_645_compat,
	},
	{
		.compatible = "sunplus,q645-sdio",
		.data = &sp_sdio_645_compat,
	},
	{
		.compatible = "sunplus,sp7350-card",
		.data = &sp_sd_654_compat,
	},
	{
		.compatible = "sunplus,sp7350-sdio",
		.data = &sp_sdio_654_compat,
	},
	{/* sentinel */}
};
MODULE_DEVICE_TABLE(of, spsdc_of_table);



static const struct mmc_host_ops spsdc_ops = {
	.request = spsdc_request,
	.set_ios = spsdc_set_ios,
	.get_cd = spsdc_get_cd,
#ifdef SPMMC_SUPPORT_VOLTAGE_1V8
	.card_busy = spmmc_card_busy,
	.start_signal_voltage_switch = spmmc_start_signal_voltage_switch,
#endif
	.enable_sdio_irq = spsdc_enable_sdio_irq,
};

static void tsklet_func_finish_req(unsigned long data)
{
	struct spsdc_host *host = (struct spsdc_host *)data;

	spin_lock(&host->lock);
	spsdc_finish_request(host, host->mrq);
	spin_unlock(&host->lock);
}

static int spsdc_drv_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct mmc_host *mmc;
	struct resource *resource;
	struct spsdc_host *host;
	const struct spsdc_compatible *dev_mode;

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto probe_free_host;
	}

	spsdc_pr(INFO, "%s\n", __func__);

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->power_state = MMC_POWER_OFF;
	host->dma_int_threshold = 1024;
	host->dmapio_mode = SPSDC_DMA_MODE;
	//host->dmapio_mode = SPSDC_PIO_MODE;

	host->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(host->clk)) {
		spsdc_pr(ERROR, "Can not find clock source\n");
		ret = PTR_ERR(host->clk);
		goto probe_free_host;
	}

	host->rstc = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(host->rstc)) {
		spsdc_pr(ERROR, "Can not find reset controller\n");
		ret = PTR_ERR(host->rstc);
		goto probe_free_host;
	}

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR(resource)) {
		spsdc_pr(ERROR, "get sd register resource fail\n");
		ret = PTR_ERR(resource);
		goto probe_free_host;
	}

	if ((resource->end - resource->start + 1) < sizeof(*host->base)) {
		spsdc_pr(ERROR, "register size is not right\n");
		ret = -EINVAL;
		goto probe_free_host;
	}

	host->base = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR((void *)host->base)) {
		spsdc_pr(ERROR, "devm_ioremap_resource fail\n");
		ret = PTR_ERR((void *)host->base);
		goto probe_free_host;
	}

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq <= 0) {
		spsdc_pr(ERROR, "get sd irq resource fail\n");
		ret = -EINVAL;
		goto probe_free_host;
	}
	if (devm_request_irq(&pdev->dev, host->irq, spsdc_irq, IRQF_SHARED, dev_name(&pdev->dev), host)) {
		spsdc_pr(ERROR, "Failed to request sd card interrupt.\n");
		ret = -ENOENT;
		goto probe_free_host;
	}
	spsdc_pr(INFO, "spsdc driver probe, reg base:0x%08x, irq:%d\n", (unsigned int)(long)host->base, host->irq);

	ret = mmc_of_parse(mmc);
	if (ret)
		goto probe_free_host;

	ret = clk_prepare(host->clk);

	if (ret)
		goto probe_free_host;
	ret = clk_enable(host->clk);
	if (ret)
		goto probe_clk_unprepare;

	spin_lock_init(&host->lock);
	mutex_init(&host->mrq_lock);
	tasklet_init(&host->tsklet_finish_req, tsklet_func_finish_req, (unsigned long)host);
	mmc->ops = &spsdc_ops;
	mmc->f_min = SPSDC_MIN_CLK;
	if (mmc->f_max > SPSDC_MAX_CLK) {
		spsdc_pr(DEBUG, "max-frequency is too high, set it to %d\n", SPSDC_MAX_CLK);
		mmc->f_max = SPSDC_MAX_CLK;
	}
	//mmc->ocr_avail |= MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->ocr_avail |= MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;


	dev_mode = of_device_get_match_data(&pdev->dev);
	spsdc_select_mode(host, dev_mode->mode);
	host->soc_clk = dev_mode->source_clk;

	mmc->caps |= MMC_CAP_4_BIT_DATA  | MMC_CAP_SD_HIGHSPEED
		    | MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25
		    | MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50;
			//| MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_DDR50 ;

	mmc->max_seg_size = SPSDC_MAX_BLK_COUNT * 512;
	/* Host controller supports up to "SPSDC_MAX_DMA_MEMORY_SECTORS",
	 * a.k.a. max scattered memory segments per request
	 */
	mmc->max_segs = SPSDC_MAX_DMA_MEMORY_SECTORS;
	mmc->max_req_size = SPSDC_MAX_BLK_COUNT * 512;
	mmc->max_blk_size = 512; /* Limited by the max value of dma_size & data_length, set it to 512 bytes for now */
	mmc->max_blk_count = SPSDC_MAX_BLK_COUNT; /* Limited by sd_page_num */

	dev_set_drvdata(&pdev->dev, host);
	spsdc_controller_init(host);
	mmc_add_host(mmc);
	host->tuning_info.enable_tuning = 1;
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	return 0;

probe_clk_unprepare:
	spsdc_pr(ERROR, "unable to enable controller clock\n");
	clk_unprepare(host->clk);
probe_free_host:
	if (mmc)
		mmc_free_host(mmc);

	return ret;
}

static int spsdc_drv_remove(struct platform_device *dev)
{
	struct spsdc_host *host = platform_get_drvdata(dev);

	spsdc_pr(INFO, "%s\n", __func__);
	mmc_remove_host(host->mmc);
	clk_disable(host->clk);
	clk_unprepare(host->clk);
	pm_runtime_disable(&dev->dev);
	platform_set_drvdata(dev, NULL);
	mmc_free_host(host->mmc);

	return 0;
}


static int spsdc_drv_suspend(struct platform_device *dev, pm_message_t state)
{
	struct spsdc_host *host;

	host = platform_get_drvdata(dev);
	mutex_lock(&host->mrq_lock); /* Make sure that no one is holding the controller */
	mutex_unlock(&host->mrq_lock);
	clk_disable(host->clk);
	return 0;
}

static int spsdc_drv_resume(struct platform_device *dev)
{
	struct spsdc_host *host;

	host = platform_get_drvdata(dev);
	return clk_enable(host->clk);
}

#ifdef CONFIG_PM
#ifdef CONFIG_PM_SLEEP
static int spsdc_pm_suspend(struct device *dev)
{
	pm_runtime_force_suspend(dev);
	return 0;
}

static int spsdc_pm_resume(struct device *dev)
{
	pm_runtime_force_resume(dev);
	return 0;
}
#endif /* ifdef CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_RUNTIME_SD
static int spsdc_pm_runtime_suspend(struct device *dev)
{
	struct spsdc_host *host;

	spsdc_pr(DEBUG, "%s\n", __func__);
	host = dev_get_drvdata(dev);
	if (__clk_is_enabled(host->clk))
		clk_disable(host->clk);
	return 0;
}

static int spsdc_pm_runtime_resume(struct device *dev)
{
	struct spsdc_host *host;
	int ret = 0;

	spsdc_pr(DEBUG, "%s\n", __func__);
	host = dev_get_drvdata(dev);
	if (!host->mmc)
		return -EINVAL;
	if (mmc_can_gpio_cd(host->mmc)) {
		ret = mmc_gpio_get_cd(host->mmc);
		if (!ret) {
			spsdc_pr(DEBUG, "No card insert\n");
			return 0;
		}
	}
	return clk_enable(host->clk);
}
#endif /* ifdef CONFIG_PM_RUNTIME_SD */

static const struct dev_pm_ops spsdc_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(spsdc_pm_suspend, spsdc_pm_resume)
#ifdef CONFIG_PM_RUNTIME_SD
	SET_RUNTIME_PM_OPS(spsdc_pm_runtime_suspend, spsdc_pm_runtime_resume, NULL)
#endif
};
#endif /* ifdef CONFIG_PM */


static struct platform_driver spsdc_driver = {
	.probe = spsdc_drv_probe,
	.remove = spsdc_drv_remove,
	.suspend = spsdc_drv_suspend,
	.resume = spsdc_drv_resume,
	.driver = {
		.name = "spsdc",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &spsdc_pm_ops,
#endif
		.of_match_table = spsdc_of_table,
	},
};
module_platform_driver(spsdc_driver);

MODULE_AUTHOR("lh.kuo <lh.kuo@sunplus.com>");
MODULE_DESCRIPTION("Sunplus SD/SDIO host controller v3.0 driver");
MODULE_LICENSE("GPL v2");
