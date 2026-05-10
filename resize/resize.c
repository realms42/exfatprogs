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
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
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
	unsigned int shift;

	switch (*unit) {
	case 'G': case 'g': shift = 30; break;
	case 'M': case 'm': shift = 20; break;
	case 'K': case 'k': shift = 10; break;
	case '\0': shift = 0; break;
	default:
		exfat_err("unknown size unit '%c'\n", *unit);
		return -EINVAL;
	}
	if (shift && v > (ULLONG_MAX >> shift)) {
		exfat_err("size value overflows\n");
		return -EINVAL;
	}
	v <<= shift;
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
	unsigned int num_fats      = bs->bsx.num_fats;
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
		if (num_fats == 2) {
			off_t fat2_end = old_fat_end +
					 (off_t)fat_length_sect * sector_size;

			ret = exfat_write_zero(fd, new_fat_bytes, fat2_end);
			if (ret) {
				exfat_err("failed to zero-extend FAT2: %s\n",
					  strerror(-ret));
				return ret;
			}
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

/*
 * Execute the shrink operation.  The block device is NOT truncated here;
 * callers are responsible for truncating the underlying file/device afterward.
 *
 * Write order is crash-safe: stale data is neutralised before the metadata
 * fields that constrain it are shrunk, so a crash mid-way still leaves the
 * old geometry valid rather than pointing at uninitialised content.
 *
 *  1. Zero-fill trailing FAT entries      (mark clusters beyond new boundary free)
 *  2. Zero-fill trailing bitmap bytes     (clear stale allocated bits)
 *  3. Update bitmap dentry DataLength
 *  4. Update fat_length + vol_length + clu_count in both boot sectors
 *  5. Recompute boot checksums
 *  6. fsync
 *  7. fsck.exfat -n
 */
static int do_shrink(int fd, const struct pbr *bs, const char *dev_name,
		     unsigned long long new_vol_sectors,
		     unsigned long long new_clu_count)
{
	unsigned int sector_size     = EXFAT_SECTOR_SIZE(bs);
	unsigned int cluster_size    = EXFAT_CLUSTER_SIZE(bs);
	unsigned int fat_offset_sect = le32_to_cpu(bs->bsx.fat_offset);
	unsigned int fat_length_sect = le32_to_cpu(bs->bsx.fat_length);
	unsigned int clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	unsigned int clu_count       = le32_to_cpu(bs->bsx.clu_count);
	unsigned int num_fats        = bs->bsx.num_fats;
	clus_t bm_start_clu;
	unsigned long long old_bm_bytes, new_bm_bytes;
	unsigned int new_fat_length_sect;
	off_t bitmap_off, dentry_off;
	struct exfat_dentry de;
	void *boot_buf;
	struct pbr *bs_write;
	int ret;

	ret = find_bitmap_dentry(fd, bs, &bm_start_clu, &old_bm_bytes,
				 &dentry_off);
	if (ret)
		return ret;

	if (bm_start_clu < EXFAT_FIRST_CLUSTER) {
		exfat_err("invalid bitmap start cluster %u\n", bm_start_clu);
		return -EINVAL;
	}

	new_bm_bytes = ((unsigned long long)new_clu_count + 7) / 8;

	new_fat_length_sect = (unsigned int)(
		((unsigned long long)(new_clu_count + EXFAT_RESERVED_CLUSTERS)
		 * sizeof(__le32) + sector_size - 1) / sector_size);

	bitmap_off = (off_t)clu_offset_sect * sector_size +
		     (off_t)(bm_start_clu - EXFAT_FIRST_CLUSTER) * cluster_size;

	/* Step 1 ── zero-fill FAT entries for clusters beyond the new boundary */
	{
		off_t fat_trim_off = (off_t)fat_offset_sect * sector_size +
			(off_t)(new_clu_count + EXFAT_RESERVED_CLUSTERS) *
			sizeof(__le32);
		size_t fat_trim_bytes =
			(size_t)(clu_count - (unsigned int)new_clu_count) *
			sizeof(__le32);

		if (fat_trim_bytes) {
			ret = exfat_write_zero(fd, fat_trim_bytes, fat_trim_off);
			if (ret) {
				exfat_err("failed to zero-trim FAT: %s\n",
					  strerror(-ret));
				return ret;
			}
			if (num_fats == 2) {
				off_t fat2_trim_off = fat_trim_off +
					(off_t)fat_length_sect * sector_size;

				ret = exfat_write_zero(fd, fat_trim_bytes,
						       fat2_trim_off);
				if (ret) {
					exfat_err("failed to zero-trim FAT2: %s\n",
						  strerror(-ret));
					return ret;
				}
			}
			exfat_debug("FAT trimmed: entries %llu..%u zeroed\n",
				    new_clu_count + EXFAT_RESERVED_CLUSTERS,
				    clu_count + EXFAT_RESERVED_CLUSTERS - 1);
		}
	}

	/*
	 * Step 2 ── clear bits beyond new_clu_count in the last partial bitmap
	 * byte, then zero-fill the now-unused trailing bitmap bytes.
	 */
	{
		unsigned int partial_bits = (unsigned int)(new_clu_count % 8);

		if (partial_bits) {
			unsigned char b;
			off_t last_off = bitmap_off + (off_t)(new_bm_bytes - 1);

			if (pread(fd, &b, 1, last_off) != 1) {
				exfat_err("failed to read last bitmap byte\n");
				return -EIO;
			}
			b &= (unsigned char)((1u << partial_bits) - 1);
			if (pwrite(fd, &b, 1, last_off) != 1) {
				exfat_err("failed to write last bitmap byte\n");
				return -EIO;
			}
		}

		if (old_bm_bytes > new_bm_bytes) {
			ret = exfat_write_zero(fd,
					       (size_t)(old_bm_bytes - new_bm_bytes),
					       bitmap_off + (off_t)new_bm_bytes);
			if (ret) {
				exfat_err("failed to zero-trim bitmap: %s\n",
					  strerror(-ret));
				return ret;
			}
			exfat_debug("bitmap trimmed: %llu -> %llu bytes\n",
				    old_bm_bytes, new_bm_bytes);
		}
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
	 * sectors.  Read the full sector to avoid clobbering boot_code.
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
	bs_write->bsx.fat_length  = cpu_to_le32(new_fat_length_sect);
	bs_write->bsx.vol_length  = cpu_to_le64(new_vol_sectors);
	bs_write->bsx.clu_count   = cpu_to_le32((unsigned int)new_clu_count);
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

	/* Step 6b ── truncate image file to new size (no-op for block devices) */
	{
		struct stat st;

		if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
			off_t new_size = (off_t)new_vol_sectors * sector_size;

			if (ftruncate(fd, new_size))
				exfat_err("ftruncate: %s (non-fatal)\n",
					  strerror(errno));
			else
				exfat_debug("image truncated to %llu bytes\n",
					    (unsigned long long)new_size);
		}
	}

	exfat_info("Shrink complete: %u -> %llu clusters.\n",
		   clu_count, new_clu_count);

	/* Step 7 ── read-only verification */
	return run_fsck(dev_name);
}

/* ══════════════════════════════════════════════════════════════
 * Cluster relocation — moves allocated clusters that lie at or
 * beyond the new shrink boundary into free space below it.
 * ══════════════════════════════════════════════════════════════ */

#define MAX_DIR_DEPTH	64

struct chain_entry {
	clus_t		head;
	off_t		stream_off;	  /* on-disk offset of STREAM dentry */
	clus_t		dir_clu;	  /* start cluster of containing dir */
	unsigned int	file_dentry_idx;  /* linear index of FILE dentry in dir */
	int		num_ext;	  /* file_num_ext from FILE dentry */
	bool		is_contiguous;
	unsigned long long chain_size;	  /* stream_size (for contiguous chains) */
};

struct chain_map {
	struct chain_entry *entries;
	unsigned int	    count;
	unsigned int	    cap;
};

struct move_entry {
	clus_t		old_clu;
	clus_t		new_clu;	/* filled in Phase 2 */
	bool		is_head;
	clus_t		pred_clu;	/* predecessor (when !is_head) */
	off_t		stream_off;
	clus_t		dir_clu;
	unsigned int	file_dentry_idx;
	int		num_ext;
};

struct move_list {
	struct move_entry *entries;
	unsigned int	   count;
	unsigned int	   cap;
};

/* ── FAT helpers ──────────────────────────────────────────── */

static off_t fat_off(unsigned int fat_offset_sect, unsigned int sector_size,
		     clus_t clu)
{
	return (off_t)fat_offset_sect * sector_size +
	       (off_t)clu * sizeof(__le32);
}

static int read_fat(int fd, unsigned int fat_offset_sect,
		    unsigned int sector_size, clus_t clu, clus_t *next)
{
	__le32 v;

	if (pread(fd, &v, sizeof(v), fat_off(fat_offset_sect, sector_size, clu))
	    != sizeof(v))
		return -EIO;
	*next = le32_to_cpu(v);
	return 0;
}

static int write_fat(int fd, unsigned int fat_offset_sect,
		     unsigned int fat_length_sect,
		     unsigned int sector_size, unsigned int num_fats,
		     clus_t clu, clus_t next)
{
	__le32 v = cpu_to_le32(next);
	off_t off0 = fat_off(fat_offset_sect, sector_size, clu);

	if (pwrite(fd, &v, sizeof(v), off0) != sizeof(v))
		return -EIO;

	if (num_fats == 2) {
		off_t off1 = off0 + (off_t)fat_length_sect * sector_size;

		if (pwrite(fd, &v, sizeof(v), off1) != sizeof(v))
			return -EIO;
	}
	return 0;
}

/* ── Bitmap helper ────────────────────────────────────────── */

static int bitmap_update_bit(int fd, off_t bm_off, clus_t clu, bool set)
{
	unsigned int bit_idx = clu - EXFAT_FIRST_CLUSTER;
	off_t byte_off = bm_off + (off_t)(bit_idx / 8);
	unsigned char mask = (unsigned char)(1u << (bit_idx % 8));
	unsigned char b;

	if (pread(fd, &b, 1, byte_off) != 1)
		return -EIO;
	if (set)
		b |= mask;
	else
		b &= ~mask;
	if (pwrite(fd, &b, 1, byte_off) != 1)
		return -EIO;
	return 0;
}

/* ── Directory dentry offset by linear index ──────────────── */

static int get_dir_dentry_off(int fd, clus_t dir_clu, unsigned int idx,
			      unsigned int cluster_size,
			      unsigned int sector_size,
			      unsigned int fat_offset_sect,
			      unsigned int clu_offset_sect,
			      off_t *out)
{
	unsigned int dpc = cluster_size / sizeof(struct exfat_dentry);
	unsigned int steps = idx / dpc;
	clus_t clu = dir_clu;
	unsigned int i;

	for (i = 0; i < steps; i++) {
		__le32 v;
		off_t foff = (off_t)fat_offset_sect * sector_size +
			     (off_t)clu * sizeof(__le32);

		if (pread(fd, &v, sizeof(v), foff) != sizeof(v))
			return -EIO;
		clu = le32_to_cpu(v);
		if (clu < EXFAT_FIRST_CLUSTER || clu == EXFAT_EOF_CLUSTER)
			return -EINVAL;
	}

	*out = (off_t)clu_offset_sect * sector_size +
	       (off_t)(clu - EXFAT_FIRST_CLUSTER) * cluster_size +
	       (off_t)(idx % dpc) * sizeof(struct exfat_dentry);
	return 0;
}

/* ── Dentry-set checksum (same algorithm as exfat_calc_dentry_checksum) ── */

static void reloc_checksum_dentry(const struct exfat_dentry *de,
				  uint16_t *checksum, bool primary)
{
	unsigned int i;
	const uint8_t *b = (const uint8_t *)de;

	*checksum = (*checksum << 15) | (*checksum >> 1);
	*checksum += b[0];
	*checksum = (*checksum << 15) | (*checksum >> 1);
	*checksum += b[1];

	for (i = primary ? 4u : 2u; i < sizeof(*de); i++) {
		*checksum = (*checksum << 15) | (*checksum >> 1);
		*checksum += b[i];
	}
}

/*
 * Read the full dentry set for a chain_entry, compute the EntrySetChecksum,
 * and write the updated FILE dentry back to disk.
 */
static int update_file_checksum(int fd, clus_t dir_clu,
				unsigned int file_dentry_idx, int num_ext,
				unsigned int cluster_size,
				unsigned int sector_size,
				unsigned int fat_offset_sect,
				unsigned int clu_offset_sect)
{
	int total = num_ext + 1;
	struct exfat_dentry *dset;
	uint16_t checksum;
	off_t file_off;
	int i, ret = 0;

	dset = malloc((size_t)total * sizeof(*dset));
	if (!dset)
		return -ENOMEM;

	for (i = 0; i < total; i++) {
		off_t off;

		ret = get_dir_dentry_off(fd, dir_clu, file_dentry_idx + i,
					 cluster_size, sector_size,
					 fat_offset_sect, clu_offset_sect, &off);
		if (ret)
			goto out;
		if (pread(fd, &dset[i], sizeof(*dset), off) !=
		    (ssize_t)sizeof(*dset)) {
			ret = -EIO;
			goto out;
		}
	}

	checksum = 0;
	reloc_checksum_dentry(&dset[0], &checksum, true);
	for (i = 1; i < total; i++)
		reloc_checksum_dentry(&dset[i], &checksum, false);

	ret = get_dir_dentry_off(fd, dir_clu, file_dentry_idx,
				 cluster_size, sector_size,
				 fat_offset_sect, clu_offset_sect, &file_off);
	if (ret)
		goto out;

	dset[0].file_checksum = cpu_to_le16(checksum);
	if (pwrite(fd, &dset[0], sizeof(dset[0]), file_off) !=
	    (ssize_t)sizeof(dset[0]))
		ret = -EIO;

out:
	free(dset);
	return ret;
}

/* ── Dynamic-array helpers ────────────────────────────────── */

static int chain_map_add(struct chain_map *cm, const struct chain_entry *ce)
{
	if (cm->count == cm->cap) {
		unsigned int new_cap = cm->cap ? cm->cap * 2 : 64;
		struct chain_entry *tmp;

		tmp = realloc(cm->entries, new_cap * sizeof(*tmp));
		if (!tmp)
			return -ENOMEM;
		cm->entries = tmp;
		cm->cap = new_cap;
	}
	cm->entries[cm->count++] = *ce;
	return 0;
}

static int move_list_add(struct move_list *ml, clus_t old_clu,
			 bool is_head, clus_t pred_clu,
			 const struct chain_entry *ce)
{
	struct move_entry *me;

	if (ml->count == ml->cap) {
		unsigned int new_cap = ml->cap ? ml->cap * 2 : 64;
		struct move_entry *tmp;

		tmp = realloc(ml->entries, new_cap * sizeof(*tmp));
		if (!tmp)
			return -ENOMEM;
		ml->entries = tmp;
		ml->cap = new_cap;
	}
	me = &ml->entries[ml->count++];
	me->old_clu       = old_clu;
	me->new_clu       = EXFAT_FREE_CLUSTER;
	me->is_head       = is_head;
	me->pred_clu      = pred_clu;
	me->stream_off    = ce->stream_off;
	me->dir_clu       = ce->dir_clu;
	me->file_dentry_idx = ce->file_dentry_idx;
	me->num_ext       = ce->num_ext;
	return 0;
}

/* ── Phase 0: scan directory tree ─────────────────────────── */

static int scan_directory(int fd, unsigned int sector_size,
			  unsigned int cluster_size,
			  unsigned int fat_offset_sect,
			  unsigned int clu_offset_sect,
			  clus_t dir_clu,
			  bool dir_is_contiguous,
			  unsigned long long dir_size,
			  struct chain_map *cm, int depth)
{
	int dpc = (int)(cluster_size / sizeof(struct exfat_dentry));
	clus_t clu = dir_clu;
	clus_t dir_num_clus = dir_is_contiguous && dir_size
		? (clus_t)((dir_size + cluster_size - 1) / cluster_size) : 0;
	unsigned int linear_idx = 0;
	int sec_remain = 0;
	int saved_num_ext = 0;
	unsigned int saved_file_idx = 0;
	bool is_subdir = false, got_stream = false;

	if (depth > MAX_DIR_DEPTH) {
		exfat_err("directory recursion depth limit exceeded\n");
		return -EINVAL;
	}

	while (clu >= EXFAT_FIRST_CLUSTER && clu != EXFAT_EOF_CLUSTER) {
		off_t clu_off = (off_t)clu_offset_sect * sector_size +
				(off_t)(clu - EXFAT_FIRST_CLUSTER) * cluster_size;
		int i;

		for (i = 0; i < dpc; i++, linear_idx++) {
			struct exfat_dentry de;
			off_t de_off = clu_off + (off_t)i * sizeof(de);

			if (pread(fd, &de, sizeof(de), de_off) !=
			    (ssize_t)sizeof(de))
				return -EIO;

			if (de.type == EXFAT_LAST)
				return 0;

			if (sec_remain > 0) {
				sec_remain--;
				if (!got_stream &&
				    de.type == EXFAT_STREAM) {
					clus_t head =
						le32_to_cpu(de.stream_start_clu);

					got_stream = true;
					if (head >= EXFAT_FIRST_CLUSTER) {
						struct chain_entry ce = {
							.head = head,
							.stream_off = de_off,
							.dir_clu = dir_clu,
							.file_dentry_idx = saved_file_idx,
							.num_ext = saved_num_ext,
							.is_contiguous = !!(de.stream_flags & EXFAT_SF_CONTIGUOUS),
							.chain_size = le64_to_cpu(de.stream_size),
						};
						int ret = chain_map_add(cm, &ce);

						if (ret)
							return ret;
						if (is_subdir) {
							ret = scan_directory(
								fd, sector_size,
								cluster_size,
								fat_offset_sect,
								clu_offset_sect,
								head,
								ce.is_contiguous,
								ce.chain_size,
								cm,
								depth + 1);
							if (ret)
								return ret;
						}
					}
				}
				continue;
			}

			/* primary dentry */
			got_stream = false;
			is_subdir = false;

			if (de.type != EXFAT_FILE)
				continue;

			saved_file_idx = linear_idx;
			saved_num_ext  = de.file_num_ext;
			sec_remain     = de.file_num_ext;
			is_subdir      = !!(le16_to_cpu(de.file_attr) &
					    ATTR_SUBDIR);
		}

		/* advance to next cluster */
		if (dir_is_contiguous) {
			clus_t offset = clu - dir_clu + 1;

			clu = (dir_num_clus && offset >= dir_num_clus)
				? EXFAT_EOF_CLUSTER : clu + 1;
		} else {
			clus_t next;
			int ret = read_fat(fd, fat_offset_sect, sector_size,
					   clu, &next);

			if (ret)
				return ret;
			clu = next;
		}
	}
	return 0;
}

/* ── Phase 0.5: write FAT entries and clear NoFatChain flag ── */

static int normalize_contiguous_chain(int fd,
				      const struct chain_entry *ce,
				      unsigned int sector_size,
				      unsigned int cluster_size,
				      unsigned int fat_offset_sect,
				      unsigned int fat_length_sect,
				      unsigned int num_fats,
				      unsigned int clu_offset_sect)
{
	unsigned long long num_clus;
	struct exfat_dentry sde;
	clus_t c;
	unsigned long long k;
	int ret;

	if (!ce->is_contiguous)
		return 0;

	num_clus = ce->chain_size
		   ? (ce->chain_size + cluster_size - 1) / cluster_size
		   : 0;
	if (num_clus == 0)
		return 0;

	/* Write sequential FAT entries */
	for (k = 0; k + 1 < num_clus; k++) {
		c = ce->head + (clus_t)k;
		ret = write_fat(fd, fat_offset_sect, fat_length_sect,
				sector_size, num_fats, c, c + 1);
		if (ret)
			return ret;
	}
	c = ce->head + (clus_t)(num_clus - 1);
	ret = write_fat(fd, fat_offset_sect, fat_length_sect,
			sector_size, num_fats, c, EXFAT_EOF_CLUSTER);
	if (ret)
		return ret;

	/* Clear NoFatChain flag in STREAM dentry */
	if (pread(fd, &sde, sizeof(sde), ce->stream_off) !=
	    (ssize_t)sizeof(sde))
		return -EIO;
	sde.stream_flags &= (uint8_t)~EXFAT_SF_CONTIGUOUS;
	if (pwrite(fd, &sde, sizeof(sde), ce->stream_off) !=
	    (ssize_t)sizeof(sde))
		return -EIO;

	/* Recompute FILE dentry checksum */
	return update_file_checksum(fd, ce->dir_clu, ce->file_dentry_idx,
				    ce->num_ext, cluster_size, sector_size,
				    fat_offset_sect, clu_offset_sect);
}

/* ── Helper: find upcase table start cluster and size ─────── */

static int find_upcase_start_clu(int fd, const struct pbr *bs,
				 clus_t *start_clu,
				 unsigned long long *size_bytes)
{
	unsigned int sector_size  = EXFAT_SECTOR_SIZE(bs);
	unsigned int cluster_size = EXFAT_CLUSTER_SIZE(bs);
	unsigned int clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	clus_t root_clu = le32_to_cpu(bs->bsx.root_cluster);
	int max_de = (int)(cluster_size / sizeof(struct exfat_dentry));
	off_t root_off = (off_t)clu_offset_sect * sector_size +
			 (off_t)(root_clu - EXFAT_FIRST_CLUSTER) * cluster_size;
	int i;

	for (i = 0; i < max_de; i++) {
		struct exfat_dentry de;
		off_t de_off = root_off + (off_t)i * sizeof(de);

		if (pread(fd, &de, sizeof(de), de_off) != (ssize_t)sizeof(de))
			return -EIO;
		if (de.type == EXFAT_LAST)
			break;
		if (de.type == EXFAT_UPCASE) {
			*start_clu = le32_to_cpu(de.upcase_start_clu);
			if (size_bytes)
				*size_bytes = le64_to_cpu(de.upcase_size);
			return 0;
		}
	}
	exfat_err("upcase table dentry not found in root directory\n");
	return -EINVAL;
}

/*
 * Walk the FAT chain starting at start_clu and return an error if any
 * cluster in the chain is >= boundary.  Uses arithmetic for contiguous
 * chains (fat[c] == 0) when is_contiguous is true.
 */
static int check_chain_in_bounds(int fd, unsigned int fat_offset_sect,
				 unsigned int sector_size,
				 clus_t start_clu, clus_t boundary,
				 const char *name)
{
	clus_t clu = start_clu;

	while (clu >= EXFAT_FIRST_CLUSTER && clu != EXFAT_EOF_CLUSTER) {
		if (clu >= boundary) {
			exfat_err("SHRINK BLOCKED — %s cluster %u is beyond "
				  "new boundary\n", name, clu);
			return -EINVAL;
		}
		{
			clus_t next;
			int ret = read_fat(fd, fat_offset_sect, sector_size,
					   clu, &next);

			if (ret)
				return ret;
			/*
			 * A FAT entry of 0 (EXFAT_FREE_CLUSTER) for a cluster
			 * that is part of a contiguous chain means there is no
			 * chain beyond this point — treat it as EOF.
			 */
			if (next == EXFAT_FREE_CLUSTER)
				break;
			clu = next;
		}
	}
	return 0;
}

/*
 * try_relocate — move every allocated cluster at or beyond new_clu_count
 * into free space below it, update FAT / dentries / bitmap, then fsync.
 *
 * Returns 0 on success; prints its own diagnostic and returns <0 on failure.
 * The caller must have opened fd read-write.
 */
static int try_relocate(int fd, const struct pbr *bs,
			clus_t new_clu_count)
{
	unsigned int sector_size     = EXFAT_SECTOR_SIZE(bs);
	unsigned int cluster_size    = EXFAT_CLUSTER_SIZE(bs);
	unsigned int fat_offset_sect = le32_to_cpu(bs->bsx.fat_offset);
	unsigned int fat_length_sect = le32_to_cpu(bs->bsx.fat_length);
	unsigned int clu_offset_sect = le32_to_cpu(bs->bsx.clu_offset);
	unsigned int num_fats        = bs->bsx.num_fats;
	clus_t root_clu  = le32_to_cpu(bs->bsx.root_cluster);
	clus_t boundary  = new_clu_count + EXFAT_FIRST_CLUSTER;

	clus_t bm_start_clu, uc_start_clu;
	unsigned long long bm_bytes;
	off_t bm_off;

	struct chain_map cm = {0};
	struct move_list ml = {0};
	unsigned char *bmap = NULL;
	void *data_buf = NULL;
	unsigned int i;
	int ret;

	/* ── Locate allocation bitmap ── */
	ret = find_bitmap_dentry(fd, bs, &bm_start_clu, &bm_bytes, NULL);
	if (ret)
		return ret;

	/* ── Find upcase start cluster ── */
	ret = find_upcase_start_clu(fd, bs, &uc_start_clu, NULL);
	if (ret)
		return ret;

	/* ── Guard: walk full FAT chains of all metadata objects ── */
	ret = check_chain_in_bounds(fd, fat_offset_sect, sector_size,
				    bm_start_clu, boundary, "bitmap");
	if (ret)
		return ret;
	ret = check_chain_in_bounds(fd, fat_offset_sect, sector_size,
				    uc_start_clu, boundary, "upcase table");
	if (ret)
		return ret;
	ret = check_chain_in_bounds(fd, fat_offset_sect, sector_size,
				    root_clu, boundary, "root directory");
	if (ret)
		return ret;

	bm_off = (off_t)clu_offset_sect * sector_size +
		 (off_t)(bm_start_clu - EXFAT_FIRST_CLUSTER) * cluster_size;

	/* ── Read allocation bitmap into a working copy ── */
	bmap = malloc(bm_bytes);
	if (!bmap) {
		ret = -ENOMEM;
		goto out;
	}
	if (pread(fd, bmap, bm_bytes, bm_off) != (ssize_t)bm_bytes) {
		ret = -EIO;
		goto free_bmap;
	}

	/* ── Phase 0: build chain map from directory tree ── */
	ret = scan_directory(fd, sector_size, cluster_size,
			     fat_offset_sect, clu_offset_sect,
			     root_clu, false, 0, &cm, 0);
	if (ret)
		goto free_bmap;

	/*
	 * ── Phase 0.5: normalise contiguous chains ──
	 * Write sequential FAT entries for chains with NoFatChain set so that
	 * Phase 1 can walk every chain uniformly via the FAT.
	 */
	for (i = 0; i < cm.count; i++) {
		if (!cm.entries[i].is_contiguous)
			continue;
		ret = normalize_contiguous_chain(fd, &cm.entries[i],
						 sector_size, cluster_size,
						 fat_offset_sect,
						 fat_length_sect, num_fats,
						 clu_offset_sect);
		if (ret)
			goto free_bmap;
	}

	/* ── Phase 1: build move list ── */
	for (i = 0; i < cm.count; i++) {
		struct chain_entry *ce = &cm.entries[i];
		clus_t clu  = ce->head;
		clus_t prev = EXFAT_FREE_CLUSTER;
		bool is_head = true;

		while (clu >= EXFAT_FIRST_CLUSTER &&
		       clu != EXFAT_EOF_CLUSTER) {
			if (clu >= boundary) {
				ret = move_list_add(&ml, clu, is_head,
						    prev, ce);
				if (ret)
					goto free_ml;
			}

			{
				clus_t next;

				ret = read_fat(fd, fat_offset_sect,
					       sector_size, clu, &next);
				if (ret)
					goto free_ml;
				prev = clu;
				clu  = next;
			}
			is_head = false;
		}
	}

	if (ml.count == 0) {
		/* nothing to relocate */
		ret = 0;
		goto free_ml;
	}

	/* ── Phase 2: dry-allocate replacement clusters below boundary ── */
	{
		unsigned int alloc_idx = 0;
		clus_t c;

		for (c = EXFAT_FIRST_CLUSTER;
		     c < boundary && alloc_idx < ml.count; c++) {
			unsigned int bit_idx = c - EXFAT_FIRST_CLUSTER;

			if (!(bmap[bit_idx / 8] & (1u << (bit_idx % 8)))) {
				ml.entries[alloc_idx].new_clu = c;
				bmap[bit_idx / 8] |=
					(unsigned char)(1u << (bit_idx % 8));
				alloc_idx++;
			}
		}

		if (alloc_idx < ml.count) {
			exfat_err("SHRINK BLOCKED — insufficient free space "
				  "below new boundary\n");
			ret = -ENOSPC;
			goto free_ml;
		}
	}

	/* ── Phase 3: copy data, redirect FAT, update dentries + bitmap ── */
	data_buf = malloc(cluster_size);
	if (!data_buf) {
		ret = -ENOMEM;
		goto free_ml;
	}

	for (i = 0; i < ml.count; i++) {
		struct move_entry *me = &ml.entries[i];
		clus_t old = me->old_clu;
		clus_t neu = me->new_clu;
		off_t old_off = (off_t)clu_offset_sect * sector_size +
				(off_t)(old - EXFAT_FIRST_CLUSTER) *
				cluster_size;
		off_t new_off = (off_t)clu_offset_sect * sector_size +
				(off_t)(neu - EXFAT_FIRST_CLUSTER) *
				cluster_size;
		clus_t succ;

		/* 1. Copy cluster data */
		if (pread(fd, data_buf, cluster_size, old_off) !=
		    (ssize_t)cluster_size) {
			ret = -EIO;
			goto free_buf;
		}
		if (pwrite(fd, data_buf, cluster_size, new_off) !=
		    (ssize_t)cluster_size) {
			ret = -EIO;
			goto free_buf;
		}

		/* 2. Read old FAT entry (successor) */
		ret = read_fat(fd, fat_offset_sect, sector_size, old, &succ);
		if (ret)
			goto free_buf;

		/* 3. Write fat[new] = successor */
		ret = write_fat(fd, fat_offset_sect, fat_length_sect,
				sector_size, num_fats, neu, succ);
		if (ret)
			goto free_buf;

		/* 4a. Update predecessor */
		if (me->is_head) {
			/* Update stream_start_clu in STREAM dentry */
			struct exfat_dentry sde;

			if (pread(fd, &sde, sizeof(sde), me->stream_off) !=
			    (ssize_t)sizeof(sde)) {
				ret = -EIO;
				goto free_buf;
			}
			sde.stream_start_clu = cpu_to_le32(neu);
			if (pwrite(fd, &sde, sizeof(sde), me->stream_off) !=
			    (ssize_t)sizeof(sde)) {
				ret = -EIO;
				goto free_buf;
			}
			/* Recompute FILE dentry checksum */
			ret = update_file_checksum(fd, me->dir_clu,
						   me->file_dentry_idx,
						   me->num_ext, cluster_size,
						   sector_size, fat_offset_sect,
						   clu_offset_sect);
			if (ret)
				goto free_buf;
		} else {
			/*
			 * Find effective predecessor: if pred_clu itself was
			 * relocated, point to its new address.
			 */
			clus_t eff_pred = me->pred_clu;
			unsigned int j;

			for (j = 0; j < i; j++) {
				if (ml.entries[j].old_clu == eff_pred) {
					eff_pred = ml.entries[j].new_clu;
					break;
				}
			}
			ret = write_fat(fd, fat_offset_sect, fat_length_sect,
					sector_size, num_fats, eff_pred, neu);
			if (ret)
				goto free_buf;
		}

		/* 4b. Clear old FAT entry */
		ret = write_fat(fd, fat_offset_sect, fat_length_sect,
				sector_size, num_fats, old, EXFAT_FREE_CLUSTER);
		if (ret)
			goto free_buf;

		/* 4c. Update on-disk bitmap */
		ret = bitmap_update_bit(fd, bm_off, old, false);
		if (ret)
			goto free_buf;
		ret = bitmap_update_bit(fd, bm_off, neu, true);
		if (ret)
			goto free_buf;

		exfat_debug("relocated cluster %u -> %u\n", old, neu);
	}

	exfat_info("Relocated %u cluster(s) below new boundary.\n",
		   ml.count);

	if (fsync(fd)) {
		exfat_err("fsync after relocation: %s\n", strerror(errno));
		ret = -errno;
		goto free_buf;
	}
	ret = 0;

free_buf:
	free(data_buf);
free_ml:
	free(ml.entries);
free_bmap:
	free(bmap);
	free(cm.entries);
out:
	return ret;
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
		exit(EXIT_SUCCESS);

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
	sectors_per_clu  = 1ULL << bs->bsx.sect_per_clus_bits;

	if (fat_offset_sect >= clu_offset_sect) {
		exfat_err("malformed boot sector: FAT offset (%u) >= cluster heap offset (%u)\n",
			  fat_offset_sect, clu_offset_sect);
		ret = -EINVAL;
		goto free_bs;
	}

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
	if (bitmap_size_bytes >
	    (unsigned long long)(clu_count + 7) / 8 + cluster_size - 1) {
		exfat_err("bitmap dentry size %llu is unreasonably large\n",
			  bitmap_size_bytes);
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

	if (bitmap_size_bytes > (unsigned long long)SSIZE_MAX) {
		exfat_err("bitmap size %llu exceeds SSIZE_MAX\n",
			  bitmap_size_bytes);
		ret = -EINVAL;
		goto free_bitmap;
	}

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

		if (!dry_run) {
			free(bitmap);
			bitmap = NULL;
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

			ret = do_shrink(bd.dev_fd, bs, ui.dev_name,
					new_vol_sectors, new_clu_count);
			goto free_bitmap;
		}
	} else {
		/*
		 * Clusters exist beyond the new boundary.  In dry-run mode
		 * report how many; otherwise relocate them then shrink.
		 */
		if (dry_run) {
			unsigned int n_beyond = 0;
			clus_t c;

			for (c = EXFAT_FIRST_CLUSTER + (clus_t)new_clu_count;
			     c < EXFAT_FIRST_CLUSTER + clu_count; c++) {
				unsigned int bit_idx = c - EXFAT_FIRST_CLUSTER;

				if (bitmap[bit_idx / 8] &
				    (1u << (bit_idx % 8)))
					n_beyond++;
			}
			printf("Result: SHRINK BLOCKED\n"
			       "  %u cluster(s) beyond new boundary;"
			       " re-run without -n to relocate.\n",
			       n_beyond);
			ret = EXIT_FAILURE;
		} else {
			/* Reopen read-write for relocation + shrink */
			free(bitmap);
			bitmap = NULL;
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

			exfat_info("Relocating clusters beyond new boundary...\n");
			ret = try_relocate(bd.dev_fd, bs,
					   (clus_t)new_clu_count);
			if (ret)
				goto free_bitmap; /* already printed error */

			ret = do_shrink(bd.dev_fd, bs, ui.dev_name,
					new_vol_sectors, new_clu_count);
			goto free_bitmap;
		}
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
