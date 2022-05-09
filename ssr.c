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

struct pretty_bio {
struct work_struct work;
struct bio *bio;
};

static struct pretty_block_dev {
struct gendisk *gd;

struct block_device *phys_bdev_1;
struct block_device *phys_bdev_2;

struct workqueue_struct *queue;
} pretty_dev;

void locate_crc_on_disks(unsigned long long data_sector, unsigned long long *crc_sector, unsigned long long *crc_offset)
{
	unsigned long long crc_address;

	crc_address = LOGICAL_DISK_SIZE + data_sector * sizeof(unsigned int);
	*crc_sector = LOGICAL_DISK_SECTORS + data_sector / (KERNEL_SECTOR_SIZE / 4);
	*crc_offset = crc_address - KERNEL_SECTOR_SIZE * (*crc_sector);
}

struct page *read_sector_crc_from_disk(struct gendisk *gd, unsigned long long crc_sector)
{
	struct bio *bio_sector_crc;
	struct page *page_crc;

	bio_sector_crc = bio_alloc(GFP_NOIO, 1);						// alloc bio to read sector

	bio_sector_crc->bi_disk = gd;									// set gendisk
	bio_sector_crc->bi_opf = 0;										// set operation type as READ
	bio_sector_crc->bi_iter.bi_sector = crc_sector;					// set sector

	page_crc = alloc_page(GFP_NOIO);

	bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);

	submit_bio_wait(bio_sector_crc);								// submit bio

	bio_put(bio_sector_crc);

	return page_crc;
}

void modify_sector_crc_on_disk(struct gendisk *gd, struct page *page_crc, unsigned long long crc_sector)
{
	struct bio *bio_sector_crc = bio_alloc(GFP_NOIO, 1);

	bio_sector_crc->bi_disk = gd;									 // set gendisk
	bio_sector_crc->bi_opf = 1;                                      // set operation type as READ
	bio_sector_crc->bi_iter.bi_sector = crc_sector;                  // set sector
	bio_add_page(bio_sector_crc, page_crc, KERNEL_SECTOR_SIZE, 0);

	submit_bio_wait(bio_sector_crc);                                 // submit bio

	bio_put(bio_sector_crc);
}

unsigned int compute_crc(struct page *page, int len)
{
	unsigned int checksum;
	char *buffer_data;

	buffer_data = kmap_atomic(page);
	checksum = crc32(0, (unsigned char *)buffer_data + len, KERNEL_SECTOR_SIZE);

	kunmap_atomic(buffer_data);

	return checksum;
}

void compute_and_modify_crc_on_disks(struct gendisk *gd1, struct gendisk *gd2, struct bio *my_bio)
{
	struct bio_vec bvec;
	struct bvec_iter i;

	int j;
	unsigned long long number_sectors_in_bvec;
	unsigned long long crc_sector, crc_offset;

	char *buffer_crc;
	unsigned int checksum;

	struct page *page_crc;

	bio_for_each_segment(bvec, my_bio, i) {
		/* bvec.bv_len can be > KERNEL_SECTOR_SIZE => in one read, multiple adiacent sectors can be read =>
		 * => itterate through each sector in a bvec_page and compute crc for each sector
		 */
		number_sectors_in_bvec = bvec.bv_len / KERNEL_SECTOR_SIZE;

		for (j = 0; j < number_sectors_in_bvec; j++) {
			checksum = compute_crc(bvec.bv_page, bvec.bv_offset + j * KERNEL_SECTOR_SIZE);

			locate_crc_on_disks(i.bi_sector + j,  &crc_sector, &crc_offset);

			page_crc = read_sector_crc_from_disk(pretty_dev.phys_bdev_1->bd_disk, crc_sector);

			buffer_crc = kmap_atomic(page_crc);

			memcpy(buffer_crc + crc_offset, &checksum, sizeof(unsigned int));  // write new CRC in CRC page

			kunmap_atomic(buffer_crc);

			modify_sector_crc_on_disk(pretty_dev.phys_bdev_1->bd_disk, page_crc, crc_sector);
			modify_sector_crc_on_disk(pretty_dev.phys_bdev_2->bd_disk, page_crc, crc_sector);

			__free_page(page_crc);
		}
	}
}

void retransmit_bio_data_on_disk(struct gendisk *gd, struct bio *my_bio, int dir)
{
	struct bio *new_bio = bio_alloc(GFP_KERNEL, 1);                 // alloc bio

	memcpy(new_bio, my_bio, sizeof(struct bio));
	bio_copy_data(new_bio, my_bio);
	new_bio->bi_disk = gd;											// set gendisk
	new_bio->bi_iter.bi_sector = my_bio->bi_iter.bi_sector;         // set sector
	new_bio->bi_opf = dir;                                          // set operation type

	submit_bio_wait(new_bio);

	bio_put(new_bio);
}

struct page *read_sector_data_from_disk(struct gendisk *gd, unsigned long long sector, int len)
{
	struct bio *bio_data_disk1;
	struct page *page_data_disk1;

	bio_data_disk1 = bio_alloc(GFP_NOIO, 1);						// alloc bio to read sector data

	bio_data_disk1->bi_disk = gd;									// set gendisk
	bio_data_disk1->bi_opf = 0;										// set operation type as READ
	bio_data_disk1->bi_iter.bi_sector = sector;						// set sector

	page_data_disk1 = alloc_page(GFP_KERNEL);

	bio_add_page(bio_data_disk1, page_data_disk1, len, 0);

	submit_bio_wait(bio_data_disk1);

	bio_put(bio_data_disk1);

	return page_data_disk1;
}

void write_from_disk_to_disk(struct page *page_src_disk, struct gendisk *gd_dest, unsigned long long sector)
{
	struct bio *bio_disk_dest;
	struct page *page_disk_dest;
	char *buffer_disk_src, *buffer_disk_dest;

	bio_disk_dest = bio_alloc(GFP_NOIO, 1);							// alloc bio to read sector data

	bio_disk_dest->bi_disk = gd_dest;								// set gendisk
	bio_disk_dest->bi_opf = 1;										// set operation type as WRITE
	bio_disk_dest->bi_iter.bi_sector = sector;						// set sector

	page_disk_dest = alloc_page(GFP_KERNEL);

	bio_add_page(bio_disk_dest, page_disk_dest, KERNEL_SECTOR_SIZE, 0);

	/* copy data from src disk page sector into dest disk page sector */
	buffer_disk_src = kmap_atomic(page_src_disk);
	buffer_disk_dest = kmap_atomic(page_disk_dest);

	memcpy(buffer_disk_dest, buffer_disk_src, KERNEL_SECTOR_SIZE);
	kunmap_atomic(buffer_disk_src);
	kunmap_atomic(buffer_disk_dest);

	submit_bio_wait(bio_disk_dest);

	bio_put(bio_disk_dest);
	__free_page(page_disk_dest);
}

void work_handler(struct work_struct *work)
{
	struct pretty_bio *bio_struct = container_of(work, struct pretty_bio, work);
	int err = 0, j;

	struct bio *my_bio = bio_struct->bio;

	struct page *page_data_disk1, *page_data_disk2, *page_crc_disk1, *page_crc_disk2;
	unsigned long long data_sector, crc_sector, crc_offset, number_sectors_in_bvec;
	unsigned int checksum_disk1, checksum_disk2;

	char *initial_buffer, *buffer_data_disk1, *buffer_data_disk2, *buffer_crc_disk1, *buffer_crc_disk2;

	int dir = bio_data_dir(my_bio);

	if (dir == REQ_OP_WRITE) {
		/* WRITE BIO */
		retransmit_bio_data_on_disk(pretty_dev.phys_bdev_1->bd_disk, my_bio, dir);
		retransmit_bio_data_on_disk(pretty_dev.phys_bdev_2->bd_disk, my_bio, dir);

		compute_and_modify_crc_on_disks(pretty_dev.phys_bdev_1->bd_disk, pretty_dev.phys_bdev_2->bd_disk, my_bio);

	} else {
		/* READ BIO */
		struct bio_vec bvec;
		struct bvec_iter i;

		bio_for_each_segment(bvec, my_bio, i) {
			/* read data from disk1 */
			page_data_disk1 = read_sector_data_from_disk(pretty_dev.phys_bdev_1->bd_disk, i.bi_sector, bvec.bv_len);

			/* bvec.bv_len can be > KERNEL_SECTOR_SIZE => in one read, multiple adiacent sectors can be read =>
			 * => itterate through each sector in a bvec_page and compute crc for each sector
			 */
			number_sectors_in_bvec = bvec.bv_len / KERNEL_SECTOR_SIZE;

			for (j = 0; j < number_sectors_in_bvec; j++) {
				checksum_disk1 = compute_crc(page_data_disk1, j * KERNEL_SECTOR_SIZE);

				data_sector = i.bi_sector + j;
				locate_crc_on_disks(data_sector,  &crc_sector, &crc_offset);

				/* read crc sector from disk1 */
				page_crc_disk1 = read_sector_crc_from_disk(pretty_dev.phys_bdev_1->bd_disk, crc_sector);

				buffer_crc_disk1 = kmap_atomic(page_crc_disk1);

				if (memcmp(buffer_crc_disk1 + crc_offset, &checksum_disk1, sizeof(unsigned int)) == 0) {
					/* DATA IS CORRECT ON DISK1 */
					kunmap_atomic(buffer_crc_disk1);

					/* copy data from disk1's data page to initial bio's page */
					initial_buffer = kmap_atomic(bvec.bv_page);
					buffer_data_disk1 = kmap_atomic(page_data_disk1);

					memcpy(initial_buffer + bvec.bv_offset + j * KERNEL_SECTOR_SIZE, buffer_data_disk1 + j * KERNEL_SECTOR_SIZE, KERNEL_SECTOR_SIZE);

					kunmap_atomic(buffer_data_disk1);
					kunmap_atomic(initial_buffer);

					/* VERIFY DATA IS CORRECT ON DISK2 as well, if not => recover from DISK1 */

					/* read data from disk2 */
					page_data_disk2 = read_sector_data_from_disk(pretty_dev.phys_bdev_2->bd_disk, data_sector, bvec.bv_len);

					checksum_disk2 = compute_crc(page_data_disk2, 0);

					if (memcmp(&checksum_disk1, &checksum_disk2, sizeof(unsigned int)) != 0) {
						/* INCORRECT DATA ON DISK 2 => copy from DISK1 on DISK2 */

						/* write sector on disk2 */
						write_from_disk_to_disk(page_data_disk1, pretty_dev.phys_bdev_2->bd_disk, data_sector);

						/* write crc sector data into disk2 crc sector */
						write_from_disk_to_disk(page_crc_disk1, pretty_dev.phys_bdev_2->bd_disk, crc_sector);
					}

				} else {
					/* DATA IS INCORRECT ON DISK1 */
					kunmap_atomic(buffer_crc_disk1);

					/* read data from disk2 */
					page_data_disk2 = read_sector_data_from_disk(pretty_dev.phys_bdev_2->bd_disk, data_sector, bvec.bv_len);

					checksum_disk2 = compute_crc(page_data_disk2, 0);

					/* read crc from disk2 */
					page_crc_disk2 = read_sector_crc_from_disk(pretty_dev.phys_bdev_2->bd_disk, crc_sector);

					buffer_crc_disk2 = kmap_atomic(page_crc_disk2);

					if (memcmp(buffer_crc_disk2 + crc_offset, &checksum_disk2, sizeof(unsigned int)) == 0) {
						/* CRC CORRECT ON DISK2 => start disk1 recovery */
						kunmap_atomic(buffer_crc_disk2);

						/* copy data in original bio_vec page */
						initial_buffer = kmap_atomic(bvec.bv_page);
						buffer_data_disk2 = kmap_atomic(page_data_disk2);

						memcpy(initial_buffer + bvec.bv_offset + j * KERNEL_SECTOR_SIZE, buffer_data_disk2, KERNEL_SECTOR_SIZE);

						kunmap_atomic(buffer_data_disk2);
						kunmap_atomic(initial_buffer);

						/* write sector on disk1 */
						write_from_disk_to_disk(page_data_disk2, pretty_dev.phys_bdev_1->bd_disk, data_sector);

						/* write crc sector data into disk1 crc sector */
						write_from_disk_to_disk(page_crc_disk2, pretty_dev.phys_bdev_1->bd_disk, crc_sector);

					} else {
						/* INCORRECT DATA ON DISK1 and DISK2 */
						kunmap_atomic(buffer_crc_disk2);
						err = 1;
					}

					__free_page(page_data_disk2);
					__free_page(page_crc_disk2);
				}

				__free_page(page_crc_disk1);
			}

			__free_page(page_data_disk1);
		}
	}

	if (err == 1)
		bio_io_error(my_bio);
	else
		bio_endio(my_bio);
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
	struct pretty_bio *new_bio;

	new_bio = kmalloc(sizeof(struct pretty_bio), GFP_ATOMIC);
	if (new_bio == NULL)
		return -1;

	new_bio->bio = bio;
	INIT_WORK(&new_bio->work, work_handler);

	/* add work to queue */
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
		pr_err("alloc_disk: failure\n");
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
		pr_err("unable to register mybdev block device\n");
		return -EBUSY;
	}

	err = create_block_device(&pretty_dev);
	if (err)
		goto out;

	pretty_dev.phys_bdev_1 = open_disk(PHYSICAL_DISK1_NAME);
	if (pretty_dev.phys_bdev_1 == NULL) {
		pr_err("%s No such device\n", PHYSICAL_DISK1_NAME);
		err = -EINVAL;
		goto out_delete_logical_block_device;
	}

	pretty_dev.phys_bdev_2 = open_disk(PHYSICAL_DISK2_NAME);
	if (pretty_dev.phys_bdev_2 == NULL) {
		pr_err("%s No such device\n", PHYSICAL_DISK2_NAME);
		err = -EINVAL;
		goto out_close_phys_block_device;
	}

	/* init work_queue */
	pretty_dev.queue = create_singlethread_workqueue("pretty_queue");

	return 0;

out_close_phys_block_device:
	close_disk(pretty_dev.phys_bdev_1);
	close_disk(pretty_dev.phys_bdev_2);

out_delete_logical_block_device:
	delete_block_device(&pretty_dev);

out:
	unregister_blkdev(SSR_MAJOR, "ssr");
	return err;
}

static void __exit ssr_exit(void)
{
	close_disk(pretty_dev.phys_bdev_1);
	close_disk(pretty_dev.phys_bdev_2);

	delete_block_device(&pretty_dev);
	unregister_blkdev(SSR_MAJOR, "ssr");
}

module_init(ssr_init);
module_exit(ssr_exit);
