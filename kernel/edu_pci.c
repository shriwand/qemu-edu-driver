// SPDX-License-Identifier: GPL-2.0-only
/*
 * QEMU EDU hints (BAR0, 1 MB) — find the details yourself in
 * qemu docs/specs/edu.rst and record them in docs/design.md:
 *   0x00 ident, 0x04 liveness, 0x08 factorial, 0x20 status,
 *   0x24 irq status, 0x60 raise, 0x64 ack,
 *   0x80/0x88/0x90/0x98 DMA, 0x40000 DMA buffer 4096 B.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/debugfs.h>

#include "uapi/edu_uapi.h"

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_BAR 0
#define EDU_REG_IDENT 0
#define EDU_NAME "edu"
#define EDU_DRIVER_NAME EDU_NAME "_pci"
#define EDU_MISCDEV_NAME EDU_NAME "0"

#define EDU_REG_FACTORIAL 0x08
#define EDU_REG_STATUS 0x20

#define EDU_STATUS_INT_ENABLE 0x80

#define EDU_REG_IRQ_STATUS 0x24
#define EDU_REG_IRQ_RAISE 0x60
#define EDU_REG_IRQ_ACK 0x64

#define EDU_IRQ_FACTORIAL 0x01
#define EDU_FACTORIAL_TIMEOUT_MS 1000

struct edu_dev {
	struct pci_dev *pdev;
	void __iomem *mmio;

	struct mutex op_lock;
	struct completion factorial_done;
	atomic_t irq_count;

	int irq;
	struct miscdevice miscdev;

	struct dentry *debugfs_dir;
};


static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static int fail_step;
module_param(fail_step, int, 0444);
MODULE_PARM_DESC(fail_step, "Inject probe failure: 1=after enable, "
			    "2=after BAR request, 3=after BAR map, "
			    "4=after identification read, 5=after IRQ request, "
			    "6=after debugfs init, 7=after misc register");

static struct dentry *edu_debugfs_root;

static void edu_debugfs_init(struct edu_dev *edev)
{
	edev->debugfs_dir =
		debugfs_create_dir(pci_name(edev->pdev), edu_debugfs_root);
	if (IS_ERR_OR_NULL(edev->debugfs_dir)) {
		edev->debugfs_dir = NULL;
		return;
	}

	debugfs_create_atomic_t("irq_count", 0444, edev->debugfs_dir,
				&edev->irq_count);
}

static void edu_debugfs_remove(struct edu_dev *edev)
{
	debugfs_remove_recursive(edev->debugfs_dir);
	edev->debugfs_dir = NULL;
}

static void edu_disable_factorial_irq(struct edu_dev *edev)
{
	u32 status;

    status = ioread32(edev->mmio + EDU_REG_STATUS);
    iowrite32(status & ~EDU_STATUS_INT_ENABLE,
              edev->mmio + EDU_REG_STATUS);
}

static void edu_ack_pending_irqs(struct edu_dev *edev)
{
	u32 status;

	status = ioread32(edev->mmio + EDU_REG_IRQ_STATUS);
	if (status)
		iowrite32(status, edev->mmio + EDU_REG_IRQ_ACK);
}

static irqreturn_t edu_irq_handler(int irq, void *data)
{
	struct edu_dev *edev = data;
	u32 status;

	status = ioread32(edev->mmio + EDU_REG_IRQ_STATUS);

	if (!(status & EDU_IRQ_FACTORIAL))
		return IRQ_NONE;

	atomic_inc(&edev->irq_count);

	iowrite32(status, edev->mmio + EDU_REG_IRQ_ACK);

	complete(&edev->factorial_done);

	return IRQ_HANDLED;
}

static int edu_run_factorial(struct edu_dev *edev,
			     struct edu_factorial_req *req)
{
	u32 status;
	unsigned long timeout;

	reinit_completion(&edev->factorial_done);

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	iowrite32(status | EDU_STATUS_INT_ENABLE, edev->mmio + EDU_REG_STATUS);

	iowrite32(req->input, edev->mmio + EDU_REG_FACTORIAL);

	timeout = wait_for_completion_timeout(
		&edev->factorial_done,
		msecs_to_jiffies(EDU_FACTORIAL_TIMEOUT_MS));

	edu_disable_factorial_irq(edev);

	if (!timeout) {
		dev_err(&edev->pdev->dev, "factorial operation timed out\n");
		return -ETIMEDOUT;
	}

	req->result = ioread32(edev->mmio + EDU_REG_FACTORIAL);
	return 0;
}

static int edu_open(struct inode *unused_inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct edu_dev *edev = container_of(miscdev, struct edu_dev, miscdev);
	file->private_data = edev;

	return 0;
}

static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct edu_dev *edev = file->private_data;
	struct edu_factorial_req req;
	void __user *argp = (void __user *)arg;
	int ret;

	if (_IOC_TYPE(cmd) != EDU_IOCTL_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case EDU_IOCTL_FACTORIAL:
		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;

		if (req.input > EDU_FACTORIAL_MAX_INPUT)
			return -ERANGE;

		ret = mutex_lock_interruptible(&edev->op_lock);
		if (ret)
			return ret;

		ret = edu_run_factorial(edev, &req);

		mutex_unlock(&edev->op_lock);

		if (ret)
			return ret;

		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;

		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations edu_fops = {
	.owner = THIS_MODULE,
	.open = edu_open,
	.unlocked_ioctl = edu_ioctl,
	.compat_ioctl = edu_ioctl,
	.llseek = noop_llseek,
};

static int edu_maybe_fail(struct pci_dev *pdev, int step)
{
	if (fail_step != step)
		return 0;

	dev_info(&pdev->dev, "injecting failure at step %d\n", step);
	return -EIO;
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	resource_size_t bar_len;
	unsigned long bar_flags;
	u32 ident;
	struct edu_dev *edev;

	dev_info(&pdev->dev, "probing EDU at %s\n", pci_name(pdev));

	edev = devm_kzalloc(&pdev->dev, sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return -ENOMEM;

	edev->pdev = pdev;
	pci_set_drvdata(pdev, edev);

	mutex_init(&edev->op_lock);
	init_completion(&edev->factorial_done);
	atomic_set(&edev->irq_count, 0);

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

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		goto err_disable_device;
	}

	ret = edu_maybe_fail(pdev, 1);
	if (ret)
		goto err_disable_device;

	ret = pci_request_region(pdev, EDU_BAR, EDU_DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR%d: %d\n", EDU_BAR,
			ret);
		goto err_disable_device;
	}

	ret = edu_maybe_fail(pdev, 2);
	if (ret)
		goto err_release_region;

	edev->mmio = pci_iomap(pdev, EDU_BAR, 0);
	if (!edev->mmio) {
		dev_err(&pdev->dev, "failed to map BAR%d\n", EDU_BAR);
		ret = -ENOMEM;
		goto err_release_region;
	}

	ret = edu_maybe_fail(pdev, 3);
	if (ret)
		goto err_iounmap;

	ident = ioread32(edev->mmio + EDU_REG_IDENT);
	dev_info(&pdev->dev, "EDU identification register: %#010x\n", ident);

	ret = edu_maybe_fail(pdev, 4);
	if (ret)
		goto err_iounmap;

	edev->irq = pdev->irq;
	if (edev->irq <= 0) {
		dev_err(&pdev->dev, "no legacy IRQ assigned\n");
		ret = -ENXIO;
		goto err_iounmap;
	}

	ret = request_irq(edev->irq, edu_irq_handler, IRQF_SHARED,
			  EDU_DRIVER_NAME, edev);

	if (ret) {
		dev_err(&pdev->dev, "request_irq(%d) failed: %d\n", edev->irq,
			ret);
		goto err_iounmap;
	}

	ret = edu_maybe_fail(pdev, 5);
	if (ret)
		goto err_free_irq;

	edu_ack_pending_irqs(edev);

	edu_debugfs_init(edev);

	ret = edu_maybe_fail(pdev, 6);
	if (ret)
		goto err_debugfs;

	edev->miscdev.minor = MISC_DYNAMIC_MINOR;
	edev->miscdev.name = EDU_MISCDEV_NAME;
	edev->miscdev.fops = &edu_fops;
	edev->miscdev.parent = &pdev->dev;

	ret = misc_register(&edev->miscdev);
	if (ret)
		goto err_debugfs;

	ret = edu_maybe_fail(pdev, 7);
	if (ret)
		goto err_misc_deregister;

	dev_info(&pdev->dev, "userspace device /dev/%s ready\n",
		 edev->miscdev.name);

	return 0;

err_misc_deregister:
	misc_deregister(&edev->miscdev);
err_debugfs:
	edu_debugfs_remove(edev);
err_free_irq:
	free_irq(edev->irq, edev);
err_iounmap:
	pci_iounmap(pdev, edev->mmio);
	edev->mmio = NULL;
err_release_region:
	pci_release_region(pdev, EDU_BAR);
err_disable_device:
	pci_disable_device(pdev);

	return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "removing EDU at %s\n", pci_name(pdev));

	misc_deregister(&edev->miscdev);
	edu_debugfs_remove(edev);

	edu_disable_factorial_irq(edev);
	edu_ack_pending_irqs(edev);

	free_irq(edev->irq, edev);

	pci_iounmap(pdev, edev->mmio);
	edev->mmio = NULL;

	pci_release_region(pdev, EDU_BAR);
	pci_disable_device(pdev);

	dev_info(&pdev->dev, "EDU remove completed\n");
}

static struct pci_driver edu_driver = {
	.name = EDU_DRIVER_NAME,
	.id_table = edu_ids,
	.probe = edu_probe,
	.remove = edu_remove,
};

static int __init edu_init(void)
{
	int ret;

	edu_debugfs_root = debugfs_create_dir(EDU_NAME, NULL);
	if (IS_ERR_OR_NULL(edu_debugfs_root)) {
		pr_info("edu: debugfs unavailable, continuing without debug counters\n");
		edu_debugfs_root = NULL;
	}

	ret = pci_register_driver(&edu_driver);
	if (ret) {
		debugfs_remove_recursive(edu_debugfs_root);
		edu_debugfs_root = NULL;
		return ret;
	}

	return 0;
}

static void __exit edu_exit(void)
{
	pci_unregister_driver(&edu_driver);
	debugfs_remove_recursive(edu_debugfs_root);
	edu_debugfs_root = NULL;
}

module_init(edu_init);
module_exit(edu_exit);

MODULE_AUTHOR("Ivan Sharavuev<shriwand@gmail.com>");
MODULE_DESCRIPTION("QEMU edu pic driver");
MODULE_LICENSE("GPL");
