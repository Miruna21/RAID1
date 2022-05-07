// SPDX-License-Identifier: GPL
  /*
   * ssr.c - RAID1 driver
   *
   * Author: Grigorie Ruxandra <ruxi.grigorie@gmail.com>
   * Author: Orzata Miruna Narcisa <mirunaorzata21@gmail.com>
   */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#include "ssr.h"

MODULE_AUTHOR("Grigorie Ruxandra <ruxi.grigorie@gmail.com");
MODULE_AUTHOR("Orzata Miruna Narcisa <mirunaorzata21@gmail.com");
MODULE_DESCRIPTION("RAID1 Driver");
MODULE_LICENSE("GPL");

struct pretty_bio_list {
    struct list_head list;

    struct bio *bio;
};

static struct pretty_block_dev {
	struct gendisk *gd;
  struct block_device *phys_bdev_1;
  struct block_device *phys_bdev_2;

  struct work_struct work;
  struct workqueue_struct *queue;

  struct list_head head;
  struct pretty_bio_list bios;
  spinlock_t list_lock;

} pretty_dev;

void work_handler(struct work_struct *work)
{
  struct pretty_block_dev *dev;

	dev = container_of(work, struct pretty_block_dev, work);

  /* ia din lista un bio */
  struct list_head *p,*q;
  struct pretty_bio_list *mc;

  pr_info("handler\n");

  list_for_each_safe(p, q, &dev->head) {
    mc = list_entry(p, struct pretty_bio_list, list);
    pr_info("ceva\n");
    
    list_del(p);
    break;
  }
  pr_info("dupa list foreach\n");

  /* READ:
    read from both disks
    read CRC from both disks
    calculate CRC from both disks
    check read CRC == calculated CRC
    if CRC wrong on a partition, but correct on the other: write info => + 2 bios (CRC + data)
  */

  /* WRITE:
    calculezi CRC
    scrii datele pe ambele discuri
    scrii CRC pe ambele discuri
  */

  // submit_bio_wait(bio);

  // bio_end(bio_original);
  bio_endio(mc->bio);
  kfree(mc);
}

static int pretty_block_open(struct block_device *bdev, fmode_t mode)
{
	return 0;
}

static void pretty_block_release(struct gendisk *gd, fmode_t mode)
{
}

static blk_qc_t pretty_submit_bio(struct bio *bio) {
  struct pretty_bio_list *new_bio;

  new_bio = kvmalloc(sizeof(*new_bio), GFP_ATOMIC);
	if (new_bio == NULL)
		return -1;

  new_bio->bio = bio;

  // add bio to bio_list
	list_add(&new_bio->list, pretty_dev.head.prev);

  // add work to queue
  queue_work(pretty_dev.queue, &pretty_dev.work);
  
  return 0;
}

static const struct block_device_operations pretty_block_ops = {
	.owner = THIS_MODULE,
	.open = pretty_block_open,
	.release = pretty_block_release,
  .submit_bio = pretty_submit_bio,
};


static int create_block_device(struct pretty_block_dev *dev)
{
  int err = 0;
  

	/* initialize the gendisk structure */
	dev->gd = alloc_disk(SSR_NUM_MINORS);
	if (!dev->gd) {
		printk(KERN_ERR "alloc_disk: failure\n");
		err = -ENOMEM;
		goto out;
	}
  blk_alloc_queue(NUMA_NO_NODE);

	dev->gd->major = SSR_MAJOR;
	dev->gd->first_minor = SSR_FIRST_MINOR;
	dev->gd->fops = &pretty_block_ops;
	dev->gd->private_data = dev;
  dev->gd->queue = blk_alloc_queue(NUMA_NO_NODE);
  
	snprintf(dev->gd->disk_name, 4, "ssr");
  pr_info("%s\n", dev->gd->disk_name);
	set_capacity(dev->gd, LOGICAL_DISK_SECTORS);

	add_disk(dev->gd);

  return 0;

out:
	return err;
}

static struct block_device *open_disk(char *name)
{
	struct block_device *bdev;

	bdev = blkdev_get_by_path(name, FMODE_READ | FMODE_WRITE | FMODE_EXCL, THIS_MODULE);

	return bdev;
}

static void close_disk(struct block_device *bdev)
{
	blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
}

static void delete_block_device(struct pretty_block_dev *dev)
{
	if (dev->gd) {
		del_gendisk(dev->gd);
		put_disk(dev->gd);
	}
}

static int __init ssr_init(void)
{
  int err = 0;

  err = register_blkdev(SSR_MAJOR, "ssr");
  if (err < 0) {
    printk(KERN_ERR "unable to register mybdev block device\n");
    return -EBUSY;
  }

  err = create_block_device(&pretty_dev);
  if (err)
    goto out;

  pretty_dev.phys_bdev_1 = open_disk(PHYSICAL_DISK1_NAME);
  if (pretty_dev.phys_bdev_1 == NULL) {
    printk(KERN_ERR "%s No such device\n", PHYSICAL_DISK1_NAME);
    err = -EINVAL;
    goto out_delete_logical_block_device;
  }

  pretty_dev.phys_bdev_2 = open_disk(PHYSICAL_DISK2_NAME);
  if (pretty_dev.phys_bdev_2 == NULL) {
    printk(KERN_ERR "%s No such device\n", PHYSICAL_DISK2_NAME);
    err = -EINVAL;
    goto out_close_phys_block_device;
  }

  // init work_queue
  pretty_dev.queue = create_singlethread_workqueue("pretty_queue");

  INIT_LIST_HEAD(&pretty_dev.head);
  INIT_WORK(&pretty_dev.work, work_handler);

  spin_lock_init(&pretty_dev.list_lock);

  return 0;

  out_close_phys_block_device:
    close_disk(pretty_dev.phys_bdev_1);
    close_disk(pretty_dev.phys_bdev_2);

  out_delete_logical_block_device:
    delete_block_device(&pretty_dev);

  out:
    unregister_blkdev(SSR_MAJOR, LOGICAL_DISK_NAME);
    return err;
  }


static void __exit ssr_exit(void)
{

  close_disk(pretty_dev.phys_bdev_1);
  close_disk(pretty_dev.phys_bdev_2);

  delete_block_device(&pretty_dev);
	unregister_blkdev(SSR_MAJOR, LOGICAL_DISK_NAME);

}

module_init(ssr_init);
module_exit(ssr_exit);
