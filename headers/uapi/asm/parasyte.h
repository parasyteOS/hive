#ifndef _UAPI__ASM_PARASYTE_H
#define _UAPI__ASM_PARASYTE_H

#include <linux/types.h>
#include <linux/limits.h>

#define PARASYTE_MSG_TYPE_IO_REQUEST	0
#define PARASYTE_MSG_TYPE_SOFTIRQ	1

#define PARASYTE_MSG_FLAG_COMPLETE	(1ULL << 63)
#define PARASYTE_MSG_FLAG_PENDING	(1ULL << 62)
#define PARASYTE_MSG_FLAG_ERROR		(1ULL << 61)
#define PARASYTE_MSG_FLAG_SYNC		(1ULL << 60)
#define PARASYTE_MSG_FLAG_IO_READ	(1ULL << 0)
#define PARASYTE_MSG_FLAG_IO_USE_PTR	(1ULL << 1)

#define PARASYTE_MSG_STATUS_SHUTDOWN	(1 << 0)
#define PARASYTE_MSG_STATUS_PENDING	(1 << 1)

#define PARASYTE_HIVE_ANY_CPU		(U32_MAX)

struct parasyte_io_request {
	__u64 address;
	__u64 size;
	union {
		__u64 value;
		__u64 ptr;
	} data;
} __attribute__((__packed__));

struct parasyte_softirq {
	__u64 nr;
} __attribute__((__packed__));

union parasyte_msg_payload {
	struct parasyte_io_request io_request;
	struct parasyte_softirq softirq;
} __attribute__((__packed__));

struct parasyte_msg {
	__u64 cid;
	__u32 cpu;
	__u32 padding;
	__u64 type;
	__u64 flags;
	union parasyte_msg_payload payload;
} __attribute__((__packed__));

struct parasyte_msg_queue {
	/* Ring slot will be used to place NEXT msg,
	 * write-only by producer */
	__u64 producer_head;
	/* Ring slot that will be consumed next,
	 * write-only by consumer */
	__u64 consumer_head;
	/* Ring slot that will be checked next,
	 * write-only by producer */
	__u64 tail;
	/* Number of slot in the ring,
	 * the maximum usable slots is (capacity - 1) */
	__u64 capacity;
	struct parasyte_msg ring[];
} __attribute__((__packed__));

#define PARASYTE_MSG_QSIZE(capacity) (sizeof(struct parasyte_msg_queue) + capacity * sizeof(struct parasyte_msg))

struct parasyte_alloc_params {
	/* Input */
	char* cpus;
	__u64 cpus_len;
	__u64 ram_size;
	/* Output */
	int ram_fd;
	__u64 ram_paddr;
	int fdt_fd;
	__u64 fdt_size;
	int hb_fd;
	__u64 hb_size;
};

struct parasyte_setup_params {
	__u64 kimage_offset;
	__u64 hive_queue_offset;
	__u64 spore_queue_offset;
};

#define PARASYTE_IOCTL_TYPE	0xEE

#define PARASYTE_IOCTL_ALLOC	_IOWR(PARASYTE_IOCTL_TYPE, 0x00, struct parasyte_alloc_params)
#define PARASYTE_IOCTL_SETUP	_IOW(PARASYTE_IOCTL_TYPE, 0x01, struct parasyte_setup_params)
#define PARASYTE_IOCTL_START	_IO(PARASYTE_IOCTL_TYPE, 0x02)
#define PARASYTE_IOCTL_SHUTDOWN	_IO(PARASYTE_IOCTL_TYPE, 0x03)
#define PARASYTE_IOCTL_WAIT_MSG _IO(PARASYTE_IOCTL_TYPE, 0x04)
#define PARASYTE_IOCTL_NOTIFY	_IO(PARASYTE_IOCTL_TYPE, 0x05)

#endif
