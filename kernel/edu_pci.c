// SPDX-License-Identifier: GPL-2.0
/*
 * edu_pci.c — SKELETON. Fill it in step by step following README.md.
 *
 * QEMU EDU hints (BAR0, 1 MB) — find the details yourself in
 * qemu docs/specs/edu.rst and record them in docs/design.md:
 *   0x00 ident, 0x04 liveness, 0x08 factorial, 0x20 status,
 *   0x24 irq status, 0x60 raise, 0x64 ack,
 *   0x80/0x88/0x90/0x98 DMA, 0x40000 DMA buffer 4096 B.
 */
#include <linux/module.h>
#include <linux/pci.h>

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

/* TODO LAB2: add register #defines here once you reach MMIO. */

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	dev_info(&pdev->dev, "edu: probe hit! TODO LAB1: pcim_enable_device + BAR0\n");
	/* TODO LAB1: pcim_enable_device(), pci_set_master(), pcim_request_region(BAR0) + pcim_iomap(). */
	/* TODO LAB2: ioread32(magic 0x00), liveness inversion check on 0x04. */
	/* TODO LAB3: pci_alloc_irq_vectors(MSI|INTX) + request_irq, ack via 0x64. */
	/* TODO LAB4: dma_set_mask_and_coherent(28 bits) + DMA self-test. */
	return 0;
}

static void edu_remove(struct pci_dev *pdev)
{
	dev_info(&pdev->dev, "edu: remove\n");
}

static struct pci_driver edu_driver = {
	.name = "edu",
	.id_table = edu_ids,
	.probe = edu_probe,
	.remove = edu_remove,
};

module_pci_driver(edu_driver);

MODULE_AUTHOR("you");
MODULE_DESCRIPTION("QEMU EDU skeleton — fill me in");
MODULE_LICENSE("GPL");
