#define pr_fmt(fmt)	"parasyte/spore: " fmt

#include <linux/mm.h>
#include <linux/cpu.h>
#include <linux/reboot.h>
#include <linux/kmsg_dump.h>
#include <asm/cpu_ops.h>
#include <asm/daifflags.h>

#include "ring.h"
#include "spore.h"

static u64 cpu_idx_mapping[NR_CPUS];

struct io_args {
	__u64 address;
	__u64 size;
	__u64 value_or_ptr;
	bool read;
};

struct _softirq_action {
	struct work_struct work;
	__u64 nr;
	irq_handler_t handler;
	void *data;

	bool enabled;
	bool allocated;
	bool initialised;
};

static struct _softirq_action *softirq_line;
static spinlock_t softirq_line_lock;
static __u64 max_softirq_nr;
static struct workqueue_struct *softirq_workqueue;
#define DEFAULT_MAX_SOFTIRQ 63

static void default_notify_hive(unsigned int irq)
{
	WARN(true, "Try to notify host without setting a function.\n");
}

static void (*__notify_hive)(unsigned int irq) = default_notify_hive;

static int fill_io_msg(__u64 *msg_flags, union parasyte_msg_payload *msg_payload, void *data)
{
	struct io_args *args = (struct io_args *)data;
	struct parasyte_io_request *req = &msg_payload->io_request;

	if (args->read) *msg_flags |= PARASYTE_MSG_FLAG_IO_READ;

	WRITE_ONCE(req->address, args->address);
	WRITE_ONCE(req->size, args->size);
	if (unlikely(args->size > sizeof(req->data.value))) {
		*msg_flags |= PARASYTE_MSG_FLAG_IO_USE_PTR;
		WRITE_ONCE(req->data.ptr, args->value_or_ptr);
	} else
		WRITE_ONCE(req->data.value, args->value_or_ptr);

	return 0;
}

static int io_callback(__u64 *msg_flags, union parasyte_msg_payload *msg_payload, void *data)
{
	struct io_args *args = (struct io_args *)data;
	struct parasyte_io_request *req = &msg_payload->io_request;

	if (args->read && !(MSG_IO_USE_PTR(*msg_flags)))
		memcpy((void *)args->value_or_ptr, &req->data.value, args->size);

	return 0;
}

static void fire_softirq(struct work_struct *work)
{
	struct _softirq_action *action = container_of(work, struct _softirq_action, work);
	irqreturn_t result;
	if (action->handler) {
		result = action->handler(action->nr, action->data);
		if (result != IRQ_HANDLED)
			pr_debug("softirq %ld not handled, result: %d\n", action->nr, result);
		else
			pr_debug("softirq %ld handled\n", action->nr);
	} else {
		pr_err("softirq %ld fired but no handler found.\n", action->nr);
	}
}

void parasyte_spore_process_msg(struct parasyte_msg *msg)
{
	__u64 type = READ_ONCE(msg->type);
	__u64 nr;

	switch (type) {
	case PARASYTE_MSG_TYPE_SOFTIRQ:
		nr = READ_ONCE(msg->payload.softirq.nr);
		if (nr > max_softirq_nr) {
			pr_err("Received out of scope softirq nr %ld\n", nr);
			break;
		}
		if (!softirq_line[nr].enabled) {
			pr_err("Received disabled softirq %ld\n", nr);
			break;
		}
		queue_work(softirq_workqueue, &softirq_line[nr].work);
		break;
	default:
		pr_err("Received unhandled message type: 0x%lx\n", type);
	}
}

void parasyte_set_notify_hive(void (*notify_hive)(unsigned int irq))
{
	__notify_hive = notify_hive;
}

void __parasyte_notify_hive(unsigned int irqnr)
{
	__notify_hive(irqnr);
}

int parasyte_spore_commit_io(struct parasyte_msg_client *client,
			     __u64 address, __u64 size, __u64 value_or_ptr,
			     bool read, bool sync)
{
	struct io_args args = {
		.address = address,
		.size = size,
		.value_or_ptr = value_or_ptr,
		.read = read,
	};

	BUG_ON(read && !sync);

	return parasyte_ring_produce_msg(client, PARASYTE_MSG_TYPE_IO_REQUEST,
					 cpu_idx_mapping[smp_processor_id()],
					 sync, (void *)&args, fill_io_msg, io_callback);
}
EXPORT_SYMBOL_GPL(parasyte_spore_commit_io);

int parasyte_spore_request_softirq(__u64 nr, irq_handler_t handler, void *data)
{
	int err = 0;
	unsigned long flags;
	struct _softirq_action *action;

	pr_info("Requesting softirq %ld\n", nr);

	if (nr > max_softirq_nr)
		return -EINVAL;

	spin_lock_irqsave(&softirq_line_lock, flags);

	action = &softirq_line[nr];

	if (!action->initialised) {
		INIT_WORK(&action->work, fire_softirq);
		action->nr = nr;
		action->enabled = false;
		action->allocated = false;
		action->initialised = true;
	}

	if (unlikely(action->allocated)) {
		pr_err("Request already allocated softirq %ld\n", nr);
		err = -EBUSY;
		goto out;
	}

	action->handler = handler;
	action->data = data;
	action->allocated = true;
	action->enabled = true;
out:
	spin_unlock_irqrestore(&softirq_line_lock, flags);

	return err;
}
EXPORT_SYMBOL_GPL(parasyte_spore_request_softirq);

int parasyte_spore_free_softirq(__u64 nr)
{
	int err = 0;
	unsigned long flags;
	struct _softirq_action *action;

	if (nr > max_softirq_nr)
		return -EINVAL;

	spin_lock_irqsave(&softirq_line_lock, flags);

	action = &softirq_line[nr];

	if (unlikely(!action->allocated)) {
		pr_err("Freeing an unallocated softirq %ld\n", nr);
		err = -ENODEV;
		goto out;
	}

	action->enabled = false;
	action->allocated = false;
	action->data = NULL;
	action->handler = NULL;
out:
	spin_unlock_irqrestore(&softirq_line_lock, flags);

	return err;
}
EXPORT_SYMBOL_GPL(parasyte_spore_free_softirq);

int parasyte_spore_synchronize_softirq(__u64 nr)
{
	int err = 0;
	struct _softirq_action *action;

	if (nr > max_softirq_nr)
		return -EINVAL;

	action = &softirq_line[nr];

	if (unlikely(!action->allocated)) {
		pr_err("Scynchronizing an unallocated softirq %ld\n", nr);
		err = -ENODEV;
		goto out;
	}

	flush_work(&action->work);
out:
	return err;
}
EXPORT_SYMBOL_GPL(parasyte_spore_synchronize_softirq);

void parasyte_spore_shutdown(void)
{
	unsigned int this_cpu;
	const struct cpu_operations *ops;

	pr_emerg("Shutdown Spore\n");
	kernel_restart_prepare(NULL);
	migrate_to_reboot_cpu();
	cpu_hotplug_enable();
	machine_shutdown();
	kmsg_dump(KMSG_DUMP_SHUTDOWN);
	parasyte_report_shutdown();

	local_daif_mask();
	this_cpu = smp_processor_id();
	ops = get_cpu_ops(this_cpu);
	ops->cpu_die(this_cpu);
	BUG();
}

void parasyte_spore_set_cpu_mapping(unsigned int cpu, struct device_node *dn)
{
	const __be32 *cell;
	u64 hive_idx;

	cell = of_get_property(dn, "parasyte-hive-index", NULL);
	if (!cell) {
		pr_err("%pOF: missing parasyte-hive-index property\n", dn);
		cpu_idx_mapping[cpu] = PARASYTE_HIVE_ANY_CPU;
		return;
	}

	hive_idx = of_read_number(cell, of_n_addr_cells(dn));
	cpu_idx_mapping[cpu] = hive_idx;
}

int __init parasyte_spore_init(void)
{
	spin_lock_init(&softirq_line_lock);

	max_softirq_nr = DEFAULT_MAX_SOFTIRQ;
	softirq_line = kvcalloc(max_softirq_nr + 1, sizeof(struct _softirq_action), GFP_KERNEL);
	if (!softirq_line) {
		pr_err("Failed to allocate softirq line.\n");
		return -ENOMEM;
	}

	softirq_workqueue = alloc_workqueue("softirq_workqueue", WQ_UNBOUND | WQ_HIGHPRI | WQ_MEM_RECLAIM, 0);
	if (!softirq_workqueue) {
		pr_err("Failed to allocate softirq workqueue.\n");
		kvfree(softirq_line);
		return -ENOMEM;
	}

	return 0;
}
