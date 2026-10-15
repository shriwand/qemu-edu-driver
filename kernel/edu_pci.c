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
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/lockdep.h>
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
#define EDU_REG_IDENT 0x0
#define EDU_NAME "edu"
#define EDU_DRIVER_NAME EDU_NAME "_pci"
#define EDU_MISCDEV_NAME EDU_NAME "0"
#define EDU_IRQ_KNOWN (EDU_IRQ_FACTORIAL | EDU_IRQ_DMA)

#define EDU_REG_FACTORIAL         0x08
#define EDU_REG_STATUS            0x20
#define EDU_REG_IRQ_STATUS        0x24
#define EDU_REG_IRQ_ACK           0x64

#define EDU_STATUS_BUSY           0x01
#define EDU_STATUS_INT_ENABLE     0x80
#define EDU_IRQ_FACTORIAL         0x01

#define EDU_FACTORIAL_TIMEOUT_MS  1000

#define EDU_REG_DMA_SRC       0x80
#define EDU_REG_DMA_DST       0x88
#define EDU_REG_DMA_COUNT     0x90
#define EDU_REG_DMA_CMD       0x98

#define EDU_DMA_START         BIT_ULL(0)
#define EDU_DMA_FROM_DEVICE   BIT_ULL(1)
#define EDU_DMA_IRQ_ENABLE    BIT_ULL(2)

#define EDU_IRQ_DMA           BIT(8)

#define EDU_DMA_INTERNAL_BASE 0x40000ULL
#define EDU_DMA_BUFFER_SIZE   4096U
#define EDU_DMA_ALLOC_SIZE    (2U * EDU_DMA_BUFFER_SIZE)

#define EDU_DMA_TIMEOUT_MS    1000U
#define EDU_DMA_RX_GUARD      0xa5

#define EDU_BAR_MIN_SIZE			\
	(EDU_REG_DMA_CMD + sizeof(u64))

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

	void *dma_cpu;
	dma_addr_t dma_handle;
	size_t dma_size;

	struct completion dma_done;
	bool dma_active;
	bool dma_faulted;

	struct miscdevice miscdev;
	struct dentry *debugfs_dir;

	/* ownship flags for generic cleanup */
	bool pci_enabled;
	bool bar_requested;
	bool irq_requested;
	bool misc_registered;
	bool msi_enabled;
	bool irq_vectors_allocated;

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
	"5=IRQ, 6=debugfs, 7=misc publication, 8=IRQ vector allocation, 9=before request_irq, "
	"10=after coherent DMA allocation");

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

static bool test_force_dma_timeout;
module_param(test_force_dma_timeout, bool, 0444);
MODULE_PARM_DESC(test_force_dma_timeout,
	"Test-only: ACK DMA IRQ but suppress DMA completion");

static unsigned int test_dma_hold_ms;
module_param(test_dma_hold_ms, uint, 0444);
MODULE_PARM_DESC(test_dma_hold_ms,
	"Test-only cancellable hold after DMA start, max 10000 ms");

static int test_stream_fail_step;
module_param(test_stream_fail_step, int, 0444);
MODULE_PARM_DESC(test_stream_fail_step,
	"Streaming test failure: 1=after TX map, 2=before RX map, 3=after RX map");

static struct dentry *edu_debugfs_root;

static int edu_irq_snapshot_open(struct inode *inode,
	struct file *file)
{
	struct edu_dev *edev = inode->i_private;
	struct edu_irq_snapshot *snapshot;
	int count;

	snapshot = kzalloc_obj(*snapshot, GFP_KERNEL);
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
	bool notify_factorial = false;
	bool notify_dma = false;

	spin_lock_irqsave(&edev->state_lock, flags);

	status = ioread32(edev->mmio + EDU_REG_IRQ_STATUS);

	if (!status) {
		spin_unlock_irqrestore(&edev->state_lock, flags);
		return IRQ_NONE;
	}

	/*
	 * irq_count now counts handled device IRQ snapshots,
	 * not factorial completions alone.
	 */
	atomic_inc(&edev->irq_count);

	if (status & ~EDU_IRQ_KNOWN) {
		pr_warn_ratelimited(
			"edu: %s unsupported IRQ causes=%#x\n",
			edev->bdf, status);
		edev->faulted = true;
		edev->dma_faulted = true;
		notify_factorial = edev->op_active;
		notify_dma = edev->dma_active;
	}

	if (status & EDU_IRQ_FACTORIAL) {
		if (edev->op_active && !edev->suppress_completion)
			notify_factorial = true;
	}

	if (status & EDU_IRQ_DMA && !test_force_dma_timeout) {
		if (edev->dma_active)
			notify_dma = true;
	}

	iowrite32(status, edev->mmio + EDU_REG_IRQ_ACK);

	if (notify_factorial)
		complete(&edev->factorial_done);

	if (notify_dma)
		complete(&edev->dma_done);

	spin_unlock_irqrestore(&edev->state_lock, flags);

	return IRQ_HANDLED;
}

static int edu_dma_transfer_locked(struct edu_dev *edev, u64 source,
	u64 destination, size_t length, bool from_device)
{
	unsigned long flags;
	unsigned long completed;
	u64 command;
	int ret;

	lockdep_assert_held(&edev->op_lock);

	if (!length || length > EDU_DMA_BUFFER_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (edev->dma_faulted) {
		ret = -EIO;
		goto out_unlock;
	}

	command = readq(edev->mmio + EDU_REG_DMA_CMD);
	if (command & EDU_DMA_START) {
		edev->dma_faulted = true;
		ret = -EIO;
		goto out_unlock;
	}

	/*
	 * Clear only the DMA cause. Factorial cause belongs
	 * to the existing factorial protocol.
	 */
	iowrite32(EDU_IRQ_DMA,
		edev->mmio + EDU_REG_IRQ_ACK);

	reinit_completion(&edev->dma_done);
	edev->dma_active = true;

	/*
	 * Publish prior coherent-buffer stores before starting
	 * the transaction. Ordered MMIO accessors are used below.
	 */
	dma_wmb();

	writeq(source, edev->mmio + EDU_REG_DMA_SRC);
	writeq(destination, edev->mmio + EDU_REG_DMA_DST);
	writeq(length, edev->mmio + EDU_REG_DMA_COUNT);

	command = EDU_DMA_START | EDU_DMA_IRQ_ENABLE;
	if (from_device)
		command |= EDU_DMA_FROM_DEVICE;

	writeq(command, edev->mmio + EDU_REG_DMA_CMD);
	readq(edev->mmio + EDU_REG_DMA_CMD);

	spin_unlock_irqrestore(&edev->state_lock, flags);

	if (test_dma_hold_ms) {
		pr_info("edu: %s DMA test point: transfer started, dir=%s\n",
			edev->bdf,
			from_device ? "EDU->RAM" : "RAM->EDU");

		wait_event_timeout(
			edev->test_wq,
			READ_ONCE(edev->disconnected),
			msecs_to_jiffies(test_dma_hold_ms));
	}

	completed = wait_for_completion_timeout(
		&edev->dma_done,
		msecs_to_jiffies(EDU_DMA_TIMEOUT_MS));

	spin_lock_irqsave(&edev->state_lock, flags);

	/*
	 * Before any post-wait MMIO access.
	 */
	if (edev->disconnected) {
		ret = -ENODEV;
		goto out_finish;
	}

	if (!completed) {
		edev->dma_faulted = true;
		ret = -ETIMEDOUT;
		goto out_finish;
	}

	if (edev->dma_faulted) {
		ret = -EIO;
		goto out_finish;
	}

	command = readq(edev->mmio + EDU_REG_DMA_CMD);
	if (command & EDU_DMA_START) {
		edev->dma_faulted = true;
		ret = -EIO;
		goto out_finish;
	}

	ret = 0;

out_finish:
	edev->dma_active = false;

out_unlock:
	spin_unlock_irqrestore(&edev->state_lock, flags);
	return ret;
}

static int edu_dma_selftest_locked(struct edu_dev *edev, size_t length)
{
	unsigned long flags;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
	u8 *tx;
	u8 *rx;
	size_t i;
	int ret = 0;

	lockdep_assert_held(&edev->op_lock);

	if (!length || length > EDU_DMA_BUFFER_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected)
		ret = -ENODEV;
	else if (edev->dma_faulted)
		ret = -EIO;
	else if (!edev->dma_cpu)
		ret = -ENODEV;
	else
		ret = 0;

	spin_unlock_irqrestore(&edev->state_lock, flags);

	if (ret)
		return ret;

	/*
	 * op_lock prevents cleanup from freeing this allocation
	 * while CPU prepares or verifies it.
	 */
	tx = edev->dma_cpu;
	rx = tx + EDU_DMA_BUFFER_SIZE;

	tx_dma = edev->dma_handle;
	rx_dma = edev->dma_handle + EDU_DMA_BUFFER_SIZE;

	for (i = 0; i < EDU_DMA_BUFFER_SIZE; i++)
		tx[i] = (u8)(i * 37U + 11U);

	memset(rx, EDU_DMA_RX_GUARD, EDU_DMA_BUFFER_SIZE);

	/*
	 * Every requested byte begins with a value different
	 * from its expected result.
	 */
	for (i = 0; i < length; i++)
		rx[i] = tx[i] ^ 0xff;

	ret = edu_dma_transfer_locked(
		edev,
		(u64)tx_dma,
		EDU_DMA_INTERNAL_BASE,
		length,
		false);
	if (ret)
		return ret;

	/*
	 * The transfer helper rechecks disconnect before starting
	 * this second transaction.
	 */
	ret = edu_dma_transfer_locked(
		edev,
		EDU_DMA_INTERNAL_BASE,
		(u64)rx_dma,
		length,
		true);
	if (ret)
		return ret;

	dma_rmb();

	for (i = 0; i < length; i++) {
		if (rx[i] != tx[i]) {
			pr_err("edu: %s DMA mismatch at %zu: expected=%#x actual=%#x\n",
				edev->bdf, i,
				(unsigned int)tx[i],
				(unsigned int)rx[i]);
			goto out_fault;
		}
	}

	for (i = length; i < EDU_DMA_BUFFER_SIZE; i++) {
		if (rx[i] != EDU_DMA_RX_GUARD) {
			pr_err("edu: %s DMA RX tail modified at %zu: %#x\n",
				edev->bdf, i,
				(unsigned int)rx[i]);
			goto out_fault;
		}
	}

	return 0;

out_fault:
	spin_lock_irqsave(&edev->state_lock, flags);
	edev->dma_faulted = true;
	spin_unlock_irqrestore(&edev->state_lock, flags);

	return -EIO;
}

static void edu_dma_free(struct edu_dev *edev);
static void edu_dma_drain_locked(struct edu_dev *edev);

static int edu_dma_stream_selftest_locked(struct edu_dev *edev, size_t length)
{
	struct device *dev = &edev->pdev->dev;
	const size_t map_size = EDU_DMA_BUFFER_SIZE;
	unsigned long flags;
	dma_addr_t tx_dma = 0;
	dma_addr_t rx_dma = 0;
	bool tx_mapped = false;
	bool rx_mapped = false;
	u8 *tx = NULL;
	u8 *rx = NULL;
	size_t i;
	int ret = 0;

	lockdep_assert_held(&edev->op_lock);

	if (!length || length > EDU_DMA_BUFFER_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected)
		ret = -ENODEV;
	else if (edev->dma_faulted)
		ret = -EIO;
	else
		ret = 0;

	spin_unlock_irqrestore(&edev->state_lock, flags);

	if (ret)
		return ret;

	tx = kmalloc(map_size, GFP_KERNEL);
	rx = kmalloc(map_size, GFP_KERNEL);

	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out_cleanup;
	}

	for (i = 0; i < map_size; i++)
		tx[i] = (u8)(i * 37U + 11U);

	/*
	 * Initialize memory before mapping.
	 * Do not access mapped RX payload until unmap.
	 */
	memset(rx, 0, map_size);

	tx_dma = dma_map_single(dev, tx, map_size,
		DMA_TO_DEVICE);
	if (dma_mapping_error(dev, tx_dma)) {
		ret = -ENOMEM;
		goto out_cleanup;
	}

	tx_mapped = true;

	if (test_stream_fail_step == 1) {
		ret = -EIO;
		goto out_cleanup;
	}

	ret = edu_dma_transfer_locked(
		edev,
		(u64)tx_dma,
		EDU_DMA_INTERNAL_BASE,
		length,
		false);

	/*
	 * Required even after timeout or disconnect.
	 * Unmap does not stop the DMA engine.
	 */
	edu_dma_drain_locked(edev);

	dma_unmap_single(dev, tx_dma, map_size,
		DMA_TO_DEVICE);
	tx_mapped = false;

	if (ret)
		goto out_cleanup;

	if (test_stream_fail_step == 2) {
		ret = -ENOMEM;
		goto out_cleanup;
	}

	rx_dma = dma_map_single(dev, rx, map_size,
		DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, rx_dma)) {
		ret = -ENOMEM;
		goto out_cleanup;
	}

	rx_mapped = true;

	if (test_stream_fail_step == 3) {
		ret = -EIO;
		goto out_cleanup;
	}

	ret = edu_dma_transfer_locked(
		edev,
		EDU_DMA_INTERNAL_BASE,
		(u64)rx_dma,
		length,
		true);

	edu_dma_drain_locked(edev);

	dma_unmap_single(dev, rx_dma, map_size,
		DMA_FROM_DEVICE);
	rx_mapped = false;

	if (ret)
		goto out_cleanup;

	/*
	 * Both mappings are gone. CPU owns both buffers.
	 */
	for (i = 0; i < length; i++) {
		if (rx[i] != tx[i]) {
			pr_err("edu: %s streaming mismatch at %zu: expected=%#x actual=%#x\n",
				edev->bdf, i,
				(unsigned int)tx[i],
				(unsigned int)rx[i]);

			spin_lock_irqsave(&edev->state_lock, flags);
			edev->dma_faulted = true;
			spin_unlock_irqrestore(&edev->state_lock, flags);

			ret = -EIO;
			goto out_cleanup;
		}
	}

	ret = 0;

out_cleanup:
	/*
	 * Defensive cleanup for future error-injection branches.
	 * Keep mapping flags accurate.
	 */
	if (tx_mapped || rx_mapped)
		edu_dma_drain_locked(edev);

	if (rx_mapped)
		dma_unmap_single(dev, rx_dma, map_size,
			DMA_FROM_DEVICE);

	if (tx_mapped)
		dma_unmap_single(dev, tx_dma, map_size,
			DMA_TO_DEVICE);

	kfree(rx);
	kfree(tx);

	return ret;
}

static int edu_run_factorial_locked(struct edu_dev *edev, struct edu_factorial_req *req)
{
	u32 status;
	unsigned long timeout;
	unsigned long flags;
	int ret;

	lockdep_assert_held(&edev->op_lock);

	spin_lock_irqsave(&edev->state_lock, flags);

	if (edev->disconnected) {
		ret = -ENODEV;
		goto out_lock;

	}

	if (edev->faulted) {
		pr_warn_ratelimited(
			"edu: %s factorial rejected: engine already faulted\n",
			edev->bdf);
		ret = -EIO;
		goto out_lock;
	}

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	if (status & EDU_STATUS_BUSY) {
		pr_warn_ratelimited(
			"edu: %s factorial rejected: busy before start, status=%#x\n",
			edev->bdf, status);
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
		pr_warn_ratelimited(
			"edu: %s factorial failed: fault latched while waiting\n",
			edev->bdf);
		edu_disable_factorial_irq_locked(edev);
		edu_ack_all_pending_locked(edev);
		ret = -EIO;
		goto out_finish;
	}

	status = ioread32(edev->mmio + EDU_REG_STATUS);
	if (status & EDU_STATUS_BUSY) {
		pr_warn_ratelimited(
			"edu: %s factorial failed: busy after completion, status=%#x\n",
			edev->bdf, status);
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

	efile = kzalloc_obj(*efile, GFP_KERNEL);
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
	case EDU_IOCTL_FACTORIAL: {
		struct edu_factorial_req req = {0};

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;

		if (req.input > EDU_FACTORIAL_MAX_INPUT)
			return -ERANGE;


		if (req.reserved[0] || req.reserved[1] || req.reserved[2]
			|| req.reserved[3] || req.reserved[4] || req.reserved[5])
			return -EINVAL;

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
	}

	case EDU_IOCTL_DMA_SELFTEST: {
		struct edu_dma_selftest_req dma_req = {0};

		if (copy_from_user(&dma_req, argp, sizeof(dma_req)))
			return -EFAULT;

		if (!dma_req.length ||
			dma_req.length > EDU_DMA_MAX_LENGTH)
			return -EINVAL;

		if (dma_req.reserved[0] ||
			dma_req.reserved[1] ||
			dma_req.reserved[2])
			return -EINVAL;

		ret = mutex_lock_interruptible(&edev->op_lock);
		if (ret)
			return ret;

		ret = edu_dma_selftest_locked(edev, dma_req.length);

		mutex_unlock(&edev->op_lock);

		return ret;
	}

	case EDU_IOCTL_DMA_STREAM_SELFTEST: {
		struct edu_dma_selftest_req stream_req = {0};

		if (copy_from_user(&stream_req, argp, sizeof(stream_req)))
			return -EFAULT;

		if (!stream_req.length ||
			stream_req.length > EDU_DMA_MAX_LENGTH)
			return -EINVAL;

		if (stream_req.reserved[0] ||
			stream_req.reserved[1] ||
			stream_req.reserved[2])
			return -EINVAL;
		ret = mutex_lock_interruptible(&edev->op_lock);
		if (ret)
			return ret;

		ret = edu_dma_stream_selftest_locked(
			edev, stream_req.length);

		mutex_unlock(&edev->op_lock);

		return ret;
	}

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

static int edu_setup_irqs(struct edu_dev *edev)
{
	struct pci_dev *pdev = edev->pdev;
	unsigned long flags;
	int ret = 0;

	ret = pci_alloc_irq_vectors(pdev, 1, 1,
		PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (ret < 0) {
		dev_err(&pdev->dev, "failed to allocate irq vector: %d\n", ret);
		return ret;
	}

	edev->irq_vectors_allocated = true;

	ret = edu_maybe_fail(pdev, 8);
	if (ret)
		return ret;

	ret = pci_irq_vector(pdev, 0);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"failed to get IRQ vector 0: %d\n", ret);
		return ret;
	}

	edev->irq = ret;

	/*
	 * MSI-X is not requested by this driver.
	 * Therefore true means MSI in this setup.
	 */
	edev->msi_enabled = pci_dev_msi_enabled(pdev);

	flags = edev->msi_enabled ? 0 : IRQF_SHARED;

	ret = edu_maybe_fail(pdev, 9);
	if (ret)
		return ret;

	ret = request_irq(edev->irq, edu_irq_handler,
		flags, EDU_DRIVER_NAME, edev);
	if (ret) {
		dev_err(&pdev->dev,
			"request_irq(%d, mode=%s) failed: %d\n",
			edev->irq,
			edev->msi_enabled ? "MSI" : "INTx",
			ret);
		return ret;
	}

	edev->irq_requested = true;

	dev_info(&pdev->dev,
		"IRQ setup: mode=%s, irq=%d, vectors=1\n",
		edev->msi_enabled ? "MSI" : "INTx",
		edev->irq);

	return ret;
}

static void edu_teardown_irqs(struct edu_dev *edev)
{
	if (edev->irq_requested) {
		free_irq(edev->irq, edev);
		edev->irq_requested = false;
	}
	if (edev->irq_vectors_allocated) {
		pci_free_irq_vectors(edev->pdev);
		edev->irq_vectors_allocated = false;
	}

	edev->irq = -1;
	edev->msi_enabled = false;
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
	complete_all(&edev->dma_done);
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


	edu_dma_drain_locked(edev);

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
	edu_teardown_irqs(edev);

	edu_dma_free(edev);

	if (edev->mmio) {
		pci_iounmap(pdev, edev->mmio);
		edev->mmio = NULL;
	}

	if (edev->bar_requested) {
		pci_release_region(pdev, EDU_BAR);
		edev->bar_requested = false;
	}

	if (edev->pci_enabled) {
		pci_clear_master(pdev);
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

static int edu_dma_alloc(struct edu_dev *edev)
{
	int ret;

	ret = dma_set_mask_and_coherent(&edev->pdev->dev,
		DMA_BIT_MASK(28));
	if (ret) {
		dev_err(&edev->pdev->dev,
			"28-bit DMA mask unavailable: %d\n", ret);
		return ret;
	}

	edev->dma_size = EDU_DMA_ALLOC_SIZE;
	edev->dma_cpu = dma_alloc_coherent(
		&edev->pdev->dev,
		edev->dma_size,
		&edev->dma_handle,
		GFP_KERNEL);

	if (!edev->dma_cpu)
		return -ENOMEM;

	dev_info(&edev->pdev->dev,
		"coherent DMA allocated: size=%zu dma=%pad\n",
		edev->dma_size, &edev->dma_handle);

	return 0;
}

static void edu_dma_free(struct edu_dev *edev)
{
	if (!edev->dma_cpu)
		return;
	dma_free_coherent(&edev->pdev->dev,
		edev->dma_size,
		edev->dma_cpu,
		edev->dma_handle);

	edev->dma_cpu = NULL;
	edev->dma_size = 0;
}

/*
 * Caller holds op_lock.
 * No other driver transaction can start DMA while op_lock is held.
 * MMIO and PCI resources remain valid throughout this call.
 *
 * Used by both operation cleanup and device teardown.
 * Intentionally unbounded: EDU has no documented abort command.
 */
static void edu_dma_drain_locked(struct edu_dev *edev)
{
	unsigned long flags;
	u64 command;
	bool reported = false;

	lockdep_assert_held(&edev->op_lock);

	if (!edev->mmio)
		return;

	for (;;) {
		spin_lock_irqsave(&edev->state_lock, flags);
		command = readq(edev->mmio + EDU_REG_DMA_CMD);
		spin_unlock_irqrestore(&edev->state_lock, flags);

		if (!(command & EDU_DMA_START))
			break;

		if (!reported) {
			pr_info("edu: %s waiting for DMA hardware idle\n",
				edev->bdf);
			reported = true;
		}

		usleep_range(1000, 2000);
	}

	if (reported)
		pr_info("edu: %s DMA hardware is idle\n", edev->bdf);
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

	edev = kzalloc_obj(*edev, GFP_KERNEL);
	if (!edev)
		return -ENOMEM;

	edev->pdev = pdev;
	strscpy(edev->bdf, pci_name(pdev), sizeof(edev->bdf));

	kref_init(&edev->refcount);
	mutex_init(&edev->op_lock);
	spin_lock_init(&edev->state_lock);
	init_completion(&edev->factorial_done);
	init_completion(&edev->dma_done);
	init_waitqueue_head(&edev->test_wq);
	atomic_set(&edev->irq_count, 0);
	edev->pci_enabled    = false;
	edev->bar_requested   = false;
	edev->irq_requested   = false;
	edev->misc_registered = false;
	edev->msi_enabled           = false;
	edev->irq_vectors_allocated = false;
	edev->irq = -1;
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

	if (readq(edev->mmio + EDU_REG_DMA_CMD) & EDU_DMA_START) {
		dev_err(&pdev->dev,
			"DMA engine is busy during probe\n");
		ret = -EBUSY;
		goto err_cleanup;
	}

	ret = edu_dma_alloc(edev);
	if (ret)
		goto err_cleanup;

	ret = edu_maybe_fail(pdev, 10);
	if (ret)
		goto err_cleanup;

	/*
	 * MMIO is mapped, stale device interrupt state is cleared,
	 * and the factorial engine has been checked for busy.
	 *
	 * MSI delivery is a device-initiated memory write.
	 */
	pci_set_master(pdev);

	ret = edu_setup_irqs(edev);
	if (ret)
		goto err_cleanup;

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

	if (fail_step < 0 || fail_step > 10 ||
		test_hold_ms > 10000 ||
		test_publish_pause_ms > 10000 ||
		test_dma_hold_ms > 10000 ||
		test_stream_fail_step < 0 ||
		test_stream_fail_step > 3)
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
