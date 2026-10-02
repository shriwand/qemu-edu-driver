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

#define EDU_BAR 0
#define EDU_REG_IDENT 0
#define EDU_DRIVER_NAME "edu_pci"

struct edu_dev {
    struct pci_dev *pdev;
    void __iomem *mmio;
};

static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static int fail_step;
module_param(fail_step, int, 0444);
MODULE_PARM_DESC(fail_step,
                 "Inject probe failure: 1=after enable, "
                 "2=after BAR request, 3=after BAR map, "
                 "4=after identification read");

static int edu_maybe_fail(struct pci_dev *pdev, int step)
{
    if (fail_step != step)
        return 0;

    dev_info(&pdev->dev, "injecting failure at step %d\n", step);
    return -EIO;
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int rc;
    resource_size_t bar_start;
    resource_size_t bar_len;
    unsigned long bar_flags;
    u32 ident;
    struct edu_dev *edev;

    dev_info(&pdev->dev, "probing EDU at %s\n", pci_name(pdev));

    edev = devm_kzalloc(&pdev->dev, sizeof(*edev), GFP_KERNEL);
    if(!edev)
        return -ENOMEM;

    edev->pdev = pdev;
    pci_set_drvdata(pdev, edev);

    bar_start = pci_resource_start(pdev, EDU_BAR);
    bar_len = pci_resource_len(pdev, EDU_BAR);
    bar_flags = pci_resource_flags(pdev, EDU_BAR);

    if (!(bar_flags & IORESOURCE_MEM)) {
        dev_err(&pdev->dev, "BAR%d is not MMIO\n", EDU_BAR);
        return -ENODEV;
    }

    if (!bar_len) {
        dev_err(&pdev->dev, "BAR%d has zero length\n", EDU_BAR);
        return -ENODEV;
    }

    rc = pci_enable_device(pdev);
    if(rc){
        dev_err(&pdev->dev, "pci_enable_device failed: %d\n", rc);
        goto err_disable_device;
    }

    rc = edu_maybe_fail(pdev, 1);
    if (rc)
        goto err_disable_device;

    rc = pci_request_region(pdev, EDU_BAR, EDU_DRIVER_NAME);
    if(rc){
        dev_err(&pdev->dev, "failed to request BAR%d: %d\n",
                    EDU_BAR, rc);
        goto err_disable_device;
    }

    rc = edu_maybe_fail(pdev, 2);
    if (rc)
        goto err_release_region;

    edev->mmio=pci_iomap(pdev, EDU_BAR, 0);
    if(!edev->mmio) {
        dev_err(&pdev->dev, "failed to map BAR%d\n", EDU_BAR);
        rc = -ENOMEM;
        goto err_release_region;
    }

    rc = edu_maybe_fail(pdev, 3);
    if (rc)
        goto err_iounmap;

    ident = ioread32(edev->mmio + EDU_REG_IDENT);
    dev_info(&pdev->dev,
            "EDU identification register: %#010x\n",
            ident);

    rc = edu_maybe_fail(pdev, 4);
    if (rc)
        goto err_iounmap;

    dev_info(&pdev->dev, "EDU probe completed\n");

	return 0;

err_iounmap:
    pci_iounmap(pdev, edev->mmio);
    edev->mmio = NULL;
err_release_region:
    pci_release_region(pdev, EDU_BAR);
err_disable_device:
    pci_disable_device(pdev);

    return rc;
}

static void edu_remove(struct pci_dev *pdev)
{
    struct edu_dev *edev = pci_get_drvdata(pdev);

    dev_info(&pdev->dev, "removing EDU device\n");

    pci_iounmap(pdev, edev->mmio);
    edev->mmio = NULL;

    pci_release_region(pdev, EDU_BAR);
    pci_disable_device(pdev);

    dev_info(&pdev->dev, "EDU remove completed\n");
}

static struct pci_driver edu_driver = {
	.name = "edu",
	.id_table = edu_ids,
	.probe = edu_probe,
	.remove = edu_remove,
};

module_pci_driver(edu_driver);

MODULE_AUTHOR("Ivan Sharavuev<shriwand@gmail.com>");
MODULE_DESCRIPTION("QEMU edu pic driver");
MODULE_LICENSE("GPL");
