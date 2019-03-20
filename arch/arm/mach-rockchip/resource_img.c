/*
 * (C) Copyright 2017 Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */
#include <common.h>
#include <adc.h>
#include <asm/io.h>
#include <malloc.h>
#include <sysmem.h>
#include <linux/list.h>
#include <asm/arch/resource_img.h>
#include <boot_rkimg.h>
#include <dm/ofnode.h>
#ifdef CONFIG_ANDROID_AB
#include <android_avb/libavb_ab.h>
#include <android_avb/rk_avb_ops_user.h>
#endif
#ifdef CONFIG_ANDROID_BOOT_IMAGE
#include <android_bootloader.h>
#include <android_image.h>
#endif

DECLARE_GLOBAL_DATA_PTR;

#define PART_RESOURCE			"resource"
#define RESOURCE_MAGIC			"RSCE"
#define RESOURCE_MAGIC_SIZE		4
#define RESOURCE_VERSION		0
#define CONTENT_VERSION			0
#define ENTRY_TAG			"ENTR"
#define ENTRY_TAG_SIZE			4
#define MAX_FILE_NAME_LEN		256

/*
 *         resource image structure
 * ----------------------------------------------
 * |                                            |
 * |    header  (1 block)                       |
 * |                                            |
 * ---------------------------------------------|
 * |                      |                     |
 * |    entry0  (1 block) |                     |
 * |                      |                     |
 * ------------------------                     |
 * |                      |                     |
 * |    entry1  (1 block) | contents (n blocks) |
 * |                      |                     |
 * ------------------------                     |
 * |    ......            |                     |
 * ------------------------                     |
 * |                      |                     |
 * |    entryn  (1 block) |                     |
 * |                      |                     |
 * ----------------------------------------------
 * |                                            |
 * |    file0  (x blocks)                       |
 * |                                            |
 * ----------------------------------------------
 * |                                            |
 * |    file1  (y blocks)                       |
 * |                                            |
 * ----------------------------------------------
 * |                   ......                   |
 * |---------------------------------------------
 * |                                            |
 * |    filen  (z blocks)                       |
 * |                                            |
 * ----------------------------------------------
 */

/**
 * struct resource_image_header
 *
 * @magic: should be "RSCE"
 * @version: resource image version, current is 0
 * @c_version: content version, current is 0
 * @blks: the size of the header ( 1 block = 512 bytes)
 * @c_offset: contents offset(by block) in the image
 * @e_blks: the size(by block) of the entry in the contents
 * @e_num: numbers of the entrys.
 */

struct resource_img_hdr {
	char		magic[4];
	uint16_t	version;
	uint16_t	c_version;
	uint8_t		blks;
	uint8_t		c_offset;
	uint8_t		e_blks;
	uint32_t	e_nums;
};

struct resource_entry {
	char		tag[4];
	char		name[MAX_FILE_NAME_LEN];
	uint32_t	f_offset;
	uint32_t	f_size;
};

struct resource_file {
	char		name[MAX_FILE_NAME_LEN];
	uint32_t	f_offset;
	uint32_t	f_size;
	struct list_head link;
	uint32_t 	rsce_base;	/* Base addr of resource */
};

static LIST_HEAD(entrys_head);

static int resource_image_check_header(const struct resource_img_hdr *hdr)
{
	int ret;

	ret = memcmp(RESOURCE_MAGIC, hdr->magic, RESOURCE_MAGIC_SIZE);
	if (ret) {
		printf("bad resource image magic: %s\n",
		       hdr->magic ? hdr->magic : "none");
		ret = -EINVAL;
	}
	debug("resource image header:\n");
	debug("magic:%s\n", hdr->magic);
	debug("version:%d\n", hdr->version);
	debug("c_version:%d\n", hdr->c_version);
	debug("blks:%d\n", hdr->blks);
	debug("c_offset:%d\n", hdr->c_offset);
	debug("e_blks:%d\n", hdr->e_blks);
	debug("e_num:%d\n", hdr->e_nums);

	return ret;
}

static int add_file_to_list(struct resource_entry *entry, int rsce_base)
{
	struct resource_file *file;

	if (memcmp(entry->tag, ENTRY_TAG, ENTRY_TAG_SIZE)) {
		printf("invalid entry tag\n");
		return -ENOENT;
	}
	file = malloc(sizeof(*file));
	if (!file) {
		printf("out of memory\n");
		return -ENOMEM;
	}
	strcpy(file->name, entry->name);
	file->rsce_base = rsce_base;
	file->f_offset = entry->f_offset;
	file->f_size = entry->f_size;
	list_add_tail(&file->link, &entrys_head);
	debug("entry:%p  %s offset:%d size:%d\n",
	      entry, file->name, file->f_offset, file->f_size);

	return 0;
}

static int init_resource_list(struct resource_img_hdr *hdr)
{
	struct resource_entry *entry;
	void *content;
	int size;
	int ret;
	int e_num;
	int offset = 0;
	int resource_found = 0;
	struct blk_desc *dev_desc;
	disk_partition_t part_info;
	char *boot_partname = PART_BOOT;

/*
 * Primary detect AOSP format image, try to get resource image from
 * boot/recovery partition. If not, it's an RK format image and try
 * to get from resource partition.
 */
#ifdef CONFIG_ANDROID_BOOT_IMAGE
	struct andr_img_hdr *andr_hdr;
#endif

	if (hdr) {
		content = (void *)((char *)hdr
				   + (hdr->c_offset) * RK_BLK_SIZE);
		for (e_num = 0; e_num < hdr->e_nums; e_num++) {
			size = e_num * hdr->e_blks * RK_BLK_SIZE;
			entry = (struct resource_entry *)(content + size);
			add_file_to_list(entry, offset);
		}
		return 0;
	}

	dev_desc = rockchip_get_bootdev();
	if (!dev_desc) {
		printf("%s: dev_desc is NULL!\n", __func__);
		return -ENODEV;
	}
	hdr = memalign(ARCH_DMA_MINALIGN, RK_BLK_SIZE);
	if (!hdr) {
		printf("%s: out of memory!\n", __func__);
		return -ENOMEM;
	}

#ifdef CONFIG_ANDROID_BOOT_IMAGE
	/* Get boot mode from misc */
#ifndef CONFIG_ANDROID_AB
	if (rockchip_get_boot_mode() == BOOT_MODE_RECOVERY)
		boot_partname = PART_RECOVERY;
#endif

	/* Read boot/recovery and chenc if this is an AOSP img */
#ifdef CONFIG_ANDROID_AB
	char slot_suffix[3] = {0};

	if (rk_avb_get_current_slot(slot_suffix))
		goto out;
	boot_partname = android_str_append(boot_partname, slot_suffix);
	if (boot_partname == NULL)
		goto out;
#endif
	ret = part_get_info_by_name(dev_desc, boot_partname, &part_info);
	if (ret < 0) {
		printf("%s: failed to get %s part, ret=%d\n",
		       __func__, boot_partname, ret);
		/* RKIMG can support part table without 'boot' */
		goto next;
	}

	/*
	 * Only read header and check magic, is a AOSP format image?
	 * If so, get resource image from second part.
	 */
	andr_hdr = (void *)hdr;
	ret = blk_dread(dev_desc, part_info.start, 1, andr_hdr);
	if (ret != 1) {
		printf("%s: failed to read %s hdr, ret=%d\n",
		       __func__, part_info.name, ret);
		goto out;
	}
	ret = android_image_check_header(andr_hdr);
	if (!ret) {
		debug("%s: Load resource from %s second pos\n",
		      __func__, part_info.name);
		/* Read resource from second offset */
		offset = part_info.start * RK_BLK_SIZE;
		offset += andr_hdr->page_size;
		offset += ALIGN(andr_hdr->kernel_size, andr_hdr->page_size);
		offset += ALIGN(andr_hdr->ramdisk_size, andr_hdr->page_size);
		offset = offset / RK_BLK_SIZE;

		resource_found = 1;
	}
next:
#endif
	/*
	 * If not found resource image in AOSP format images(boot/recovery part),
	 * try to read RK format images(resource part).
	 */
	if (!resource_found) {
		debug("%s: Load resource from resource part\n", __func__);
		/* Read resource from Rockchip Resource partition */
		boot_partname = PART_RESOURCE;
		ret = part_get_info_by_name(dev_desc, boot_partname, &part_info);
		if (ret < 0) {
			printf("%s: failed to get resource part, ret=%d\n",
			       __func__, ret);
			goto out;
		}
		offset = part_info.start;
	}

	/* Only read header and check magic */
	ret = blk_dread(dev_desc, offset, 1, hdr);
	if (ret != 1) {
		printf("%s: failed to read resource hdr, ret=%d\n",
		       __func__, ret);
		goto out;
	}

	ret = resource_image_check_header(hdr);
	if (ret < 0)
		goto out;

	content = memalign(ARCH_DMA_MINALIGN,
			   hdr->e_blks * hdr->e_nums * RK_BLK_SIZE);
	if (!content) {
		printf("%s: failed to alloc memory for content\n", __func__);
		goto out;
	}

	/* Read all entries from resource image */
	ret = blk_dread(dev_desc, offset + hdr->c_offset,
			hdr->e_blks * hdr->e_nums, content);
	if (ret != (hdr->e_blks * hdr->e_nums)) {
		printf("%s: failed to read resource entries, ret=%d\n",
		       __func__, ret);
		goto err;
	}

	for (e_num = 0; e_num < hdr->e_nums; e_num++) {
		size = e_num * hdr->e_blks * RK_BLK_SIZE;
		entry = (struct resource_entry *)(content + size);
		add_file_to_list(entry, offset);
	}

	printf("Load FDT from %s part\n", boot_partname);
err:
	free(content);
out:
	free(hdr);

	return 0;
}

static struct resource_file *get_file_info(struct resource_img_hdr *hdr,
					   const char *name)
{
	struct resource_file *file;
	struct list_head *node;

	if (list_empty(&entrys_head))
		init_resource_list(hdr);

	list_for_each(node, &entrys_head) {
		file = list_entry(node, struct resource_file, link);
		if (!strcmp(file->name, name))
			return file;
	}

	return NULL;
}

int rockchip_get_resource_file_offset(void *resc_hdr, const char *name)
{
	struct resource_file *file;

	file = get_file_info(resc_hdr, name);
	if (!file)
		return -ENFILE;

	return file->f_offset;
}

int rockchip_get_resource_file_size(void *resc_hdr, const char *name)
{
	struct resource_file *file;

	file = get_file_info(resc_hdr, name);
	if (!file)
		return -ENFILE;

	return file->f_size;
}

/*
 * read file from resource partition
 * @buf: destination buf to store file data;
 * @name: file name
 * @offset: blocks offset in the file, 1 block = 512 bytes
 * @len: the size(by bytes) of file to read.
 */
int rockchip_read_resource_file(void *buf, const char *name,
				int offset, int len)
{
	struct resource_file *file;
	int ret = 0;
	int blks;
	struct blk_desc *dev_desc;

	file = get_file_info(NULL, name);
	if (!file) {
		printf("Can't find file:%s\n", name);
		return -ENOENT;
	}

	if (len <= 0 || len > file->f_size)
		len = file->f_size;
	blks = DIV_ROUND_UP(len, RK_BLK_SIZE);
	dev_desc = rockchip_get_bootdev();
	if (!dev_desc) {
		printf("%s: dev_desc is NULL!\n", __func__);
		return -ENODEV;
	}
	ret = blk_dread(dev_desc, file->rsce_base + file->f_offset + offset,
			blks, buf);
	if (ret != blks)
		ret = -EIO;
	else
		ret = len;

	return ret;
}

#define DTB_FILE		"rk-kernel.dtb"
#define GPIO_EXT_PORT		0x50
#define MAX_ADC_CH_NR		10
#define MAX_GPIO_NR		10

static int gpio_parse_base_address(fdt_addr_t *gpio_base_addr)
{
	static int initial;
	ofnode parent, node;
	int i = 0;

	if (initial)
		return 0;

	parent = ofnode_path("/pinctrl");
	if (!ofnode_valid(parent)) {
		debug("   - Can't find pinctrl node\n");
		return -EINVAL;
	}

	ofnode_for_each_subnode(node, parent) {
		if (!ofnode_get_property(node, "gpio-controller", NULL)) {
			debug("   - Can't find gpio-controller\n");
			continue;
		}

		gpio_base_addr[i++] = ofnode_get_addr(node);
		debug("   - gpio%d: 0x%x\n", i - 1, (uint32_t)gpio_base_addr[i - 1]);
	}

	if (i == 0) {
		debug("   - parse gpio address failed\n");
		return -EINVAL;
	}

	initial = 1;

	return 0;
}

/*
 * Board revision list: <GPIO4_D1 | GPIO4_D0>
 *  0b00 - NanoPC-T4
 *  0b01 - NanoPi M4
 *
 *  0b03 - Extended by ADC_IN4
 *  0b04 - NanoPi NEO4
 */

/*
 * ID info:
 *  ID : Volts : ADC value :   Bucket
 *  ==   =====   =========   ===========
 *   0 : 0.102V:        58 :    0 -   81
 *
 *  ------------------------------------
 *  Reserved
 *   1 : 0.211V:       120 :   82 -  150
 *   2 : 0.319V:       181 :  151 -  211
 *   3 : 0.427V:       242 :  212 -  274
 *   4 : 0.542V:       307 :  275 -  342
 *   5 : 0.666V:       378 :  343 -  411
 *   6 : 0.781V:       444 :  412 -  477
 *   7 : 0.900V:       511 :  478 -  545
 *   8 : 1.023V:       581 :  546 -  613
 *   9 : 1.137V:       646 :  614 -  675
 *  10 : 1.240V:       704 :  676 -  733
 *  11 : 1.343V:       763 :  734 -  795
 *  12 : 1.457V:       828 :  796 -  861
 *  13 : 1.576V:       895 :  862 -  925
 *  14 : 1.684V:       956 :  926 -  989
 *  15 : 1.800V:      1023 :  990 - 1023
 */
static const int id_readings[] = {
   81, 150, 211, 274, 342, 411, 477, 545,
  613, 675, 733, 795, 861, 925, 989, 1023
};

uint32_t rockchip_read_gpio(const char *name)
{
	fdt_addr_t gpio_base_addr[MAX_GPIO_NR];
	uint8_t port = *(name + 0) - '0';
	uint8_t bank = *(name + 1) - 'a';
	uint8_t pin  = *(name + 2) - '0';

	int ret = gpio_parse_base_address(gpio_base_addr);
	if (ret) {
		debug("   - Can't parse gpio base address: %d\n", ret);
		return -1;
	}

	uint32_t cached_v = readl(gpio_base_addr[port] + GPIO_EXT_PORT);
	uint8_t bit = bank * 8 + pin;
	uint8_t val = cached_v & (1 << bit) ? 1 : 0;

	return val;
}

uint32_t rockchip_read_adc(int channel)
{
	uint32_t raw_adc = 0;
	int ret = adc_channel_single_shot("saradc",
                  channel, &raw_adc);
	if (ret) {
		printf("read adc, ret=%d\n", ret);
	}

	for (int i = 0; i < ARRAY_SIZE(id_readings); i++) {
		if (raw_adc <= id_readings[i]) {
			debug("ADC reading %d, ID %d\n", raw_adc, i);
			return i;
		}
  }

  return 0;
}

int rockchip_read_dtb_file(void *fdt_addr)
{
	struct resource_file *file;
	struct list_head *node;
	char *dtb_name = DTB_FILE;
	int ret, size;

	if (list_empty(&entrys_head))
		init_resource_list(NULL);

	uint32_t pcb_rev = rockchip_read_gpio("4d0") | rockchip_read_gpio("4d1") << 1;
	if (pcb_rev == 0x3)
		pcb_rev += rockchip_read_adc(4) + 1;
	printf("board rev : %d\n", pcb_rev);

	char target[64] = {0};
	snprintf(target, ARRAY_SIZE(target),
			"rk3399-nanopi4-rev%02x.dtb", pcb_rev);

	list_for_each(node, &entrys_head) {
		file = list_entry(node, struct resource_file, link);
		if (!strstr(file->name, ".dtb"))
			continue;

		if (strcmp(target, file->name) == 0) {
			dtb_name = file->name;
			break;
		}
	}

	printf("DTB: %s\n", dtb_name);

	size = rockchip_get_resource_file_size((void *)fdt_addr, dtb_name);
	if (size < 0)
		return size;

	if (!sysmem_alloc_base("fdt", (phys_addr_t)fdt_addr,
			       ALIGN(size, RK_BLK_SIZE)))
		return -ENOMEM;

	ret = rockchip_read_resource_file((void *)fdt_addr, dtb_name, 0, 0);
	if (ret < 0)
		return ret;

#if defined(CONFIG_CMD_DTIMG) && defined(CONFIG_OF_LIBFDT_OVERLAY)
	android_fdt_overlay_apply((void *)fdt_addr);
#endif

	return ret;
}
