// SPDX-License-Identifier: GPL-2.0
/*
 * Image Server Driver - Provides memory mapping interface with RMM
 *
 * Copyright (C) 2023 Your Name
 */
#include <asm/rsi.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/ioctl.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/memremap.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include "image-server.h"

#define DRIVER_NAME "image-server"
#define DRIVER_VERSION "1.0"
#define DEVICE_NAME "image-server"
#define IMG_MAGIC 'i'
#define IMG_IOCTL_GET_RD_IPA _IOR(IMG_MAGIC, 1, struct rd_ipa_size_data)
#define IMG_IOCTL_MAP_IPA _IOW(IMG_MAGIC, 2, struct map_ipa_data)
#define IMG_IOCTL_LOAD_FILE _IOWR(IMG_MAGIC, 3, struct load_file_data)
#define IMG_IOCTL_WRITE_FILE _IOW(IMG_MAGIC, 4, struct write_file_data)

static int major;
static struct cdev img_cdev;
static struct class *img_class;

static phys_addr_t reserved_mem_phys;
static size_t reserved_mem_size;
static void *reserved_mem;

void get_ripas_from_range(unsigned long base, unsigned long end)
{
	int ret;
	enum ripas state;
	phys_addr_t top;
	unsigned long ipa = base;

	while (ipa < end) {
		ret = rsi_ipa_state_get(ipa, end, &state, &top);
		if (ret)
			break;
		ipa = top;
	}
}

int get_rd_addr(unsigned long *rd_addr)
{
	int ret = 0;

	if (!rd_addr) {
		pr_err("%s: Invalid output pointer\n", DRIVER_NAME);
		return -EINVAL;
	}

	ret = rsi_get_rd_addr(rd_addr);
	if (ret) {
		pr_err("%s: Failed to get RD address, err %d\n", DRIVER_NAME,
		       ret);
		return ret;
	}

	if (!*rd_addr) {
		pr_warn("%s: Got zero RD address\n", DRIVER_NAME);
		return -ENODATA;
	}

	pr_info("%s: Got RD address: 0x%lx\n", DRIVER_NAME, *rd_addr);
	return 0;
}

int map_mem(unsigned long guest_rd_addr, unsigned long guest_ipa,
	    unsigned long map_size)
{
	int ret;

	if (map_size > reserved_mem_size) {
		pr_err("%s: Requested map size exceeds reserved memory\n",
		       DRIVER_NAME);
		return -ENOMEM;
	}

	ret = rsi_map_mem(guest_rd_addr, guest_ipa, reserved_mem_phys,
			  map_size);
	if (ret) {
		pr_err("%s: Failed to map mem to guest cvm, err %d\n",
		       DRIVER_NAME, ret);
		return ret;
	}

	return 0;
}

int load_file(struct load_file_data *load_file_request)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;
	loff_t size;

	filp = filp_open(load_file_request->file_path, O_RDONLY, 0);
	if (IS_ERR(filp)) {
		pr_err("%s: Failed to open file %s: %ld\n", DRIVER_NAME,
		       load_file_request->file_path, PTR_ERR(filp));
		return PTR_ERR(filp);
	}

	size = i_size_read(file_inode(filp));
	pr_info("%s: Load file size: %lld\n", DRIVER_NAME, size);

	if (size <= 0 || (size_t)size > reserved_mem_size) {
		filp_close(filp, NULL);
		return -ENOMEM;
	}

	ret = kernel_read(filp, reserved_mem, size, &pos);
	if (ret != size) {
		pr_err("%s: File read incomplete: expected %lld, got %zd\n",
		       DRIVER_NAME, size, ret);
		filp_close(filp, NULL);
		return -EIO;
	}

	filp_close(filp, NULL);

	load_file_request->file_size = size;
	return 0;
}

int write_file(struct write_file_data *write_file_request)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t ret;

	if (write_file_request->file_size == 0 ||
	    write_file_request->file_size > reserved_mem_size) {
		pr_err("%s: Invalid write file size\n", DRIVER_NAME);
		return -EINVAL;
	}

	filp = filp_open(write_file_request->file_path,
			 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (IS_ERR(filp)) {
		pr_err("%s: Failed to open %s for write\n", DRIVER_NAME,
		       write_file_request->file_path);
		return PTR_ERR(filp);
	}

	ret = kernel_write(filp, reserved_mem, write_file_request->file_size,
			   &pos);
	if (ret < 0) {
		pr_err("%s: kernel_write failed: %zd\n", DRIVER_NAME, ret);
		filp_close(filp, NULL);
		return ret;
	}

	filp_close(filp, NULL);
	return 0;
}

static int reserved_init(void)
{
	reserved_mem =
		memremap(reserved_mem_phys, reserved_mem_size, MEMREMAP_WB);
	if (!reserved_mem) {
		pr_err("%s: Failed to memremap reserved memory\n", DRIVER_NAME);
		return -ENOMEM;
	}

	pr_info("%s: Reserved memory mapped at %p (phys 0x%llx, size 0x%zx)\n",
		DRIVER_NAME, reserved_mem, (u64)reserved_mem_phys,
		reserved_mem_size);

	return 0;
}

static long img_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rd_ipa_size_data rd_ipa_size_response;
	struct map_ipa_data map_ipa_request;
	struct load_file_data load_file_request;
	struct write_file_data write_file_request;
	int ret = 0;

	switch (cmd) {
	case IMG_IOCTL_GET_RD_IPA:
		ret = get_rd_addr(&rd_ipa_size_response.rd_addr);
		if (ret)
			break;

		rd_ipa_size_response.ipa_start = reserved_mem_phys;
		rd_ipa_size_response.ipa_size = reserved_mem_size;

		if (copy_to_user((struct rd_ipa_size_data __user *)arg,
				 &rd_ipa_size_response,
				 sizeof(rd_ipa_size_response))) {
			ret = -EFAULT;
		}
		break;

	case IMG_IOCTL_MAP_IPA:
		if (copy_from_user(&map_ipa_request,
				   (struct map_ipa_data __user *)arg,
				   sizeof(map_ipa_request))) {
			ret = -EFAULT;
			break;
		}

		ret = map_mem(map_ipa_request.guest_rd_addr,
			      map_ipa_request.guest_ipa,
			      map_ipa_request.map_size);
		break;

	case IMG_IOCTL_LOAD_FILE:
		if (copy_from_user(&load_file_request,
				   (struct load_file_data __user *)arg,
				   sizeof(load_file_request))) {
			ret = -EFAULT;
			break;
		}

		ret = load_file(&load_file_request);
		if (ret)
			break;

		if (copy_to_user((struct load_file_data __user *)arg,
				 &load_file_request,
				 sizeof(load_file_request))) {
			ret = -EFAULT;
		}
		break;

	case IMG_IOCTL_WRITE_FILE:
		if (copy_from_user(&write_file_request,
				   (struct write_file_data __user *)arg,
				   sizeof(write_file_request))) {
			ret = -EFAULT;
			break;
		}

		ret = write_file(&write_file_request);
		break;

	default:
		ret = -ENOTTY;
	}

	return ret;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = img_ioctl,
};

static int __init image_server_init(void)
{
	struct device_node *mem_np;
	struct resource mem_res;
	dev_t dev;
	int ret;

	pr_info("%s v%s initializing\n", DRIVER_NAME, DRIVER_VERSION);

	mem_np = of_find_compatible_node(NULL, NULL, "shared-dma-pool");
	if (!mem_np) {
		pr_err("%s: No 'shared-dma-pool' node found in device tree\n",
		       DRIVER_NAME);
		return -ENODEV;
	}

	ret = of_address_to_resource(mem_np, 0, &mem_res);
	of_node_put(mem_np);
	if (ret) {
		pr_err("%s: Failed to get reserved memory resource from device tree\n",
		       DRIVER_NAME);
		return ret;
	}

	reserved_mem_phys = mem_res.start;
	reserved_mem_size = resource_size(&mem_res);
	ret = rsi_set_reserved_memory(reserved_mem_phys, reserved_mem_size);
	if (ret) {
		pr_err("%s: Failed to set reserved memory in RSI (%d)\n",
		       DRIVER_NAME, ret);
	}
	pr_info("%s: Reserved memory from DT: phys 0x%llx, size 0x%zx\n",
		DRIVER_NAME, (u64)reserved_mem_phys, reserved_mem_size);

	ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	if (ret < 0) {
		pr_err("%s: Failed to allocate device number\n", DRIVER_NAME);
		return ret;
	}

	major = MAJOR(dev);

	cdev_init(&img_cdev, &fops);
	ret = cdev_add(&img_cdev, dev, 1);
	if (ret < 0) {
		unregister_chrdev_region(dev, 1);
		pr_err("%s: Failed to add cdev\n", DRIVER_NAME);
		return ret;
	}

	img_class = class_create("image-server");
	if (IS_ERR(img_class)) {
		cdev_del(&img_cdev);
		unregister_chrdev_region(dev, 1);
		pr_err("%s: Failed to create device class\n", DRIVER_NAME);
		return PTR_ERR(img_class);
	}

	device_create(img_class, NULL, dev, NULL, "image-server");

	ret = reserved_init();
	if (ret < 0) {
		device_destroy(img_class, dev);
		class_destroy(img_class);
		cdev_del(&img_cdev);
		unregister_chrdev_region(dev, 1);
		return ret;
	}

	pr_info("%s v%s initialized successfully\n", DRIVER_NAME,
		DRIVER_VERSION);
	return 0;
}

static void __exit image_server_exit(void)
{
	dev_t dev = MKDEV(major, 0);

	device_destroy(img_class, dev);
	class_destroy(img_class);
	cdev_del(&img_cdev);
	unregister_chrdev_region(dev, 1);

	if (reserved_mem)
		memunmap(reserved_mem);

	pr_info("%s v%s unloaded\n", DRIVER_NAME, DRIVER_VERSION);
}

module_init(image_server_init);
module_exit(image_server_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MZH");
MODULE_DESCRIPTION("Image server Driver for memory mapping with RMM");
MODULE_VERSION(DRIVER_VERSION);