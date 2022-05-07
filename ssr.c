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
#include <linux/crc32.h>
#include "ssr.h"

MODULE_AUTHOR("Grigorie Ruxandra <ruxi.grigorie@gmail.com");
MODULE_AUTHOR("Orzata Miruna Narcisa <mirunaorzata21@gmail.com");
MODULE_DESCRIPTION("RAID1 Driver");
MODULE_LICENSE("GPL");

struct pretty_bio_list {
    struct list_head list;
    struct work_struct work; 
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

  //struct pretty_block_dev *dev;

	//dev = container_of(work, struct pretty_block_dev, work);
 struct pretty_bio_list *bio_struct = container_of(work, struct pretty_bio_list, work);

  /* ia din lista un bio */
  /*struct list_head *p,*q;
  struct pretty_bio_list *mc;

 // //pr_info("handler\n");

  /*list_for_each_safe(p, q, &dev->head) {
    mc = list_entry(p, struct pretty_bio_list, list);
    list_del(p);
    break;
  }*/
  ////pr_info("dupa list foreach\n");

  

  struct bio *mc = bio_struct->bio;
  int dir = bio_data_dir(mc);
  // WRITE
  if (dir == REQ_OP_WRITE) {
    
    //pr_info ("Write bio\n");
    // create data bios

    struct bio *bio_1 = bio_alloc(GFP_KERNEL, 1);             // alloc bio
    memcpy(bio_1, mc, sizeof(struct bio));
    bio_1->bi_disk = pretty_dev.phys_bdev_1->bd_disk;             // set gendisk
    bio_1->bi_iter.bi_sector = mc->bi_iter.bi_sector;  // set sector
    bio_1->bi_opf = dir;                                    // set operation type

    //pr_info("MESAJ BIO 1\n");

    struct bio *bio_2 = bio_alloc(GFP_KERNEL, 1);             // alloc bio
    memcpy(bio_2, mc, sizeof(struct bio));             // copy bio information in new bio
    bio_2->bi_disk = pretty_dev.phys_bdev_2->bd_disk;             // set gendisk
    bio_2->bi_iter.bi_sector = mc->bi_iter.bi_sector;  // set sector
    bio_2->bi_opf = dir;                                    // set operation type
    //pr_info("MESAJ BIO 2\n");


    // send data bios
    submit_bio_wait(bio_1);                                 
    submit_bio_wait(bio_2);

    //pr_info("MESAJ BIO 3\n");
    
    struct bio_vec bvec;
    struct bvec_iter i;

    // crc bios
    bio_for_each_segment(bvec, mc, i) {

      // CONFIGURE BIO TO READ EACH DATA SECTOR IN SEGMENT VECTOR
      struct bio *bio_sector_data = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector data
      bio_sector_data->bi_disk = pretty_dev.phys_bdev_1->bd_disk;             // set gendisk
      bio_sector_data->bi_opf = 0;                                      // set operation type as READ

      sector_t sector = i.bi_sector;  
      bio_sector_data->bi_iter.bi_sector = sector;                      // set sector

      struct page *page_data = alloc_page(GFP_KERNEL);
      bio_add_page(bio_sector_data, page_data, KERNEL_SECTOR_SIZE, 0);

       //pr_info("MESAJ BIO 4\n");
      
      submit_bio_wait(bio_sector_data);                                     // submit bio
      
      char *buffer_data = kmap_atomic(page_data);                                 // map page with read data

      unsigned int checksum = crc32(0, (unsigned char *)buffer_data, KERNEL_SECTOR_SIZE);  // compute check-sum

      kunmap_atomic(buffer_data);                                            // cleanup page
     
       //pr_info("MESAJ BIO 5\n");

      ////////////////////////
      // read check-sum sector
      long long unsigned crc_address = LOGICAL_DISK_SIZE + sector * sizeof(unsigned int);
      //long long unsigned crc_sector = crc_address % KERNEL_SECTOR_SIZE;

      long long unsigned  crc_sector = LOGICAL_DISK_SECTORS + i.bi_sector / (KERNEL_SECTOR_SIZE / 4);
      long long unsigned crc_offset = crc_address - KERNEL_SECTOR_SIZE * crc_sector; 

  

      // CONFIGURE BIO TO READ CRC SECTOR FOR EACH SEGMENT IN VEC
      struct bio *bio_sector_crc = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector data
      ////pr_info("bio addr: %x\n", bio_sector_crc);
     
      bio_sector_crc->bi_disk = pretty_dev.phys_bdev_1->bd_disk;             // set gendisk
      bio_sector_crc->bi_opf = 0;                                      // set operation type as READ

      bio_sector_crc->bi_iter.bi_sector = crc_sector;                  // set sector

      struct page *page_crc = alloc_page(GFP_NOIO);
      bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);


       //pr_info("MESAJ BIO 6\n");

      submit_bio_wait(bio_sector_crc);                                 // submit bio
       
      char *buffer_crc = kmap_atomic(page_crc);  
      //pr_info("Offset: %lld\n", crc_offset);
      memcpy(buffer_crc + crc_offset, &checksum, sizeof(unsigned int));  // write new CRC in CRC page
      kunmap_atomic(buffer_crc);                                            // cleanup page
     

      //pr_info("MESAJ BIO 7\n");
      // CONFIGURE BIO TO MODIFY CRC SECTOR
      bio_sector_crc->bi_opf = 1;                                      // set operation type as READ
      
      

      submit_bio_wait(bio_sector_crc);                                 // submit bio


      bio_put(bio_sector_crc);
      __free_page(page_crc);
       bio_put(bio_sector_data);
      __free_page(page_data);


      ////////////////////////////

     

      ////pr_info("In iter: bio - dir_sus: %d, dir_jos: %d, sector: %d, offset: %u, size: %u\n", dir,  bio->bi_opf, sector, offset, len);
       
    }
  
  } else {
        //pr_info ("Read bio\n");
   

      /* Do each segment independently. */
      /*
      bio_for_each_segment(bvec, bio, i) {
          sector_t sector = i.bi_sector;
          
          unsigned long offset = bvec.bv_offset;
          size_t len = bvec.bv_len;

          //pr_info("In iter: bio - dir_sus: %d, dir_jos: %d, sector: %d, offset: %u, size: %u\n", dir,  bio->bi_opf, sector, offset, len);
      }*/

  }




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
  bio_endio(mc);
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

  new_bio = kmalloc(sizeof(struct pretty_bio_list), GFP_ATOMIC);
	if (new_bio == NULL)
		return -1;

  new_bio->bio = bio;
  INIT_WORK(&new_bio->work, work_handler);

  // add bio to bio_list
	//list_add(&new_bio->list, pretty_dev.head.prev);

  // add work to queue
  queue_work(pretty_dev.queue, &new_bio->work);
  
  return BLK_QC_T_NONE;
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
  //pr_info("%s\n", dev->gd->disk_name);
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
