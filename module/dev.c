#define pr_fmt(fmt)	"parasyte/dev: " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/cma.h>
#include <linux/init.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/uaccess.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <linux/psci.h>
#include <linux/libfdt.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <asm/cpu_ops.h>
#include <asm/cacheflush.h>
#include <asm/parasyte.h>

#include "dev.h"

extern phys_addr_t parasyte_hb, parasyte_fdt, parasyte_kimage, parasyte_start, parasyte_end;
extern void parasyte_trampoline(void);

struct cma *parasyte_cma;

int __init parasyte_reserve_cma(void)
{
	int ret = cma_declare_contiguous(0, 4096UL * 1024UL * 1024UL, 0, 0, 0, false, "parasyte", &parasyte_cma);
	if (ret)
		pr_err("Failed to reserve CMA area for parasyte: %d\n", ret);
	else
		pr_err("Reserved CMA area for parasyte.\n");
	return ret;
}

static struct page *parasyte_alloc_cma(size_t size, gfp_t gfp)
{
	unsigned int align;
	if (!gfpflags_allow_blocking(gfp))
		return NULL;

	align = min(get_order(size), CONFIG_CMA_ALIGNMENT);
	return cma_alloc(parasyte_cma, size >> PAGE_SHIFT, align, gfp & __GFP_NOWARN);
}

static void parasyte_free_cma(struct page *page, size_t size)
{
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	if (!cma_release(parasyte_cma, page, count))
		pr_warn("Pages not belong to parasyte_cma\n");
}

struct parasyte_mem {
	struct page* pages;
	size_t size;
	void *virt_addr;
	phys_addr_t phys_addr;
};

enum parasyte_state {
	SPORE_FREED,
	SPORE_ALLOCATED,
	SPORE_SETUP,
	SPORE_RUNNING
};

struct parasyte_spore {
	struct miscdevice device;
	/* Status */
	atomic_t opened;
	enum parasyte_state state;
	/* Resource */
	struct cpumask used_cpu;
	wait_queue_head_t client_wq;

	struct parasyte_mem fdt;
	struct parasyte_mem ram;
	struct parasyte_mem hb;

	phys_addr_t kimage_addr;
	struct parasyte_msg_queue *hive_msg_queue;
	struct parasyte_msg_queue *spore_msg_queue;
};

static int parasyte_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct parasyte_mem *mem = filp->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	phys_addr_t offset = (phys_addr_t)vma->vm_pgoff << PAGE_SHIFT;
	unsigned long pfn = __phys_to_pfn(mem->phys_addr + offset);

	if (offset >> PAGE_SHIFT != vma->vm_pgoff) {
		pr_err("vm_pgoff too large.\n");
		return -EINVAL;
	}

	if (offset + (phys_addr_t)size - 1 < offset) {
		pr_err("offset wraps.\n");
		return -EINVAL;
	}

	if (offset + (phys_addr_t)size - 1 > mem->size) {
		pr_err("offset or size too large.\n");
		return -EINVAL;
	}

	// vma->vm_page_prot = phys_mem_access_prot(file, pfn, size, vma->vm_page_prot);
	return remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot);
}

static const struct file_operations parasyte_mem_fops = {
	.owner = THIS_MODULE,
	.mmap = parasyte_mmap,
};

static int parasyte_mem_alloc(struct parasyte_spore *spore, struct parasyte_mem *mem, size_t size)
{
		mem->size = size;
		mem->pages = parasyte_alloc_cma(size, GFP_USER);
		if (!mem->pages)
			return -ENOMEM;
		mem->virt_addr = page_to_virt(mem->pages);
		mem->phys_addr = page_to_phys(mem->pages);
		return 0;
}

static void set_cpu_used(struct parasyte_spore *spore, unsigned int cpu, bool used)
{
	if (used)
		cpumask_set_cpu(cpu, &spore->used_cpu);
	else
		cpumask_clear_cpu(cpu, &spore->used_cpu);
}

static int notify_spore(struct parasyte_spore *spore, unsigned long ioctl_param)
{
	unsigned int target_cpu = ioctl_param == PARASYTE_HIVE_ANY_CPU
				? cpumask_any(&spore->used_cpu) : ioctl_param;

	BUILD_BUG_ON(sizeof(unsigned long) < sizeof(__u32) || sizeof(unsigned int) < sizeof(__u32));

	if (target_cpu >= nr_cpu_ids) {
		pr_err("No cpu reserved for parasyte.\n");
		return -EINVAL;
	}

	parasyte_notify_mask(cpumask_of(target_cpu));
	return 0;
}

static void reclaim_resource(struct parasyte_spore *spore)
{
	unsigned int cpu;
	const struct cpu_operations *ops;

	if (spore->state >= SPORE_RUNNING || spore->state < SPORE_ALLOCATED)
		return;

	for_each_cpu(cpu, &spore->used_cpu) {
		ops = get_cpu_ops(cpu);
		if (!ops->cpu_kill || ops->cpu_kill(cpu))
			pr_err("CPU %d may not shutdown cleanly.\n", cpu);
		set_cpu_used(spore, cpu, false);
		set_cpu_reserved(cpu, false);
		add_cpu(cpu);
		if (cpu_online(cpu))
			pr_err("Brought CPU%d back.\n", cpu);
		else
			pr_err("Failed to bring CPU %d back from spore.\n", cpu);
	}

	parasyte_free_cma(spore->fdt.pages, spore->fdt.size);
	parasyte_free_cma(spore->ram.pages, spore->ram.size);

	spore->state = SPORE_FREED;
}

static void shutdown_spore(struct parasyte_spore *spore)
{
	unsigned int target_cpu = cpumask_any(&spore->used_cpu);

	if (spore->state < SPORE_RUNNING) {
		pr_err("Parasyte spore is not running.\n");
		return;
	}

	if (target_cpu >= nr_cpu_ids) {
		pr_err("No cpu reserved for parasyte.\n");
		return;
	}

	parasyte_request_shutdown_mask(cpumask_of(target_cpu));
	wait_event_interruptible(spore->client_wq, spore->state < SPORE_RUNNING);
}

static bool has_pending_message(struct parasyte_spore *spore)
{
	return spore->spore_msg_queue->producer_head != spore->spore_msg_queue->consumer_head;
}

static int parasyte_dev_open(struct inode *inode, struct file *filp)
{
	struct parasyte_spore *spore = container_of(filp->private_data, struct parasyte_spore, device);
	if (spore->state > SPORE_FREED) {
		pr_err("Parasyte spore is still running, refuse to open device.\n");
		return -EBUSY;
	}
	if (atomic_cmpxchg(&spore->opened, 0, 1)) {
		pr_err("Parasyte device is already opened.\n");
		return -EBUSY;
	}
	return 0;
}

static int parasyte_dev_release(struct inode *inode, struct file *filp)
{
	struct parasyte_spore *spore = container_of(filp->private_data, struct parasyte_spore, device);

	if (spore->state >= SPORE_RUNNING)
		shutdown_spore(spore);
	reclaim_resource(spore);
	atomic_set(&spore->opened, 0);

	return 0;
}

static int parasyte_dev_ioctl_alloc(struct parasyte_spore *spore, void __user *user_params)
{
	int ret = 0;
	unsigned int cpu;
	struct parasyte_alloc_params *alloc_params;
	struct cpumask alloc_cpus;
	const struct cpu_operations *ops;

	if (spore->state >= SPORE_RUNNING) {
		pr_err("Spore is still running while trying to alloc.\n");
		return -EBUSY;
	}

	if (spore->state >= SPORE_ALLOCATED)
		pr_warn("Re-allocating spore.\n");

	reclaim_resource(spore);
	alloc_params = memdup_user(user_params, sizeof(struct parasyte_alloc_params));
	if (IS_ERR(alloc_params))
		return PTR_ERR(alloc_params);

	ret = cpumask_parselist_user(alloc_params->cpus, alloc_params->cpus_len, &alloc_cpus);
	if (ret) {
		pr_err("Failed to parse cpu list: %d\n", ret);
		return ret;
	}

	ret = parasyte_mem_alloc(spore, &spore->ram, alloc_params->ram_size);
	if (ret) {
		pr_err("Failed to allocate ram for spore\n");
		return ret;
	}
	alloc_params->ram_paddr = spore->ram.phys_addr;

	ret = parasyte_mem_alloc(spore, &spore->fdt, MAX_FDT_SIZE);
	if (ret) {
		pr_err("Failed to allocate fdt for spore\n");
		goto free_ram;
	}
	alloc_params->fdt_size = MAX_FDT_SIZE;

	ret = parasyte_mem_alloc(spore, &spore->hb, PAGE_SIZE);
	if (ret) {
		pr_err("Failed to allocate hb for spore\n");
		goto free_fdt;
	}
	alloc_params->hb_size = PAGE_SIZE;

	for_each_cpu(cpu, &alloc_cpus) {
		if (!cpu_present(cpu)) {
			pr_err("CPU %d not present.\n", cpu);
			ret = -EINVAL;
			goto release_cpus;
		}

		ops = get_cpu_ops(cpu);
		if (strcmp(ops->name, "psci")) {
			pr_err("CPU %d use enable method %s, not supported.\n", cpu, ops->name);
			ret = -EINVAL;
			goto release_cpus;
		}

		set_cpu_reserved(cpu, true);
		remove_cpu(cpu);
		if (cpu_online(cpu)) {
			pr_err("CPU %d failed to be reserved.\n", cpu);
			set_cpu_reserved(cpu, false);
			ret = -EBUSY;
			goto release_cpus;
		}

		set_cpu_used(spore, cpu, true);
	}

	alloc_params->ram_fd = anon_inode_getfd("parasyte:ram", &parasyte_mem_fops, &spore->ram, O_RDWR);
	if (alloc_params->ram_fd < 0) {
		pr_err("Failed to alloc fd for parasyte ram.\n");
		ret = alloc_params->ram_fd;
		goto release_cpus;
	}

	alloc_params->fdt_fd = anon_inode_getfd("parasyte:fdt", &parasyte_mem_fops, &spore->fdt, O_RDWR);
	if (alloc_params->fdt_fd < 0) {
		pr_err("Failed to alloc fd for parasyte fdt.\n");
		ret = alloc_params->fdt_fd;
		goto close_ram_fd;
	}

	alloc_params->hb_fd = anon_inode_getfd("parasyte:hb", &parasyte_mem_fops, &spore->hb, O_RDWR);
	if (alloc_params->hb_fd < 0) {
		pr_err("Failed to alloc fd for parasyte hb.\n");
		ret = alloc_params->hb_fd;
		goto close_fdt_fd;
	}

	spore->state = SPORE_ALLOCATED;

	ret = copy_to_user(user_params, alloc_params, sizeof(struct parasyte_alloc_params));
	if (ret)
		goto reclaim;
	return ret;

reclaim:
	spore->state = SPORE_FREED;
close_fdt_fd:
	close_fd(alloc_params->fdt_fd);
close_ram_fd:
	close_fd(alloc_params->ram_fd);
release_cpus:
	for_each_cpu(cpu, &spore->used_cpu) {
		set_cpu_used(spore, cpu, false);
		set_cpu_reserved(cpu, false);
		add_cpu(cpu);
	}
free_fdt:
	parasyte_free_cma(spore->fdt.pages, spore->fdt.size);
free_ram:
	parasyte_free_cma(spore->ram.pages, spore->ram.size);
	return ret;
}

static int parasyte_dev_ioctl_setup(struct parasyte_spore *spore, void __user *user_params)
{
	struct parasyte_setup_params *setup_params;

	if (spore->state < SPORE_ALLOCATED) {
		pr_err("Spore is not allocated yet.\n");
		return -EINVAL;
	}

	if (spore->state >= SPORE_RUNNING) {
		pr_err("Trying to setup an already running spore.\n");
		return -EBUSY;
	}

	setup_params = memdup_user(user_params, sizeof(struct parasyte_setup_params));
	if (IS_ERR(setup_params))
		return PTR_ERR(setup_params);

	/* Just some basic checks. Trust userspace to setup everything else. */
	if (setup_params->kimage_offset >= spore->ram.size)
		return -EINVAL;
	if (setup_params->hive_queue_offset >= spore->ram.size)
		return -EINVAL;
	if (setup_params->spore_queue_offset >= spore->ram.size)
		return -EINVAL;

	spore->kimage_addr = spore->ram.phys_addr + setup_params->kimage_offset;
	spore->hive_msg_queue = spore->ram.virt_addr + setup_params->hive_queue_offset;
	spore->spore_msg_queue = spore->ram.virt_addr + setup_params->spore_queue_offset;

	spore->state = SPORE_SETUP;

	return 0;
}

static int parasyte_dev_ioctl_start(struct parasyte_spore *spore)
{
	int ret = 0;
	unsigned int cpu;
	phys_addr_t pa_trampoline;
	phys_addr_t *ptr_start = &parasyte_start, *ptr_end = &parasyte_end;

	if (spore->state >= SPORE_RUNNING) {
		pr_err("Parasyte spore is already running.\n");
		return -EBUSY;
	}
	if (spore->state < SPORE_SETUP) {
		pr_err("Parasyte spore is not yet setup.\n");
		return -ENODATA;
	}
	ret = fdt_check_header(spore->fdt.virt_addr);
	if (ret) {
		pr_err("Invalid fdt: %d\n", ret);
		return -EINVAL;
	}

	cpu = cpumask_any(&spore->used_cpu);
	// FIXME: protect by lock if we allow multiple spore
	smp_store_mb(parasyte_fdt, spore->fdt.phys_addr);
	smp_store_mb(parasyte_kimage, spore->kimage_addr);
	smp_store_mb(parasyte_hb, spore->hb.phys_addr);
	dcache_clean_inval_poc((unsigned long)ptr_start, (unsigned long)ptr_end);

	pa_trampoline = __pa_symbol(function_nocfi(parasyte_trampoline));
	ret = psci_ops.cpu_on(cpu_logical_map(cpu), pa_trampoline);
	if (ret)
		pr_err("failed to boot CPU%d (%d)\n", cpu, ret);
	else
		spore->state = SPORE_RUNNING;
	return ret;
}

static long parasyte_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long ioctl_param)
{
	int ret = 0;
	struct parasyte_spore *spore = container_of(filp->private_data, struct parasyte_spore, device);
	void __user *user_params = (void __user *)ioctl_param;

	switch (cmd) {
	case PARASYTE_IOCTL_ALLOC:
		ret = parasyte_dev_ioctl_alloc(spore, user_params);
		break;

	case PARASYTE_IOCTL_SETUP:
		ret = parasyte_dev_ioctl_setup(spore, user_params);
		break;

	case PARASYTE_IOCTL_START:
		ret = parasyte_dev_ioctl_start(spore);
		break;

	case PARASYTE_IOCTL_SHUTDOWN:
		if (spore->state < SPORE_RUNNING) {
			pr_err("Parasyte spore is not running.\n");
			ret = -ENODEV;
			break;
		}
		shutdown_spore(spore);
		break;

	case PARASYTE_IOCTL_WAIT_MSG:
		wait_event_interruptible(spore->client_wq, spore->state < SPORE_RUNNING || has_pending_message(spore));
		if (spore->state < SPORE_RUNNING)
			ret |= PARASYTE_MSG_STATUS_SHUTDOWN;
		if (has_pending_message(spore))
			ret |= PARASYTE_MSG_STATUS_PENDING;
		break;

	case PARASYTE_IOCTL_NOTIFY:
		ret = notify_spore(spore, ioctl_param);
		break;

	default:
		pr_err("Unknown command\n");
		ret = -EOPNOTSUPP;
	}

	return ret;
}

static const struct file_operations parasyte_dev_fops = {
	.owner		= THIS_MODULE,
	.open		= parasyte_dev_open,
	.release	= parasyte_dev_release,
	.unlocked_ioctl = parasyte_dev_ioctl,
};

/* Currently only a single spore is allowed */
static struct parasyte_spore nano_spore;

void parasyte_post_spore_shutdown(void)
{
	if (nano_spore.state < SPORE_RUNNING) {
		pr_err("IPI_PARASYTE_SHUTDOWN received but spore is already shutdown.\n");
	}

	nano_spore.state = SPORE_SETUP;
	wake_up_interruptible(&nano_spore.client_wq);
}

void parasyte_handle_irq(void)
{
	wake_up_interruptible(&nano_spore.client_wq);
}

int __init parasyte_dev_init(void)
{
	int err;

	atomic_set(&nano_spore.opened, 0);
	cpumask_clear(&nano_spore.used_cpu);
	nano_spore.state = SPORE_FREED;
	nano_spore.device.minor	= MISC_DYNAMIC_MINOR,
	nano_spore.device.name = "parasyte",
	nano_spore.device.fops = &parasyte_dev_fops,

	err = misc_register(&nano_spore.device);
	if (err) {
		pr_err("Failed to register parasyte dev: %d\n", err);
		return err;
	}

	init_waitqueue_head(&nano_spore.client_wq);

	return err;
}
