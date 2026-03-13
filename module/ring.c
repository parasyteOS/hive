#define pr_fmt(fmt)	"parasyte/ring: " fmt

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>

#include "spore.h"
#include "ring.h"

/* Produced by the Hive */
static struct parasyte_msg_queue *hive_msg_queue = NULL;
phys_addr_t hive_msg_queue_size = 0;
/* Produced by the Spore */
static struct parasyte_msg_queue *spore_msg_queue = NULL;
phys_addr_t spore_msg_queue_size = 0;

struct parasyte_msg_client {
	__u64 cid;
	wait_queue_head_t wq;
	bool allocated;
	bool initialised;
};

static struct parasyte_msg_client *msg_clients;
static spinlock_t msg_clients_lock;
static int max_clients, next_cid = 0;
#define DEFAULT_MAX_CLIENTS 64

static spinlock_t msg_queue_lock;
/* Assigned with the queue produced by current kernel*/
static struct parasyte_msg_queue *producing_queue;
static struct workqueue_struct *post_produce_workqueue;
static void post_produce(struct work_struct *work);
static DECLARE_WORK(post_produce_work, post_produce);
/* Assigned with the queue consumed by current kernel*/
static struct parasyte_msg_queue *consuming_queue;
static struct workqueue_struct *consume_msg_workqueue;
static void consume_msg(struct work_struct *work);
static DECLARE_WORK(consume_msg_work, consume_msg);

static int __init setup_hive_queue(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (of_get_flat_dt_prop(node, "no-map", NULL))
		return -EINVAL;

	hive_msg_queue = phys_to_virt(rmem->base);
	hive_msg_queue_size = rmem->size;

	return 0;
}

static int __init setup_spore_queue(struct reserved_mem *rmem)
{
	unsigned long node = rmem->fdt_node;

	if (of_get_flat_dt_prop(node, "no-map", NULL))
		return -EINVAL;

	spore_msg_queue = phys_to_virt(rmem->base);
	spore_msg_queue_size = rmem->size;

	return 0;
}

RESERVEDMEM_OF_DECLARE(parasyte_hive_queue, "parasyte,msg-queue-hive", setup_hive_queue);
RESERVEDMEM_OF_DECLARE(parasyte_spore_queue, "parasyte,msg-queue-spore", setup_spore_queue);

static void consume_msg(struct work_struct *work)
{
	__u64 consumer_head = smp_load_acquire(&consuming_queue->consumer_head);
	__u64 producer_head = smp_load_acquire(&consuming_queue->producer_head);
	__u64 capacity = consuming_queue->capacity;

	__u64 idx = consumer_head;
	struct parasyte_msg *msg;
	__u64 flags;

loop:
	while (idx != producer_head) {
		msg = &consuming_queue->ring[idx];
		flags = READ_ONCE(msg->flags);

		if (!MSG_PENDING(flags))
			pr_err("Consumed message ahead of consumer head.\n");
		else {
			parasyte_spore_process_msg(msg);
			flags = READ_ONCE(msg->flags);
			MSG_CLEAR_PENDING(flags);
			WRITE_ONCE(msg->flags, flags);
		}

		idx = (idx + 1) % capacity;
		consumer_head = idx;

		if (MSG_SYNC(flags)) {
			smp_store_release(&consuming_queue->consumer_head, consumer_head);
			parasyte_notify_hive();
		}
	}

	producer_head = smp_load_acquire(&consuming_queue->producer_head);
	if (idx != producer_head) goto loop;

	smp_store_release(&consuming_queue->consumer_head, consumer_head);
	parasyte_notify_hive();
}

static void post_produce(struct work_struct *work)
{
	bool moving_tail = true;
	__u64 tail = smp_load_acquire(&producing_queue->tail);
	__u64 head = smp_load_acquire(&producing_queue->consumer_head);
	__u64 capacity = producing_queue->capacity;

	__u64 idx = tail;
	struct parasyte_msg *msg;
	__u64 flags;

loop:
	while (idx != head) {
		msg = &producing_queue->ring[idx];
		flags = READ_ONCE(msg->flags);

		if (MSG_PENDING(flags)) {
			pr_err("Pending message left behind consumer head.\n");
			MSG_CLEAR_PENDING(flags);
			MSG_SET_ERROR(flags);
			smp_store_release(&msg->flags, flags);
		}

		if (!MSG_COMPLETE(flags)) {
			if (MSG_SYNC(flags)) {
				moving_tail = false;
				wake_up_interruptible(&msg_clients[READ_ONCE(msg->cid)].wq);
			} else {
				pr_err("Incomplete message without sync flag set.\n");
				MSG_SET_COMPLETE(flags);
				WRITE_ONCE(msg->flags, flags);
			}
		}

		idx = (idx + 1) % capacity;
		if (moving_tail) tail = idx;
	}

	head = smp_load_acquire(&producing_queue->consumer_head);
	if (idx != head) goto loop;

	smp_store_release(&producing_queue->tail, tail);
}

int parasyte_ring_produce_msg(struct parasyte_msg_client *client, __u64 type, __u64 hive_cpu_idx, bool sync, void *data,
			      int (*fill_msg)(__u64 *msg_flags, union parasyte_msg_payload *msg_payload, void *data),
			      int (*callback)(__u64 *msg_flags, union parasyte_msg_payload *msg_payload, void *data))
{
	__u64 msg_flags;
	__u64 head, next_head, tail;
	struct parasyte_msg *msg;
	union parasyte_msg_payload *msg_payload;
	unsigned long irq_flags;
	int ret = 0;

	msg_flags = PARASYTE_MSG_FLAG_PENDING;
	msg_flags |= sync ? PARASYTE_MSG_FLAG_SYNC : PARASYTE_MSG_FLAG_COMPLETE;

	spin_lock_irqsave(&msg_queue_lock, irq_flags);
	head = smp_load_acquire(&producing_queue->producer_head);
	next_head = (head + 1) % producing_queue->capacity;
	tail = smp_load_acquire(&producing_queue->tail);

	if (next_head == tail) {
		pr_err("producing_queue is full.\n");
		spin_unlock_irqrestore(&msg_queue_lock, irq_flags);
		queue_work(post_produce_workqueue, &post_produce_work);
		return -1;
	}

	msg = &producing_queue->ring[head];
	msg_payload = &msg->payload;

	if (fill_msg(&msg_flags, msg_payload, data)) {
		pr_err("Failed to fill message.\n");
		spin_unlock_irqrestore(&msg_queue_lock, irq_flags);
		queue_work(post_produce_workqueue, &post_produce_work);
		return -1;
	}

	WRITE_ONCE(msg->cid, client->cid);
	WRITE_ONCE(msg->cpu, hive_cpu_idx);
	WRITE_ONCE(msg->type, type);
	WRITE_ONCE(msg->flags, msg_flags);

	smp_store_release(&producing_queue->producer_head, next_head);
	spin_unlock_irqrestore(&msg_queue_lock, irq_flags);

	parasyte_notify_hive();

	if (sync) {
		if (in_atomic_preempt_off() || in_atomic()) {
			msg_flags = smp_cond_load_acquire(&msg->flags, !(VAL & PARASYTE_MSG_FLAG_PENDING));
		} else {
restart:
			ret = wait_event_interruptible(client->wq, (!(smp_load_acquire(&msg->flags) & PARASYTE_MSG_FLAG_PENDING)));
			if (ret < 0) goto restart;
			msg_flags = READ_ONCE(msg->flags);
		}
		if (MSG_ERROR(msg_flags))
			pr_err("Error when host consuming the message.\n");
		else
			callback(&msg_flags, msg_payload, data);
		MSG_SET_COMPLETE(msg_flags);
		smp_store_release(&msg->flags, msg_flags);
	}

	queue_work(post_produce_workqueue, &post_produce_work);

	return MSG_ERROR(msg_flags) ? -1 : 0;
}

struct parasyte_msg_client *parasyte_ring_allocate_msg_client(void)
{
	unsigned long flags;
	struct parasyte_msg_client *client;
	int i, cid;

	spin_lock_irqsave(&msg_clients_lock, flags);

	if (next_cid == -1) {
		pr_err("No io client can be allocated.\n");
		spin_unlock_irqrestore(&msg_clients_lock, flags);
		return NULL;
	}

	cid = next_cid;
	client = &msg_clients[cid];
	next_cid = -1;

	for (i = (cid + 1) % max_clients; i != cid; i = (i + 1) % max_clients) {
		if (!msg_clients[i].allocated) {
			next_cid = i;
			break;
		}
	}

	spin_unlock_irqrestore(&msg_clients_lock, flags);

	if (!client->initialised) {
		client->cid = cid;
		init_waitqueue_head(&client->wq);
		client->initialised = true;
		client->allocated = true;
	}

	return client;
}

void parasyte_ring_free_msg_client(struct parasyte_msg_client *client)
{
	unsigned long flags;
	client->allocated = false;

	spin_lock_irqsave(&msg_clients_lock, flags);
	next_cid = client->cid;
	spin_unlock_irqrestore(&msg_clients_lock, flags);
}

void parasyte_handle_irq(void)
{
	queue_work(consume_msg_workqueue, &consume_msg_work);
	queue_work(post_produce_workqueue, &post_produce_work);
}

int __init parasyte_ring_init(void)
{
	if (!hive_msg_queue || !spore_msg_queue)
		panic("parasyte message queue is not setup correctly.\n");

	if (PARASYTE_MSG_QSIZE(hive_msg_queue->capacity) < hive_msg_queue_size)
		panic("No enough memory reserved for hive message queue.\n");

	if (PARASYTE_MSG_QSIZE(spore_msg_queue->capacity) < spore_msg_queue_size)
		panic("No enough memory reserved for spore message queue.\n");

	producing_queue = spore_msg_queue;
	consuming_queue = hive_msg_queue;

	spin_lock_init(&msg_clients_lock);
	spin_lock_init(&msg_queue_lock);

	max_clients = DEFAULT_MAX_CLIENTS;
	msg_clients = kvcalloc(max_clients, sizeof(struct parasyte_msg_client), GFP_KERNEL);
	if (!msg_clients) {
		pr_err("Failed to allocate msg clients.\n");
		return -1;
	}

	consume_msg_workqueue = alloc_ordered_workqueue("parasyte_consume_msg", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!consume_msg_workqueue) {
		pr_err("Failed to allocate consume_msg_workqueue.\n");
		goto free_clients;
	}

	post_produce_workqueue = alloc_ordered_workqueue("parasyte_post_produce", WQ_HIGHPRI | WQ_MEM_RECLAIM);
	if (!post_produce_workqueue) {
		pr_err("Failed to allocate post_produce_workqueue.\n");
		goto destroy_consume_workqueue;
	}

	return 0;

destroy_consume_workqueue:
	destroy_workqueue(consume_msg_workqueue);
free_clients:
	kvfree(msg_clients);
	return -1;
}
