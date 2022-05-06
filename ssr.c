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

static int __init ssr_init(void)
{
    return 0;
}

static void __exit ssr_exit(void)
{

}

module_init(ssr_init);
module_exit(ssr_exit);
