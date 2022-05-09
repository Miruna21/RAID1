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

void modify_crc_on_disk(struct bvec_iter i, struct bio_vec bvec)
{
	/* bvec.bv_len can be > KERNEL_SECTOR_SIZE => in one read, multiple adiacent sectors can be read =>
	 * => itterate through each sector in a bvec_page and compute crc for each sector
	 */
	int j;
	unsigned long long number_sectors_in_bvec = bvec.bv_len / KERNEL_SECTOR_SIZE;

	for (j = 0; j < number_sectors_in_bvec; j++) {
		char *buffer_data = kmap_atomic(bvec.bv_page);
		unsigned int checksum = crc32(0, (unsigned char *)buffer_data +  bvec.bv_offset +  j * KERNEL_SECTOR_SIZE, KERNEL_SECTOR_SIZE);

		kunmap_atomic(buffer_data);

		unsigned long long current_sector = i.bi_sector + j;
		unsigned long long crc_address = LOGICAL_DISK_SIZE + current_sector * sizeof(unsigned int);
		unsigned long long crc_sector = LOGICAL_DISK_SECTORS + current_sector / (KERNEL_SECTOR_SIZE / 4);
		unsigned long long crc_offset = crc_address - KERNEL_SECTOR_SIZE * crc_sector;

		struct bio *bio_sector_crc = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector

		bio_sector_crc->bi_disk = pretty_dev.phys_bdev_1->bd_disk;       // set gendisk
		bio_sector_crc->bi_opf = 0;                                      // set operation type as READ
		bio_sector_crc->bi_iter.bi_sector = crc_sector;                  // set sector

		struct page *page_crc = alloc_page(GFP_NOIO);

		bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);

		submit_bio_wait(bio_sector_crc);
		bio_put(bio_sector_crc);                             // submit bio

		char *buffer_crc = kmap_atomic(page_crc);

		memcpy(buffer_crc + crc_offset, &checksum, sizeof(unsigned int));  // write new CRC in CRC page
		kunmap_atomic(buffer_crc);                                         // cleanup page

		bio_sector_crc = bio_alloc(GFP_NOIO, 1);

		bio_sector_crc->bi_disk = pretty_dev.phys_bdev_1->bd_disk;       // set gendisk
		bio_sector_crc->bi_opf = 1;                                      // set operation type as READ
		bio_sector_crc->bi_iter.bi_sector = crc_sector;                  // set sector
		bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);
		submit_bio_wait(bio_sector_crc);                                 // submit bio
		bio_put(bio_sector_crc);

		bio_sector_crc = bio_alloc(GFP_NOIO, 1);
		bio_sector_crc->bi_disk = pretty_dev.phys_bdev_2->bd_disk;       // set gendisk
		bio_sector_crc->bi_opf = 1;                                      // set operation type as READ
		bio_sector_crc->bi_iter.bi_sector = crc_sector;                  // set sector
		bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);

		submit_bio_wait(bio_sector_crc);                                 // submit bio

		bio_put(bio_sector_crc);

		__free_page(page_crc);
	}
}

void work_handler(struct work_struct *work)
{
	struct pretty_bio_list *bio_struct = container_of(work, struct pretty_bio_list, work);

	struct bio *mc = bio_struct->bio;
	int dir = bio_data_dir(mc);

	if (dir == REQ_OP_WRITE) {
		// create data bios and resubmit them
		struct bio *bio_1 = bio_alloc(GFP_KERNEL, 1);                 // alloc bio

		memcpy(bio_1, mc, sizeof(struct bio));
		bio_copy_data(bio_1, mc);
		bio_1->bi_disk = pretty_dev.phys_bdev_1->bd_disk;             // set gendisk
		bio_1->bi_iter.bi_sector = mc->bi_iter.bi_sector;             // set sector
		bio_1->bi_opf = dir;                                          // set operation type

		struct bio *bio_2 = bio_alloc(GFP_KERNEL, 1);                 // alloc bio

		memcpy(bio_2, mc, sizeof(struct bio));                        // copy bio information in new bio
		bio_copy_data(bio_2, mc);
		bio_2->bi_disk = pretty_dev.phys_bdev_2->bd_disk;             // set gendisk
		bio_2->bi_iter.bi_sector = mc->bi_iter.bi_sector;             // set sector
		bio_2->bi_opf = dir;                                          // set operation type

		submit_bio_wait(bio_1);
		submit_bio_wait(bio_2);

		struct bio_vec bvec;
		struct bvec_iter i;

		// crc bios
		bio_for_each_segment(bvec, mc, i) {
			modify_crc_on_disk(i, bvec);
		}

		bio_put(bio_1);
		bio_put(bio_2);

	} else {

		struct bio_vec bvec;
		struct bvec_iter i;

		// crc bios
		bio_for_each_segment(bvec, mc, i) {

			// read data from disk1
			struct bio *bio_data_disk1 = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector data

			bio_data_disk1->bi_disk = pretty_dev.phys_bdev_1->bd_disk;       // set gendisk
			bio_data_disk1->bi_opf = 0;                                      // set operation type as READ
			bio_data_disk1->bi_iter.bi_sector = i.bi_sector;                 // set sector

			struct page *page_data_disk1 = alloc_page(GFP_KERNEL);

			bio_add_page(bio_data_disk1, page_data_disk1, bvec.bv_len, 0);

			submit_bio_wait(bio_data_disk1);

			/* bvec.bv_len can be > KERNEL_SECTOR_SIZE => in one read, multiple adiacent sectors can be read =>
			 * => itterate through each sector in a bvec_page and compute crc for each sector
			 */
			int j;
			unsigned long long number_sectors_in_bvec = bvec.bv_len / KERNEL_SECTOR_SIZE;

			for (j = 0; j < number_sectors_in_bvec; j++) {
				char *buffer_data_disk1 = kmap_atomic(page_data_disk1);
				unsigned int checksum_disk1 = crc32(0, (unsigned char *)buffer_data_disk1 + j * KERNEL_SECTOR_SIZE, KERNEL_SECTOR_SIZE);

				kunmap_atomic(buffer_data_disk1);

				unsigned long long current_sector = i.bi_sector + j;
				unsigned long long crc_address = LOGICAL_DISK_SIZE + current_sector * sizeof(unsigned int);
				unsigned long long crc_sector = LOGICAL_DISK_SECTORS + current_sector / (KERNEL_SECTOR_SIZE / 4);
				unsigned long long crc_offset = crc_address - KERNEL_SECTOR_SIZE * crc_sector;

				// READ CRC SECTOR FROM DISK 1 START
				struct bio *bio_sector_crc_disk1 = bio_alloc(GFP_NOIO, 1);                   // alloc bio to read sector

				bio_sector_crc_disk1->bi_disk = pretty_dev.phys_bdev_1->bd_disk;             // set gendisk
				bio_sector_crc_disk1->bi_opf = 0;                                            // set operation type as READ
				bio_sector_crc_disk1->bi_iter.bi_sector = crc_sector;                        // set sector

				struct page *page_crc_disk1 = alloc_page(GFP_NOIO);

				bio_add_page(bio_sector_crc_disk1, page_crc_disk1, KERNEL_SECTOR_SIZE, 0);

				submit_bio_wait(bio_sector_crc_disk1);

				// READ CRC SECTOR FROM DISK 1 END
				char *buffer_crc_disk1 = kmap_atomic(page_crc_disk1);
				// DATA IS CORRECT ON DISK1
				if (memcmp(buffer_crc_disk1 + crc_offset, &checksum_disk1, sizeof(unsigned int)) == 0) {

					kunmap_atomic(buffer_crc_disk1);

					char *initial_buffer = kmap_atomic(bvec.bv_page);

					buffer_data_disk1 = kmap_atomic(page_data_disk1);

					memcpy(initial_buffer + bvec.bv_offset + j * KERNEL_SECTOR_SIZE, buffer_data_disk1 + j * KERNEL_SECTOR_SIZE, KERNEL_SECTOR_SIZE);

					kunmap_atomic(buffer_data_disk1);
					kunmap_atomic(initial_buffer);

					// VERIFY DATA IS CORRECT ON DISK2 as well, if not => recover from DISK1

					// read data from disk2 START
					struct bio *bio_sector_data_disk2 = bio_alloc(GFP_NOIO, 1);                                      // alloc bio to read sector data

					bio_sector_data_disk2->bi_disk = pretty_dev.phys_bdev_2->bd_disk;                                // set gendisk
					bio_sector_data_disk2->bi_opf = 0;                                                               // set operation type as READ
					bio_sector_data_disk2->bi_iter.bi_sector = current_sector;                                       // set sector

					struct page *page_data_disk2 = alloc_page(GFP_KERNEL);

					bio_add_page(bio_sector_data_disk2, page_data_disk2, KERNEL_SECTOR_SIZE, 0);

					submit_bio_wait(bio_sector_data_disk2);                                                          // submit bio

					char *buffer_data_disk2 = kmap_atomic(page_data_disk2);                                          // map page with read data
					unsigned int checksum_disk2 = crc32(0, (unsigned char *)buffer_data_disk2, KERNEL_SECTOR_SIZE);  // compute check-sum

					kunmap_atomic(buffer_data_disk2);
					// read data from disk2 END

					// INCORRECT DATA ON DISK 2 => copy from DISK1 on DISK2
					if (memcmp(&checksum_disk1, &checksum_disk2, sizeof(unsigned int)) != 0) {

						// write sector on disk2
						struct bio *bio_sector_data_disk2_write = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector data

						bio_sector_data_disk2_write->bi_disk = pretty_dev.phys_bdev_2->bd_disk;       // set gendisk
						bio_sector_data_disk2_write->bi_opf = 1;                                      // set operation type as WRITE
						bio_sector_data_disk2_write->bi_iter.bi_sector = current_sector;              // set sector

						struct page *page_data_disk2_write = alloc_page(GFP_KERNEL);

						bio_add_page(bio_sector_data_disk2_write, page_data_disk2_write, KERNEL_SECTOR_SIZE, 0);

						// copy data from disk1 page sector into disk2 page sector
						buffer_data_disk1 = kmap_atomic(page_data_disk1);
						char *page_data_disk2_buffer_data = kmap_atomic(page_data_disk2_write);

						memcpy(page_data_disk2_buffer_data, buffer_data_disk1, KERNEL_SECTOR_SIZE);
						kunmap_atomic(buffer_data_disk1);
						kunmap_atomic(page_data_disk2_buffer_data);

						submit_bio_wait(bio_sector_data_disk2_write);

						bio_put(bio_sector_data_disk2_write);
						__free_page(page_data_disk2_write);

						// write crc sector data into disk2 crc sector
						struct bio *bio_sector_crc_disk2_write = bio_alloc(GFP_NOIO, 1);                 // alloc bio to read sector data

						bio_sector_crc_disk2_write->bi_disk = pretty_dev.phys_bdev_2->bd_disk;           // set gendisk
						bio_sector_crc_disk2_write->bi_opf = 1;                                          // set operation type as WRITE

						bio_sector_crc_disk2_write->bi_iter.bi_sector = crc_sector;                      // set sector

						struct page *page_crc_disk2_write = alloc_page(GFP_KERNEL);

						bio_add_page(bio_sector_crc_disk2_write, page_crc_disk2_write, KERNEL_SECTOR_SIZE, 0);

						// copy data from disk1 crc sector page to disk2 crc sector page
						buffer_crc_disk1 = kmap_atomic(page_crc_disk1);

						char *page_data_disk2_crc_data = kmap_atomic(page_crc_disk2_write);

						memcpy(page_data_disk2_crc_data, buffer_crc_disk1, KERNEL_SECTOR_SIZE);
						kunmap_atomic(buffer_crc_disk1);
						kunmap_atomic(page_data_disk2_crc_data);

						submit_bio_wait(bio_sector_crc_disk2_write);

						bio_put(bio_sector_crc_disk2_write);
						__free_page(page_crc_disk2_write);
					}

				} else {

					kunmap_atomic(buffer_crc_disk1);

					// read data from disk2 START
					struct bio *bio_sector_data_disk2 = bio_alloc(GFP_NOIO, 1);                                      // alloc bio to read sector data

					bio_sector_data_disk2->bi_disk = pretty_dev.phys_bdev_2->bd_disk;                                // set gendisk
					bio_sector_data_disk2->bi_opf = 0;                                                               // set operation type as READ
					bio_sector_data_disk2->bi_iter.bi_sector = current_sector;                                       // set sector

					struct page *page_data_disk2 = alloc_page(GFP_KERNEL);

					bio_add_page(bio_sector_data_disk2, page_data_disk2, KERNEL_SECTOR_SIZE, 0);

					submit_bio_wait(bio_sector_data_disk2);                                                          // submit bio

					char *buffer_data_disk2 = kmap_atomic(page_data_disk2);                                          // map page with read data
					unsigned int checksum_disk2 = crc32(0, (unsigned char *)buffer_data_disk2, KERNEL_SECTOR_SIZE);  // compute check-sum

					kunmap_atomic(buffer_data_disk2);
					// read data from disk2 END

					// read crc from disk2 START
					struct bio *bio_sector_crc_disk2 = bio_alloc(GFP_NOIO, 1);               // alloc bio to read sector data

					bio_sector_crc_disk2->bi_disk = pretty_dev.phys_bdev_2->bd_disk;         // set gendisk
					bio_sector_crc_disk2->bi_opf = 0;                                        // set operation type as READ
					bio_sector_crc_disk2->bi_iter.bi_sector = crc_sector;                    // set sector

					struct page *page_crc_disk2 = alloc_page(GFP_NOIO);

					bio_add_page(bio_sector_crc_disk2, page_crc_disk2, KERNEL_SECTOR_SIZE, 0);

					submit_bio_wait(bio_sector_crc_disk2);                                   // submit bio
					// read crc from disk2 END

					char *buffer_crc_disk2 = kmap_atomic(page_crc_disk2);

					// CRC CORRECT ON DISK2 => start recovery
					if (memcmp(buffer_crc_disk2 + crc_offset, &checksum_disk2, sizeof(unsigned int)) == 0) {
						kunmap_atomic(buffer_crc_disk2);

						// copy data in original bio_vec page
						char *initial_buffer = kmap_atomic(bvec.bv_page);

						buffer_data_disk2 = kmap_atomic(page_data_disk2);

						memcpy(initial_buffer + bvec.bv_offset + j * KERNEL_SECTOR_SIZE, buffer_data_disk2, KERNEL_SECTOR_SIZE);

						kunmap_atomic(buffer_data_disk2);
						kunmap_atomic(initial_buffer);

						// write sector on disk1
						struct bio *bio_sector_data_disk1_write = bio_alloc(GFP_NOIO, 1);             // alloc bio to read sector data

						bio_sector_data_disk1_write->bi_disk = pretty_dev.phys_bdev_1->bd_disk;       // set gendisk
						bio_sector_data_disk1_write->bi_opf = 1;                                      // set operation type as WRITE
						bio_sector_data_disk1_write->bi_iter.bi_sector = current_sector;              // set sector

						struct page *page_data_disk1_write = alloc_page(GFP_KERNEL);

						bio_add_page(bio_sector_data_disk1_write, page_data_disk1_write, KERNEL_SECTOR_SIZE, 0);

						// copy data from disk2 page sector into disk1 page sector
						buffer_data_disk2 = kmap_atomic(page_data_disk2);

						char *page_data_disk1_buffer_data = kmap_atomic(page_data_disk1_write);

						memcpy(page_data_disk1_buffer_data, buffer_data_disk2, KERNEL_SECTOR_SIZE);
						kunmap_atomic(buffer_data_disk2);
						kunmap_atomic(page_data_disk1_buffer_data);

						submit_bio_wait(bio_sector_data_disk1_write);

						bio_put(bio_sector_data_disk1_write);
						__free_page(page_data_disk1_write);

						// write crc sector data into disk1 crc sector
						struct bio *bio_sector_crc_disk1_write = bio_alloc(GFP_NOIO, 1);                 // alloc bio to read sector data

						bio_sector_crc_disk1_write->bi_disk = pretty_dev.phys_bdev_1->bd_disk;           // set gendisk
						bio_sector_crc_disk1_write->bi_opf = 1;                                          // set operation type as WRITE

						bio_sector_crc_disk1_write->bi_iter.bi_sector = crc_sector;                      // set sector

						struct page *page_crc_disk1_write = alloc_page(GFP_KERNEL);

						bio_add_page(bio_sector_crc_disk1_write, page_crc_disk1_write, KERNEL_SECTOR_SIZE, 0);

						// copy data from disk2 crc sector page to disk1 crc sector page
						buffer_crc_disk2 = kmap_atomic(page_crc_disk2);

						char *page_data_disk1_crc_data = kmap_atomic(page_crc_disk1_write);

						memcpy(page_data_disk1_crc_data, buffer_crc_disk2, KERNEL_SECTOR_SIZE);
						kunmap_atomic(buffer_crc_disk2);
						kunmap_atomic(page_data_disk1_crc_data);

						submit_bio_wait(bio_sector_crc_disk1_write);

						bio_put(bio_sector_crc_disk1_write);
						__free_page(page_crc_disk1_write);

					} else {
						kunmap_atomic(buffer_crc_disk2);
					}

					bio_put(bio_sector_data_disk2);
					__free_page(page_data_disk2);
					bio_put(bio_sector_crc_disk2);
					__free_page(page_crc_disk2);
				}

				bio_put(bio_sector_crc_disk1);
				__free_page(page_crc_disk1);
			}

			bio_put(bio_data_disk1);
			__free_page(page_data_disk1);
		}
	}

	bio_endio(mc);
}

static int pretty_block_open(struct block_device *bdev, fmode_t mode)
{
	return 0;
}

static void pretty_block_release(struct gendisk *gd, fmode_t mode)
{
}

static blk_qc_t pretty_submit_bio(struct bio *bio)
{
	struct pretty_bio_list *new_bio;

	new_bio = kmalloc(sizeof(struct pretty_bio_list), GFP_ATOMIC);
	if (new_bio == NULL)
		return -1;

	new_bio->bio = bio;
	INIT_WORK(&new_bio->work, work_handler);

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
		print_err("alloc_disk: failure\n");
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
		print_err("unable to register mybdev block device\n");
		return -EBUSY;
	}

	err = create_block_device(&pretty_dev);
	if (err)
		goto out;

	pretty_dev.phys_bdev_1 = open_disk(PHYSICAL_DISK1_NAME);
	if (pretty_dev.phys_bdev_1 == NULL) {
		print_err("%s No such device\n", PHYSICAL_DISK1_NAME);
		err = -EINVAL;
		goto out_delete_logical_block_device;
	}

	pretty_dev.phys_bdev_2 = open_disk(PHYSICAL_DISK2_NAME);
	if (pretty_dev.phys_bdev_2 == NULL) {
		print_err("%s No such device\n", PHYSICAL_DISK2_NAME);
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
