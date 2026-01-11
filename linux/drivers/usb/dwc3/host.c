// SPDX-License-Identifier: GPL-2.0
/*
 * host.c - DesignWare USB3 DRD Controller Host Glue
 *
 * Copyright (C) 2011 Texas Instruments Incorporated - https://www.ti.com
 *
 * Authors: Felipe Balbi <balbi@ti.com>,
 */

#include <linux/irq.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/usb.h>
#include <linux/usb/hcd.h>
#include <linux/of_device.h>

#include <linux/of_device.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>

#include "../host/xhci-port.h"
#include "../host/xhci-ext-caps.h"
#include "../host/xhci-caps.h"
#include "../host/xhci-plat.h"
#include "core.h"

#define XHCI_HCSPARAMS1		0x4
#define XHCI_PORTSC_BASE	0x400


/**
 * dwc3_power_off_all_roothub_ports - Power off all Root hub ports
 * @dwc: Pointer to our controller context structure
 */
static void dwc3_power_off_all_roothub_ports(struct dwc3 *dwc)
{
	void __iomem *xhci_regs;
	u32 op_regs_base;
	int port_num;
	u32 offset;
	u32 reg;
	int i;

	/* xhci regs are not mapped yet, do it temporarily here */
	if (dwc->xhci_resources[0].start) {
		xhci_regs = ioremap(dwc->xhci_resources[0].start, DWC3_XHCI_REGS_END);
		if (!xhci_regs) {
			dev_err(dwc->dev, "Failed to ioremap xhci_regs\n");
			return;
		}

		op_regs_base = HC_LENGTH(readl(xhci_regs));
		reg = readl(xhci_regs + XHCI_HCSPARAMS1);
		port_num = HCS_MAX_PORTS(reg);

		for (i = 1; i <= port_num; i++) {
			offset = op_regs_base + XHCI_PORTSC_BASE + 0x10 * (i - 1);
			reg = readl(xhci_regs + offset);
			reg &= ~PORT_POWER;
			writel(reg, xhci_regs + offset);
		}

		iounmap(xhci_regs);
	} else {
		dev_err(dwc->dev, "xhci base reg invalid\n");
	}
}

static void dwc3_xhci_plat_start(struct usb_hcd *hcd)
{
	struct platform_device *pdev;
	struct dwc3 *dwc;

	if (!usb_hcd_is_primary_hcd(hcd))
		return;

	pdev = to_platform_device(hcd->self.controller);
	dwc = dev_get_drvdata(pdev->dev.parent);

	dwc3_enable_susphy(dwc, true);
}

static const struct xhci_plat_priv dwc3_xhci_plat_quirk = {
	.plat_start = dwc3_xhci_plat_start,
};

static void dwc3_host_fill_xhci_irq_res(struct dwc3 *dwc,
					int irq, char *name)
{
	struct platform_device *pdev = to_platform_device(dwc->dev);
	struct device_node *np = dev_of_node(&pdev->dev);

	dwc->xhci_resources[1].start = irq;
	dwc->xhci_resources[1].end = irq;
	dwc->xhci_resources[1].flags = IORESOURCE_IRQ | irq_get_trigger_type(irq);
	if (!name && np)
		dwc->xhci_resources[1].name = of_node_full_name(pdev->dev.of_node);
	else
		dwc->xhci_resources[1].name = name;
}

static int dwc3_host_get_irq(struct dwc3 *dwc)
{
	struct platform_device	*dwc3_pdev = to_platform_device(dwc->dev);
	int irq;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "host");
	if (irq > 0) {
		dwc3_host_fill_xhci_irq_res(dwc, irq, "host");
		goto out;
	}

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq_byname_optional(dwc3_pdev, "dwc_usb3");
	if (irq > 0) {
		dwc3_host_fill_xhci_irq_res(dwc, irq, "dwc_usb3");
		goto out;
	}

	if (irq == -EPROBE_DEFER)
		goto out;

	irq = platform_get_irq(dwc3_pdev, 0);
	if (irq > 0)
		dwc3_host_fill_xhci_irq_res(dwc, irq, NULL);

out:
	return irq;
}

int dwc3_host_init(struct dwc3 *dwc)
{
	struct property_entry	props[6];
	struct platform_device	*xhci;
	int			ret, irq;
	int			prop_idx = 0;

	dwc3_power_off_all_roothub_ports(dwc);

	irq = dwc3_host_get_irq(dwc);
	if (irq < 0)
		return irq;

	xhci = platform_device_alloc("xhci-hcd", PLATFORM_DEVID_AUTO);
	if (!xhci)
		return -ENOMEM;

	/* parent / fwnode 先設好 */
	xhci->dev.parent = dwc->dev;
	xhci->dev.fwnode = dwc->dev->fwnode;

	/*
	 * 先把 DMA mask 設好！
	 * 很多平台 arch_setup_dma_ops()/of_dma_configure() 會依賴 dma_mask
	 * 你之前 get_dma_ops = NULL 八成就是因為這個順序錯。
	 */
	xhci->dev.dma_mask = dwc->dev->dma_mask;
	xhci->dev.coherent_dma_mask = dwc->dev->coherent_dma_mask;

	/*
	 * 關鍵：只呼叫一次 of_dma_configure()
	 * 讓 xhci 裝置複製 parent node 的 dma/iommu 設定（包含 iommus 解析）
	 */
	if (dwc->dev->of_node) {
		pr_info("dwc3: configuring DMA/IOMMU for xhci from parent DT node\n");
		ret = of_dma_configure(&xhci->dev, dwc->dev->of_node, true);
		if (ret) {
			dev_err(dwc->dev, "of_dma_configure(xhci) failed: %d\n", ret);
			goto err;
		}
	} else {
		/*
		 * 沒有 of_node 的情況就不要硬搞 IOMMU
		 * 讓它走一般 platform 的 dma_ops 配置。
		 */
		dev_warn(dwc->dev, "no of_node; xhci will use default DMA ops\n");
	}

	/* 這裡再存到 dwc */
	dwc->xhci = xhci;

	/* resources */
	ret = platform_device_add_resources(xhci, dwc->xhci_resources,
					    DWC3_XHCI_RESOURCES_NUM);
	if (ret)
		goto err;

	/* quirks software node */
	memset(props, 0, sizeof(props));
	props[prop_idx++] = PROPERTY_ENTRY_BOOL("xhci-sg-trb-cache-size-quirk");
	props[prop_idx++] = PROPERTY_ENTRY_BOOL("write-64-hi-lo-quirk");
	if (dwc->usb3_lpm_capable)
		props[prop_idx++] = PROPERTY_ENTRY_BOOL("usb3-lpm-capable");

	if (prop_idx) {
		ret = device_create_managed_software_node(&xhci->dev, props, NULL);
		if (ret)
			goto err;
	}

	platform_device_add_data(xhci, &dwc3_xhci_plat_quirk,
				 sizeof(struct xhci_plat_priv));

	/* register */
	ret = platform_device_add(xhci);
	if (ret)
		goto err;

	return 0;

err:
	platform_device_put(xhci);
	dwc->xhci = NULL;
	return ret;
}


void dwc3_host_exit(struct dwc3 *dwc)
{
	if (!dwc->xhci)
		return;

	if (dwc->sys_wakeup)
		device_init_wakeup(&dwc->xhci->dev, false);

	dwc3_enable_susphy(dwc, false);

	platform_device_unregister(dwc->xhci);
	dwc->xhci = NULL;
}