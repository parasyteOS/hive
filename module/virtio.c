// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio paravirtualized device driver for parasyte kernel
 *
 * This module allows virtio devices to be used over parasyte ring.
 * The emulated memory layout is same as Virtio MMIO devices, so reuse
 * a lot of macros here.
 *
 * The guest device(s) may be instantiated in Device Tree node, eg.:
 *
 *		virtio_block@1e000 {
 *			compatible = "virtio,parasyte";
 *			reg = <0x1e000 0x100>;
 *			softirq = <42>;
 *		}
 *
 * Based on Virtio MMIO driver by Pawel Moll, Copyright 2011-2014, ARM Ltd.
 */

#define pr_fmt(fmt) "virtio-parasyte: " fmt

#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <uapi/linux/virtio_mmio.h>
#include <linux/virtio_ring.h>
#include <linux/version.h>

#include "ring.h"
#include "spore.h"

/* The alignment to use between consumer and producer parts of vring.
 * Currently hardcoded to the page size. */
#define VIRTIO_PARASYTE_VRING_ALIGN		PAGE_SIZE

#define to_virtio_parasyte_device(_plat_dev) \
	container_of(_plat_dev, struct virtio_parasyte_device, vdev)

struct virtio_parasyte_device {
	struct virtio_device vdev;
	struct platform_device *pdev;

	struct parasyte_msg_client *client;
	struct resource *res;
	unsigned long version;
};

static inline void __parasyte_write(struct virtio_parasyte_device *vp_dev, __u64 size, __u64 value, __u64 offset, bool blocking)
{
	if (offset + size > vp_dev->res->end - vp_dev->res->start + 1) {
		dev_err(&vp_dev->vdev.dev, "Write out of range: offset 0x%lx, size %d", offset, size);
		return;
	}

	if (parasyte_spore_commit_io(vp_dev->client, vp_dev->res->start + offset, size, value, false, blocking))
		dev_err(&vp_dev->vdev.dev, "Failed to write to address 0x%p", vp_dev->res->start + offset);
}

#define DEFINE_PARASYTE_WRITE(suffix, type, size) \
static inline void parasyte_write##suffix(struct virtio_parasyte_device *vp_dev, type value, __u64 offset) \
{ \
	__parasyte_write(vp_dev, size, (__u64)value, offset, true); \
} \
static inline void parasyte_write##suffix##_nonblocking(struct virtio_parasyte_device *vp_dev, type value, __u64 offset) \
{ \
	__parasyte_write(vp_dev, size, (__u64)value, offset, false); \
}

DEFINE_PARASYTE_WRITE(b, u8,  1)
DEFINE_PARASYTE_WRITE(w, u16, 2)
DEFINE_PARASYTE_WRITE(l, u32, 4)

static inline void __parasyte_read(struct virtio_parasyte_device *vp_dev, __u64 size, __u64 offset, void *out_value)
{
	if (offset + size > vp_dev->res->end - vp_dev->res->start + 1) {
		dev_err(&vp_dev->vdev.dev, "Read out of range: offset 0x%lx, size %d", offset, size);
		return;
	}

	if (parasyte_spore_commit_io(vp_dev->client, vp_dev->res->start + offset, size, (__u64)out_value, true, true)) {
		dev_err(&vp_dev->vdev.dev, "Failed to read address 0x%p", vp_dev->res->start + offset);
		memset(out_value, 0, size);
	}
}

#define DEFINE_PARASYTE_READ(suffix, type, size) \
static inline type parasyte_read##suffix(struct virtio_parasyte_device *vp_dev, __u64 offset) \
{ \
	type result; \
	__parasyte_read(vp_dev, size, offset, (void *)&result); \
	return result; \
}

DEFINE_PARASYTE_READ(b, u8,  1)
DEFINE_PARASYTE_READ(w, u16, 2)
DEFINE_PARASYTE_READ(l, u32, 4)

static inline u32 vp_get_softirq(struct virtio_parasyte_device *vp_dev)
{
	int rc;
	u32 ret;

	rc = of_property_read_u32(vp_dev->pdev->dev.of_node, "softirq", &ret);
	if (rc) return rc;

	return ret;
}

/* Configuration interface */

static u64 vp_get_features(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	u64 features;

	parasyte_writel(vp_dev, 1, VIRTIO_MMIO_DEVICE_FEATURES_SEL);
	features = parasyte_readl(vp_dev, VIRTIO_MMIO_DEVICE_FEATURES);
	features <<= 32;

	parasyte_writel(vp_dev, 0, VIRTIO_MMIO_DEVICE_FEATURES_SEL);
	features |= parasyte_readl(vp_dev, VIRTIO_MMIO_DEVICE_FEATURES);

	return features;
}

static int vp_finalize_features(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	/* Give virtio_ring a chance to accept features. */
	vring_transport_features(vdev);

	/* Make sure there are no mixed devices */
	if (vp_dev->version == 2 &&
			!__virtio_test_bit(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev, "New virtio-parasyte devices (version 2) must provide VIRTIO_F_VERSION_1 feature!\n");
		return -EINVAL;
	}

	parasyte_writel(vp_dev, 1, VIRTIO_MMIO_DRIVER_FEATURES_SEL);
	parasyte_writel(vp_dev, vdev->features >> 32, VIRTIO_MMIO_DRIVER_FEATURES);

	parasyte_writel(vp_dev, 0, VIRTIO_MMIO_DRIVER_FEATURES_SEL);
	parasyte_writel(vp_dev, vdev->features, VIRTIO_MMIO_DRIVER_FEATURES);

	return 0;
}

static void vp_get(struct virtio_device *vdev, unsigned int offset,
		   void *buf, unsigned int len)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	u64 base = VIRTIO_MMIO_CONFIG;
	u8 b;
	__le16 w;
	__le32 l;

	if (vp_dev->version == 1) {
		u8 *ptr = buf;
		int i;

		for (i = 0; i < len; i++)
			ptr[i] = parasyte_readb(vp_dev, base + offset + i);
		return;
	}

	switch (len) {
	case 1:
		b = parasyte_readb(vp_dev, base + offset);
		memcpy(buf, &b, sizeof b);
		break;
	case 2:
		w = cpu_to_le16(parasyte_readw(vp_dev, base + offset));
		memcpy(buf, &w, sizeof w);
		break;
	case 4:
		l = cpu_to_le32(parasyte_readl(vp_dev, base + offset));
		memcpy(buf, &l, sizeof l);
		break;
	case 8:
		l = cpu_to_le32(parasyte_readl(vp_dev, base + offset));
		memcpy(buf, &l, sizeof l);
		l = cpu_to_le32(parasyte_readl(vp_dev, base + offset + sizeof l));
		memcpy(buf + sizeof l, &l, sizeof l);
		break;
	default:
		BUG();
	}
}

static void vp_set(struct virtio_device *vdev, unsigned int offset,
		   const void *buf, unsigned int len)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	u64 base = VIRTIO_MMIO_CONFIG;
	u8 b;
	__le16 w;
	__le32 l;

	if (vp_dev->version == 1) {
		const u8 *ptr = buf;
		int i;

		for (i = 0; i < len; i++)
			parasyte_writeb(vp_dev, ptr[i], base + offset + i);

		return;
	}

	switch (len) {
	case 1:
		memcpy(&b, buf, sizeof b);
		parasyte_writeb(vp_dev, b, base + offset);
		break;
	case 2:
		memcpy(&w, buf, sizeof w);
		parasyte_writew(vp_dev, le16_to_cpu(w), base + offset);
		break;
	case 4:
		memcpy(&l, buf, sizeof l);
		parasyte_writel(vp_dev, le32_to_cpu(l), base + offset);
		break;
	case 8:
		memcpy(&l, buf, sizeof l);
		parasyte_writel(vp_dev, le32_to_cpu(l), base + offset);
		memcpy(&l, buf + sizeof l, sizeof l);
		parasyte_writel(vp_dev, le32_to_cpu(l), base + offset + sizeof l);
		break;
	default:
		BUG();
	}
}

static u32 vp_generation(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	if (vp_dev->version == 1)
		return 0;
	else
		return parasyte_readl(vp_dev, VIRTIO_MMIO_CONFIG_GENERATION);
}

static u8 vp_get_status(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	return parasyte_readl(vp_dev, VIRTIO_MMIO_STATUS) & 0xff;
}

static void vp_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	/* We should never be setting status to 0. */
	BUG_ON(status == 0);

	/*
	 * Per memory-barriers.txt, wmb() is not needed to guarantee
	 * that the cache coherent memory writes have completed
	 * before writing to the MMIO region.
	 */
	parasyte_writel(vp_dev, status, VIRTIO_MMIO_STATUS);
}

static void vp_reset(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	/* 0 status means a reset. */
	parasyte_writel(vp_dev, 0, VIRTIO_MMIO_STATUS);
}



/* Transport interface */

/* the notify function used when creating a virt queue */
static bool vp_notify(struct virtqueue *vq)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vq->vdev);

	/* We write the queue's selector into the notification register to
	 * signal the other end */
	parasyte_writel_nonblocking(vp_dev, vq->index, VIRTIO_MMIO_QUEUE_NOTIFY);
	return true;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
static bool vp_notify_with_data(struct virtqueue *vq)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vq->vdev);
	u32 data = vring_notification_data(vq);

	parasyte_writel_nonblocking(vp_dev, data, VIRTIO_MMIO_QUEUE_NOTIFY);

	return true;
}
#endif

/* Notify all virtqueues on an interrupt. */
static irqreturn_t vp_interrupt(int irq, void *opaque)
{
	struct virtio_parasyte_device *vp_dev = opaque;
	struct virtqueue *vq;
	unsigned long status;
	irqreturn_t ret = IRQ_NONE;

	/* Read and acknowledge interrupts */
	status = parasyte_readl(vp_dev, VIRTIO_MMIO_INTERRUPT_STATUS);
	parasyte_writel(vp_dev, status, VIRTIO_MMIO_INTERRUPT_ACK);

	if (unlikely(status & VIRTIO_MMIO_INT_CONFIG)) {
		virtio_config_changed(&vp_dev->vdev);
		ret = IRQ_HANDLED;
	}

	if (likely(status & VIRTIO_MMIO_INT_VRING)) {
		virtio_device_for_each_vq((&vp_dev->vdev), vq)
			ret |= vring_interrupt(irq, vq);
	}

	return ret;
}



static void vp_del_vq(struct virtqueue *vq)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vq->vdev);
	unsigned int index = vq->index;

	/* Select and deactivate the queue */
	parasyte_writel(vp_dev, index, VIRTIO_MMIO_QUEUE_SEL);
	if (vp_dev->version == 1) {
		parasyte_writel(vp_dev, 0, VIRTIO_MMIO_QUEUE_PFN);
	} else {
		parasyte_writel(vp_dev, 0, VIRTIO_MMIO_QUEUE_READY);
		WARN_ON(parasyte_readl(vp_dev, VIRTIO_MMIO_QUEUE_READY));
	}

	vring_del_virtqueue(vq);
}

static void vp_del_vqs(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	struct virtqueue *vq, *n;

	list_for_each_entry_safe(vq, n, &vdev->vqs, list)
		vp_del_vq(vq);

	parasyte_spore_free_softirq(vp_get_softirq(vp_dev));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static void vp_synchronize_cbs(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	parasyte_spore_synchronize_softirq(vp_get_softirq(vp_dev));
}
#endif

static struct virtqueue *vp_setup_vq(struct virtio_device *vdev, unsigned int index,
				  void (*callback)(struct virtqueue *vq),
				  const char *name, bool ctx)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	bool (*notify)(struct virtqueue *vq);
	struct virtqueue *vq;
	unsigned int num;
	int err;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	if (__virtio_test_bit(vdev, VIRTIO_F_NOTIFICATION_DATA))
		notify = vp_notify_with_data;
	else
#endif
		notify = vp_notify;

	if (!name)
		return NULL;

	/* Select the queue we're interested in */
	parasyte_writel(vp_dev, index, VIRTIO_MMIO_QUEUE_SEL);

	/* Queue shouldn't already be set up. */
	if (parasyte_readl(vp_dev, (vp_dev->version == 1 ?
			VIRTIO_MMIO_QUEUE_PFN : VIRTIO_MMIO_QUEUE_READY))) {
		err = -ENOENT;
		goto error_available;
	}

	num = parasyte_readl(vp_dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (num == 0) {
		err = -ENOENT;
		goto error_new_virtqueue;
	}

	/* Create the vring */
	vq = vring_create_virtqueue(index, num, VIRTIO_PARASYTE_VRING_ALIGN, vdev,
				 true, true, ctx, notify, callback, name);
	if (!vq) {
		err = -ENOMEM;
		goto error_new_virtqueue;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
	vq->num_max = num;
#endif

	/* Activate the queue */
	parasyte_writel(vp_dev, virtqueue_get_vring_size(vq), VIRTIO_MMIO_QUEUE_NUM);
	if (vp_dev->version == 1) {
		u64 q_pfn = virtqueue_get_desc_addr(vq) >> PAGE_SHIFT;

		/*
		 * virtio-mmio v1 uses a 32bit QUEUE PFN. If we have something
		 * that doesn't fit in 32bit, fail the setup rather than
		 * pretending to be successful.
		 */
		if (q_pfn >> 32) {
			dev_err(&vdev->dev,
				"platform bug: legacy virtio-parasyte must not be used with RAM above 0x%llxGB\n",
				0x1ULL << (32 + PAGE_SHIFT - 30));
			err = -E2BIG;
			goto error_bad_pfn;
		}

		parasyte_writel(vp_dev, PAGE_SIZE, VIRTIO_MMIO_QUEUE_ALIGN);
		parasyte_writel(vp_dev, q_pfn, VIRTIO_MMIO_QUEUE_PFN);
	} else {
		u64 addr;

		addr = virtqueue_get_desc_addr(vq);
		parasyte_writel(vp_dev, (u32)addr, VIRTIO_MMIO_QUEUE_DESC_LOW);
		parasyte_writel(vp_dev, (u32)(addr >> 32), VIRTIO_MMIO_QUEUE_DESC_HIGH);

		addr = virtqueue_get_avail_addr(vq);
		parasyte_writel(vp_dev, (u32)addr, VIRTIO_MMIO_QUEUE_AVAIL_LOW);
		parasyte_writel(vp_dev, (u32)(addr >> 32), VIRTIO_MMIO_QUEUE_AVAIL_HIGH);

		addr = virtqueue_get_used_addr(vq);
		parasyte_writel(vp_dev, (u32)addr, VIRTIO_MMIO_QUEUE_USED_LOW);
		parasyte_writel(vp_dev, (u32)(addr >> 32), VIRTIO_MMIO_QUEUE_USED_HIGH);

		parasyte_writel(vp_dev, 1, VIRTIO_MMIO_QUEUE_READY);
	}

	return vq;

error_bad_pfn:
	vring_del_virtqueue(vq);
error_new_virtqueue:
	if (vp_dev->version == 1) {
		parasyte_writel(vp_dev, 0, VIRTIO_MMIO_QUEUE_PFN);
	} else {
		parasyte_writel(vp_dev, 0, VIRTIO_MMIO_QUEUE_READY);
		WARN_ON(parasyte_readl(vp_dev, VIRTIO_MMIO_QUEUE_READY));
	}
error_available:
	return ERR_PTR(err);
}

static int vp_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
		       struct virtqueue *vqs[],
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
		       struct virtqueue_info vqs_info[],
#else
		       vq_callback_t *callbacks[],
		       const char * const names[],
		       const bool *ctx,
#endif
		       struct irq_affinity *desc)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	u32 irq = vp_get_softirq(vp_dev);
	int i, err, queue_idx = 0;

	pr_info("Find softirq: %d\n", irq);
	if (irq < 0)
		return irq;

	err = parasyte_spore_request_softirq(irq, vp_interrupt, vp_dev);
	if (err)
		return err;

	for (i = 0; i < nvqs; ++i) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
		struct virtqueue_info *vqi = &vqs_info[i];

		if (!vqi->name) {
#else
		if (!names[i]) {
#endif
			vqs[i] = NULL;
			continue;
		}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
		vqs[i] = vp_setup_vq(vdev, queue_idx++, vqi->callback,
				     vqi->name, vqi->ctx);
#else
		vqs[i] = vp_setup_vq(vdev, queue_idx++, callbacks[i], names[i],
				     ctx ? ctx[i] : false);
#endif
		if (IS_ERR(vqs[i])) {
			vp_del_vqs(vdev);
			return PTR_ERR(vqs[i]);
		}
	}

	return 0;
}

static const char *vp_bus_name(struct virtio_device *vdev)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);

	return vp_dev->pdev->name;
}

static bool vp_get_shm_region(struct virtio_device *vdev,
			      struct virtio_shm_region *region, u8 id)
{
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	u64 len, addr;

	/* Select the region we're interested in */
	parasyte_writel(vp_dev, id, VIRTIO_MMIO_SHM_SEL);

	/* Read the region size */
	len = (u64) parasyte_readl(vp_dev, VIRTIO_MMIO_SHM_LEN_LOW);
	len |= (u64) parasyte_readl(vp_dev, VIRTIO_MMIO_SHM_LEN_HIGH) << 32;

	region->len = len;

	/* Check if region length is -1. If that's the case, the shared memory
	 * region does not exist and there is no need to proceed further.
	 */
	if (len == ~(u64)0)
		return false;

	/* Read the region base address */
	addr = (u64) parasyte_readl(vp_dev, VIRTIO_MMIO_SHM_BASE_LOW);
	addr |= (u64) parasyte_readl(vp_dev, VIRTIO_MMIO_SHM_BASE_HIGH) << 32;

	region->addr = addr;

	return true;
}

static const struct virtio_config_ops virtio_parasyte_config_ops = {
	.get		= vp_get,
	.set		= vp_set,
	.generation	= vp_generation,
	.get_status	= vp_get_status,
	.set_status	= vp_set_status,
	.reset		= vp_reset,
	.find_vqs	= vp_find_vqs,
	.del_vqs	= vp_del_vqs,
	.get_features	= vp_get_features,
	.finalize_features = vp_finalize_features,
	.bus_name	= vp_bus_name,
	.get_shm_region = vp_get_shm_region,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
	.synchronize_cbs = vp_synchronize_cbs,
#endif
};

#ifdef CONFIG_PM_SLEEP
static int virtio_parasyte_freeze(struct device *dev)
{
	struct virtio_parasyte_device *vp_dev = dev_get_drvdata(dev);

	return virtio_device_freeze(&vp_dev->vdev);
}

static int virtio_parasyte_restore(struct device *dev)
{
	struct virtio_parasyte_device *vp_dev = dev_get_drvdata(dev);

	if (vp_dev->version == 1)
		parasyte_writel(vp_dev, PAGE_SIZE, VIRTIO_MMIO_GUEST_PAGE_SIZE);

	return virtio_device_restore(&vp_dev->vdev);
}

static const struct dev_pm_ops virtio_parasyte_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(virtio_parasyte_freeze, virtio_parasyte_restore)
};
#endif

static void virtio_parasyte_release_dev(struct device *_d)
{
	struct virtio_device *vdev =
			container_of(_d, struct virtio_device, dev);
	struct virtio_parasyte_device *vp_dev = to_virtio_parasyte_device(vdev);
	parasyte_ring_free_msg_client(vp_dev->client);

	kfree(vp_dev);
}

/* Platform device */

static int virtio_parasyte_probe(struct platform_device *pdev)
{
	struct virtio_parasyte_device *vp_dev;
	unsigned long magic;
	int rc;

	vp_dev = kzalloc(sizeof(*vp_dev), GFP_KERNEL);
	if (!vp_dev)
		return -ENOMEM;

	vp_dev->client = parasyte_ring_allocate_msg_client();
	if (!vp_dev->client) {
		rc = -ENOMEM;
		goto free_vp_dev;
	}

	vp_dev->vdev.dev.parent = &pdev->dev;
	vp_dev->vdev.dev.release = virtio_parasyte_release_dev;
	vp_dev->vdev.config = &virtio_parasyte_config_ops;
	vp_dev->pdev = pdev;

	vp_dev->res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!vp_dev->res) {
		rc = -ENODEV;
		goto free_client;
	}

	/* Check magic value */
	magic = parasyte_readl(vp_dev, VIRTIO_MMIO_MAGIC_VALUE);
	if (magic != ('v' | 'i' << 8 | 'r' << 16 | 't' << 24)) {
		dev_warn(&pdev->dev, "Wrong magic value 0x%08lx!\n", magic);
		rc = -ENODEV;
		goto free_client;
	}

	/* Check device version */
	vp_dev->version = parasyte_readl(vp_dev, VIRTIO_MMIO_VERSION);
	if (vp_dev->version < 1 || vp_dev->version > 2) {
		dev_err(&pdev->dev, "Version %ld not supported!\n",
				vp_dev->version);
		rc = -ENXIO;
		goto free_client;
	}

	vp_dev->vdev.id.device = parasyte_readl(vp_dev, VIRTIO_MMIO_DEVICE_ID);
	if (vp_dev->vdev.id.device == 0) {
		/*
		 * virtio-mmio device with an ID 0 is a (dummy) placeholder
		 * with no function. End probing now with no error reported.
		 */
		rc = -ENODEV;
		goto free_client;
	}
	vp_dev->vdev.id.vendor = parasyte_readl(vp_dev, VIRTIO_MMIO_VENDOR_ID);

	if (vp_dev->version == 1) {
		parasyte_writel(vp_dev, PAGE_SIZE, VIRTIO_MMIO_GUEST_PAGE_SIZE);

		rc = dma_set_mask(&pdev->dev, DMA_BIT_MASK(64));
		/*
		 * In the legacy case, ensure our coherently-allocated virtio
		 * ring will be at an address expressable as a 32-bit PFN.
		 */
		if (!rc)
			dma_set_coherent_mask(&pdev->dev,
					      DMA_BIT_MASK(32 + PAGE_SHIFT));
	} else {
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	}
	if (rc)
		rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (rc)
		dev_warn(&pdev->dev, "Failed to enable 64-bit or 32-bit DMA.  Trying to continue, but this might not work.\n");

	platform_set_drvdata(pdev, vp_dev);

	rc = register_virtio_device(&vp_dev->vdev);
	if (rc)
		put_device(&vp_dev->vdev.dev);

	return rc;

free_client:
	parasyte_ring_free_msg_client(vp_dev->client);
free_vp_dev:
	kfree(vp_dev);
	return rc;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static void virtio_parasyte_remove(struct platform_device *pdev)
#else
static int virtio_parasyte_remove(struct platform_device *pdev)
#endif
{
	struct virtio_parasyte_device *vp_dev = platform_get_drvdata(pdev);
	unregister_virtio_device(&vp_dev->vdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	return 0;
#endif
}

/* Platform driver */

static const struct of_device_id virtio_parasyte_match[] = {
	{ .compatible = "virtio,parasyte", },
	{},
};
MODULE_DEVICE_TABLE(of, virtio_parasyte_match);

static struct platform_driver virtio_parasyte_driver = {
	.probe		= virtio_parasyte_probe,
	.remove		= virtio_parasyte_remove,
	.driver		= {
		.name	= "virtio-parasyte",
		.of_match_table	= virtio_parasyte_match,
#ifdef CONFIG_PM_SLEEP
		.pm	= &virtio_parasyte_pm_ops,
#endif
	},
};

static int __init virtio_parasyte_init(void)
{
	pr_info("init virtio parasyte module\n");
	return platform_driver_register(&virtio_parasyte_driver);
}

static void __exit virtio_parasyte_exit(void)
{
	platform_driver_unregister(&virtio_parasyte_driver);
}

module_init(virtio_parasyte_init);
module_exit(virtio_parasyte_exit);

MODULE_LICENSE("GPL");
