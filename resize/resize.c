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
#include <sys/types.h>
#include <sys/wait.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

static void usage(void)
{
	fprintf(stderr,
		"Usage: resize.exfat [options] <device> <new-size>[K|M|G]\n"
		"\t-n | --dry-run    Check feasibility only, do not write\n"
		"\t-V | --version    Show version\n"
		"\t-v | --verbose    Print debug\n"
		"\t-h | --help       Show help\n");
	exit(EXIT_FAILURE);
}

static const struct option opts[] = {
	{"dry-run",	no_argument,	NULL,	'n'},
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
 * Walk the root directory looking for the primary allocation bitmap dentry
 * (type 0x81, flags == 0).  Returns start cluster, declared byte size, and
 * optionally the on-disk offset of the dentry (dentry_off may be NULL).
 */
static int find_bitmap_dentry(int fd, const struct pbr *bs,
			      clus_t *start_clu,
			      unsigned long long *size_bytes,
			      off_t *dentry_off)
{
	unsigned int sector_size  = EXFAT_SECTOR_SIZE(bs);
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
		off_t de_off = root_off + (off_t)i * sizeof(de);

		n = pread(fd, &de, sizeof(de), de_off);
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
		if (dentry_off)
			*dentry_off = de_off;
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

/*
 * Recompute the boot region checksum (sectors 0-10 for main, 12-22 for
 * backup) and write it to the checksum sector (11 or 23).
 */
static int recompute_boot_checksum(int fd, unsigned int sector_size,
				   bool is_backup)
{
	unsigned int checksum = 0;
	unsigned char *buf;
	unsigned int base = is_backup ? BACKUP_BOOT_SEC_IDX : 0;
	unsigned int csum_sec_idx = base + CHECKSUM_SEC_IDX;
	int i;
	ssize_t n;
	unsigned int j;

	buf = malloc(sector_size);
	if (!buf)
		return -ENOMEM;

	for (i = BOOT_SEC_IDX; i < CHECKSUM_SEC_IDX; i++) {
		n = pread(fd, buf, sector_size,
			  (off_t)(base + i) * sector_size);
		if (n != (ssize_t)sector_size) {
			exfat_err("failed to read sector %u for checksum\n",
				  base + i);
			free(buf);
			return -EIO;
		}
		boot_calc_checksum(buf, (unsigned short)sector_size,
				   i == BOOT_SEC_IDX, (__le32 *)&checksum);
	}

	for (j = 0; j < sector_size / sizeof(__le32); j++)
		((__le32 *)buf)[j] = cpu_to_le32(checksum);

	n = pwrite(fd, buf, sector_size, (off_t)csum_sec_idx * sector_size);
	free(buf);
	if (n != (ssize_t)sector_size) {
		exfat_err("failed to write checksum sector %u\n", csum_sec_idx);
		return -EIO;
	}
	return 0;
}

/*
 * Fork fsck.exfat -n to verify the volume after the grow.
 */
static int run_fsck(const char *dev_name)
{
	pid_t pid;
	int status;
	char *args[] = {"fsck.exfat", "-n", (char *)dev_name, NULL};

	exfat_info("Verifying with fsck.exfat -n ...\n");

	pid = fork();
	if (pid < 0) {
		exfat_err("fork: %s\n", strerror(errno));
		return -errno;
	}
	if (pid == 0) {
		execvp("fsck.exfat", args);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0) {
		exfat_err("waitpid: %s\n", strerror(errno));
		return -errno;
	}
	if (!WIFEXITED(status)) {
		exfat_err("fsck.exfat terminated abnormally\n");
		return -EIO;
	}
	if (WEXITSTATUS(status) == 127) {
		exfat_err("fsck.exfat not found in PATH\n");
		return -ENOENT;
	}
	if (WEXITSTATUS(status) != 0) {
		exfat_err("fsck.exfat -n reported errors (exit %d)\n",
			  WEXITSTATUS(status));
		return -EINVAL;
	}
	return 0;
}

/*
 * Execute the grow operation.  The block device must already be at least
 * new_vol_sectors long before this is called.
 *
 * Write order is chosen for crash safety: new data is committed before the
 * metadata fields that expose it, so a crash mid-way leaves the old geometry
 * still valid rather than pointing at uninitialised content.
 *
 *  1. Zero-fill new FAT entries      (free = 0, extends FAT coverage)
 *  2. Zero-fill new bitmap bits      (new clusters start free)
 *  3. Update bitmap dentry DataLength
 *  4. Update fat_length + vol_length + clu_count in both boot sectors
 *  5. Recompute boot checksums
 *  6. fsync
 *  7. fsck.exfat -n
 */
static int do_grow(int fd, const struct pbr *bs, const char *dev_name,
		   unsigned long long new_vol_sectors,
		   unsigned long long new_clu_count)
{
	unsigned int sector_size   = EXFAT_SECTOR_SIZE(bs);
	unsigned int cluster_size  = EXFAT_CLUSTER_SIZE(bs);
	unsigned int fat_offset_sect = le32_to_cpu(bs->bsx.fat_offset);
	unsigned int fat_length_sect = le32_to_cpu(bs->bsx.fat_length);
	unsigned int clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	unsigned int clu_count     = le32_to_cpu(bs->bsx.clu_count);
	clus_t bm_start_clu;
	unsigned long long old_bm_bytes, new_bm_bytes, bm_alloc_bytes;
	unsigned int new_fat_length_sect;
	off_t bitmap_off, dentry_off;
	struct exfat_dentry de;
	void *boot_buf;
	struct pbr *bs_write;
	int ret;

	/* --- locate primary allocation bitmap --- */
	ret = find_bitmap_dentry(fd, bs, &bm_start_clu, &old_bm_bytes,
				 &dentry_off);
	if (ret)
		return ret;

	if (bm_start_clu < EXFAT_FIRST_CLUSTER) {
		exfat_err("invalid bitmap start cluster %u\n", bm_start_clu);
		return -EINVAL;
	}

	if (old_bm_bytes < (unsigned long long)(clu_count + 7) / 8) {
		exfat_err("bitmap dentry size %llu is smaller than needed for "
			  "%u clusters\n", old_bm_bytes, clu_count);
		return -EINVAL;
	}

	new_bm_bytes   = ((unsigned long long)new_clu_count + 7) / 8;
	bm_alloc_bytes = ((old_bm_bytes + cluster_size - 1) /
			  cluster_size) * (unsigned long long)cluster_size;

	/*
	 * The allocation bitmap is contiguous.  Extending it beyond its
	 * current cluster allocation would require moving the metadata that
	 * immediately follows it (upcase table, root dir) -- not supported.
	 * Report the largest grow target that fits inside the current bitmap
	 * cluster allocation.
	 */
	if (new_bm_bytes > bm_alloc_bytes) {
		unsigned long long max_clu  = bm_alloc_bytes * 8;
		unsigned long long max_size = (unsigned long long)clu_offset_sect
					      * sector_size
					      + max_clu
					      * (unsigned long long)cluster_size;

		exfat_err("grow needs %llu bitmap bytes but only %llu are "
			  "allocated in the current bitmap cluster run.\n"
			  "  Maximum supported target: ",
			  new_bm_bytes, bm_alloc_bytes);
		print_bytes(max_size);
		fprintf(stderr, "\n");
		return -ENOSPC;
	}

	bitmap_off = (off_t)clu_offset_sect * sector_size +
		     (off_t)(bm_start_clu - EXFAT_FIRST_CLUSTER) *
		     cluster_size;

	/*
	 * New FAT length needed: ceil((new_clu_count + EXFAT_RESERVED_CLUSTERS)
	 *                              * sizeof(__le32) / sector_size)
	 */
	new_fat_length_sect = (unsigned int)(
		((unsigned long long)(new_clu_count + EXFAT_RESERVED_CLUSTERS)
		 * sizeof(__le32) + sector_size - 1) / sector_size);

	/* Step 1 ── zero-fill new FAT entries so new clusters appear free */
	if (new_fat_length_sect > fat_length_sect) {
		off_t old_fat_end = (off_t)fat_offset_sect * sector_size
				    + (off_t)fat_length_sect * sector_size;
		size_t new_fat_bytes = (size_t)(new_fat_length_sect -
						fat_length_sect) * sector_size;

		ret = exfat_write_zero(fd, new_fat_bytes, old_fat_end);
		if (ret) {
			exfat_err("failed to zero-extend FAT: %s\n",
				  strerror(-ret));
			return ret;
		}
		exfat_debug("FAT extended: %u -> %u sectors\n",
			    fat_length_sect, new_fat_length_sect);
	}

	/* Step 2 ── zero-fill new cluster bits in bitmap (new clusters = free) */
	if (new_bm_bytes > old_bm_bytes) {
		ret = exfat_write_zero(fd,
				       (size_t)(new_bm_bytes - old_bm_bytes),
				       bitmap_off + (off_t)old_bm_bytes);
		if (ret) {
			exfat_err("failed to zero-extend bitmap: %s\n",
				  strerror(-ret));
			return ret;
		}
		exfat_debug("bitmap extended: %llu -> %llu bytes\n",
			    old_bm_bytes, new_bm_bytes);
	}

	/* Step 3 ── update bitmap dentry DataLength */
	if (pread(fd, &de, sizeof(de), dentry_off) != (ssize_t)sizeof(de)) {
		exfat_err("failed to re-read bitmap dentry\n");
		return -EIO;
	}
	de.bitmap_size = cpu_to_le64(new_bm_bytes);
	if (pwrite(fd, &de, sizeof(de), dentry_off) != (ssize_t)sizeof(de)) {
		exfat_err("failed to write updated bitmap dentry\n");
		return -EIO;
	}
	exfat_debug("bitmap dentry DataLength: %llu -> %llu bytes\n",
		    old_bm_bytes, new_bm_bytes);

	/*
	 * Step 4 ── update fat_length, vol_length, clu_count in both boot
	 * sectors.  Read the full sector so we don't clobber boot_code or
	 * anything beyond the pbr struct.
	 */
	boot_buf = malloc(sector_size);
	if (!boot_buf)
		return -ENOMEM;

	if (pread(fd, boot_buf, sector_size, 0) != (ssize_t)sector_size) {
		exfat_err("failed to read main boot sector\n");
		free(boot_buf);
		return -EIO;
	}
	bs_write = (struct pbr *)boot_buf;
	bs_write->bsx.fat_length = cpu_to_le32(new_fat_length_sect);
	bs_write->bsx.vol_length = cpu_to_le64(new_vol_sectors);
	bs_write->bsx.clu_count  = cpu_to_le32((unsigned int)new_clu_count);
	/* PercentInUse is informational; mark unknown after structural change */
	bs_write->bsx.perc_in_use = 0xFF;

	if (pwrite(fd, boot_buf, sector_size, 0) != (ssize_t)sector_size) {
		exfat_err("failed to write main boot sector\n");
		free(boot_buf);
		return -EIO;
	}
	if (pwrite(fd, boot_buf, sector_size,
		   (off_t)BACKUP_BOOT_SEC_IDX * sector_size) !=
	    (ssize_t)sector_size) {
		exfat_err("failed to write backup boot sector\n");
		free(boot_buf);
		return -EIO;
	}
	free(boot_buf);

	exfat_debug("boot sectors: fat_length=%u vol_length=%llu clu_count=%llu\n",
		    new_fat_length_sect, new_vol_sectors, new_clu_count);

	/* Step 5 ── recompute boot checksums */
	ret = recompute_boot_checksum(fd, sector_size, false);
	if (ret) {
		exfat_err("failed to update main boot checksum\n");
		return ret;
	}
	ret = recompute_boot_checksum(fd, sector_size, true);
	if (ret) {
		exfat_err("failed to update backup boot checksum\n");
		return ret;
	}

	/* Step 6 ── flush */
	if (fsync(fd)) {
		exfat_err("fsync: %s\n", strerror(errno));
		return -errno;
	}

	exfat_info("Grow complete: %u -> %llu clusters.\n",
		   clu_count, new_clu_count);

	/* Step 7 ── read-only verification */
	return run_fsck(dev_name);
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
	bool dry_run = false;
	long long new_size;
	unsigned int sector_size, cluster_size;
	unsigned long long cur_size, new_vol_sectors;
	unsigned long long sectors_per_clu, new_clu_count;
	unsigned int fat_offset_sect, clu_offset_sect, clu_count;
	/*
	 * avail_fat_cap: max clusters the FAT region can address if fully
	 * used up to the start of the cluster heap.  This is the true upper
	 * bound for a grow that doesn't need to move the cluster heap.
	 */
	unsigned long long avail_fat_cap;
	clus_t bm_start_clu, highest;
	unsigned long long bitmap_size_bytes;
	off_t bitmap_off;

	init_user_input(&ui);

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "nVvh", opts, NULL)) != EOF) {
		switch (c) {
		case 'n':
			dry_run = true;
			break;
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

	/*
	 * Open read-only first to determine direction.  For a grow we will
	 * close and reopen read-write after confirming feasibility.
	 */
	ui.writeable = false;
	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	ret = read_boot_sect(&bd, &bs);
	if (ret) {
		exfat_err("failed to read boot sector\n");
		goto close;
	}

	sector_size      = EXFAT_SECTOR_SIZE(bs);
	cluster_size     = EXFAT_CLUSTER_SIZE(bs);
	cur_size         = le64_to_cpu(bs->bsx.vol_length) * sector_size;
	fat_offset_sect  = le32_to_cpu(bs->bsx.fat_offset);
	clu_offset_sect  = le32_to_cpu(bs->bsx.clu_offset);
	clu_count        = le32_to_cpu(bs->bsx.clu_count);
	sectors_per_clu  = 1u << bs->bsx.sect_per_clus_bits;

	/*
	 * The FAT can expand into the gap between its current end and the
	 * start of the cluster heap without touching any cluster data.
	 */
	avail_fat_cap = (unsigned long long)(clu_offset_sect - fat_offset_sect)
			* sector_size / sizeof(__le32)
			- EXFAT_RESERVED_CLUSTERS;

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
	printf("FAT available capacity:%llu clusters\n\n", avail_fat_cap);

	if ((unsigned long long)new_size == cur_size) {
		printf("Result: NO CHANGE\n"
		       "  Target size equals current volume size.\n");
		ret = EXIT_SUCCESS;
		goto free_bs;
	}

	if ((unsigned long long)new_size > cur_size) {
		/* ── GROW ── */

		/* The block device must already be physically large enough. */
		if ((unsigned long long)new_size > bd.size) {
			printf("Result: GROW BLOCKED\n"
			       "  Device is only ");
			print_bytes(bd.size);
			printf("; extend it before running resize.exfat.\n");
			ret = EXIT_FAILURE;
			goto free_bs;
		}

		/*
		 * The FAT can grow into the gap between its end and the
		 * cluster heap start.  If even that gap is insufficient the
		 * cluster heap would have to move -- not supported.
		 */
		if (new_clu_count > avail_fat_cap) {
			printf("Result: GROW BLOCKED\n"
			       "  FAT region can address at most %llu clusters;"
			       " new size requires %llu.\n"
			       "  The cluster heap would need to move"
			       " -- not yet supported.\n",
			       avail_fat_cap, new_clu_count);
			ret = EXIT_FAILURE;
			goto free_bs;
		}

		{
			unsigned long long added =
				(new_clu_count - clu_count) *
				(unsigned long long)cluster_size;

			printf("Result: GROW POSSIBLE\n"
			       "  Volume would expand from %u to %llu"
			       " clusters (+",
			       clu_count, new_clu_count);
			print_bytes(added);
			printf(").\n");
		}

		if (dry_run) {
			ret = EXIT_SUCCESS;
			goto free_bs;
		}

		/*
		 * Reopen the device read-write.  O_EXCL prevents a concurrent
		 * mounter from racing the metadata updates below.
		 */
		free(bs);
		bs = NULL;
		close(bd.dev_fd);
		memset(&bd, 0, sizeof(bd));

		ui.writeable = true;
		ret = exfat_get_blk_dev_info(&ui, &bd);
		if (ret < 0)
			goto out;

		ret = read_boot_sect(&bd, &bs);
		if (ret) {
			exfat_err("failed to re-read boot sector (rw)\n");
			goto close;
		}

		ret = do_grow(bd.dev_fd, bs, ui.dev_name,
			      new_vol_sectors, new_clu_count);
		goto free_bs;
	}

	/* ── SHRINK feasibility (no writes yet) ── */
	ret = find_bitmap_dentry(bd.dev_fd, bs,
				 &bm_start_clu, &bitmap_size_bytes, NULL);
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
