// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2022 Mediatek Inc. All rights reserved.
//
// Author: Allen-KH Cheng <allen-kh.cheng@mediatek.com>
//         Tinghan Shen <tinghan.shen@mediatek.com>

/*
 * Hardware interface for audio DSP on mt8186
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/module.h>

#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "../../ops.h"
#include "../../sof-of-dev.h"
#include "../../sof-audio.h"
#include "../adsp_helper.h"
#include "mt8186.h"
#include "mt8186-clk.h"

static int mt8186_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return MBOX_OFFSET;
}

static int mt8186_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	return MBOX_OFFSET;
}

static int mt8186_send_msg(struct snd_sof_dev *sdev,
			   struct snd_sof_ipc_msg *msg)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;

	sof_mailbox_write(sdev, sdev->host_box.offset, msg->msg_data,
			  msg->msg_size);

	return mtk_adsp_ipc_send(priv->dsp_ipc, MTK_ADSP_IPC_REQ, MTK_ADSP_IPC_OP_REQ);
}

static void mt8186_get_reply(struct snd_sof_dev *sdev)
{
	struct snd_sof_ipc_msg *msg = sdev->msg;
	struct sof_ipc_reply reply;
	int ret = 0;

	if (!msg) {
		dev_warn(sdev->dev, "unexpected ipc interrupt\n");
		return;
	}

	/* get reply */
	sof_mailbox_read(sdev, sdev->host_box.offset, &reply, sizeof(reply));
	if (reply.error < 0) {
		memcpy(msg->reply_data, &reply, sizeof(reply));
		ret = reply.error;
	} else {
		/* reply has correct size? */
		if (reply.hdr.size != msg->reply_size) {
			dev_err(sdev->dev, "error: reply expected %zu got %u bytes\n",
				msg->reply_size, reply.hdr.size);
			ret = -EINVAL;
		}

		/* read the message */
		if (msg->reply_size > 0)
			sof_mailbox_read(sdev, sdev->host_box.offset,
					 msg->reply_data, msg->reply_size);
	}

	msg->reply_error = ret;
}

static void mt8186_dsp_handle_reply(struct mtk_adsp_ipc *ipc)
{
	struct adsp_priv *priv = mtk_adsp_ipc_get_data(ipc);
	unsigned long flags;

	spin_lock_irqsave(&priv->sdev->ipc_lock, flags);
	mt8186_get_reply(priv->sdev);
	snd_sof_ipc_reply(priv->sdev, 0);
	spin_unlock_irqrestore(&priv->sdev->ipc_lock, flags);
}

static void mt8186_dsp_handle_request(struct mtk_adsp_ipc *ipc)
{
	struct adsp_priv *priv = mtk_adsp_ipc_get_data(ipc);
	u32 p; /* panic code */
	int ret;

	/* Read the message from the debug box. */
	sof_mailbox_read(priv->sdev, priv->sdev->debug_box.offset + 4,
			 &p, sizeof(p));

	/* Check to see if the message is a panic code 0x0dead*** */
	if ((p & SOF_IPC_PANIC_MAGIC_MASK) == SOF_IPC_PANIC_MAGIC) {
		snd_sof_dsp_panic(priv->sdev, p, true);
	} else {
		snd_sof_ipc_msgs_rx(priv->sdev);

		/* tell DSP cmd is done */
		ret = mtk_adsp_ipc_send(priv->dsp_ipc, MTK_ADSP_IPC_RSP, MTK_ADSP_IPC_OP_RSP);
		if (ret)
			dev_err(priv->dev, "request send ipc failed");
	}
}

static struct mtk_adsp_ipc_ops dsp_ops = {
	.handle_reply		= mt8186_dsp_handle_reply,
	.handle_request		= mt8186_dsp_handle_request,
};

static int platform_parse_resource(struct platform_device *pdev, void *data)
{
	struct resource *mmio;
	struct resource res;
	struct device_node *mem_region;
	struct device *dev = &pdev->dev;
	struct mtk_adsp_chip_info *adsp = data;
	int ret;

	mem_region = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_region) {
		dev_err(dev, "no dma memory-region phandle\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_region, 0, &res);
	of_node_put(mem_region);
	if (ret) {
		dev_err(dev, "of_address_to_resource dma failed\n");
		return ret;
	}

	dev_dbg(dev, "DMA %pR\n", &res);

	ret = of_reserved_mem_device_init(dev);
	if (ret) {
		dev_err(dev, "of_reserved_mem_device_init failed\n");
		return ret;
	}

	mem_region = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!mem_region) {
		dev_err(dev, "no memory-region sysmem phandle\n");
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_region, 0, &res);
	of_node_put(mem_region);
	if (ret) {
		dev_err(dev, "of_address_to_resource sysmem failed\n");
		return ret;
	}

	adsp->pa_dram = (phys_addr_t)res.start;
	if (adsp->pa_dram & DRAM_REMAP_MASK) {
		dev_err(dev, "adsp memory(%#x) is not 4K-aligned\n",
			(u32)adsp->pa_dram);
		return -EINVAL;
	}

	adsp->dramsize = resource_size(&res);
	if (adsp->dramsize < TOTAL_SIZE_SHARED_DRAM_FROM_TAIL) {
		dev_err(dev, "adsp memory(%#x) is not enough for share\n",
			adsp->dramsize);
		return -EINVAL;
	}

	dev_dbg(dev, "dram pbase=%pa size=%#x\n", &adsp->pa_dram, adsp->dramsize);

	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
	if (!mmio) {
		dev_err(dev, "no ADSP-CFG register resource\n");
		return -ENXIO;
	}

	adsp->va_cfgreg = devm_ioremap_resource(dev, mmio);
	if (IS_ERR(adsp->va_cfgreg))
		return PTR_ERR(adsp->va_cfgreg);

	adsp->pa_cfgreg = (phys_addr_t)mmio->start;
	adsp->cfgregsize = resource_size(mmio);

	dev_dbg(dev, "cfgreg pbase=%pa size=%#x\n", &adsp->pa_cfgreg, adsp->cfgregsize);

	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (!mmio) {
		dev_err(dev, "no SRAM resource\n");
		return -ENXIO;
	}

	adsp->pa_sram = (phys_addr_t)mmio->start;
	adsp->sramsize = resource_size(mmio);

	dev_dbg(dev, "sram pbase=%pa size=%#x\n", &adsp->pa_sram, adsp->sramsize);

	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sec");
	if (!mmio) {
		dev_err(dev, "no SEC register resource\n");
		return -ENXIO;
	}

	adsp->va_secreg = devm_ioremap_resource(dev, mmio);
	if (IS_ERR(adsp->va_secreg))
		return PTR_ERR(adsp->va_secreg);

	adsp->pa_secreg = (phys_addr_t)mmio->start;
	adsp->secregsize = resource_size(mmio);

	dev_dbg(dev, "secreg pbase=%pa size=%#x\n", &adsp->pa_secreg, adsp->secregsize);

	mmio = platform_get_resource_byname(pdev, IORESOURCE_MEM, "bus");
	if (!mmio) {
		dev_err(dev, "no BUS register resource\n");
		return -ENXIO;
	}

	adsp->va_busreg = devm_ioremap_resource(dev, mmio);
	if (IS_ERR(adsp->va_busreg))
		return PTR_ERR(adsp->va_busreg);

	adsp->pa_busreg = (phys_addr_t)mmio->start;
	adsp->busregsize = resource_size(mmio);

	dev_dbg(dev, "busreg pbase=%pa size=%#x\n", &adsp->pa_busreg, adsp->busregsize);

	return 0;
}

static void adsp_sram_power_on(struct snd_sof_dev *sdev)
{
	snd_sof_dsp_update_bits(sdev, DSP_BUSREG_BAR, ADSP_SRAM_POOL_CON,
				DSP_SRAM_POOL_PD_MASK, 0);
}

static void adsp_sram_power_off(struct snd_sof_dev *sdev)
{
	snd_sof_dsp_update_bits(sdev, DSP_BUSREG_BAR, ADSP_SRAM_POOL_CON,
				DSP_SRAM_POOL_PD_MASK, DSP_SRAM_POOL_PD_MASK);
}

/*  Init the basic DSP DRAM address */
static int adsp_memory_remap_init(struct snd_sof_dev *sdev, struct mtk_adsp_chip_info *adsp)
{
	u32 offset;

	offset = adsp->pa_dram - DRAM_PHYS_BASE_FROM_DSP_VIEW;
	adsp->dram_offset = offset;
	offset >>= DRAM_REMAP_SHIFT;

	dev_dbg(sdev->dev, "adsp->pa_dram %pa, offset %#x\n", &adsp->pa_dram, offset);

	snd_sof_dsp_write(sdev, DSP_BUSREG_BAR, DSP_C0_EMI_MAP_ADDR, offset);
	snd_sof_dsp_write(sdev, DSP_BUSREG_BAR, DSP_C0_DMAEMI_MAP_ADDR, offset);

	if (offset != snd_sof_dsp_read(sdev, DSP_BUSREG_BAR, DSP_C0_EMI_MAP_ADDR) ||
	    offset != snd_sof_dsp_read(sdev, DSP_BUSREG_BAR, DSP_C0_DMAEMI_MAP_ADDR)) {
		dev_err(sdev->dev, "emi remap fail\n");
		return -EIO;
	}

	return 0;
}

static int adsp_shared_base_ioremap(struct platform_device *pdev, void *data)
{
	struct device *dev = &pdev->dev;
	struct mtk_adsp_chip_info *adsp = data;
	u32 shared_size;

	/* remap shared-dram base to be non-cachable */
	shared_size = TOTAL_SIZE_SHARED_DRAM_FROM_TAIL;
	adsp->pa_shared_dram = adsp->pa_dram + adsp->dramsize - shared_size;
	if (adsp->va_dram) {
		adsp->shared_dram = adsp->va_dram + DSP_DRAM_SIZE - shared_size;
	} else {
		adsp->shared_dram = devm_ioremap(dev, adsp->pa_shared_dram,
						 shared_size);
		if (!adsp->shared_dram) {
			dev_err(dev, "ioremap failed for shared DRAM\n");
			return -ENOMEM;
		}
	}
	dev_dbg(dev, "shared-dram vbase=%p, phy addr :%pa, size=%#x\n",
		adsp->shared_dram, &adsp->pa_shared_dram, shared_size);

	return 0;
}

static int mt8186_run(struct snd_sof_dev *sdev)
{
	u32 adsp_bootup_addr;

	adsp_bootup_addr = SRAM_PHYS_BASE_FROM_DSP_VIEW;
	dev_dbg(sdev->dev, "HIFIxDSP boot from base : 0x%08X\n", adsp_bootup_addr);
	mt8186_sof_hifixdsp_boot_sequence(sdev, adsp_bootup_addr);

	return 0;
}

static int mt8186_dsp_probe(struct snd_sof_dev *sdev)
{
	struct platform_device *pdev = container_of(sdev->dev, struct platform_device, dev);
	struct adsp_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	sdev->pdata->hw_pdata = priv;
	priv->dev = sdev->dev;
	priv->sdev = sdev;

	priv->adsp = devm_kzalloc(&pdev->dev, sizeof(struct mtk_adsp_chip_info), GFP_KERNEL);
	if (!priv->adsp)
		return -ENOMEM;

	ret = platform_parse_resource(pdev, priv->adsp);
	if (ret)
		return ret;

	sdev->bar[SOF_FW_BLK_TYPE_IRAM] = devm_ioremap(sdev->dev,
						       priv->adsp->pa_sram,
						       priv->adsp->sramsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_IRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size %#x\n",
			&priv->adsp->pa_sram, priv->adsp->sramsize);
		return -ENOMEM;
	}

	sdev->bar[SOF_FW_BLK_TYPE_SRAM] = devm_ioremap_wc(sdev->dev,
							  priv->adsp->pa_dram,
							  priv->adsp->dramsize);
	if (!sdev->bar[SOF_FW_BLK_TYPE_SRAM]) {
		dev_err(sdev->dev, "failed to ioremap base %pa size %#x\n",
			&priv->adsp->pa_dram, priv->adsp->dramsize);
		return -ENOMEM;
	}

	priv->adsp->va_dram = sdev->bar[SOF_FW_BLK_TYPE_SRAM];

	ret = adsp_shared_base_ioremap(pdev, priv->adsp);
	if (ret) {
		dev_err(sdev->dev, "adsp_shared_base_ioremap fail!\n");
		return ret;
	}

	sdev->bar[DSP_REG_BAR] = priv->adsp->va_cfgreg;
	sdev->bar[DSP_SECREG_BAR] = priv->adsp->va_secreg;
	sdev->bar[DSP_BUSREG_BAR] = priv->adsp->va_busreg;

	sdev->mmio_bar = SOF_FW_BLK_TYPE_SRAM;
	sdev->mailbox_bar = SOF_FW_BLK_TYPE_SRAM;

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = mt8186_get_mailbox_offset(sdev);

	ret = adsp_memory_remap_init(sdev, priv->adsp);
	if (ret) {
		dev_err(sdev->dev, "adsp_memory_remap_init fail!\n");
		return ret;
	}

	/* enable adsp clock before touching registers */
	ret = mt8186_adsp_init_clock(sdev);
	if (ret) {
		dev_err(sdev->dev, "mt8186_adsp_init_clock failed\n");
		return ret;
	}

	ret = mt8186_adsp_clock_on(sdev);
	if (ret) {
		dev_err(sdev->dev, "mt8186_adsp_clock_on fail!\n");
		return ret;
	}

	adsp_sram_power_on(sdev);

	priv->ipc_dev = platform_device_register_data(&pdev->dev, "mtk-adsp-ipc",
						      PLATFORM_DEVID_NONE,
						      pdev, sizeof(*pdev));
	if (IS_ERR(priv->ipc_dev)) {
		ret = PTR_ERR(priv->ipc_dev);
		dev_err(sdev->dev, "failed to create mtk-adsp-ipc device\n");
		goto err_adsp_off;
	}

	priv->dsp_ipc = dev_get_drvdata(&priv->ipc_dev->dev);
	if (!priv->dsp_ipc) {
		ret = -EPROBE_DEFER;
		dev_err(sdev->dev, "failed to get drvdata\n");
		goto exit_pdev_unregister;
	}

	mtk_adsp_ipc_set_data(priv->dsp_ipc, priv);
	priv->dsp_ipc->ops = &dsp_ops;

	return 0;

exit_pdev_unregister:
	platform_device_unregister(priv->ipc_dev);
err_adsp_off:
	adsp_sram_power_off(sdev);
	mt8186_adsp_clock_off(sdev);

	return ret;
}

static int mt8186_dsp_remove(struct snd_sof_dev *sdev)
{
	struct adsp_priv *priv = sdev->pdata->hw_pdata;

	platform_device_unregister(priv->ipc_dev);
	mt8186_sof_hifixdsp_shutdown(sdev);
	adsp_sram_power_off(sdev);
	mt8186_adsp_clock_off(sdev);

	return 0;
}

static int mt8186_dsp_suspend(struct snd_sof_dev *sdev, u32 target_state)
{
	mt8186_sof_hifixdsp_shutdown(sdev);
	adsp_sram_power_off(sdev);
	mt8186_adsp_clock_off(sdev);

	return 0;
}

static int mt8186_dsp_resume(struct snd_sof_dev *sdev)
{
	int ret;

	ret = mt8186_adsp_clock_on(sdev);
	if (ret) {
		dev_err(sdev->dev, "mt8186_adsp_clock_on fail!\n");
		return ret;
	}

	adsp_sram_power_on(sdev);

	return ret;
}

/* on mt8186 there is 1 to 1 match between type and BAR idx */
static int mt8186_get_bar_index(struct snd_sof_dev *sdev, u32 type)
{
	return type;
}

static int mt8186_ipc_msg_data(struct snd_sof_dev *sdev,
			       struct snd_pcm_substream *substream,
			       void *p, size_t sz)
{
	sof_mailbox_read(sdev, sdev->dsp_box.offset, p, sz);
	return 0;
}

/* mt8186 ops */
static struct snd_sof_dsp_ops sof_mt8186_ops = {
	/* probe and remove */
	.probe		= mt8186_dsp_probe,
	.remove		= mt8186_dsp_remove,

	/* DSP core boot */
	.run		= mt8186_run,

	/* Block IO */
	.block_read	= sof_block_read,
	.block_write	= sof_block_write,

	/* Register IO */
	.write		= sof_io_write,
	.read		= sof_io_read,
	.write64	= sof_io_write64,
	.read64		= sof_io_read64,

	/* ipc */
	.send_msg		= mt8186_send_msg,
	.get_mailbox_offset	= mt8186_get_mailbox_offset,
	.get_window_offset	= mt8186_get_window_offset,
	.ipc_msg_data		= mt8186_ipc_msg_data,
	.set_stream_data_offset = sof_set_stream_data_offset,

	/* misc */
	.get_bar_index	= mt8186_get_bar_index,

	/* firmware loading */
	.load_firmware	= snd_sof_load_firmware_memcpy,

	/* Firmware ops */
	.dsp_arch_ops = &sof_xtensa_arch_ops,

	/* PM */
	.suspend	= mt8186_dsp_suspend,
	.resume		= mt8186_dsp_resume,

	/* ALSA HW info flags */
	.hw_info =	SNDRV_PCM_INFO_MMAP |
			SNDRV_PCM_INFO_MMAP_VALID |
			SNDRV_PCM_INFO_INTERLEAVED |
			SNDRV_PCM_INFO_PAUSE |
			SNDRV_PCM_INFO_NO_PERIOD_WAKEUP,
};

static const struct sof_dev_desc sof_of_mt8186_desc = {
	.ipc_supported_mask	= BIT(SOF_IPC),
	.ipc_default		= SOF_IPC,
	.default_fw_path = {
		[SOF_IPC] = "mediatek/sof",
	},
	.default_tplg_path = {
		[SOF_IPC] = "mediatek/sof-tplg",
	},
	.default_fw_filename = {
		[SOF_IPC] = "sof-mt8186.ri",
	},
	.nocodec_tplg_filename = "sof-mt8186-nocodec.tplg",
	.ops = &sof_mt8186_ops,
};

static const struct of_device_id sof_of_mt8186_ids[] = {
	{ .compatible = "mediatek,mt8186-dsp", .data = &sof_of_mt8186_desc},
	{ }
};
MODULE_DEVICE_TABLE(of, sof_of_mt8186_ids);

/* DT driver definition */
static struct platform_driver snd_sof_of_mt8186_driver = {
	.probe = sof_of_probe,
	.remove = sof_of_remove,
	.driver = {
	.name = "sof-audio-of-mt8186",
		.pm = &sof_of_pm,
		.of_match_table = sof_of_mt8186_ids,
	},
};
module_platform_driver(snd_sof_of_mt8186_driver);

MODULE_IMPORT_NS(SND_SOC_SOF_XTENSA);
MODULE_IMPORT_NS(SND_SOC_SOF_MTK_COMMON);
MODULE_LICENSE("Dual BSD/GPL");
