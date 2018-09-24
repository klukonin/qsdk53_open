/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * Copyright 2015 Linaro Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/phy/phy.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "pcie-designware.h"

#define PCIE20_PARF_SYS_CTRL			0x00
#define MST_WAKEUP_EN				BIT(13)
#define SLV_WAKEUP_EN				BIT(12)
#define MSTR_ACLK_CGC_DIS			BIT(10)
#define SLV_ACLK_CGC_DIS			BIT(9)
#define CORE_CLK_CGC_DIS			BIT(6)
#define AUX_PWR_DET				BIT(4)
#define L23_CLK_RMV_DIS				BIT(2)
#define L1_CLK_RMV_DIS				BIT(1)

#define PCIE20_PARF_Q2A_FLUSH			0x1AC

#define PCIE20_PARF_LTSSM			0x1B0
#define LTSSM_EN				(1 << 8)

#define PCIE20_PARF_PHY_CTRL			0x40
#define PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK	(0x1f << 16)
#define PHY_CTRL_PHY_TX0_TERM_OFFSET(x)		(x << 16)

#define PCIE20_PARF_PHY_REFCLK			0x4C
#define REF_SSP_EN				BIT(16)
#define REF_USE_PAD				BIT(12)

#define PCIE20_PARF_DBI_BASE_ADDR		0x168
#define PCIE20_PARF_SLV_ADDR_SPACE_SIZE		0x16c
#define PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT	0x178

#define PCIE20_ELBI_SYS_CTRL			0x04
#define PCIE20_ELBI_SYS_CTRL_LT_ENABLE		BIT(0)

#define PCIE20_CAP				0x70
#define PCIE20_CAP_LINKCTRLSTATUS		(PCIE20_CAP + 0x10)

#define PCIE20_AXI_MSTR_RESP_COMP_CTRL0		0x818
#define PCIE20_AXI_MSTR_RESP_COMP_CTRL1		0x81c

#define PCIE20_PLR_IATU_VIEWPORT		0x900
#define PCIE20_PLR_IATU_REGION_OUTBOUND		(0x0 << 31)
#define PCIE20_PLR_IATU_REGION_INDEX(x)		(x << 0)

#define PCIE20_PLR_IATU_CTRL1			0x904
#define PCIE20_PLR_IATU_TYPE_CFG0		(0x4 << 0)
#define PCIE20_PLR_IATU_TYPE_MEM		(0x0 << 0)

#define PCIE20_PLR_IATU_CTRL2			0x908
#define PCIE20_PLR_IATU_ENABLE			BIT(31)

#define PCIE20_PLR_IATU_LBAR			0x90C
#define PCIE20_PLR_IATU_UBAR			0x910
#define PCIE20_PLR_IATU_LAR			0x914
#define PCIE20_PLR_IATU_LTAR			0x918
#define PCIE20_PLR_IATU_UTAR			0x91c

#define MSM_PCIE_DEV_CFG_ADDR		0x01000000
#define PCIE20_CAP_LINK_CAPABILITIES		(PCIE20_CAP + 0xC)
#define PCIE20_CAP_LINK_1			(PCIE20_CAP + 0x14)
#define PCIE_CAP_LINK1_VAL			0x2fd7f


#define PCIE20_COMMAND_STATUS			0x04
#define CMD_BME_VAL				0x4
#define PCIE20_DEVICE_CONTROL2_STATUS2		0x98
#define PCIE_CAP_CPL_TIMEOUT_DISABLE		0x10

#define PCIE20_MISC_CONTROL_1_REG		0x8BC
#define DBI_RO_WR_EN				1

#define PERST_DELAY_US				1000
/* PARF registers */
#define PCIE20_PARF_PCS_DEEMPH			0x34
#define PCS_DEEMPH_TX_DEEMPH_GEN1(x)		(x << 16)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(x)	(x << 8)
#define PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(x)	(x << 0)

#define PCIE20_PARF_PCS_SWING			0x38
#define PCS_SWING_TX_SWING_FULL(x)		(x << 8)
#define PCS_SWING_TX_SWING_LOW(x)		(x << 0)

#define PCIE20_PARF_CONFIG_BITS			0x50
#define PHY_RX0_EQ(x)				(x << 24)

#define PCIE20_LNK_CONTROL2_LINK_STATUS2        0xA0

#define __set(v, a, b)	(((v) << (b)) & GENMASK(a, b))
#define __mask(a, b)	(((1 << ((a) + 1)) - 1) & ~((1 << (b)) - 1))
#define PCIE20_DEV_CAS			0x78
#define PCIE20_MRRS_MASK		__mask(14, 12)
#define PCIE20_MRRS(x)			__set(x, 14, 12)
#define PCIE20_MPS_MASK			__mask(7, 5)
#define PCIE20_MPS(x)			__set(x, 7, 5)

#define AXI_CLK_RATE		200000000

#define PCIE20_v3_PARF_SLV_ADDR_SPACE_SIZE	0x358
#define SLV_ADDR_SPACE_SZ                       0x10000000

struct qcom_pcie_resources_v0 {
	struct clk *iface_clk;
	struct clk *core_clk;
	struct clk *phy_clk;
	struct clk *aux_clk;
	struct clk *ref_clk;
	struct reset_control *pci_reset;
	struct reset_control *axi_reset;
	struct reset_control *ahb_reset;
	struct reset_control *por_reset;
	struct reset_control *phy_reset;
	struct reset_control *ext_reset;
	struct regulator *vdda;
	struct regulator *vdda_phy;
	struct regulator *vdda_refclk;
	uint8_t phy_tx0_term_offset;
};

struct qcom_pcie_resources_v1 {
	struct clk *iface;
	struct clk *aux;
	struct clk *master_bus;
	struct clk *slave_bus;
	struct reset_control *core;
	struct regulator *vdda;
};

struct qcom_pcie_resources_v2 {
	struct clk *ahb_clk;
	struct clk *axi_m_clk;
	struct clk *axi_s_clk;
	struct reset_control *axi_m_reset;
	struct reset_control *axi_s_reset;
	struct reset_control *pipe_reset;
	struct reset_control *axi_m_vmid_reset;
	struct reset_control *axi_s_xpu_reset;
	struct reset_control *parf_reset;
	struct reset_control *phy_reset;
	struct reset_control *axi_m_sticky_reset;
	struct reset_control *pipe_sticky_reset;
	struct reset_control *pwr_reset;
	struct reset_control *ahb_reset;
	struct reset_control *phy_ahb_reset;
	struct regulator *vdda;
	struct regulator *vdda_phy;
	struct regulator *vdda_refclk;
};

struct qcom_pcie_resources_v3 {
	struct clk *sys_noc_clk;
	struct clk *axi_m_clk;
	struct clk *axi_s_clk;
	struct clk *ahb_clk;
	struct clk *aux_clk;
	struct reset_control *axi_m_reset;
	struct reset_control *axi_s_reset;
	struct reset_control *pipe_reset;
	struct reset_control *axi_m_sticky_reset;
	struct reset_control *ahb_reset;
	struct reset_control *sticky_reset;
	struct reset_control *sleep_reset;

	struct regulator *vdda;
	struct regulator *vdda_phy;
	struct regulator *vdda_refclk;
};

union qcom_pcie_resources {
	struct qcom_pcie_resources_v0 v0;
	struct qcom_pcie_resources_v1 v1;
	struct qcom_pcie_resources_v2 v2;
	struct qcom_pcie_resources_v3 v3;
};

struct qcom_pcie;

struct qcom_pcie_ops {
	int (*get_resources)(struct qcom_pcie *pcie);
	int (*init)(struct qcom_pcie *pcie);
	void (*deinit)(struct qcom_pcie *pcie);
};

struct qcom_pcie {
	struct pcie_port pp;
	struct device *dev;
	union qcom_pcie_resources res;
	void __iomem *parf;
	void __iomem *dbi;
	void __iomem *elbi;
	struct phy *phy;
	struct gpio_desc *reset;
	struct qcom_pcie_ops *ops;
	uint32_t force_gen1;
	u32 is_emulation;
};

#define to_qcom_pcie(x)		container_of(x, struct qcom_pcie, pp)

#define MAX_RC_NUM	3
static struct qcom_pcie *qcom_pcie_dev[MAX_RC_NUM];
static atomic_t rc_removed;

static inline void
writel_masked(void __iomem *addr, u32 clear_mask, u32 set_mask)
{
	u32 val = readl(addr);

	val &= ~clear_mask;
	val |= set_mask;
	writel(val, addr);
}

static void qcom_ep_reset_assert(struct qcom_pcie *pcie)
{
	gpiod_set_value(pcie->reset, 1);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static void qcom_ep_reset_deassert(struct qcom_pcie *pcie)
{
	gpiod_set_value(pcie->reset, 0);
	usleep_range(PERST_DELAY_US, PERST_DELAY_US + 500);
}

static irqreturn_t qcom_pcie_msi_irq_handler(int irq, void *arg)
{
	struct pcie_port *pp = arg;

	return dw_handle_msi_irq(pp);
}

static int qcom_pcie_establish_link(struct qcom_pcie *pcie)
{
	u32 val;

	if (dw_pcie_link_up(&pcie->pp))
		return 0;

	/* enable link training */
	val = readl(pcie->elbi + PCIE20_ELBI_SYS_CTRL);
	val |= PCIE20_ELBI_SYS_CTRL_LT_ENABLE;
	writel(val, pcie->elbi + PCIE20_ELBI_SYS_CTRL);

	return dw_pcie_wait_for_link(&pcie->pp);
}

static void qcom_pcie_prog_viewport_cfg0(struct qcom_pcie *pcie, u32 busdev)
{
	struct pcie_port *pp = &pcie->pp;

	/*
	 * program and enable address translation region 0 (device config
	 * address space); region type config;
	 * axi config address range to device config address range
	 */
	writel(PCIE20_PLR_IATU_REGION_OUTBOUND |
	       PCIE20_PLR_IATU_REGION_INDEX(0),
	       pcie->dbi + PCIE20_PLR_IATU_VIEWPORT);

	writel(PCIE20_PLR_IATU_TYPE_CFG0, pcie->dbi + PCIE20_PLR_IATU_CTRL1);
	writel(PCIE20_PLR_IATU_ENABLE, pcie->dbi + PCIE20_PLR_IATU_CTRL2);
	writel(pp->cfg0_base, pcie->dbi + PCIE20_PLR_IATU_LBAR);
	writel((pp->cfg0_base >> 32), pcie->dbi + PCIE20_PLR_IATU_UBAR);
	writel((pp->cfg0_base + pp->cfg0_size - 1),
	       pcie->dbi + PCIE20_PLR_IATU_LAR);
	writel(busdev, pcie->dbi + PCIE20_PLR_IATU_LTAR);
	writel(0, pcie->dbi + PCIE20_PLR_IATU_UTAR);
}

static void qcom_pcie_prog_viewport_mem2_outbound(struct qcom_pcie *pcie)
{
	struct pcie_port *pp = &pcie->pp;

	/*
	 * program and enable address translation region 2 (device resource
	 * address space); region type memory;
	 * axi device bar address range to device bar address range
	 */
	writel(PCIE20_PLR_IATU_REGION_OUTBOUND |
	       PCIE20_PLR_IATU_REGION_INDEX(2),
	       pcie->dbi + PCIE20_PLR_IATU_VIEWPORT);

	writel(PCIE20_PLR_IATU_TYPE_MEM, pcie->dbi + PCIE20_PLR_IATU_CTRL1);
	writel(PCIE20_PLR_IATU_ENABLE, pcie->dbi + PCIE20_PLR_IATU_CTRL2);
	writel(pp->mem_base, pcie->dbi + PCIE20_PLR_IATU_LBAR);
	writel((pp->mem_base >> 32), pcie->dbi + PCIE20_PLR_IATU_UBAR);
	writel(pp->mem_base + pp->mem_size - 1,
	       pcie->dbi + PCIE20_PLR_IATU_LAR);
	writel(pp->mem_bus_addr, pcie->dbi + PCIE20_PLR_IATU_LTAR);
	writel(upper_32_bits(pp->mem_bus_addr),
	       pcie->dbi + PCIE20_PLR_IATU_UTAR);

	/* 256B PCIE buffer setting */
	writel(0x1, pcie->dbi + PCIE20_AXI_MSTR_RESP_COMP_CTRL0);
	writel(0x1, pcie->dbi + PCIE20_AXI_MSTR_RESP_COMP_CTRL1);
}

static int qcom_pcie_get_resources_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;
	struct device *dev = pcie->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->vdda_phy = devm_regulator_get(dev, "vdda_phy");
	if (IS_ERR(res->vdda_phy))
		return PTR_ERR(res->vdda_phy);

	res->vdda_refclk = devm_regulator_get(dev, "vdda_refclk");
	if (IS_ERR(res->vdda_refclk))
		return PTR_ERR(res->vdda_refclk);

	res->iface_clk = devm_clk_get(dev, "iface");
	if (IS_ERR(res->iface_clk))
		return PTR_ERR(res->iface_clk);

	res->core_clk = devm_clk_get(dev, "core");
	if (IS_ERR(res->core_clk))
		return PTR_ERR(res->core_clk);

	res->phy_clk = devm_clk_get(dev, "phy");
	if (IS_ERR(res->phy_clk))
		return PTR_ERR(res->phy_clk);

	res->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(res->aux_clk))
		return PTR_ERR(res->aux_clk);

	res->ref_clk = devm_clk_get(dev, "ref");
	if (IS_ERR(res->ref_clk))
		return PTR_ERR(res->ref_clk);

	res->pci_reset = devm_reset_control_get(dev, "pci");
	if (IS_ERR(res->pci_reset))
		return PTR_ERR(res->pci_reset);

	res->axi_reset = devm_reset_control_get(dev, "axi");
	if (IS_ERR(res->axi_reset))
		return PTR_ERR(res->axi_reset);

	res->ahb_reset = devm_reset_control_get(dev, "ahb");
	if (IS_ERR(res->ahb_reset))
		return PTR_ERR(res->ahb_reset);

	res->por_reset = devm_reset_control_get(dev, "por");
	if (IS_ERR(res->por_reset))
		return PTR_ERR(res->por_reset);

	res->phy_reset = devm_reset_control_get(dev, "phy");
	if (IS_ERR(res->phy_reset))
		return PTR_ERR(res->phy_reset);

	res->ext_reset = devm_reset_control_get(dev, "ext");
	if (IS_ERR(res->ext_reset))
		return PTR_ERR(res->ext_reset);

	if (of_property_read_u8(dev->of_node, "phy-tx0-term-offset",
				&res->phy_tx0_term_offset))
		res->phy_tx0_term_offset = 0;

	return 0;
}

static int qcom_pcie_get_resources_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;
	struct device *dev = pcie->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->iface = devm_clk_get(dev, "iface");
	if (IS_ERR(res->iface))
		return PTR_ERR(res->iface);

	res->aux = devm_clk_get(dev, "aux");
	if (IS_ERR(res->aux))
		return PTR_ERR(res->aux);

	res->master_bus = devm_clk_get(dev, "master_bus");
	if (IS_ERR(res->master_bus))
		return PTR_ERR(res->master_bus);

	res->slave_bus = devm_clk_get(dev, "slave_bus");
	if (IS_ERR(res->slave_bus))
		return PTR_ERR(res->slave_bus);

	res->core = devm_reset_control_get(dev, "core");
	if (IS_ERR(res->core))
		return PTR_ERR(res->core);

	return 0;
}

static int qcom_pcie_get_resources_v2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v2 *res = &pcie->res.v2;
	struct device *dev = pcie->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->vdda_phy = devm_regulator_get(dev, "vdda_phy");
	if (IS_ERR(res->vdda_phy))
		return PTR_ERR(res->vdda_phy);

	res->vdda_refclk = devm_regulator_get(dev, "vdda_refclk");
	if (IS_ERR(res->vdda_refclk))
		return PTR_ERR(res->vdda_refclk);

	res->ahb_clk = devm_clk_get(dev, "ahb");
	if (IS_ERR(res->ahb_clk))
		return PTR_ERR(res->ahb_clk);

	res->axi_m_clk = devm_clk_get(dev, "axi_m");
	if (IS_ERR(res->axi_m_clk))
		return PTR_ERR(res->axi_m_clk);

	res->axi_s_clk = devm_clk_get(dev, "axi_s");
	if (IS_ERR(res->axi_s_clk))
		return PTR_ERR(res->axi_s_clk);

	res->axi_m_reset = devm_reset_control_get(dev, "axi_m");
	if (IS_ERR(res->axi_m_reset))
		return PTR_ERR(res->axi_m_reset);

	res->axi_s_reset = devm_reset_control_get(dev, "axi_s");
	if (IS_ERR(res->axi_s_reset))
		return PTR_ERR(res->axi_s_reset);

	res->pipe_reset = devm_reset_control_get(dev, "pipe");
	if (IS_ERR(res->pipe_reset))
		return PTR_ERR(res->pipe_reset);

	res->axi_m_vmid_reset = devm_reset_control_get(dev, "axi_m_vmid");
	if (IS_ERR(res->axi_m_vmid_reset))
		return PTR_ERR(res->axi_m_vmid_reset);

	res->axi_s_xpu_reset = devm_reset_control_get(dev, "axi_s_xpu");
	if (IS_ERR(res->axi_s_xpu_reset))
		return PTR_ERR(res->axi_s_xpu_reset);

	res->parf_reset = devm_reset_control_get(dev, "parf");
	if (IS_ERR(res->parf_reset))
		return PTR_ERR(res->parf_reset);

	res->phy_reset = devm_reset_control_get(dev, "phy");
	if (IS_ERR(res->phy_reset))
		return PTR_ERR(res->phy_reset);

	res->axi_m_sticky_reset = devm_reset_control_get(dev, "axi_m_sticky");
	if (IS_ERR(res->axi_m_sticky_reset))
		return PTR_ERR(res->axi_m_sticky_reset);

	res->pipe_sticky_reset = devm_reset_control_get(dev, "pipe_sticky");
	if (IS_ERR(res->pipe_sticky_reset))
		return PTR_ERR(res->pipe_sticky_reset);

	res->pwr_reset = devm_reset_control_get(dev, "pwr");
	if (IS_ERR(res->pwr_reset))
		return PTR_ERR(res->pwr_reset);

	res->ahb_reset = devm_reset_control_get(dev, "ahb");
	if (IS_ERR(res->ahb_reset))
		return PTR_ERR(res->ahb_reset);

	res->phy_ahb_reset = devm_reset_control_get(dev, "phy_ahb");
	if (IS_ERR(res->phy_ahb_reset))
		return PTR_ERR(res->phy_ahb_reset);

	return 0;
}


static int qcom_pcie_get_resources_v3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v3 *res = &pcie->res.v3;
	struct device *dev = pcie->dev;

	res->vdda = devm_regulator_get(dev, "vdda");
	if (IS_ERR(res->vdda))
		return PTR_ERR(res->vdda);

	res->vdda_phy = devm_regulator_get(dev, "vdda_phy");
	if (IS_ERR(res->vdda_phy))
		return PTR_ERR(res->vdda_phy);

	res->vdda_refclk = devm_regulator_get(dev, "vdda_refclk");
	if (IS_ERR(res->vdda_refclk))
		return PTR_ERR(res->vdda_refclk);

	res->sys_noc_clk = devm_clk_get(dev, "sys_noc");
	if (IS_ERR(res->sys_noc_clk))
		return PTR_ERR(res->sys_noc_clk);

	res->axi_m_clk = devm_clk_get(dev, "axi_m");
	if (IS_ERR(res->axi_m_clk))
		return PTR_ERR(res->axi_m_clk);

	res->axi_s_clk = devm_clk_get(dev, "axi_s");
	if (IS_ERR(res->axi_s_clk))
		return PTR_ERR(res->axi_s_clk);

	res->ahb_clk = devm_clk_get(dev, "ahb");
	if (IS_ERR(res->ahb_clk))
		return PTR_ERR(res->ahb_clk);

	res->aux_clk = devm_clk_get(dev, "aux");
	if (IS_ERR(res->aux_clk))
		return PTR_ERR(res->aux_clk);

	res->axi_m_reset = devm_reset_control_get(dev, "axi_m");
	if (IS_ERR(res->axi_m_reset))
		return PTR_ERR(res->axi_m_reset);

	res->axi_s_reset = devm_reset_control_get(dev, "axi_s");
	if (IS_ERR(res->axi_s_reset))
		return PTR_ERR(res->axi_s_reset);

	res->pipe_reset = devm_reset_control_get(dev, "pipe");
	if (IS_ERR(res->pipe_reset))
		return PTR_ERR(res->pipe_reset);

	res->axi_m_sticky_reset = devm_reset_control_get(dev, "axi_m_sticky");
	if (IS_ERR(res->axi_m_sticky_reset))
		return PTR_ERR(res->axi_m_sticky_reset);

	res->sticky_reset = devm_reset_control_get(dev, "sticky");
	if (IS_ERR(res->sticky_reset))
		return PTR_ERR(res->sticky_reset);

	res->ahb_reset = devm_reset_control_get(dev, "ahb");
	if (IS_ERR(res->ahb_reset))
		return PTR_ERR(res->ahb_reset);

	res->sleep_reset = devm_reset_control_get(dev, "sleep");
	if (IS_ERR(res->sleep_reset))
		return PTR_ERR(res->sleep_reset);

	return 0;
}

static void qcom_pcie_deinit_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;

	clk_disable_unprepare(res->phy_clk);
	reset_control_assert(res->phy_reset);
	reset_control_assert(res->axi_reset);
	reset_control_assert(res->ahb_reset);
	reset_control_assert(res->por_reset);
	reset_control_assert(res->pci_reset);
	reset_control_assert(res->ext_reset);
	clk_disable_unprepare(res->iface_clk);
	clk_disable_unprepare(res->core_clk);
	clk_disable_unprepare(res->aux_clk);
	clk_disable_unprepare(res->ref_clk);
	regulator_disable(res->vdda);
	regulator_disable(res->vdda_phy);
	regulator_disable(res->vdda_refclk);
}

static int qcom_pcie_init_v0(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v0 *res = &pcie->res.v0;
	struct device *dev = pcie->dev;
	int ret;

	ret = reset_control_assert(res->ahb_reset);
	if (ret) {
		dev_err(dev, "cannot assert ahb reset\n");
		return ret;
	}

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		return ret;
	}

	ret = regulator_enable(res->vdda_refclk);
	if (ret) {
		dev_err(dev, "cannot enable vdda_refclk regulator\n");
		goto err_refclk;
	}

	ret = regulator_enable(res->vdda_phy);
	if (ret) {
		dev_err(dev, "cannot enable vdda_phy regulator\n");
		goto err_vdda_phy;
	}

	ret = reset_control_deassert(res->ext_reset);
	if (ret) {
		dev_err(dev, "cannot assert ext reset\n");
		goto err_reset_ext;
	}

	ret = clk_prepare_enable(res->iface_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable iface clock\n");
		goto err_iface;
	}

	ret = clk_prepare_enable(res->core_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable core clock\n");
		goto err_clk_core;
	}

	ret = clk_prepare_enable(res->aux_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clock\n");
		goto err_clk_aux;
	}

	ret = clk_prepare_enable(res->ref_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable ref clock\n");
		goto err_clk_ref;
	}

	ret = reset_control_deassert(res->ahb_reset);
	if (ret) {
		dev_err(dev, "cannot deassert ahb reset\n");
		goto err_deassert_ahb;
	}

	writel_masked(pcie->parf + PCIE20_PARF_PHY_CTRL, BIT(0), 0);

	/* Set Tx termination offset */
	writel_masked(pcie->parf + PCIE20_PARF_PHY_CTRL,
		      PHY_CTRL_PHY_TX0_TERM_OFFSET_MASK,
		      PHY_CTRL_PHY_TX0_TERM_OFFSET(res->phy_tx0_term_offset));

	/* PARF programming */
	writel(PCS_DEEMPH_TX_DEEMPH_GEN1(0x18) |
	       PCS_DEEMPH_TX_DEEMPH_GEN2_3_5DB(0x18) |
	       PCS_DEEMPH_TX_DEEMPH_GEN2_6DB(0x22),
	       pcie->parf + PCIE20_PARF_PCS_DEEMPH);
	writel(PCS_SWING_TX_SWING_FULL(0x78) |
	       PCS_SWING_TX_SWING_LOW(0x78),
	       pcie->parf + PCIE20_PARF_PCS_SWING);
	writel(PHY_RX0_EQ(0x4), pcie->parf + PCIE20_PARF_CONFIG_BITS);

	/* Enable reference clock */
	writel_masked(pcie->parf + PCIE20_PARF_PHY_REFCLK,
		      REF_USE_PAD, REF_SSP_EN);


	ret = reset_control_deassert(res->phy_reset);
	if (ret) {
		dev_err(dev, "cannot deassert phy reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->pci_reset);
	if (ret) {
		dev_err(dev, "cannot deassert pci reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->por_reset);
	if (ret) {
		dev_err(dev, "cannot deassert por reset\n");
		return ret;
	}

	ret = reset_control_deassert(res->axi_reset);
	if (ret) {
		dev_err(dev, "cannot deassert axi reset\n");
		return ret;
	}

	ret = clk_prepare_enable(res->phy_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable phy clock\n");
		goto err_deassert_ahb;
	}

	/* wait for clock acquisition */
	usleep_range(1000, 1500);
	if (pcie->force_gen1) {
		writel_relaxed((readl_relaxed(
			pcie->dbi + PCIE20_LNK_CONTROL2_LINK_STATUS2) | 1),
			pcie->dbi + PCIE20_LNK_CONTROL2_LINK_STATUS2);
	}

	qcom_pcie_prog_viewport_cfg0(pcie, MSM_PCIE_DEV_CFG_ADDR);
	qcom_pcie_prog_viewport_mem2_outbound(pcie);

	return 0;

err_deassert_ahb:
	clk_disable_unprepare(res->ref_clk);
err_clk_ref:
	clk_disable_unprepare(res->aux_clk);
err_clk_aux:
	clk_disable_unprepare(res->core_clk);
err_clk_core:
	clk_disable_unprepare(res->iface_clk);
err_iface:
	reset_control_assert(res->ext_reset);
err_reset_ext:
	regulator_disable(res->vdda_phy);
err_vdda_phy:
	regulator_disable(res->vdda_refclk);
err_refclk:
	regulator_disable(res->vdda);

	return ret;
}

static void qcom_pcie_deinit_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;

	reset_control_assert(res->core);
	clk_disable_unprepare(res->slave_bus);
	clk_disable_unprepare(res->master_bus);
	clk_disable_unprepare(res->iface);
	clk_disable_unprepare(res->aux);
	regulator_disable(res->vdda);
}

static int qcom_pcie_init_v1(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v1 *res = &pcie->res.v1;
	struct device *dev = pcie->dev;
	int ret;

	ret = reset_control_deassert(res->core);
	if (ret) {
		dev_err(dev, "cannot deassert core reset\n");
		return ret;
	}

	ret = clk_prepare_enable(res->aux);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clock\n");
		goto err_res;
	}

	ret = clk_prepare_enable(res->iface);
	if (ret) {
		dev_err(dev, "cannot prepare/enable iface clock\n");
		goto err_aux;
	}

	ret = clk_prepare_enable(res->master_bus);
	if (ret) {
		dev_err(dev, "cannot prepare/enable master_bus clock\n");
		goto err_iface;
	}

	ret = clk_prepare_enable(res->slave_bus);
	if (ret) {
		dev_err(dev, "cannot prepare/enable slave_bus clock\n");
		goto err_master;
	}

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		goto err_slave;
	}

	/* change DBI base address */
	writel(0, pcie->parf + PCIE20_PARF_DBI_BASE_ADDR);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		u32 val = readl(pcie->parf + PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT);

		val |= BIT(31);
		writel(val, pcie->parf + PCIE20_PARF_AXI_MSTR_WR_ADDR_HALT);
	}

	return 0;
err_slave:
	clk_disable_unprepare(res->slave_bus);
err_master:
	clk_disable_unprepare(res->master_bus);
err_iface:
	clk_disable_unprepare(res->iface);
err_aux:
	clk_disable_unprepare(res->aux);
err_res:
	reset_control_assert(res->core);

	return ret;
}

static void qcom_pcie_deinit_v2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v2 *res = &pcie->res.v2;

	/* Assert pcie_pipe_ares */
	reset_control_assert(res->axi_m_reset);
	reset_control_assert(res->axi_s_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_assert(res->pipe_reset);
	reset_control_assert(res->pipe_sticky_reset);
	reset_control_assert(res->phy_reset);
	reset_control_assert(res->phy_ahb_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_assert(res->axi_m_sticky_reset);
	reset_control_assert(res->pwr_reset);
	reset_control_assert(res->ahb_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	clk_disable_unprepare(res->ahb_clk);
	clk_disable_unprepare(res->axi_m_clk);
	clk_disable_unprepare(res->axi_s_clk);
	regulator_disable(res->vdda);
	regulator_disable(res->vdda_phy);
	regulator_disable(res->vdda_refclk);
}

static int qcom_pcie_enable_resources_v2(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v2 *res = &pcie->res.v2;
	struct device *dev = pcie->dev;
	int ret;

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		return ret;
	}

	ret = regulator_enable(res->vdda_refclk);
	if (ret) {
		dev_err(dev, "cannot enable vdda_refclk regulator\n");
		goto err_refclk;
	}

	ret = regulator_enable(res->vdda_phy);
	if (ret) {
		dev_err(dev, "cannot enable vdda_phy regulator\n");
		goto err_vdda_phy;
	}

	ret = clk_prepare_enable(res->ahb_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable iface clock\n");
		goto err_ahb;
	}

	ret = clk_prepare_enable(res->axi_m_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable core clock\n");
		goto err_clk_axi_m;
	}

	ret = clk_prepare_enable(res->axi_s_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable phy clock\n");
		goto err_clk_axi_s;
	}

	udelay(1);

	return 0;

err_clk_axi_s:
	clk_disable_unprepare(res->axi_m_clk);
err_clk_axi_m:
	clk_disable_unprepare(res->ahb_clk);
err_ahb:
	regulator_disable(res->vdda_phy);
err_vdda_phy:
	regulator_disable(res->vdda_refclk);
err_refclk:
	regulator_disable(res->vdda);
	return ret;
}

static void qcom_pcie_v2_reset(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v2 *res = &pcie->res.v2;
	/* Assert pcie_pipe_ares */
	reset_control_assert(res->axi_m_reset);
	reset_control_assert(res->axi_s_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_assert(res->pipe_reset);
	reset_control_assert(res->pipe_sticky_reset);
	reset_control_assert(res->phy_reset);
	reset_control_assert(res->phy_ahb_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_assert(res->axi_m_sticky_reset);
	reset_control_assert(res->pwr_reset);
	reset_control_assert(res->ahb_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_deassert(res->phy_ahb_reset);
	reset_control_deassert(res->phy_reset);
	reset_control_deassert(res->pipe_reset);
	reset_control_deassert(res->pipe_sticky_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_deassert(res->axi_m_reset);
	reset_control_deassert(res->axi_m_sticky_reset);
	reset_control_deassert(res->axi_s_reset);
	reset_control_deassert(res->pwr_reset);
	reset_control_deassert(res->ahb_reset);
	usleep_range(10000, 12000); /* wait 12ms */
	wmb(); /* ensure data is written to hw register */
}

static int qcom_pcie_init_v2(struct qcom_pcie *pcie)
{
	int ret;

	qcom_pcie_v2_reset(pcie);
	qcom_ep_reset_assert(pcie);

	ret = qcom_pcie_enable_resources_v2(pcie);
	if (ret)
		return ret;

	writel_masked(pcie->parf + PCIE20_PARF_PHY_CTRL, BIT(0), 0);

	writel(0, pcie->parf + PCIE20_PARF_DBI_BASE_ADDR);

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS
		| SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PCIE20_PARF_SYS_CTRL);
	writel(0, pcie->parf + PCIE20_PARF_Q2A_FLUSH);
	writel(CMD_BME_VAL, pcie->dbi + PCIE20_COMMAND_STATUS);
	writel(DBI_RO_WR_EN, pcie->dbi + PCIE20_MISC_CONTROL_1_REG);
	writel(PCIE_CAP_LINK1_VAL, pcie->dbi + PCIE20_CAP_LINK_1);

	writel_masked(pcie->dbi + PCIE20_CAP_LINK_CAPABILITIES,
		BIT(10) | BIT(11), 0);
	writel(PCIE_CAP_CPL_TIMEOUT_DISABLE, pcie->dbi +
		PCIE20_DEVICE_CONTROL2_STATUS2);
	writel(LTSSM_EN, pcie->parf + PCIE20_PARF_LTSSM);

	return 0;
}

static void qcom_pcie_deinit_v3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v3 *res = &pcie->res.v3;

	clk_disable_unprepare(res->sys_noc_clk);
	clk_disable_unprepare(res->axi_m_clk);
	clk_disable_unprepare(res->axi_s_clk);
	clk_disable_unprepare(res->ahb_clk);
	clk_disable_unprepare(res->aux_clk);
	regulator_disable(res->vdda);
	regulator_disable(res->vdda_phy);
	regulator_disable(res->vdda_refclk);
}

static void qcom_pcie_v3_reset(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v3 *res = &pcie->res.v3;
	/* Assert pcie_pipe_ares */
	reset_control_assert(res->pipe_reset);
	reset_control_assert(res->sleep_reset);
	reset_control_assert(res->sticky_reset);
	reset_control_assert(res->axi_m_reset);
	reset_control_assert(res->axi_s_reset);
	reset_control_assert(res->ahb_reset);
	reset_control_assert(res->axi_m_sticky_reset);
	usleep_range(10000, 12000); /* wait 12ms */

	reset_control_deassert(res->pipe_reset);
	reset_control_deassert(res->sleep_reset);
	reset_control_deassert(res->sticky_reset);
	reset_control_deassert(res->axi_m_reset);
	reset_control_deassert(res->axi_s_reset);
	reset_control_deassert(res->ahb_reset);
	reset_control_deassert(res->axi_m_sticky_reset);
	usleep_range(10000, 12000); /* wait 12ms */
	wmb(); /* ensure data is written to hw register */
}

static int qcom_pcie_enable_resources_v3(struct qcom_pcie *pcie)
{
	struct qcom_pcie_resources_v3 *res = &pcie->res.v3;
	struct device *dev = pcie->dev;
	int ret;

	ret = regulator_enable(res->vdda);
	if (ret) {
		dev_err(dev, "cannot enable vdda regulator\n");
		return ret;
	}

	ret = regulator_enable(res->vdda_refclk);
	if (ret) {
		dev_err(dev, "cannot enable vdda_refclk regulator\n");
		goto err_refclk;
	}

	ret = regulator_enable(res->vdda_phy);
	if (ret) {
		dev_err(dev, "cannot enable vdda_phy regulator\n");
		goto err_vdda_phy;
	}

	ret = clk_prepare_enable(res->sys_noc_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable core clock\n");
		goto err_clk_sys_noc;
	}

	ret = clk_prepare_enable(res->axi_m_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable core clock\n");
		goto err_clk_axi_m;
	}

	ret = clk_set_rate(res->axi_m_clk, AXI_CLK_RATE);
	if (ret) {
		dev_err(dev, "MClk rate set failed (%d)\n", ret);
		goto err_clk_axi_m;
	}

	ret = clk_prepare_enable(res->axi_s_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable axi slave clock\n");
		goto err_clk_axi_s;
	}

	ret = clk_set_rate(res->axi_s_clk, AXI_CLK_RATE);
	if (ret) {
		dev_err(dev, "MClk rate set failed (%d)\n", ret);
		goto err_clk_axi_s;
	}

	ret = clk_prepare_enable(res->ahb_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable ahb clock\n");
		goto err_clk_ahb;
	}

	ret = clk_prepare_enable(res->aux_clk);
	if (ret) {
		dev_err(dev, "cannot prepare/enable aux clock\n");
		goto err_clk_aux;
	}

	udelay(1);

	return 0;

err_clk_aux:
	clk_disable_unprepare(res->ahb_clk);
err_clk_ahb:
	clk_disable_unprepare(res->axi_s_clk);
err_clk_axi_s:
	clk_disable_unprepare(res->axi_m_clk);
err_clk_axi_m:
	clk_disable_unprepare(res->sys_noc_clk);
err_clk_sys_noc:
	regulator_disable(res->vdda_phy);
err_vdda_phy:
	regulator_disable(res->vdda_refclk);
err_refclk:
	regulator_disable(res->vdda);
	return ret;
}


static int qcom_pcie_init_v3(struct qcom_pcie *pcie)
{
	int ret;

	qcom_pcie_v3_reset(pcie);
	if (!pcie->is_emulation)
		qcom_ep_reset_assert(pcie);

	ret = qcom_pcie_enable_resources_v3(pcie);
	if (ret)
		return ret;

	writel(SLV_ADDR_SPACE_SZ, pcie->parf + PCIE20_v3_PARF_SLV_ADDR_SPACE_SIZE);

	ret = phy_power_on(pcie->phy);
	if (ret)
		return ret;

	writel_masked(pcie->parf + PCIE20_PARF_PHY_CTRL, BIT(0), 0);

	writel(0, pcie->parf + PCIE20_PARF_DBI_BASE_ADDR);

	writel(MST_WAKEUP_EN | SLV_WAKEUP_EN | MSTR_ACLK_CGC_DIS
		| SLV_ACLK_CGC_DIS | CORE_CLK_CGC_DIS |
		AUX_PWR_DET | L23_CLK_RMV_DIS | L1_CLK_RMV_DIS,
		pcie->parf + PCIE20_PARF_SYS_CTRL);
	writel(0, pcie->parf + PCIE20_PARF_Q2A_FLUSH);
	writel(CMD_BME_VAL, pcie->dbi + PCIE20_COMMAND_STATUS);
	writel(DBI_RO_WR_EN, pcie->dbi + PCIE20_MISC_CONTROL_1_REG);
	writel(PCIE_CAP_LINK1_VAL, pcie->dbi + PCIE20_CAP_LINK_1);

	writel_masked(pcie->dbi + PCIE20_CAP_LINK_CAPABILITIES,
		BIT(10) | BIT(11), 0);
	writel(PCIE_CAP_CPL_TIMEOUT_DISABLE, pcie->dbi +
		PCIE20_DEVICE_CONTROL2_STATUS2);
	if (pcie->force_gen1) {
		writel_relaxed((readl_relaxed(
			pcie->dbi + PCIE20_LNK_CONTROL2_LINK_STATUS2) | 1),
			pcie->dbi + PCIE20_LNK_CONTROL2_LINK_STATUS2);
	}
	writel(LTSSM_EN, pcie->parf + PCIE20_PARF_LTSSM);

	return 0;
}
static int qcom_pcie_link_up(struct pcie_port *pp)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pp);
	u16 val = readw(pcie->dbi + PCIE20_CAP + PCI_EXP_LNKSTA);

	return !!(val & PCI_EXP_LNKSTA_DLLLA);
}

static int qcom_pcie_host_init(struct pcie_port *pp)
{
	struct qcom_pcie *pcie = to_qcom_pcie(pp);
	int ret;

	if (!pcie->is_emulation)
		qcom_ep_reset_assert(pcie);

	ret = pcie->ops->init(pcie);
	if (ret)
		goto err_deinit;

	ret = phy_power_on(pcie->phy);
	if (ret)
		goto err_deinit;

	dw_pcie_setup_rc(pp);

	if (IS_ENABLED(CONFIG_PCI_MSI))
		dw_pcie_msi_init(pp);

	if (!pcie->is_emulation)
		qcom_ep_reset_deassert(pcie);

	ret = qcom_pcie_establish_link(pcie);
	if (ret)
		goto err;

	return 0;
err:
	if (!pcie->is_emulation)
		qcom_ep_reset_assert(pcie);

	phy_power_off(pcie->phy);
err_deinit:
	pcie->ops->deinit(pcie);
	return ret;
}

static int qcom_pcie_rd_own_conf(struct pcie_port *pp, int where, int size,
				 u32 *val)
{
	/* the device class is not reported correctly from the register */
	if (where == PCI_CLASS_REVISION && size == 4) {
		*val = readl(pp->dbi_base + PCI_CLASS_REVISION);
		*val &= 0xff;	/* keep revision id */
		*val |= PCI_CLASS_BRIDGE_PCI << 16;
		return PCIBIOS_SUCCESSFUL;
	}

	return dw_pcie_cfg_read(pp->dbi_base + where, size, val);
}

static struct pcie_host_ops qcom_pcie_dw_ops = {
	.link_up = qcom_pcie_link_up,
	.host_init = qcom_pcie_host_init,
	.rd_own_conf = qcom_pcie_rd_own_conf,
};

static const struct qcom_pcie_ops ops_v0 = {
	.get_resources = qcom_pcie_get_resources_v0,
	.init = qcom_pcie_init_v0,
	.deinit = qcom_pcie_deinit_v0,
};

static const struct qcom_pcie_ops ops_v1 = {
	.get_resources = qcom_pcie_get_resources_v1,
	.init = qcom_pcie_init_v1,
	.deinit = qcom_pcie_deinit_v1,
};

static const struct qcom_pcie_ops ops_v2 = {
	.get_resources = qcom_pcie_get_resources_v2,
	.init = qcom_pcie_init_v2,
	.deinit = qcom_pcie_deinit_v2,
};

static const struct qcom_pcie_ops ops_v3 = {
	.get_resources = qcom_pcie_get_resources_v3,
	.init = qcom_pcie_init_v3,
	.deinit = qcom_pcie_deinit_v3,
};

int qcom_pcie_rescan(void)
{
	int i;
	struct pcie_port *pp;

	if (!atomic_read(&rc_removed))
		return 0;

	for (i = 0; i < MAX_RC_NUM; i++) {
		/* reset and enumerate the pcie devices */
		if (qcom_pcie_dev[i]) {
			pr_notice("---> Initializing %d", i);
			pp = &qcom_pcie_dev[i]->pp;
			dw_pcie_host_init_pm(pp);
			pr_notice(" ... done<---\n");
		}
	}
	atomic_set(&rc_removed, 0);

	return 0;
}

void qcom_pcie_remove_bus(void)
{
	int i;

	if (atomic_read(&rc_removed))
		return;

	for (i = 0; i < MAX_RC_NUM; i++) {
		if (qcom_pcie_dev[i]) {
			struct pcie_port *pp;

			pr_notice("---> Removing %d", i);
			pp = &qcom_pcie_dev[i]->pp;
			qcom_pcie_dev[i]->ops->deinit(qcom_pcie_dev[i]);
			pci_stop_root_bus(pp->pci_bus);
			pci_remove_root_bus(pp->pci_bus);
			pp->pci_bus = NULL;
			pr_notice(" ... done<---\n");
		}
	}
	atomic_set(&rc_removed, 1);
}

static ssize_t qcom_bus_rescan_store(struct bus_type *bus, const char *buf,
					size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	if (val) {
		pci_lock_rescan_remove();
		qcom_pcie_rescan();
		pci_unlock_rescan_remove();
	}
	return count;
}
static BUS_ATTR(rcrescan, (S_IWUSR|S_IWGRP), NULL, qcom_bus_rescan_store);

static ssize_t qcom_bus_remove_store(struct bus_type *bus, const char *buf,
					size_t count)
{
	unsigned long val;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	if (val) {
		pci_lock_rescan_remove();
		qcom_pcie_remove_bus();
		pci_unlock_rescan_remove();
	}
	return count;
}
static BUS_ATTR(rcremove, (S_IWUSR|S_IWGRP), NULL, qcom_bus_remove_store);
static int qcom_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct qcom_pcie *pcie;
	struct pcie_port *pp;
	int ret;
	uint32_t force_gen1 = 0;
	struct device_node *np = pdev->dev.of_node;
	u32 is_emulation = 0;
	static int rc_idx;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->ops = (struct qcom_pcie_ops *)of_device_get_match_data(dev);
	pcie->dev = dev;

	of_property_read_u32(np, "is_emulation", &is_emulation);
	pcie->is_emulation = is_emulation;

	pcie->reset = devm_gpiod_get_optional(dev, "perst", GPIOD_OUT_LOW);
	if (IS_ERR(pcie->reset))
		return PTR_ERR(pcie->reset);

	of_property_read_u32(np, "force_gen1", &force_gen1);
	pcie->force_gen1 = force_gen1;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "parf");
	pcie->parf = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->parf))
		return PTR_ERR(pcie->parf);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pcie->dbi = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->dbi))
		return PTR_ERR(pcie->dbi);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	pcie->elbi = devm_ioremap_resource(dev, res);
	if (IS_ERR(pcie->elbi))
		return PTR_ERR(pcie->elbi);

	pcie->phy = devm_phy_optional_get(dev, "pciephy");
	if (IS_ERR(pcie->phy))
		return PTR_ERR(pcie->phy);

	ret = pcie->ops->get_resources(pcie);
	if (ret)
		return ret;

	pp = &pcie->pp;
	pp->dev = dev;
	pp->dbi_base = pcie->dbi;
	pp->root_bus_nr = -1;
	pp->ops = &qcom_pcie_dw_ops;

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pp->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pp->msi_irq < 0)
			return pp->msi_irq;

		ret = devm_request_irq(dev, pp->msi_irq,
				       qcom_pcie_msi_irq_handler,
				       IRQF_SHARED, "qcom-pcie-msi", pp);
		if (ret) {
			dev_err(dev, "cannot request msi irq\n");
			return ret;
		}
	}

	ret = phy_init(pcie->phy);
	if (ret)
		return ret;

	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "cannot initialize host\n");
		return ret;
	}

	platform_set_drvdata(pdev, pcie);

	if (!rc_idx) {
		ret = bus_create_file(&pci_bus_type, &bus_attr_rcrescan);
		if (ret != 0) {
			dev_err(&pdev->dev,
				"Failed to create sysfs rcrescan file\n");
			return ret;
		}

		ret = bus_create_file(&pci_bus_type, &bus_attr_rcremove);
		if (ret != 0) {
			dev_err(&pdev->dev,
				"Failed to create sysfs rcremove file\n");
			return ret;
		}
	}
	qcom_pcie_dev[rc_idx++] = pcie;

	return 0;
}

static int qcom_pcie_remove(struct platform_device *pdev)
{
	struct qcom_pcie *pcie = platform_get_drvdata(pdev);

	if (!pcie->is_emulation)
		qcom_ep_reset_assert(pcie);

	phy_power_off(pcie->phy);
	phy_exit(pcie->phy);
	pcie->ops->deinit(pcie);

	return 0;
}

static void qcom_pcie_fixup_final(struct pci_dev *dev)
{
	int cap, err;
	u16 ctl, reg_val;

	cap = pci_pcie_cap(dev);
	if (!cap)
		return;

	err = pci_read_config_word(dev, cap + PCI_EXP_DEVCTL, &ctl);

	if (err)
		return;

	reg_val = ctl;

	if (((reg_val & PCIE20_MRRS_MASK) >> 12) > 1)
		reg_val = (reg_val & ~(PCIE20_MRRS_MASK)) | PCIE20_MRRS(0x1);

	if (((ctl & PCIE20_MPS_MASK) >> 5) > 1)
		reg_val = (reg_val & ~(PCIE20_MPS_MASK)) | PCIE20_MPS(0x1);

	err = pci_write_config_word(dev, cap + PCI_EXP_DEVCTL, reg_val);

	if (err)
		pr_err("pcie config write failed %d\n", err);
}
DECLARE_PCI_FIXUP_FINAL(PCI_ANY_ID, PCI_ANY_ID, qcom_pcie_fixup_final);

static const struct of_device_id qcom_pcie_match[] = {
	{ .compatible = "qcom,pcie-ipq8064", .data = &ops_v0 },
	{ .compatible = "qcom,pcie-apq8064", .data = &ops_v0 },
	{ .compatible = "qcom,pcie-apq8084", .data = &ops_v1 },
	{ .compatible = "qcom,pcie-ipq4019", .data = &ops_v2 },
	{ .compatible = "qcom,pcie-ipq807x", .data = &ops_v3 },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pcie_match);

static struct platform_driver qcom_pcie_driver = {
	.probe = qcom_pcie_probe,
	.remove = qcom_pcie_remove,
	.driver = {
		.name = "qcom-pcie",
		.of_match_table = qcom_pcie_match,
	},
};

module_platform_driver(qcom_pcie_driver);

MODULE_AUTHOR("Stanimir Varbanov <svarbanov@mm-sol.com>");
MODULE_DESCRIPTION("Qualcomm PCIe root complex driver");
MODULE_LICENSE("GPL v2");
