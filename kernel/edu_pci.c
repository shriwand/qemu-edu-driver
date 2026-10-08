// SPDX-License-Identifier: GPL-2.0-only
/*
 * QEMU EDU hints (BAR0, 1 MB) — find the details yourself in
 * qemu docs/specs/edu.rst and record them in docs/design.md:
 *   0x00 ident, 0x04 liveness, 0x08 factorial, 0x20 status,
 *   0x24 irq status, 0x60 raise, 0x64 ack,
 *   0x80/0x88/0x90/0x98 DMA, 0x40000 DMA buffer 4096 B.
 */
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "uapi/edu_uapi.h"

#define EDU_VENDOR_ID 0x1234
#define EDU_DEVICE_ID 0x11e8

#define EDU_BAR 0
#define EDU_BAR_MIN_SIZE (EDU_REG_IRQ_ACK + sizeof(u32))
#define EDU_REG_IDENT 0x0
#define EDU_NAME "edu"
#define EDU_DRIVER_NAME EDU_NAME "_pci"
#define EDU_MISCDEV_NAME EDU_NAME "0"

#define EDU_REG_FACTORIAL         0x08
#define EDU_REG_STATUS            0x20
#define EDU_REG_IRQ_STATUS        0x24
#define EDU_REG_IRQ_ACK           0x64

#define EDU_STATUS_BUSY           0x01
#define EDU_STATUS_INT_ENABLE     0x80
#define EDU_IRQ_FACTORIAL         0x01

#define EDU_FACTORIAL_TIMEOUT_MS  1000

struct edu_dev {
	struct kref refcount;
	struct mutex op_lock;
	spinlock_t state_lock;

	bool disconnected;
	bool faulted;
	bool op_active;
	bool suppress_completion;

	void __iomem *mmio;
	struct pci_dev *pdev;
	int irq;

	struct completion factorial_done;
	wait_queue_head_t test_wq;
	atomic_t irq_count;

	struct miscdevice miscdev;
	struct dentry *debugfs_dir;

	bool pci_enabled;
	bool bar_requested;
	bool irq_requested;
	bool misc_registered;

	char bdf[32];
};

struct edu_file {
    struct edu_dev *edev;
};

struct edu_irq_snapshot {
    char text[32];
    size_t len;
};


static const struct pci_device_id edu_ids[] = {
	{ PCI_DEVICE(EDU_VENDOR_ID, EDU_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, edu_ids);

static int fail_step;
module_param(fail_step, int, 0444);
MODULE_PARM_DESC(fail_step,
    "Probe failure: 1=enable, 2=BAR, 3=map, 4=prepare, "
    "5=IRQ, 6=debugfs, 7=misc publication");

static unsigned int test_hold_ms;
module_param(test_hold_ms, uint, 0444);
MODULE_PARM_DESC(test_hold_ms,
    "Test-only cancellable hold after factorial start, max 10000 ms");

static bool test_force_timeout;
module_param(test_force_timeout, bool, 0444);
MODULE_PARM_DESC(test_force_timeout,
    "Test-only suppress factorial completion notification");

static unsigned int test_publish_pause_ms;
module_param(test_publish_pause_ms, uint, 0444);
MODULE_PARM_DESC(test_publish_pause_ms,
    "Test-only pause after misc publication, max 10000 ms");

static struct dentry *edu_debugfs_root;

static int edu_irq_snapshot_open(struct inode *inode,
	struct file *file)
{
	struct edu_dev *edev = inode->i_private;
	struct edu_irq_snapshot *snapshot;
	int count;

	snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
	if (!snapshot)
		return -ENOMEM;

	/*
	 * The regular debugfs proxy protects this callback
	 * against concurrent debugfs removal.
	 */
	count = atomic_read(&edev->irq_count);

	snapshot->len = scnprintf(
		snapshot->text, sizeof(snapshot->text),
		"%d\n", count);

	file->private_data = snapshot;
	return 0;
}

static ssize_t edu_irq_snapshot_read(struct file *file,
	char __user *buf,
	size_t count,
	loff_t *ppos)
{
	struct edu_irq_snapshot *snapshot = file->private_data;

	return simple_read_from_buffer(
		buf, count, ppos,
		snapshot->text, snapshot->len);
}

static int edu_irq_snapshot_release(struct inode *inode,
	struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations edu_irq_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = edu_irq_snapshot_open,
	.read = edu_irq_snapshot_read,
	.release = edu_irq_snapshot_release,
	.llseek = noop_llseek,
};

static void edu_debugfs_init(struct edu_dev *edev)
{
	struct dentry *file;

	if (!edu_debugfs_root)
		return;

	edev->debugfs_dir = debugfs_create_dir(
		edev->bdf, edu_debugfs_root);

	if (IS_ERR_OR_NULL(edev->debugfs_dir)) {
		edev->debugfs_dir = NULL;
		return;
	}

	file = debugfs_create_file(
		"irq_count", 0444,
		edev->debugfs_dir, edev,
		&edu_irq_snapshot_fops);

	if (IS_ERR_OR_NULL(file)) {
		debugfs_remove(edev->debugfs_dir);
		edev->debugfs_dir = NULL;
	}
}

static void edu_debugfs_remove(struct edu_dev *edev)
{
	debugfs_remove(edev->debugfs_dir);
	edev->debugfs_dir = NULL;
}

/*
 * The following helpers require:
 *
 * - state_lock held;
 * - a valid MMIO mapping;
 * - hardware not yet detached.
 */
static void edu_disable_factorial_irq_locked(struct edu_dev *edev)
{
	u32 status;

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	iowrite32(status & ~EDU_STATUS_INT_ENABLE,
		edev->mmio + EDU_REG_STATUS);

	/* Read-back completes the register access sequence. */
	ioread32(edev->mmio + EDU_REG_STATUS);
}

static void edu_ack_all_pending_locked(struct edu_dev *edev)
{
	u32 status;

	status = ioread32(edev->mmio + EDU_REG_IRQ_STATUS);
	if (status) {
		iowrite32(status, edev->mmio + EDU_REG_IRQ_ACK);
		ioread32(edev->mmio + EDU_REG_IRQ_STATUS);
	}
}

static irqreturn_t edu_irq_handler(int irq, void *data)
{
	struct edu_dev *edev = data;
	unsigned long flags;
	u32 status;
	bool notify = false;

	spin_lock_irqsave(&edev->state_lock, flags);

	status = ioread32(edev->mmio + EDU_REG_IRQ_STATUS);

	if (!status) {
		spin_unlock_irqrestore(&edev->state_lock, flags);
		return IRQ_NONE;
	}

	/*
	 * No DMA is started by this driver.
	 * Unsupported device causes are acknowledged and make
	 * the factorial engine faulted.
	 */
	if (status & ~EDU_IRQ_FACTORIAL) {
		edev->faulted = true;
		notify = edev->op_active;
	}

	if (status & EDU_IRQ_FACTORIAL) {
		atomic_inc(&edev->irq_count);

		if (edev->op_active && !edev->suppress_completion)
			notify = true;
	}

	iowrite32(status, edev->mmio + EDU_REG_IRQ_ACK);

	if (notify)
		complete(&edev->factorial_done);

	spin_unlock_irqrestore(&edev->state_lock, flags);

	return IRQ_HANDLED;
}

static int edu_run_factorial_locked(struct edu_dev *edev, struct edu_factorial_req *req)
{
	u32 status;
	unsigned long timeout;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected) {
		ret = -ENODEV;
		goto out_lock;

	}

	if (edev->faulted) {
		ret = -EIO;
		goto out_lock;
	}

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	if (status & EDU_STATUS_BUSY) {
		edev->faulted = true;
		ret = -EIO;
		goto out_lock;
	}

	/*
	 * One transaction only. The previous transaction must
	 * already have completed normally; after timeout we
	 * never reach this path again before rebind.
	 */
	edu_disable_factorial_irq_locked(edev);
	edu_ack_all_pending_locked(edev);
	reinit_completion(&edev->factorial_done);

	edev->op_active = true;
	edev->suppress_completion = test_force_timeout;

	iowrite32(status | EDU_STATUS_INT_ENABLE, edev->mmio + EDU_REG_STATUS);
	iowrite32(req->input, edev->mmio + EDU_REG_FACTORIAL);

	spin_unlock_irqrestore(&edev->state_lock, flags);

	/*
	 * Test-only controlled point:
	 * hardware has started, op_lock is held, hw_lock is not.
	 * Disconnect wakes this wait immediately.
	 */
	if (test_hold_ms) {
		pr_info("edu: %s test point: operation started\n",
			edev->bdf);

		wait_event_timeout(
			edev->test_wq,
			READ_ONCE(edev->disconnected),
			msecs_to_jiffies(test_hold_ms));
	}

	timeout = wait_for_completion_timeout(
		&edev->factorial_done,
		msecs_to_jiffies(EDU_FACTORIAL_TIMEOUT_MS));

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected) {
		ret = -ENODEV;
		goto out_finish;

	}

	if (!timeout) {
		edev->faulted = true;
		edu_disable_factorial_irq_locked(edev);
		edu_ack_all_pending_locked(edev);
		ret = -ETIMEDOUT;
		goto out_finish;
	}

	if (edev->faulted) {
		edu_disable_factorial_irq_locked(edev);
		edu_ack_all_pending_locked(edev);
		ret = -EIO;
		goto out_finish;
	}

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	if (status & EDU_STATUS_BUSY) {
		edev->faulted = true;
		edu_disable_factorial_irq_locked(edev);
		edu_ack_all_pending_locked(edev);
		ret = -EIO;
		goto out_finish;
	}

	req->result = ioread32(edev->mmio + EDU_REG_FACTORIAL);
	edu_disable_factorial_irq_locked(edev);
	ret = 0;

out_finish:
	edev->op_active = false;
	edev->suppress_completion = false;

out_lock:
	spin_unlock_irqrestore(&edev->state_lock, flags);
	return ret;
}


static int edu_open(struct inode *unused_inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct edu_dev *edev = container_of(miscdev, struct edu_dev, miscdev);
	struct edu_file *efile;
	unsigned long flags;
	int ret = 0;

	efile = kzalloc(sizeof(*efile), GFP_KERNEL);
	if (!efile)
		return -ENOMEM;

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected) {
		ret = -ENODEV;
	} else {
		kref_get(&edev->refcount);

		efile->edev = edev;
		file->private_data = efile;
	}

	spin_unlock_irqrestore(&edev->state_lock, flags);

	if (ret)
		kfree(efile);

	return ret;
}

static void edu_dev_release(struct kref *kref)
{
	struct edu_dev *edev = container_of(kref, struct edu_dev, refcount);
	pr_debug("edu: object released for %s\n", edev->bdf);
	kfree(edev);
}

static int edu_release(struct inode *inode, struct file *file)
{
	struct edu_file *efile = file->private_data;
	struct edu_dev *edev = efile->edev;

	file->private_data = NULL;
	kfree(efile);
	kref_put(&edev->refcount, edu_dev_release);

	return 0;
}

static bool edu_is_disconnected(struct edu_dev *edev)
{
	unsigned long flags;
	bool disconnected;

	spin_lock_irqsave(&edev->state_lock, flags);
	disconnected = edev->disconnected;
	spin_unlock_irqrestore(&edev->state_lock, flags);

	return disconnected;
}

static long edu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct edu_file *efile = file->private_data;
	struct edu_dev *edev;
	struct edu_factorial_req req;
	void __user *argp = (void __user *)arg;
	int ret;

	if (!efile)
		return -ENODEV;

	edev = efile->edev;
	if (!edev)
		return -ENODEV;

	if (_IOC_TYPE(cmd) != EDU_IOCTL_MAGIC)
		return -ENOTTY;

	if (edu_is_disconnected(edev))
		return -ENODEV;

	switch (cmd) {
	case EDU_IOCTL_FACTORIAL:
		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;

		if (req.input > EDU_FACTORIAL_MAX_INPUT)
			return -ERANGE;

		ret = mutex_lock_interruptible(&edev->op_lock);
		if (ret)
			return ret;

		ret = edu_run_factorial_locked(edev, &req);

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
	.release = edu_release,
	.unlocked_ioctl = edu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = compat_ptr_ioctl,
#endif
	.llseek = noop_llseek,
};

static int edu_maybe_fail(struct pci_dev *pdev, int step)
{
	if (fail_step != step)
		return 0;

	dev_info(&pdev->dev, "injecting failure at step %d\n", step);
	return -EIO;
}

static void edu_disconnect_and_cleanup(struct edu_dev *edev)
{
	struct pci_dev *pdev = edev->pdev;
	unsigned long flags;

	/*
	 * Fence first. Do not wait for op_lock while holding
	 * state_lock. complete_all() cannot be lost to a later
	 * reinit: disconnected blocks new operation setup.
	 */
	spin_lock_irqsave(&edev->state_lock, flags);
	WRITE_ONCE(edev->disconnected, true);
	complete_all(&edev->factorial_done);
	spin_unlock_irqrestore(&edev->state_lock, flags);

	wake_up_all(&edev->test_wq);

	if (edev->misc_registered) {
		misc_deregister(&edev->miscdev);
		edev->misc_registered = false;
	}

	edu_debugfs_remove(edev);

	/*
	 * Drain the transaction's hardware path, not all open
	 * file descriptions and not its eventual userspace copyout.
	 */
	mutex_lock(&edev->op_lock);

	spin_lock_irqsave(&edev->state_lock, flags);
	if (edev->mmio) {
		edu_disable_factorial_irq_locked(edev);
		edu_ack_all_pending_locked(edev);
	}
	spin_unlock_irqrestore(&edev->state_lock, flags);

	/*
	 * Never hold state_lock here: an in-flight handler may
	 * need it before free_irq() can finish.
	 */
	if (edev->irq_requested) {
		free_irq(edev->irq, edev);
		edev->irq_requested = false;
	}

	if (edev->mmio) {
		pci_iounmap(pdev, edev->mmio);
		edev->mmio = NULL;
	}

	if (edev->bar_requested) {
		pci_release_region(pdev, EDU_BAR);
		edev->bar_requested = false;
	}

	if (edev->pci_enabled) {
		pci_disable_device(pdev);
		edev->pci_enabled = false;
	}

	mutex_unlock(&edev->op_lock);

	pci_set_drvdata(pdev, NULL);

	pr_info("edu: hardware detached for %s\n", edev->bdf);

	/*
	 * Last use of edev in this function.
	 */
	kref_put(&edev->refcount, edu_dev_release);
}

static int edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	int ret;
	resource_size_t bar_len;
	unsigned long bar_flags;
	u32 ident;
	u32 status;
	struct edu_dev *edev;
	unsigned long flags;

	dev_info(&pdev->dev, "probing EDU at %s\n", pci_name(pdev));

	edev = kzalloc(sizeof(*edev), GFP_KERNEL);
	if (!edev)
		return -ENOMEM;

	edev->pdev = pdev;
	strscpy(edev->bdf, pci_name(pdev), sizeof(edev->bdf));

	kref_init(&edev->refcount);
	mutex_init(&edev->op_lock);
	spin_lock_init(&edev->state_lock);
	init_completion(&edev->factorial_done);
	init_waitqueue_head(&edev->test_wq);
	atomic_set(&edev->irq_count, 0);
	edev->pci_enabled    = false;
	edev->bar_requested   = false;
	edev->irq_requested   = false;
	edev->misc_registered = false;
	pci_set_drvdata(pdev, edev);

	bar_len = pci_resource_len(pdev, EDU_BAR);
	bar_flags = pci_resource_flags(pdev, EDU_BAR);

	if (!(bar_flags & IORESOURCE_MEM)) {
		dev_err(&pdev->dev, "BAR%d is not MMIO\n", EDU_BAR);
		ret = -ENODEV;
		goto err_cleanup;
	}

	if (bar_len < EDU_BAR_MIN_SIZE) {
		dev_err(&pdev->dev,
			"BAR%d too small: %#llx bytes\n",
			EDU_BAR, (unsigned long long)bar_len);
		ret = -ENODEV;
		goto err_cleanup;
	}

	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device failed: %d\n", ret);
		goto err_cleanup;
	}
	edev->pci_enabled = true;

	ret = edu_maybe_fail(pdev, 1);
	if (ret)
		goto err_cleanup;

	ret = pci_request_region(pdev, EDU_BAR, EDU_DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR%d: %d\n", EDU_BAR,
			ret);
		goto err_cleanup;
	}
	edev->bar_requested = true;

	ret = edu_maybe_fail(pdev, 2);
	if (ret)
		goto err_cleanup;

	edev->mmio = pci_iomap(pdev, EDU_BAR, 0);
	if (!edev->mmio) {
		dev_err(&pdev->dev, "failed to map BAR%d\n", EDU_BAR);
		ret = -ENOMEM;
		goto err_cleanup;
	}

	ret = edu_maybe_fail(pdev, 3);
	if (ret)
		goto err_cleanup;

	ident = ioread32(edev->mmio + EDU_REG_IDENT);
	dev_info(&pdev->dev, "EDU identification register: %#010x\n", ident);

	/*
	* No IRQ handler or userspace entry exists yet.
	* Disable and clear stale interrupt state before request_irq.
	*/
	spin_lock_irqsave(&edev->state_lock, flags);
	edu_disable_factorial_irq_locked(edev);
	edu_ack_all_pending_locked(edev);
	spin_unlock_irqrestore(&edev->state_lock, flags);

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	if (status & EDU_STATUS_BUSY) {
		dev_err(&pdev->dev,
			"factorial engine is still busy; retry bind later\n");
		ret = -EBUSY;
		goto err_cleanup;
	}

	ret = edu_maybe_fail(pdev, 4);
	if (ret)
		goto err_cleanup;

	edev->irq = pdev->irq;
	if (edev->irq <= 0) {
		dev_err(&pdev->dev, "no legacy IRQ assigned\n");
		ret = -ENXIO;
		goto err_cleanup;
	}

	ret = request_irq(edev->irq, edu_irq_handler, IRQF_SHARED,
			  EDU_DRIVER_NAME, edev);

	if (ret) {
		dev_err(&pdev->dev, "request_irq(%d) failed: %d\n", edev->irq,
			ret);
		goto err_cleanup;
	}
	edev->irq_requested = true;

	ret = edu_maybe_fail(pdev, 5);
	if (ret)
		goto err_cleanup;

	edu_debugfs_init(edev);

	ret = edu_maybe_fail(pdev, 6);
	if (ret)
		goto err_cleanup;

	edev->miscdev.minor = MISC_DYNAMIC_MINOR;
	edev->miscdev.name = EDU_MISCDEV_NAME;
	edev->miscdev.fops = &edu_fops;
	edev->miscdev.parent = &pdev->dev;

	ret = misc_register(&edev->miscdev);
	if (ret)
		goto err_cleanup;
	edev->misc_registered = true;

	if (test_publish_pause_ms) {
		dev_info(&pdev->dev,
			"test point: misc published, pausing for %u ms\n",
			test_publish_pause_ms);
		msleep(test_publish_pause_ms);
	}

	ret = edu_maybe_fail(pdev, 7);
	if (ret)
		goto err_cleanup;

	dev_info(&pdev->dev, "userspace device /dev/%s ready\n",
		 edev->miscdev.name);

	return 0;

err_cleanup:
	edu_disconnect_and_cleanup(edev);
	return ret;
}

static void edu_remove(struct pci_dev *pdev)
{
	struct edu_dev *edev = pci_get_drvdata(pdev);
	edu_disconnect_and_cleanup(edev);
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

    if (fail_step < 0 || fail_step > 7 ||
        test_hold_ms > 10000 ||
        test_publish_pause_ms > 10000)
        return -EINVAL;

    edu_debugfs_root = debugfs_create_dir(EDU_NAME, NULL);
    if (IS_ERR_OR_NULL(edu_debugfs_root))
        edu_debugfs_root = NULL;

    ret = pci_register_driver(&edu_driver);
    if (ret) {
        debugfs_remove(edu_debugfs_root);
        edu_debugfs_root = NULL;
        return ret;
    }

    return 0;
}

static void __exit edu_exit(void)
{
    pci_unregister_driver(&edu_driver);
    debugfs_remove(edu_debugfs_root);
    edu_debugfs_root = NULL;
}

module_init(edu_init);
module_exit(edu_exit);

MODULE_AUTHOR("Ivan Sharavuev<shriwand@gmail.com>");
MODULE_DESCRIPTION("QEMU edu pic driver");
MODULE_LICENSE("GPL");
