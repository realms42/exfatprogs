// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2026 Anthropic
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <locale.h>
#include <inttypes.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

static void usage(void)
{
	fprintf(stderr, "Usage: resize.exfat <device> <new-size>[K|M|G]\n"
		"\t-V | --version    Show version\n"
		"\t-v | --verbose    Print debug\n"
		"\t-h | --help       Show help\n");
	exit(EXIT_FAILURE);
}

static const struct option opts[] = {
	{"version",	no_argument,	NULL,	'V'},
	{"verbose",	no_argument,	NULL,	'v'},
	{"help",	no_argument,	NULL,	'h'},
	{"?",		no_argument,	NULL,	'?'},
	{NULL,		0,		NULL,	 0 }
};

static long long parse_size(const char *s)
{
	char *unit;
	unsigned long long v = strtoull(s, &unit, 0);

	switch (*unit) {
	case 'G': case 'g': v <<= 30; break;
	case 'M': case 'm': v <<= 20; break;
	case 'K': case 'k': v <<= 10; break;
	case '\0': break;
	default:
		exfat_err("unknown size unit '%c'\n", *unit);
		return -EINVAL;
	}
	return (long long)v;
}

/*
 * Scan the allocation bitmap backward and return the highest allocated cluster
 * number, or EXFAT_FREE_CLUSTER (0) if the volume is empty.
 *
 * Bit layout: bit 0 of byte 0 = cluster 2 (EXFAT_FIRST_CLUSTER);
 * within each byte the LSB is the lower-numbered cluster.  We walk bytes
 * from last to first and within each byte scan from bit 7 down to bit 0,
 * skipping pad bits that lie beyond clu_count.
 */
static clus_t find_highest_used_cluster(const unsigned char *bitmap,
					unsigned int clu_count)
{
	unsigned int nbytes = (clu_count + 7) / 8;
	unsigned int i, bit;

	for (i = nbytes; i > 0; i--) {
		unsigned char b = bitmap[i - 1];

		if (!b)
			continue;
		for (bit = 7; ; bit--) {
			unsigned int clu_idx = (i - 1) * 8 + bit;

			if (clu_idx < clu_count && (b & (1u << bit)))
				return clu_idx + EXFAT_FIRST_CLUSTER;
			if (bit == 0)
				break;
		}
	}
	return EXFAT_FREE_CLUSTER;
}

/*
 * Walk the first cluster of the root directory looking for the primary
 * allocation bitmap dentry (type 0x81, flags == 0).
 */
static int find_bitmap_dentry(int fd, const struct pbr *bs,
			      clus_t *start_clu,
			      unsigned long long *size_bytes)
{
	unsigned int sector_size = EXFAT_SECTOR_SIZE(bs);
	unsigned int cluster_size = EXFAT_CLUSTER_SIZE(bs);
	unsigned int clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	clus_t root_clu = le32_to_cpu(bs->bsx.root_cluster);
	int max_dentries = (int)(cluster_size / sizeof(struct exfat_dentry));
	off_t root_off;
	struct exfat_dentry de;
	ssize_t n;
	int i;

	if (root_clu < EXFAT_FIRST_CLUSTER) {
		exfat_err("invalid root cluster %u\n", root_clu);
		return -EINVAL;
	}

	root_off = (off_t)clu_offset_sect * sector_size +
		   (off_t)(root_clu - EXFAT_FIRST_CLUSTER) * cluster_size;

	for (i = 0; i < max_dentries; i++) {
		n = pread(fd, &de, sizeof(de),
			  root_off + (off_t)i * sizeof(de));
		if (n != (ssize_t)sizeof(de)) {
			exfat_err("failed to read root dentry %d\n", i);
			return -EIO;
		}
		if (de.type == EXFAT_LAST)
			break;
		if (de.type != EXFAT_BITMAP)
			continue;
		/* skip the secondary bitmap (bit 0 of flags set) */
		if (de.bitmap_flags & 0x01)
			continue;
		*start_clu = le32_to_cpu(de.bitmap_start_clu);
		*size_bytes = le64_to_cpu(de.bitmap_size);
		return 0;
	}
	exfat_err("allocation bitmap dentry not found in root directory\n");
	return -EINVAL;
}

static void print_bytes(unsigned long long n)
{
	if (n >= (1ULL << 30))
		printf("%llu bytes (%llu GiB)", n, n >> 30);
	else if (n >= (1ULL << 20))
		printf("%llu bytes (%llu MiB)", n, n >> 20);
	else if (n >= (1ULL << 10))
		printf("%llu bytes (%llu KiB)", n, n >> 10);
	else
		printf("%llu bytes", n);
}

int main(int argc, char *argv[])
{
	int c;
	int ret = EXIT_FAILURE;
	struct exfat_blk_dev bd;
	struct exfat_user_input ui;
	struct pbr *bs = NULL;
	unsigned char *bitmap = NULL;
	bool version_only = false;
	long long new_size;
	unsigned int sector_size, cluster_size;
	unsigned long long cur_size, new_vol_sectors, fat_capacity;
	unsigned long long sectors_per_clu, new_clu_count;
	unsigned int clu_offset_sect, clu_count, fat_length_sect;
	clus_t bm_start_clu, highest;
	unsigned long long bitmap_size_bytes;
	off_t bitmap_off;

	init_user_input(&ui);
	ui.writeable = false;

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "Vvh", opts, NULL)) != EOF) {
		switch (c) {
		case 'V':
			version_only = true;
			break;
		case 'v':
			print_level = EXFAT_DEBUG;
			break;
		case '?':
		case 'h':
		default:
			usage();
		}
	}

	show_version();
	if (version_only)
		exit(EXIT_FAILURE);

	if (argc - optind != 2)
		usage();

	ui.dev_name = argv[optind];

	new_size = parse_size(argv[optind + 1]);
	if (new_size < 0)
		goto out;

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	ret = read_boot_sect(&bd, &bs);
	if (ret) {
		exfat_err("failed to read boot sector\n");
		goto close;
	}

	sector_size     = EXFAT_SECTOR_SIZE(bs);
	cluster_size    = EXFAT_CLUSTER_SIZE(bs);
	cur_size        = le64_to_cpu(bs->bsx.vol_length) * sector_size;
	fat_length_sect = le32_to_cpu(bs->bsx.fat_length);
	clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	clu_count       = le32_to_cpu(bs->bsx.clu_count);
	sectors_per_clu = 1u << bs->bsx.sect_per_clus_bits;

	if ((unsigned long long)new_size % sector_size != 0) {
		exfat_err("target size is not a multiple of sector size (%u)\n",
			  sector_size);
		ret = -EINVAL;
		goto free_bs;
	}

	new_vol_sectors = (unsigned long long)new_size / sector_size;

	if (new_vol_sectors < EXFAT_MIN_NUM_SEC_VOL) {
		exfat_err("target size too small (minimum %d sectors)\n",
			  EXFAT_MIN_NUM_SEC_VOL);
		ret = -EINVAL;
		goto free_bs;
	}

	if (new_vol_sectors <= clu_offset_sect) {
		exfat_err("target size does not cover the cluster heap "
			  "(heap starts at sector %u)\n", clu_offset_sect);
		ret = -EINVAL;
		goto free_bs;
	}

	new_clu_count = (new_vol_sectors - clu_offset_sect) / sectors_per_clu;

	if (new_clu_count > EXFAT_MAX_NUM_CLUSTER) {
		exfat_err("target size requires too many clusters\n");
		ret = -EINVAL;
		goto free_bs;
	}

	/*
	 * FAT entries 0 and 1 are reserved; the remaining entries each address
	 * one cluster.
	 */
	fat_capacity = (unsigned long long)fat_length_sect * sector_size /
		       sizeof(__le32) - EXFAT_RESERVED_CLUSTERS;

	printf("Device:                %s\n", ui.dev_name);
	printf("Current size:          ");
	print_bytes(cur_size);
	printf("\nTarget size:           ");
	print_bytes((unsigned long long)new_size);
	printf("\nSector size:           %u bytes\n", sector_size);
	printf("Cluster size:          %u bytes", cluster_size);
	if (cluster_size >= (1u << 20))
		printf(" (%u MiB)", cluster_size >> 20);
	else if (cluster_size >= (1u << 10))
		printf(" (%u KiB)", cluster_size >> 10);
	printf("\nCurrent cluster count: %u\n", clu_count);
	printf("New cluster count:     %llu\n", new_clu_count);
	printf("FAT capacity:          %llu clusters\n\n", fat_capacity);

	if ((unsigned long long)new_size == cur_size) {
		printf("Result: NO CHANGE\n"
		       "  Target size equals current volume size.\n");
		ret = EXIT_SUCCESS;
		goto free_bs;
	}

	if ((unsigned long long)new_size > cur_size) {
		if (new_clu_count > fat_capacity) {
			printf("Result: GROW BLOCKED\n"
			       "  FAT can address at most %llu clusters;"
			       " new size requires %llu.\n"
			       "  The FAT region would need relocation"
			       " -- not yet supported.\n",
			       fat_capacity, new_clu_count);
			ret = EXIT_FAILURE;
		} else {
			unsigned long long added =
				(new_clu_count - clu_count) *
				(unsigned long long)cluster_size;

			printf("Result: GROW POSSIBLE\n"
			       "  Volume would expand from %u to %llu"
			       " clusters (+",
			       clu_count, new_clu_count);
			print_bytes(added);
			printf(").\n"
			       "  FAT has capacity for %llu clusters.\n",
			       fat_capacity);
			ret = EXIT_SUCCESS;
		}
		goto free_bs;
	}

	/* Shrink: scan the allocation bitmap for the highest used cluster. */
	ret = find_bitmap_dentry(bd.dev_fd, bs,
				 &bm_start_clu, &bitmap_size_bytes);
	if (ret)
		goto free_bs;

	if (bm_start_clu < EXFAT_FIRST_CLUSTER) {
		exfat_err("invalid bitmap start cluster %u\n", bm_start_clu);
		ret = -EINVAL;
		goto free_bs;
	}

	exfat_debug("bitmap start cluster %u, declared size %llu bytes\n",
		    bm_start_clu, bitmap_size_bytes);

	if (bitmap_size_bytes < (unsigned long long)(clu_count + 7) / 8) {
		exfat_err("bitmap too small for declared cluster count\n");
		ret = -EINVAL;
		goto free_bs;
	}

	bitmap = malloc(bitmap_size_bytes);
	if (!bitmap) {
		exfat_err("failed to allocate bitmap buffer: out of memory\n");
		ret = -ENOMEM;
		goto free_bs;
	}

	bitmap_off = (off_t)clu_offset_sect * sector_size +
		     (off_t)(bm_start_clu - EXFAT_FIRST_CLUSTER) *
		     cluster_size;

	if (pread(bd.dev_fd, bitmap, bitmap_size_bytes, bitmap_off) !=
	    (ssize_t)bitmap_size_bytes) {
		exfat_err("failed to read allocation bitmap\n");
		ret = -EIO;
		goto free_bitmap;
	}

	highest = find_highest_used_cluster(bitmap, clu_count);

	printf("Highest used cluster:  ");
	if (highest == EXFAT_FREE_CLUSTER)
		printf("none (volume is empty)\n");
	else
		printf("%u\n", highest);
	printf("\n");

	if (highest == EXFAT_FREE_CLUSTER ||
	    highest <= EXFAT_FIRST_CLUSTER + (clus_t)new_clu_count - 1) {
		unsigned long long freed =
			(clu_count - (unsigned int)new_clu_count) *
			(unsigned long long)cluster_size;

		printf("Result: SHRINK POSSIBLE\n"
		       "  %u clusters freed (",
		       clu_count - (unsigned int)new_clu_count);
		print_bytes(freed);
		printf(").\n");
		ret = EXIT_SUCCESS;
	} else {
		clus_t new_last =
			EXFAT_FIRST_CLUSTER + (clus_t)new_clu_count - 1;

		printf("Result: SHRINK BLOCKED\n"
		       "  Cluster %u is allocated beyond the new boundary"
		       " (last valid cluster %u).\n"
		       "  Free or move data, then retry.\n",
		       highest, new_last);
		ret = EXIT_FAILURE;
	}

free_bitmap:
	free(bitmap);
free_bs:
	free(bs);
close:
	close(bd.dev_fd);
out:
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
