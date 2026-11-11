// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2014
 * Texas Instruments, <www.ti.com>
 *
 * Dan Murphy <dmurphy@ti.com>
 *
 * FAT Image Functions copied from spl_mmc.c
 */

#include <common.h>
#include <env.h>
#include <log.h>
#include <spl.h>
#include <asm/u-boot.h>
#include <fat.h>
#include <errno.h>
#include <image.h>
#include <linux/libfdt.h>

static int fat_registered;

static int spl_register_fat_device(struct blk_desc *block_dev, int partition)
{
	int err = 0;

	if (fat_registered)
		return err;

	err = fat_register_device(block_dev, partition);
	if (err) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		printf("%s: fat register err - %d\n", __func__, err);
#endif
		return err;
	}

	fat_registered = 1;

	return err;
}

static ulong spl_fit_read(struct spl_load_info *load, ulong file_offset,
			  ulong size, void *buf)
{
	loff_t actread;
	int ret;
	char *filename = (char *)load->filename;

	ret = fat_read_file(filename, buf, file_offset, size, &actread);
	if (ret)
		return 0;

	return actread;
}

static int spl_load_image_fat_file(struct spl_image_info *spl_image,
				   struct spl_boot_device *bootdev,
				   struct blk_desc *block_dev, int partition,
				   const char *filename, bool falcon)
{
	int err;
	struct legacy_img_hdr *header;

	err = spl_register_fat_device(block_dev, partition);
	if (err && falcon)
		return err < 0 ? err : -EIO;
	if (err)
		goto end;

	header = spl_get_load_buffer(-sizeof(*header), sizeof(*header));

	err = file_fat_read(filename, header, sizeof(struct legacy_img_hdr));
	if (falcon) {
		if (err != (int)sizeof(struct legacy_img_hdr)) {
			printf("SPL: incomplete Falcon FIT header (%d)\n", err);
			return -EIO;
		}
		if (image_get_magic(header) != FDT_MAGIC) {
			puts("SPL: invalid Falcon FIT magic\n");
			return -EINVAL;
		}
#if SPL_IOT_GATE_FALCON_FIT
		{
			struct spl_load_info load = {
				.read = spl_fit_read,
				.bl_len = 1,
				.filename = filename,
			};

			return spl_load_simple_fit_falcon(spl_image, &load, 0,
							  header, err);
		}
#else
		return -EOPNOTSUPP;
#endif
	}
	if (err <= 0)
		goto end;

	if (IS_ENABLED(CONFIG_SPL_LOAD_FIT_FULL) &&
	    image_get_magic(header) == FDT_MAGIC) {
		err = file_fat_read(filename, (void *)CONFIG_SYS_LOAD_ADDR, 0);
		if (err <= 0)
			goto end;
		err = spl_parse_image_header(spl_image, bootdev,
				(struct legacy_img_hdr *)CONFIG_SYS_LOAD_ADDR);
		if (err == -EAGAIN)
			return err;
		if (err == 0)
			err = 1;
	} else if (IS_ENABLED(CONFIG_SPL_LOAD_FIT) &&
	    image_get_magic(header) == FDT_MAGIC) {
		struct spl_load_info load;

		debug("Found FIT\n");
		load.read = spl_fit_read;
		load.bl_len = 1;
		load.filename = (void *)filename;
		load.priv = NULL;

		return spl_load_simple_fit(spl_image, &load, 0, header);
	} else {
		err = spl_parse_image_header(spl_image, bootdev, header);
		if (err)
			goto end;

		err = file_fat_read(filename,
				    (u8 *)(uintptr_t)spl_image->load_addr, 0);
	}

end:
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
	if (err <= 0)
		printf("%s: error reading image %s, err - %d\n",
		       __func__, filename, err);
#endif

	return (err <= 0);
}

int spl_load_image_fat(struct spl_image_info *spl_image,
		       struct spl_boot_device *bootdev,
		       struct blk_desc *block_dev, int partition,
		       const char *filename)
{
	return spl_load_image_fat_file(spl_image, bootdev, block_dev, partition,
				       filename, false);
}

#if CONFIG_IS_ENABLED(OS_BOOT)
#if SPL_IOT_GATE_FALCON_FIT
static int spl_load_falcon_args(const char *filename)
{
	const struct fdt_header *header = (void *)CONFIG_SYS_SPL_ARGS_ADDR;
	loff_t size, read;
	u32 total;
	int ret;

	ret = fat_size(filename, &size);
	if (ret)
		goto error;
	if (size < (loff_t)sizeof(*header) || size > 0x200000) {
		ret = -EINVAL;
		goto error;
	}
	ret = fat_read_file(filename, (void *)header, 0, size, &read);
	if (ret || read != size) {
		ret = ret < 0 ? ret : -EIO;
		goto error;
	}
	if (fdt32_to_cpu(header->magic) != FDT_MAGIC) {
		ret = -EINVAL;
		goto error;
	}
	total = fdt32_to_cpu(header->totalsize);
	if (total < sizeof(*header) || total > size ||
	    fdt_check_full(header, total)) {
		ret = -EINVAL;
		goto error;
	}
	return 0;
error:
	printf("SPL: Falcon arguments %s failed (%d)\n", filename, ret);
	return ret < 0 ? ret : -EIO;
}
#endif

int spl_load_image_fat_os(struct spl_image_info *spl_image,
			  struct spl_boot_device *bootdev,
			  struct blk_desc *block_dev, int partition)
{
	int err;
	__maybe_unused char *file;

	err = spl_register_fat_device(block_dev, partition);
	if (err)
		return err < 0 ? err : -EIO;

#if defined(CONFIG_SPL_ENV_SUPPORT) && defined(CONFIG_SPL_OS_BOOT)
	file = env_get("falcon_args_file");
	if (file) {
#if SPL_IOT_GATE_FALCON_FIT
		err = spl_load_falcon_args(file);
		if (err)
			return err;
#else
		err = file_fat_read(file, (void *)CONFIG_SYS_SPL_ARGS_ADDR, 0);
		if (err <= 0) {
			printf("spl: error reading image %s, err - %d, falling back to default\n",
			       file, err);
			goto defaults;
		}
#endif
		file = env_get("falcon_image_file");
		if (file) {
			err = spl_load_image_fat_file(spl_image, bootdev, block_dev,
						      partition, file,
						      SPL_IOT_GATE_FALCON_FIT);
			if (err != 0) {
				if (SPL_IOT_GATE_FALCON_FIT)
					return err < 0 ? err : -EIO;
				puts("spl: falling back to default\n");
				goto defaults;
			}

			return 0;
		} else
			puts("spl: falcon_image_file not set in environment, falling back to default\n");
	} else
		puts("spl: falcon_args_file not set in environment, falling back to default\n");

defaults:
#endif

#if SPL_IOT_GATE_FALCON_FIT
	err = spl_load_falcon_args(CONFIG_SPL_FS_LOAD_ARGS_NAME);
	if (err)
		return err;
#else
	err = file_fat_read(CONFIG_SPL_FS_LOAD_ARGS_NAME,
			    (void *)CONFIG_SYS_SPL_ARGS_ADDR, 0);
	if (err <= 0) {
#ifdef CONFIG_SPL_LIBCOMMON_SUPPORT
		printf("%s: error reading image %s, err - %d\n",
		       __func__, CONFIG_SPL_FS_LOAD_ARGS_NAME, err);
#endif
		return -1;
	}
#endif

	return spl_load_image_fat_file(spl_image, bootdev, block_dev, partition,
			CONFIG_SPL_FS_LOAD_KERNEL_NAME,
			SPL_IOT_GATE_FALCON_FIT);
}
#else
int spl_load_image_fat_os(struct spl_image_info *spl_image,
			  struct spl_boot_device *bootdev,
			  struct blk_desc *block_dev, int partition)
{
	return -ENOSYS;
}
#endif
