/*  Xenbus code for blkif backend
    Copyright (C) 2005 Rusty Russell <rusty@rustcorp.com.au>
    Copyright (C) 2005 XenSource Ltd

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

*/

#include <stdarg.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#include "common.h"

extern unsigned int xen_blkif_max_rings;

struct backend_info {
	struct xenbus_device	*dev;
	struct xen_blkif	*blkif;
	struct xenbus_watch	backend_watch;
	unsigned		major;
	unsigned		minor;
	char			*mode;
};

static struct kmem_cache *xen_blkif_cachep;
static void connect(struct backend_info *);
static int connect_ring(struct backend_info *);
static void backend_changed(struct xenbus_watch *, const char **,
			    unsigned int);
static void xen_ring_free(struct xen_blkif_ring *ring);
static void xen_vbd_free(struct xen_vbd *vbd);

struct xenbus_device *xen_blkbk_xenbus(struct backend_info *be)
{
	return be->dev;
}

/*
 * The last request could free the device from softirq context and
 * xen_ring_free() can sleep.
 */
static void xen_ring_deferred_free(struct work_struct *work)
{
	struct xen_blkif_ring *ring;

	ring = container_of(work, struct xen_blkif_ring, free_work);
	xen_ring_free(ring);
}

static int blkback_name(struct xen_blkif *blkif, char *buf, bool save_space)
{
	char *devpath, *devname;
	struct xenbus_device *dev = blkif->be->dev;

	devpath = xenbus_read(XBT_NIL, dev->nodename, "dev", NULL);
	if (IS_ERR(devpath))
		return PTR_ERR(devpath);

	devname = strstr(devpath, "/dev/");
	if (devname != NULL)
		devname += strlen("/dev/");
	else
		devname  = devpath;

	if (save_space)
		snprintf(buf, TASK_COMM_LEN, "blkbk.%d.%s", blkif->domid, devname);
	else
		snprintf(buf, TASK_COMM_LEN, "blkback.%d.%s", blkif->domid, devname);
	kfree(devpath);

	return 0;
}

static void xen_update_blkif_status(struct xen_blkif *blkif)
{
	int i, err;
	char name[TASK_COMM_LEN], per_ring_name[TASK_COMM_LEN];
	struct xen_blkif_ring *ring;

	/*
	 * Not ready to connect? Check irq of first ring as the others
	 * should all be the same.
	 */
	if (!blkif->rings || !blkif->rings[0].irq || !blkif->vbd.bdev)
		return;

	/* Already connected? */
	if (blkif->be->dev->state == XenbusStateConnected)
		return;

	/* Attempt to connect: exit if we fail to. */
	connect(blkif->be);
	if (blkif->be->dev->state != XenbusStateConnected)
		return;

	err = blkback_name(blkif, name, blkif->vbd.exposed_hw_queues);
	if (err) {
		xenbus_dev_error(blkif->be->dev, err, "get blkback dev name");
		return;
	}

	err = filemap_write_and_wait(blkif->vbd.bdev->bd_inode->i_mapping);
	if (err) {
		xenbus_dev_error(blkif->be->dev, err, "block flush");
		return;
	}
	invalidate_inode_pages2(blkif->vbd.bdev->bd_inode->i_mapping);

	for (i = 0 ; i < blkif->nr_rings ; i++) {
		ring = &blkif->rings[i];
		if (blkif->vbd.exposed_hw_queues)
			snprintf(per_ring_name, TASK_COMM_LEN, "%s-%d", name, i);
		else {
			BUG_ON(i != 0);
			snprintf(per_ring_name, TASK_COMM_LEN, "%s", name);
		}
		ring->xenblkd = kthread_run(xen_blkif_schedule, ring, "%s", per_ring_name);
		if (IS_ERR(ring->xenblkd)) {
			err = PTR_ERR(ring->xenblkd);
			ring->xenblkd = NULL;
			xenbus_dev_error(blkif->be->dev, err, "start %s", per_ring_name);
			return;
		}
	}
}

static struct xen_blkif_ring *xen_blkif_ring_alloc(struct xen_blkif *blkif,
						   int nr_rings)
{
	int r, i, j;
	struct xen_blkif_ring *rings;
	struct pending_req *req;

	rings = kzalloc(nr_rings * sizeof(struct xen_blkif_ring),
			GFP_KERNEL);
	if (!rings)
		return NULL;

	for (r = 0 ; r < nr_rings ; r++) {
		struct xen_blkif_ring *ring = &rings[r];

		spin_lock_init(&ring->blk_ring_lock);

		init_waitqueue_head(&ring->wq);
		init_waitqueue_head(&ring->shutdown_wq);

		ring->persistent_gnts.rb_node = NULL;
		spin_lock_init(&ring->free_pages_lock);
		INIT_LIST_HEAD(&ring->free_pages);
		INIT_LIST_HEAD(&ring->persistent_purge_list);
		ring->free_pages_num = 0;
		atomic_set(&ring->persistent_gnt_in_use, 0);
		atomic_set(&ring->refcnt, 1);
		atomic_set(&ring->inflight, 0);
		INIT_WORK(&ring->persistent_purge_work, xen_blkbk_unmap_purged_grants);
		spin_lock_init(&ring->pending_free_lock);
		init_waitqueue_head(&ring->pending_free_wq);
		INIT_LIST_HEAD(&ring->pending_free);
		for (i = 0; i < XEN_RING_REQS(nr_rings); i++) {
			req = kzalloc(sizeof(*req), GFP_KERNEL);
			if (!req)
				goto fail;
			list_add_tail(&req->free_list,
				      &ring->pending_free);
			for (j = 0; j < MAX_INDIRECT_SEGMENTS; j++) {
				req->segments[j] = kzalloc(sizeof(*req->segments[0]),
				                           GFP_KERNEL);
				if (!req->segments[j])
					goto fail;
			}
			for (j = 0; j < MAX_INDIRECT_PAGES; j++) {
				req->indirect_pages[j] = kzalloc(sizeof(*req->indirect_pages[0]),
				                                 GFP_KERNEL);
				if (!req->indirect_pages[j])
					goto fail;
			}
		}

		INIT_WORK(&ring->free_work, xen_ring_deferred_free);
		ring->blkif = blkif;
		ring->ring_index = r;

		spin_lock_init(&ring->stats_lock);
		ring->st_print = jiffies;

		atomic_inc(&blkif->refcnt);
	}

	return rings;

fail:
	kfree(rings);
	return NULL;
}

static struct xen_blkif *xen_blkif_alloc(domid_t domid)
{
	struct xen_blkif *blkif;

	BUILD_BUG_ON(MAX_INDIRECT_PAGES > BLKIF_MAX_INDIRECT_PAGES_PER_REQUEST);

	blkif = kmem_cache_zalloc(xen_blkif_cachep, GFP_KERNEL);
	if (!blkif)
		return ERR_PTR(-ENOMEM);

	blkif->domid = domid;
	init_completion(&blkif->drain_complete);
	atomic_set(&blkif->drain, 0);

	return blkif;
}

static int xen_blkif_map(struct xen_blkif_ring *ring, unsigned long shared_page,
			 unsigned int evtchn, unsigned int ring_idx)
{
	int err;
	struct xen_blkif *blkif;
	char dev_name[64];

	/* Already connected through? */
	if (ring->irq)
		return 0;

	blkif = ring->blkif;

	err = xenbus_map_ring_valloc(ring->blkif->be->dev, shared_page, &ring->blk_ring);
	if (err < 0)
		return err;

	switch (blkif->blk_protocol) {
	case BLKIF_PROTOCOL_NATIVE:
	{
		struct blkif_sring *sring;
		sring = (struct blkif_sring *)ring->blk_ring;
		BACK_RING_INIT(&ring->blk_rings.native, sring, PAGE_SIZE);
		break;
	}
	case BLKIF_PROTOCOL_X86_32:
	{
		struct blkif_x86_32_sring *sring_x86_32;
		sring_x86_32 = (struct blkif_x86_32_sring *)ring->blk_ring;
		BACK_RING_INIT(&ring->blk_rings.x86_32, sring_x86_32, PAGE_SIZE);
		break;
	}
	case BLKIF_PROTOCOL_X86_64:
	{
		struct blkif_x86_64_sring *sring_x86_64;
		sring_x86_64 = (struct blkif_x86_64_sring *)ring->blk_ring;
		BACK_RING_INIT(&ring->blk_rings.x86_64, sring_x86_64, PAGE_SIZE);
		break;
	}
	default:
		BUG();
	}

	if (blkif->vbd.exposed_hw_queues)
		snprintf(dev_name, 64, "blkif-backend-%d", ring_idx);
	else
		snprintf(dev_name, 64, "blkif-backend");
	err = bind_interdomain_evtchn_to_irqhandler(blkif->domid, evtchn,
						    xen_blkif_be_int, 0,
						    dev_name, ring);
	if (err < 0) {
		xenbus_unmap_ring_vfree(blkif->be->dev, ring->blk_ring);
		ring->blk_rings.common.sring = NULL;
		return err;
	}
	ring->irq = err;

	return 0;
}

static int xen_blkif_disconnect(struct xen_blkif *blkif)
{
	int i;

	for (i = 0 ; i < blkif->nr_rings ; i++) {
		struct xen_blkif_ring *ring = &blkif->rings[i];
		if (ring->xenblkd) {
			kthread_stop(ring->xenblkd);
			wake_up(&ring->shutdown_wq);
			ring->xenblkd = NULL;
		}

		/* The above kthread_stop() guarantees that at this point we
		 * don't have any discard_io or other_io requests. So, checking
		 * for inflight IO is enough.
		 */
		if (atomic_read(&ring->inflight) > 0)
			return -EBUSY;

		if (ring->irq) {
			unbind_from_irqhandler(ring->irq, ring);
			ring->irq = 0;
		}

		if (ring->blk_rings.common.sring) {
			xenbus_unmap_ring_vfree(blkif->be->dev, ring->blk_ring);
			ring->blk_rings.common.sring = NULL;
		}

		/* Remove all persistent grants and the cache of ballooned pages. */
		xen_blkbk_free_caches(ring);
	}

	return 0;
}

static void xen_blkif_free(struct xen_blkif *blkif)
{
	xen_blkif_disconnect(blkif);
	xen_vbd_free(&blkif->vbd);

	kfree(blkif->rings);

	kmem_cache_free(xen_blkif_cachep, blkif);
}

static void xen_ring_free(struct xen_blkif_ring *ring)
{
	struct pending_req *req, *n;
	int i, j;

	/* Make sure everything is drained before shutting down */
	BUG_ON(ring->persistent_gnt_c != 0);
	BUG_ON(atomic_read(&ring->persistent_gnt_in_use) != 0);
	BUG_ON(ring->free_pages_num != 0);
	BUG_ON(!list_empty(&ring->persistent_purge_list));
	BUG_ON(!list_empty(&ring->free_pages));
	BUG_ON(!RB_EMPTY_ROOT(&ring->persistent_gnts));

	i = 0;
	/* Check that there is no request in use */
	list_for_each_entry_safe(req, n, &ring->pending_free, free_list) {
		list_del(&req->free_list);

		for (j = 0; j < MAX_INDIRECT_SEGMENTS; j++) {
			if (!req->segments[j])
				break;
			kfree(req->segments[j]);
		}
		for (j = 0; j < MAX_INDIRECT_PAGES; j++) {
			if (!req->segments[j])
				break;
			kfree(req->indirect_pages[j]);
		}

		kfree(req);
		i++;
	}
	WARN_ON(i != XEN_RING_REQS(ring->blkif->nr_rings));

	if (atomic_dec_and_test(&ring->blkif->refcnt))
		xen_blkif_free(ring->blkif);
}

int __init xen_blkif_interface_init(void)
{
	xen_blkif_cachep = kmem_cache_create("blkif_cache",
					     sizeof(struct xen_blkif),
					     0, 0, NULL);
	if (!xen_blkif_cachep)
		return -ENOMEM;

	return 0;
}

/*
 *  sysfs interface for VBD I/O requests
 */

#define VBD_SHOW(name, format, args...)					\
	static ssize_t show_##name(struct device *_dev,			\
				   struct device_attribute *attr,	\
				   char *buf)				\
	{								\
		struct xenbus_device *dev = to_xenbus_device(_dev);	\
		struct backend_info *be = dev_get_drvdata(&dev->dev);	\
		struct xen_blkif *blkif = be->blkif;			\
		struct xen_blkif_ring *ring;				\
		int i;							\
									\
		blkif->st_oo_req = 0;					\
		blkif->st_rd_req = 0;					\
		blkif->st_wr_req = 0;					\
		blkif->st_f_req = 0;					\
		blkif->st_ds_req = 0;					\
		blkif->st_rd_sect = 0;					\
		blkif->st_wr_sect = 0;					\
		for (i = 0 ; i < blkif->nr_rings ; i++) {		\
			ring = &blkif->rings[i];			\
			spin_lock_irq(&ring->stats_lock);		\
			blkif->st_oo_req += ring->st_oo_req;		\
			blkif->st_rd_req += ring->st_rd_req;		\
			blkif->st_wr_req += ring->st_wr_req;		\
			blkif->st_f_req += ring->st_f_req;		\
			blkif->st_ds_req += ring->st_ds_req;		\
			blkif->st_rd_sect += ring->st_rd_sect;		\
			blkif->st_wr_sect += ring->st_wr_sect;		\
			spin_unlock_irq(&ring->stats_lock);		\
		}							\
									\
		return sprintf(buf, format, ##args);			\
	}								\
	static DEVICE_ATTR(name, S_IRUGO, show_##name, NULL)

VBD_SHOW(oo_req,  "%llu\n", be->blkif->st_oo_req);
VBD_SHOW(rd_req,  "%llu\n", be->blkif->st_rd_req);
VBD_SHOW(wr_req,  "%llu\n", be->blkif->st_wr_req);
VBD_SHOW(f_req,  "%llu\n", be->blkif->st_f_req);
VBD_SHOW(ds_req,  "%llu\n", be->blkif->st_ds_req);
VBD_SHOW(rd_sect, "%llu\n", be->blkif->st_rd_sect);
VBD_SHOW(wr_sect, "%llu\n", be->blkif->st_wr_sect);

static struct attribute *xen_vbdstat_attrs[] = {
	&dev_attr_oo_req.attr,
	&dev_attr_rd_req.attr,
	&dev_attr_wr_req.attr,
	&dev_attr_f_req.attr,
	&dev_attr_ds_req.attr,
	&dev_attr_rd_sect.attr,
	&dev_attr_wr_sect.attr,
	NULL
};

static struct attribute_group xen_vbdstat_group = {
	.name = "statistics",
	.attrs = xen_vbdstat_attrs,
};

VBD_SHOW(physical_device, "%x:%x\n", be->major, be->minor);
VBD_SHOW(mode, "%s\n", be->mode);

static int xenvbd_sysfs_addif(struct xenbus_device *dev)
{
	int error;

	error = device_create_file(&dev->dev, &dev_attr_physical_device);
	if (error)
		goto fail1;

	error = device_create_file(&dev->dev, &dev_attr_mode);
	if (error)
		goto fail2;

	error = sysfs_create_group(&dev->dev.kobj, &xen_vbdstat_group);
	if (error)
		goto fail3;

	return 0;

fail3:	sysfs_remove_group(&dev->dev.kobj, &xen_vbdstat_group);
fail2:	device_remove_file(&dev->dev, &dev_attr_mode);
fail1:	device_remove_file(&dev->dev, &dev_attr_physical_device);
	return error;
}

static void xenvbd_sysfs_delif(struct xenbus_device *dev)
{
	sysfs_remove_group(&dev->dev.kobj, &xen_vbdstat_group);
	device_remove_file(&dev->dev, &dev_attr_mode);
	device_remove_file(&dev->dev, &dev_attr_physical_device);
}


static void xen_vbd_free(struct xen_vbd *vbd)
{
	if (vbd->bdev)
		blkdev_put(vbd->bdev, vbd->readonly ? FMODE_READ : FMODE_WRITE);
	vbd->bdev = NULL;
}

static int xen_advertise_hw_queues(struct xen_blkif *blkif,
				   struct request_queue *q)
{
	struct xen_vbd *vbd = &blkif->vbd;
	struct xenbus_transaction xbt;
	int err;

	vbd->exposed_hw_queues = xen_blkif_max_rings ?
				 min(xen_blkif_max_rings, num_online_cpus()) :
				 num_online_cpus();

	if (q && q->mq_ops)
		vbd->exposed_hw_queues = min(vbd->exposed_hw_queues,
					     q->nr_hw_queues);

	err = xenbus_transaction_start(&xbt);
	if (err) {
		BUG_ON(!blkif->be);
		xenbus_dev_fatal(blkif->be->dev, err,
				 "starting transaction (requested rings)");
		return err;
	}

	err = xenbus_printf(xbt, blkif->be->dev->nodename, "available_hw_queues", "%u",
			    blkif->vbd.exposed_hw_queues);
	if (err)
		xenbus_dev_error(blkif->be->dev, err, "writing %s/available_hw_queues",
				 blkif->be->dev->nodename);

	xenbus_transaction_end(xbt, 0);

	return err;
}

static int xen_vbd_create(struct xen_blkif *blkif, blkif_vdev_t handle,
			  unsigned major, unsigned minor, int readonly,
			  int cdrom)
{
	struct xen_vbd *vbd;
	struct block_device *bdev;
	struct request_queue *q;
	int err;

	vbd = &blkif->vbd;
	vbd->handle   = handle;
	vbd->readonly = readonly;
	vbd->type     = 0;

	vbd->pdevice  = MKDEV(major, minor);

	bdev = blkdev_get_by_dev(vbd->pdevice, vbd->readonly ?
				 FMODE_READ : FMODE_WRITE, NULL);

	if (IS_ERR(bdev)) {
		DPRINTK("xen_vbd_create: device %08x could not be opened.\n",
			vbd->pdevice);
		return -ENOENT;
	}

	vbd->bdev = bdev;
	if (vbd->bdev->bd_disk == NULL) {
		DPRINTK("xen_vbd_create: device %08x doesn't exist.\n",
			vbd->pdevice);
		xen_vbd_free(vbd);
		return -ENOENT;
	}
	vbd->size = vbd_sz(vbd);

	if (vbd->bdev->bd_disk->flags & GENHD_FL_CD || cdrom)
		vbd->type |= VDISK_CDROM;
	if (vbd->bdev->bd_disk->flags & GENHD_FL_REMOVABLE)
		vbd->type |= VDISK_REMOVABLE;

	q = bdev_get_queue(bdev);
	if (q && q->flush_flags)
		vbd->flush_support = true;

	if (q && blk_queue_secdiscard(q))
		vbd->discard_secure = true;

	err = xen_advertise_hw_queues(blkif, q);
	if (err)
		return -ENOENT;

	DPRINTK("Successful creation of handle=%04x (dom=%u)\n",
		handle, blkif->domid);
	return 0;
}

static int xen_blkbk_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);

	DPRINTK("");

	if (be->major || be->minor)
		xenvbd_sysfs_delif(dev);

	if (be->backend_watch.node) {
		unregister_xenbus_watch(&be->backend_watch);
		kfree(be->backend_watch.node);
		be->backend_watch.node = NULL;
	}

	if (be->blkif) {
		int i = 0;
		xen_blkif_disconnect(be->blkif);
		for (; i < be->blkif->nr_rings ; i++)
			xen_ring_put(&be->blkif->rings[i]);
	}

	dev_set_drvdata(&dev->dev, NULL);
	kfree(be->mode);
	kfree(be);
	return 0;
}

int xen_blkbk_flush_diskcache(struct xenbus_transaction xbt,
			      struct backend_info *be, int state)
{
	struct xenbus_device *dev = be->dev;
	int err;

	err = xenbus_printf(xbt, dev->nodename, "feature-flush-cache",
			    "%d", state);
	if (err)
		dev_warn(&dev->dev, "writing feature-flush-cache (%d)", err);

	return err;
}

static void xen_blkbk_discard(struct xenbus_transaction xbt, struct backend_info *be)
{
	struct xenbus_device *dev = be->dev;
	struct xen_blkif *blkif = be->blkif;
	int err;
	int state = 0, discard_enable;
	struct block_device *bdev = be->blkif->vbd.bdev;
	struct request_queue *q = bdev_get_queue(bdev);

	err = xenbus_scanf(XBT_NIL, dev->nodename, "discard-enable", "%d",
			   &discard_enable);
	if (err == 1 && !discard_enable)
		return;

	if (blk_queue_discard(q)) {
		err = xenbus_printf(xbt, dev->nodename,
			"discard-granularity", "%u",
			q->limits.discard_granularity);
		if (err) {
			dev_warn(&dev->dev, "writing discard-granularity (%d)", err);
			return;
		}
		err = xenbus_printf(xbt, dev->nodename,
			"discard-alignment", "%u",
			q->limits.discard_alignment);
		if (err) {
			dev_warn(&dev->dev, "writing discard-alignment (%d)", err);
			return;
		}
		state = 1;
		/* Optional. */
		err = xenbus_printf(xbt, dev->nodename,
				    "discard-secure", "%d",
				    blkif->vbd.discard_secure);
		if (err) {
			dev_warn(&dev->dev, "writing discard-secure (%d)", err);
			return;
		}
	}
	err = xenbus_printf(xbt, dev->nodename, "feature-discard",
			    "%d", state);
	if (err)
		dev_warn(&dev->dev, "writing feature-discard (%d)", err);
}
int xen_blkbk_barrier(struct xenbus_transaction xbt,
		      struct backend_info *be, int state)
{
	struct xenbus_device *dev = be->dev;
	int err;

	err = xenbus_printf(xbt, dev->nodename, "feature-barrier",
			    "%d", state);
	if (err)
		dev_warn(&dev->dev, "writing feature-barrier (%d)", err);

	return err;
}

/*
 * Entry point to this code when a new device is created.  Allocate the basic
 * structures, and watch the store waiting for the hotplug scripts to tell us
 * the device's physical major and minor numbers.  Switch to InitWait.
 */
static int xen_blkbk_probe(struct xenbus_device *dev,
			   const struct xenbus_device_id *id)
{
	int err;
	struct backend_info *be = kzalloc(sizeof(struct backend_info),
					  GFP_KERNEL);
	if (!be) {
		xenbus_dev_fatal(dev, -ENOMEM,
				 "allocating backend structure");
		return -ENOMEM;
	}
	be->dev = dev;
	dev_set_drvdata(&dev->dev, be);

	be->blkif = xen_blkif_alloc(dev->otherend_id);
	if (IS_ERR(be->blkif)) {
		err = PTR_ERR(be->blkif);
		be->blkif = NULL;
		xenbus_dev_fatal(dev, err, "creating block interface");
		goto fail;
	}

	/* setup back pointer */
	be->blkif->be = be;

	err = xenbus_watch_pathfmt(dev, &be->backend_watch, backend_changed,
				   "%s/%s", dev->nodename, "physical-device");
	if (err)
		goto fail;

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		goto fail;

	return 0;

fail:
	DPRINTK("failed");
	xen_blkbk_remove(dev);
	return err;
}


/*
 * Callback received when the hotplug scripts have placed the physical-device
 * node.  Read it and the mode node, and create a vbd.  If the frontend is
 * ready, connect.
 */
static void backend_changed(struct xenbus_watch *watch,
			    const char **vec, unsigned int len)
{
	int err;
	unsigned major;
	unsigned minor;
	struct backend_info *be
		= container_of(watch, struct backend_info, backend_watch);
	struct xenbus_device *dev = be->dev;
	int cdrom = 0;
	unsigned long handle;
	char *device_type;

	DPRINTK("");

	err = xenbus_scanf(XBT_NIL, dev->nodename, "physical-device", "%x:%x",
			   &major, &minor);
	if (XENBUS_EXIST_ERR(err)) {
		/*
		 * Since this watch will fire once immediately after it is
		 * registered, we expect this.  Ignore it, and wait for the
		 * hotplug scripts.
		 */
		return;
	}
	if (err != 2) {
		xenbus_dev_fatal(dev, err, "reading physical-device");
		return;
	}

	if (be->major | be->minor) {
		if (be->major != major || be->minor != minor)
			pr_warn(DRV_PFX "changing physical device (from %x:%x to %x:%x) not supported.\n",
				be->major, be->minor, major, minor);
		return;
	}

	be->mode = xenbus_read(XBT_NIL, dev->nodename, "mode", NULL);
	if (IS_ERR(be->mode)) {
		err = PTR_ERR(be->mode);
		be->mode = NULL;
		xenbus_dev_fatal(dev, err, "reading mode");
		return;
	}

	device_type = xenbus_read(XBT_NIL, dev->otherend, "device-type", NULL);
	if (!IS_ERR(device_type)) {
		cdrom = strcmp(device_type, "cdrom") == 0;
		kfree(device_type);
	}

	/* Front end dir is a number, which is used as the handle. */
	err = kstrtoul(strrchr(dev->otherend, '/') + 1, 0, &handle);
	if (err)
		return;

	be->major = major;
	be->minor = minor;

	err = xen_vbd_create(be->blkif, handle, major, minor,
			     !strchr(be->mode, 'w'), cdrom);

	if (err)
		xenbus_dev_fatal(dev, err, "creating vbd structure");
	else {
		err = xenvbd_sysfs_addif(dev);
		if (err) {
			xen_vbd_free(&be->blkif->vbd);
			xenbus_dev_fatal(dev, err, "creating sysfs entries");
		}
	}

	if (err) {
		kfree(be->mode);
		be->mode = NULL;
		be->major = 0;
		be->minor = 0;
	} else {
		/* We're potentially connected now */
		xen_update_blkif_status(be->blkif);
	}
}


/*
 * Callback received when the frontend's state changes.
 */
static void frontend_changed(struct xenbus_device *dev,
			     enum xenbus_state frontend_state)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);
	int err;

	DPRINTK("%s", xenbus_strstate(frontend_state));

	switch (frontend_state) {
	case XenbusStateInitialising:
		if (dev->state == XenbusStateClosed) {
			pr_info(DRV_PFX "%s: prepare for reconnect\n",
				dev->nodename);
			xenbus_switch_state(dev, XenbusStateInitWait);
		}
		break;

	case XenbusStateInitialised:
	case XenbusStateConnected:
		/*
		 * Ensure we connect even when two watches fire in
		 * close succession and we miss the intermediate value
		 * of frontend_state.
		 */
		if (dev->state == XenbusStateConnected)
			break;

		/*
		 * Enforce precondition before potential leak point.
		 * xen_blkif_disconnect() is idempotent.
		 */
		err = xen_blkif_disconnect(be->blkif);
		if (err) {
			xenbus_dev_fatal(dev, err, "pending I/O");
			break;
		}

		err = connect_ring(be);
		if (err)
			break;
		xen_update_blkif_status(be->blkif);
		break;

	case XenbusStateClosing:
		xenbus_switch_state(dev, XenbusStateClosing);
		break;

	case XenbusStateClosed:
		xen_blkif_disconnect(be->blkif);
		xenbus_switch_state(dev, XenbusStateClosed);
		if (xenbus_dev_is_online(dev))
			break;
		/* fall through if not online */
	case XenbusStateUnknown:
		/* implies xen_blkif_disconnect() via xen_blkbk_remove() */
		device_unregister(&dev->dev);
		break;

	default:
		xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
				 frontend_state);
		break;
	}
}


/* ** Connection ** */


/*
 * Write the physical details regarding the block device to the store, and
 * switch to Connected state.
 */
static void connect(struct backend_info *be)
{
	struct xenbus_transaction xbt;
	int err;
	struct xenbus_device *dev = be->dev;

	DPRINTK("%s", dev->otherend);

	/* Supply the information about the device the frontend needs */
again:
	err = xenbus_transaction_start(&xbt);
	if (err) {
		xenbus_dev_fatal(dev, err, "starting transaction");
		return;
	}

	/* If we can't advertise it is OK. */
	xen_blkbk_flush_diskcache(xbt, be, be->blkif->vbd.flush_support);

	xen_blkbk_discard(xbt, be);

	xen_blkbk_barrier(xbt, be, be->blkif->vbd.flush_support);

	err = xenbus_printf(xbt, dev->nodename, "feature-persistent", "%u", 1);
	if (err) {
		xenbus_dev_fatal(dev, err, "writing %s/feature-persistent",
				 dev->nodename);
		goto abort;
	}
	err = xenbus_printf(xbt, dev->nodename, "feature-max-indirect-segments", "%u",
			    MAX_INDIRECT_SEGMENTS);
	if (err)
		dev_warn(&dev->dev, "writing %s/feature-max-indirect-segments (%d)",
			 dev->nodename, err);

	err = xenbus_printf(xbt, dev->nodename, "sectors", "%llu",
			    (unsigned long long)vbd_sz(&be->blkif->vbd));
	if (err) {
		xenbus_dev_fatal(dev, err, "writing %s/sectors",
				 dev->nodename);
		goto abort;
	}

	/* FIXME: use a typename instead */
	err = xenbus_printf(xbt, dev->nodename, "info", "%u",
			    be->blkif->vbd.type |
			    (be->blkif->vbd.readonly ? VDISK_READONLY : 0));
	if (err) {
		xenbus_dev_fatal(dev, err, "writing %s/info",
				 dev->nodename);
		goto abort;
	}
	err = xenbus_printf(xbt, dev->nodename, "sector-size", "%lu",
			    (unsigned long)
			    bdev_logical_block_size(be->blkif->vbd.bdev));
	if (err) {
		xenbus_dev_fatal(dev, err, "writing %s/sector-size",
				 dev->nodename);
		goto abort;
	}
	err = xenbus_printf(xbt, dev->nodename, "physical-sector-size", "%u",
			    bdev_physical_block_size(be->blkif->vbd.bdev));
	if (err)
		xenbus_dev_error(dev, err, "writing %s/physical-sector-size",
				 dev->nodename);

	err = xenbus_transaction_end(xbt, 0);
	if (err == -EAGAIN)
		goto again;
	if (err)
		xenbus_dev_fatal(dev, err, "ending transaction");

	err = xenbus_switch_state(dev, XenbusStateConnected);
	if (err)
		xenbus_dev_fatal(dev, err, "%s: switching to Connected state",
				 dev->nodename);

	return;
 abort:
	xenbus_transaction_end(xbt, 1);
}


static int connect_ring(struct backend_info *be)
{
	struct xenbus_device *dev = be->dev;
	struct xen_blkif *blkif = be->blkif;
	unsigned long ring_ref;
	unsigned int evtchn;
	unsigned int pers_grants;
	char protocol[64] = "";
	int i, err;
	char *xspath;
	size_t xspathsize;
	const size_t xenstore_path_ext_size = 11; /* sufficient for "/queue-NNN" */

	DPRINTK("%s", dev->otherend);

	be->blkif->blk_protocol = BLKIF_PROTOCOL_NATIVE;
	err = xenbus_gather(XBT_NIL, dev->otherend, "protocol",
			    "%63s", protocol, NULL);
	if (err)
		strcpy(protocol, "unspecified, assuming native");
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_NATIVE))
		be->blkif->blk_protocol = BLKIF_PROTOCOL_NATIVE;
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_X86_32))
		be->blkif->blk_protocol = BLKIF_PROTOCOL_X86_32;
	else if (0 == strcmp(protocol, XEN_IO_PROTO_ABI_X86_64))
		be->blkif->blk_protocol = BLKIF_PROTOCOL_X86_64;
	else {
		xenbus_dev_fatal(dev, err, "unknown fe protocol %s", protocol);
		err = -1;
		goto fail;
	}
	err = xenbus_gather(XBT_NIL, dev->otherend,
			    "feature-persistent", "%u",
			    &pers_grants, NULL);
	if (err)
		pers_grants = 0;

	be->blkif->vbd.feature_gnt_persistent = pers_grants;
	be->blkif->vbd.overflow_max_grants = 0;

	err = xenbus_scanf(XBT_NIL, dev->otherend, "nr_blk_rings",
			   "%u", &blkif->nr_rings);
	if (err < 0) {
		pr_debug("Advertised %u queues, fronted deaf - using one ring.\n",
			 blkif->vbd.exposed_hw_queues);
		blkif->vbd.exposed_hw_queues = 0;
		blkif->nr_rings = 1;
	} else if (blkif->nr_rings > blkif->vbd.exposed_hw_queues) {
		/* Buggy or malicious guest */
		xenbus_dev_fatal(dev, err,
				 "guest requested %u queues, "
				 "exceeding the maximum of %u",
				 blkif->nr_rings,
				 blkif->vbd.exposed_hw_queues);
		return -ENOENT;
	}

	blkif->rings = xen_blkif_ring_alloc(blkif, blkif->nr_rings);
	if (!blkif->rings)
		return -ENOMEM;


	if (blkif->vbd.exposed_hw_queues == 0) {
		err = xenbus_gather(XBT_NIL, dev->otherend, "ring-ref", "%ld",
				&ring_ref, "event-channel", "%u", &evtchn, NULL);
		if (err) {
			xenbus_dev_fatal(dev, err,
					"reading %s/ring-ref and event-channel",
					dev->otherend);
			goto fail;
		}
		pr_info(DRV_PFX "ring-ref %ld, event-channel %d, protocol %d (%s) %s\n",
				ring_ref, evtchn, be->blkif->blk_protocol, protocol,
				pers_grants ? "persistent grants" : "");
		/* Map the shared frame, irq etc. */
		err = xen_blkif_map(&blkif->rings[0], ring_ref, evtchn, 0);
		if (err) {
			xenbus_dev_fatal(dev, err, "mapping ring-ref %lu evtchn %u",
					ring_ref, evtchn);
			goto fail;
		}
	} else {
		xspathsize = strlen(dev->otherend) + xenstore_path_ext_size;
		xspath = kzalloc(xspathsize, GFP_KERNEL);
		if (!xspath) {
			xenbus_dev_fatal(dev, -ENOMEM, "reading ring references");
			err = -ENOMEM;
			goto fail;
		}

		for (i = 0; i < blkif->nr_rings; i++) {
			memset(xspath, 0, xspathsize);
			snprintf(xspath, xspathsize, "%s/queue-%u", dev->otherend, i);
			err = xenbus_gather(XBT_NIL, xspath, "ring-ref", "%ld",
					&ring_ref, "event-channel", "%u", &evtchn, NULL);
			if (err) {
				xenbus_dev_fatal(dev, err,
						"reading %s %d/ring-ref and event-channel",
						xspath, i);
				kfree(xspath);
				goto fail;
			}

			pr_info(DRV_PFX "ring-ref %ld, event-channel %d, protocol %d (%s) %s\n",
					ring_ref, evtchn, be->blkif->blk_protocol, protocol,
					pers_grants ? "persistent grants" : "");
			/* Map the shared frame, irq etc. */
			err = xen_blkif_map(&blkif->rings[i], ring_ref, evtchn, i);
			if (err) {
				xenbus_dev_fatal(dev, err, "mapping ring-ref %ld evtchn %u",
						ring_ref, evtchn);
				kfree(xspath);
				goto fail;
			}
		}
		kfree(xspath);
	}

	return 0;

fail:
	kfree(blkif->rings);
	return err;
}

static const struct xenbus_device_id xen_blkbk_ids[] = {
	{ "vbd" },
	{ "" }
};

static struct xenbus_driver xen_blkbk_driver = {
	.ids  = xen_blkbk_ids,
	.probe = xen_blkbk_probe,
	.remove = xen_blkbk_remove,
	.otherend_changed = frontend_changed
};

int xen_blkif_xenbus_init(void)
{
	return xenbus_register_backend(&xen_blkbk_driver);
}
