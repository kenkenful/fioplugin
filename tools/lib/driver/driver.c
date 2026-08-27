#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "common.h"

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic PCIe DMA Memory Driver");

#define DEVICE_NAME "genpci"
#define genpci_MINORS (1U << MINORBITS)

static DEFINE_IDA(genpci_instance_ida);

static struct class *genpci_class;
static dev_t genpci_chr_devt;

struct dma_entry {
	struct list_head list;
	struct genpci_dev *pgenpci_dev;
	struct device *dev;
	void *virt_addr;
	dma_addr_t dma_addr;
	long size;
	refcount_t vma_refs;
	bool release_requested;
	bool listed;
};

struct genpci_dev {
	struct pci_dev *pdev;
	struct cdev cdev;
	dev_t devt;
	int instance;
	bool is_open;
	struct mutex lock;
	struct list_head memlist;
};

static const struct pci_device_id genpci_ids[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_STORAGE_EXPRESS, 0xffffff) },
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, genpci_ids);

static void genpci_detach_dma_entries(struct genpci_dev *pgenpci_dev)
{
	struct dma_entry *entry;
	struct dma_entry *tmp;

	mutex_lock(&pgenpci_dev->lock);
	list_for_each_entry_safe(entry, tmp, &pgenpci_dev->memlist, list) {
		list_del(&entry->list);
		entry->listed = false;
		entry->pgenpci_dev = NULL;
		entry->release_requested = true;
	}
	mutex_unlock(&pgenpci_dev->lock);
}

static void dma_vma_open(struct vm_area_struct *vma)
{
	struct dma_entry *entry = vma->vm_private_data;

	refcount_inc(&entry->vma_refs);
}

static void dma_vma_close(struct vm_area_struct *vma)
{
	struct dma_entry *entry = vma->vm_private_data;
	struct genpci_dev *pgenpci_dev = entry->pgenpci_dev;
	bool do_free = false;

	if (pgenpci_dev)
		mutex_lock(&pgenpci_dev->lock);

	if (refcount_dec_and_test(&entry->vma_refs))
		do_free = true;

	if (do_free && entry->listed) {
		list_del(&entry->list);
		entry->listed = false;
	}

	if (pgenpci_dev)
		mutex_unlock(&pgenpci_dev->lock);

	if (do_free) {
		dma_free_coherent(entry->dev, entry->size,
				  entry->virt_addr, entry->dma_addr);
		put_device(entry->dev);
		kfree(entry);
	}
}

static const struct vm_operations_struct dma_vm_ops = {
	.open = dma_vma_open,
	.close = dma_vma_close,
};

static int genpci_open(struct inode *inode, struct file *filp)
{
	struct genpci_dev *pgenpci_dev;

	pgenpci_dev = container_of(inode->i_cdev, struct genpci_dev, cdev);
	filp->private_data = pgenpci_dev;
	pgenpci_dev->is_open = true;

	return 0;
}

static int genpci_close(struct inode *inode, struct file *filp)
{
	struct genpci_dev *pgenpci_dev = filp->private_data;

	pgenpci_dev->is_open = false;

	return 0;
}

static int mmap_dma(struct file *filp, struct vm_area_struct *vma)
{
	struct genpci_dev *pgenpci_dev = filp->private_data;
	struct dma_entry *entry;
	dma_addr_t dma_addr;
	void *virt_addr;
	long size;
	int ret;

	size = vma->vm_end - vma->vm_start;
	size = PAGE_ALIGN(size);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	virt_addr = dma_alloc_coherent(&pgenpci_dev->pdev->dev, size,
				       &dma_addr, GFP_KERNEL);
	if (!virt_addr) {
		kfree(entry);
		return -ENOMEM;
	}

	if (vma->vm_pgoff == 0) {
		ret = dma_mmap_coherent(&pgenpci_dev->pdev->dev, vma,
					virt_addr, dma_addr, size);
	} else {
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vm_flags_set(vma, VM_IO);

		ret = remap_pfn_range(vma, vma->vm_start,
				      PFN_DOWN(dma_addr) + vma->vm_pgoff,
				      size, vma->vm_page_prot);
	}

	if (ret) {
		dma_free_coherent(&pgenpci_dev->pdev->dev, size,
				  virt_addr, dma_addr);
		kfree(entry);
		return ret;
	}

	entry->pgenpci_dev = pgenpci_dev;
	entry->dev = get_device(&pgenpci_dev->pdev->dev);
	entry->size = size;
	entry->dma_addr = dma_addr;
	entry->virt_addr = virt_addr;
	refcount_set(&entry->vma_refs, 1);
	vma->vm_private_data = entry;
	vma->vm_ops = &dma_vm_ops;

	mutex_lock(&pgenpci_dev->lock);
	list_add(&entry->list, &pgenpci_dev->memlist);
	entry->listed = true;
	mutex_unlock(&pgenpci_dev->lock);

	return 0;
}

static long genpci_ioctl(struct file *filp, unsigned int ioctlnum,
			 unsigned long ioctlparam)
{
	struct genpci_dev *pgenpci_dev = filp->private_data;
	struct mem mem = { 0 };
	struct bdf bdf = { 0 };
	struct dma_entry *entry;
	struct dma_entry *tmp;

	switch (ioctlnum) {
	case IOCTL_GET_MEMFINFO:
		mutex_lock(&pgenpci_dev->lock);
		list_for_each_entry(entry, &pgenpci_dev->memlist, list) {
			if (!entry->release_requested)
				break;
		}

		if (&entry->list == &pgenpci_dev->memlist) {
			mutex_unlock(&pgenpci_dev->lock);
			return -ENOENT;
		}

		mem.dma_addr = entry->dma_addr;
		mem.kernel_virtaddr = (uint64_t)entry->virt_addr;
		mem.size = entry->size;
		mutex_unlock(&pgenpci_dev->lock);

		if (copy_to_user((void __user *)ioctlparam, &mem, sizeof(mem)))
			return -EFAULT;

		return 0;

	case IOCTL_GET_BDF:
		bdf.domain = pci_domain_nr(pgenpci_dev->pdev->bus);
		bdf.bus = pgenpci_dev->pdev->bus->number;
		bdf.dev = PCI_SLOT(pgenpci_dev->pdev->devfn);
		bdf.func = PCI_FUNC(pgenpci_dev->pdev->devfn);

		if (copy_to_user((void __user *)ioctlparam, &bdf, sizeof(bdf)))
			return -EFAULT;

		return 0;

	case IOCTL_RELEASE_MEM:
		if (copy_from_user(&mem, (void __user *)ioctlparam,
				   sizeof(mem)))
			return -EFAULT;

		mutex_lock(&pgenpci_dev->lock);
		list_for_each_entry_safe(entry, tmp, &pgenpci_dev->memlist, list) {
			if ((uint64_t)entry->virt_addr != mem.kernel_virtaddr ||
			    entry->dma_addr != mem.dma_addr)
				continue;

			entry->release_requested = true;
			mutex_unlock(&pgenpci_dev->lock);
			return 0;
		}
		mutex_unlock(&pgenpci_dev->lock);

		return -ENOENT;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations genpci_chr_fops = {
	.owner = THIS_MODULE,
	.open = genpci_open,
	.release = genpci_close,
	.unlocked_ioctl = genpci_ioctl,
	.mmap = mmap_dma,
};

static int genpci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct genpci_dev *pgenpci_dev;
	int ret;

	pr_info("%s: probe %s vendor=0x%04x device=0x%04x class=0x%06x\n",
		DEVICE_NAME, pci_name(pdev), pdev->vendor, pdev->device,
		pdev->class);

	pgenpci_dev = kzalloc(sizeof(*pgenpci_dev), GFP_KERNEL);
	if (!pgenpci_dev)
		return -ENOMEM;

	ret = ida_simple_get(&genpci_instance_ida, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;

	pgenpci_dev->instance = ret;
	pgenpci_dev->devt = MKDEV(MAJOR(genpci_chr_devt),
				  pgenpci_dev->instance);

	cdev_init(&pgenpci_dev->cdev, &genpci_chr_fops);
	pgenpci_dev->cdev.owner = THIS_MODULE;
	mutex_init(&pgenpci_dev->lock);
	INIT_LIST_HEAD(&pgenpci_dev->memlist);

	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto out_remove_ida;

	pci_set_master(pdev);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		pr_err("Error dma_set_mask_and_coherent\n");
		goto out_disable_device;
	}

	ret = pci_request_regions(pdev, DEVICE_NAME);
	if (ret)
		goto out_disable_device;

	pgenpci_dev->pdev = pci_dev_get(pdev);
	pci_set_drvdata(pdev, pgenpci_dev);

	ret = cdev_add(&pgenpci_dev->cdev, pgenpci_dev->devt,
		       genpci_MINORS);
	if (ret)
		goto out_clear_drvdata;

	if (IS_ERR(device_create(genpci_class, NULL, pgenpci_dev->devt,
				 NULL, "genpci%d", pgenpci_dev->instance))) {
		ret = -ENODEV;
		goto out_del_cdev;
	}

	pr_info("%s: created /dev/genpci%d\n", DEVICE_NAME,
		pgenpci_dev->instance);

	return 0;

out_del_cdev:
	cdev_del(&pgenpci_dev->cdev);

out_clear_drvdata:
	pci_set_drvdata(pdev, NULL);
	pci_release_regions(pdev);
	pci_dev_put(pgenpci_dev->pdev);

out_disable_device:
	pci_disable_device(pdev);

out_remove_ida:
	ida_simple_remove(&genpci_instance_ida, pgenpci_dev->instance);

out_free_dev:
	kfree(pgenpci_dev);

	return ret;
}

static void genpci_remove(struct pci_dev *pdev)
{
	struct genpci_dev *pgenpci_dev = pci_get_drvdata(pdev);

	pr_info("%s: remove %s\n", DEVICE_NAME, pci_name(pdev));

	genpci_detach_dma_entries(pgenpci_dev);
	pci_set_drvdata(pdev, NULL);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_dev_put(pgenpci_dev->pdev);

	device_destroy(genpci_class, pgenpci_dev->devt);
	cdev_del(&pgenpci_dev->cdev);
	ida_simple_remove(&genpci_instance_ida, pgenpci_dev->instance);
	kfree(pgenpci_dev);
}

static struct pci_driver genpci_driver = {
	.name = DEVICE_NAME,
	.id_table = genpci_ids,
	.probe = genpci_probe,
	.remove = genpci_remove,
};

static int genpci_init(void)
{
	int ret;

	pr_info("%s: module loading\n", DEVICE_NAME);

	ida_init(&genpci_instance_ida);

	ret = alloc_chrdev_region(&genpci_chr_devt, 0, genpci_MINORS,
				  DEVICE_NAME);
	if (ret) {
		ida_destroy(&genpci_instance_ida);
		return ret;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	genpci_class = class_create(DEVICE_NAME);
#else
	genpci_class = class_create(THIS_MODULE, DEVICE_NAME);
#endif
	if (IS_ERR(genpci_class)) {
		ret = PTR_ERR(genpci_class);
		unregister_chrdev_region(genpci_chr_devt, genpci_MINORS);
		ida_destroy(&genpci_instance_ida);
		return ret;
	}

	ret = pci_register_driver(&genpci_driver);
	if (ret) {
		class_destroy(genpci_class);
		unregister_chrdev_region(genpci_chr_devt, genpci_MINORS);
		ida_destroy(&genpci_instance_ida);
		return ret;
	}

	return 0;
}

static void genpci_exit(void)
{
	pr_info("%s: module unloading\n", DEVICE_NAME);

	pci_unregister_driver(&genpci_driver);
	class_destroy(genpci_class);
	unregister_chrdev_region(genpci_chr_devt, genpci_MINORS);
	ida_destroy(&genpci_instance_ida);
}

module_init(genpci_init);
module_exit(genpci_exit);
