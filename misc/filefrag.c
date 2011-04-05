/*
 * filefrag.c -- report if a particular file is fragmented
 *
 * Copyright 2003 by Theodore Ts'o.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#ifndef __linux__
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
	fputs("This program is only supported on Linux!\n", stderr);
	exit(EXIT_FAILURE);
}
#else
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
extern char *optarg;
extern int optind;
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fd.h>
#include <ext2fs/ext2_types.h>
#include <ext2fs/fiemap.h>

int verbose = 0;
int blocksize = 0;	/* Use specified blocksize (default 1kB) */
int sync_file = 0;	/* fsync file before getting the mapping */
int xattr_map = 0;	/* get xattr mapping */
int force_bmap = 0;	/* force use of FIBMAP instead of FIEMAP */
int force_extent = 0;	/* print output in extent format always */
int device_offset = 0;	/* extents report device-relative offsets */
int logical_width = 8;
int physical_width = 10;
char *ext_fmt = "%4d: %*llu..%*llu: %*llu..%*llu: %6llu: %s\n";
char *hex_fmt = "%4d: %*llx..%*llx: %*llx..%*llx: %6llx: %s\n";

#define FILEFRAG_FIEMAP_FLAGS_COMPAT (FIEMAP_FLAG_SYNC | FIEMAP_FLAG_XATTR |\
				      FIEMAP_FLAG_DEVICE_ORDER)

#define FIBMAP		_IO(0x00, 1)	/* bmap access */
#define FIGETBSZ	_IO(0x00, 2)	/* get the block size used for bmap */

#define LUSTRE_SUPER_MAGIC 0x0BD00BD0

#define	EXT4_EXTENTS_FL			0x00080000 /* Inode uses extents */
#define	EXT3_IOC_GETFLAGS		_IOR('f', 1, long)

#ifdef HAVE_FSTAT64
#define mystat stat64
#else
#define mystat stat
#endif

static int int_log2(int arg)
{
	int     l = 0;

	arg >>= 1;
	while (arg) {
		l++;
		arg >>= 1;
	}
	return l;
}

static int int_log10(unsigned long long arg)
{
	int     l = 0;

	arg = arg / 10;
	while (arg) {
		l++;
		arg = arg / 10;
	}
	return l;
}

static unsigned int div_ceil(unsigned int a, unsigned int b)
{
	if (!a)
		return 0;
	return ((a - 1) / b) + 1;
}

static int get_bmap(int fd, unsigned long block, unsigned long *phy_blk)
{
	int	ret;
	unsigned int b;

	b = block;
	ret = ioctl(fd, FIBMAP, &b); /* FIBMAP takes pointer to integer */
	if (ret < 0) {
		if (errno == EPERM) {
			fprintf(stderr, "No permission to use FIBMAP ioctl; "
				"must have root privileges\n");
		}
	}
	*phy_blk = b;

	return ret;
}

static void print_extent_header(void)
{
	printf(" ext: %*s %*s length: %sflags:\n",
	       logical_width * 2 + 3,
	       device_offset ? "device_logical:" : "logical_offset:",
	       physical_width * 2 + 3, "physical_offset:",
	       device_offset ? " dev: " : "");
}

static void print_extent_info(struct fiemap_extent *fm_extent, int cur_ex,
			      int blk_shift, struct mystat *fileinfo)
{
	unsigned long long physical_blk;
	unsigned long long logical_blk;
	unsigned long long ext_blks, ext_len;
	char flags[256] = "";

	/* For inline data all offsets should be in bytes, not blocks */
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_INLINE)
		blk_shift = 0;

	ext_len = fm_extent->fe_length >> blk_shift;
	ext_blks = (fm_extent->fe_length - 1) >> blk_shift;
	logical_blk = fm_extent->fe_logical >> blk_shift;
	physical_blk = fm_extent->fe_physical >> blk_shift;

	if (device_offset)
		sprintf(flags, "%04x: ", fm_extent->fe_device);

	if (fm_extent->fe_flags & FIEMAP_EXTENT_UNKNOWN)
		strcat(flags, "unknown,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DELALLOC)
		strcat(flags, "delalloc,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_ENCRYPTED)
		strcat(flags, "encrypted,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_NOT_ALIGNED)
		strcat(flags, "not_aligned,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_INLINE)
		strcat(flags, "inline,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_DATA_TAIL)
		strcat(flags, "tail_packed,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_UNWRITTEN)
		strcat(flags, "unwritten,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_MERGED)
		strcat(flags, "merged,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_NET)
		strcat(flags, "network,");
	if (fm_extent->fe_flags & FIEMAP_EXTENT_NO_DIRECT)
		strcat(flags, "no_direct,");

	if (fm_extent->fe_logical + fm_extent->fe_length >= fileinfo->st_size)
		strcat(flags, "eof,");

	/* Remove trailing comma, if any */
	if (flags[0])
		flags[strlen(flags) - 1] = '\0';

	printf(ext_fmt, cur_ex, logical_width, logical_blk,
		logical_width, logical_blk + ext_blks,
		physical_width, physical_blk,
		physical_width, physical_blk + ext_blks,
		ext_len, flags);
}

static int filefrag_fiemap(int fd, int blk_shift, int *num_extents,
				struct mystat *fileinfo)
{
	char buf[4096] = "";
	struct fiemap *fiemap = (struct fiemap *)buf;
	struct fiemap_extent *fm_ext = &fiemap->fm_extents[0];
	int count = (sizeof(buf) - sizeof(*fiemap)) /
			sizeof(struct fiemap_extent);
	unsigned long long next_physical = 0;
	unsigned long flags = 0;
	unsigned int i;
	static int fiemap_incompat_printed;
	int fiemap_header_printed = 0;
	int tot_extents = 1, n = 0;
	int previous_device = 0;
	int last = 0;
	int rc;

	fiemap->fm_length = ~0ULL;

	/* If extents are going to be device ordered, then we may need to use
	 * the first extent to pass the next starting offset and the device. */
	memset(fiemap, 0, sizeof(struct fiemap) + sizeof(struct fiemap_extent));

	if (!verbose && !force_extent)
		count = 0;

	if (sync_file)
		flags |= FIEMAP_FLAG_SYNC;

	if (xattr_map)
		flags |= FIEMAP_FLAG_XATTR;

	if (device_offset)
		flags |= FIEMAP_FLAG_DEVICE_ORDER;

retry_wo_device_offset:
	do {
		fiemap->fm_length = ~0ULL;
		fiemap->fm_flags = flags;
		fiemap->fm_extent_count = count;
		rc = ioctl(fd, FS_IOC_FIEMAP, (unsigned long) fiemap);
		if (rc < 0) {
			if (errno == EBADR && fiemap_incompat_printed == 0) {
				printf("FIEMAP failed with unsupported "
				       "flags %x\n", fiemap->fm_flags);
				fiemap_incompat_printed = 1;
			} else if (rc == EBADR && (fiemap->fm_flags &
						   FIEMAP_FLAG_DEVICE_ORDER)) {
				flags &= ~FIEMAP_FLAG_DEVICE_ORDER;
				goto retry_wo_device_offset;
			}
			return rc;
		}

		if (verbose && !fiemap_header_printed) {
			print_extent_header();
			fiemap_header_printed = 1;
		}

		if (!verbose) {
			*num_extents = fiemap->fm_mapped_extents;
			goto out;
		}

		/* If 0 extents are returned, then more ioctls are not needed */
		if (fiemap->fm_mapped_extents == 0)
			break;

		for (i = 0; i < fiemap->fm_mapped_extents; i++) {
			unsigned long long physical, logical;

			physical = fm_ext[i].fe_physical;
			logical  = fm_ext[i].fe_logical;

			if (previous_device != fm_ext[i].fe_device)
				previous_device = fm_ext[i].fe_device;

			if (logical && physical != next_physical)
				tot_extents++;
			else
				last = 0;
			print_extent_info(&fm_ext[i], n, blk_shift, fileinfo);

			next_physical = physical + fm_ext[i].fe_length;
			if (fm_ext[i].fe_flags & FIEMAP_EXTENT_LAST)
				last = 1;
			n++;
		}

		/* For DEVICE_ORDER mappings, if EXTENT_LAST not yet found then
		 * fm_start needs to be the same as it was for earlier ioctl.
		 * The first extent is used to pass the end offset and device
		 * of the last FIEMAP call.  Otherwise, we ask for extents
		 * starting from where the last mapping ended. */
		if (flags & FIEMAP_FLAG_DEVICE_ORDER) {
			fm_ext[0].fe_logical = fm_ext[i - 1].fe_logical +
						      fm_ext[i - 1].fe_length;
			fm_ext[0].fe_device = fm_ext[i - 1].fe_device;
			fiemap->fm_start = 0;
		} else {
			fiemap->fm_start = fm_ext[i - 1].fe_logical +
				    fm_ext[i - 1].fe_length;
		}
	} while (last == 0);

	*num_extents = tot_extents;
out:
	return 0;
}

#define EXT2_DIRECT	12

static int filefrag_fibmap(int fd, int blk_shift, int *num_extents,
			   struct mystat *fileinfo,
			   unsigned long numblocks, int is_ext2)
{
	struct fiemap_extent	fm_ext;
	unsigned long		i, last_block;
	unsigned long long	logical;
				/* Blocks per indirect block */
	const long		bpib = fileinfo->st_blksize / 4;
	int			count;

	if (force_extent) {
		memset(&fm_ext, 0, sizeof(fm_ext));
		fm_ext.fe_device = fileinfo->st_dev;
	}

	for (i = 0, logical = 0, *num_extents = 0, count = last_block = 0;
	     i < numblocks;
	     i++, logical += fileinfo->st_blksize) {
		unsigned long block = 0;
		int rc;

		if (is_ext2 && last_block) {
			if (((i - EXT2_DIRECT) % bpib) == 0)
				last_block++;
			if (((i - EXT2_DIRECT - bpib) % (bpib * bpib)) == 0)
				last_block++;
			if (((i - EXT2_DIRECT - bpib - bpib * bpib) %
			     (((unsigned long long)bpib )* bpib * bpib)) == 0)
				last_block++;
		}
		rc = get_bmap(fd, i, &block);
		if (rc < 0)
			return rc;
		if (block == 0)
			continue;
		if (!*num_extents) {
			(*num_extents)++;
			if (force_extent) {
				print_extent_header();
				fm_ext.fe_physical = block*fileinfo->st_blksize;
			}
		}
		count++;
		if (force_extent &&
		    (last_block && (block != last_block + 1) ||
		     fm_ext.fe_logical + fm_ext.fe_length != logical)) {
			print_extent_info(&fm_ext, *num_extents - 1,
					  blk_shift, fileinfo);
			fm_ext.fe_logical = logical;
			fm_ext.fe_physical = block*fileinfo->st_blksize;
			fm_ext.fe_length = 0;
			(*num_extents)++;
		} else if (verbose && last_block && (block != last_block + 1)) {
			printf("Discontinuity: Block %ld is at %lu (was %lu)\n",
			       i, block, last_block + 1);
			(*num_extents)++;
		}
		fm_ext.fe_length += fileinfo->st_blksize;
		last_block = block;
	}

	if (force_extent)
		print_extent_info(&fm_ext, *num_extents - 1,
				  blk_shift, fileinfo);

	return count;
}

static void frag_report(const char *filename)
{
	struct statfs	fsinfo;
	struct mystat	fileinfo;
	int		blk_shift;
	long		fd;
	unsigned long	numblocks;
	int		data_blocks_per_cyl = 1;
	int		num_extents = 1, expected;
	int		is_ext2 = 0;
	static int	last_device;
	unsigned int	flags;
	int		width;

#ifdef HAVE_OPEN64
	fd = open64(filename, O_RDONLY);
#else
	fd = open(filename, O_RDONLY);
#endif
	if (fd < 0) {
		perror("open");
		return;
	}

	if (statfs(filename, &fsinfo) < 0) {
		perror("statfs");
		return;
	}
#ifdef HAVE_FSTAT64
	if (stat64(filename, &fileinfo) < 0) {
#else
	if (stat(filename, &fileinfo) < 0) {
#endif
		perror("stat");
		return;
	}
	fileinfo.st_blksize = fsinfo.f_bsize;
	if (ioctl(fd, EXT3_IOC_GETFLAGS, &flags) < 0)
		flags = 0;
	if (!(flags & EXT4_EXTENTS_FL) &&
	    ((fsinfo.f_type == 0xef51) || (fsinfo.f_type == 0xef52) ||
	     (fsinfo.f_type == 0xef53)))
		is_ext2++;
	if (verbose && last_device != fileinfo.st_dev)
		printf("Filesystem type is: %lx\n",
		       (unsigned long) fsinfo.f_type);

	/* Check if filesystem is Lustre.  Always print in extent format
	 * with 1kB blocks, using the device-relative logical offsets. */
	if (fsinfo.f_type == LUSTRE_SUPER_MAGIC) {
		is_ext2 = 0;
		force_extent = 1;
		device_offset = 1;
		blocksize = blocksize ?: 1024;
	}

	if (is_ext2) {
		long cylgroups = div_ceil(fsinfo.f_blocks, fsinfo.f_bsize * 8);

		if (verbose && last_device != fileinfo.st_dev)
		     printf("Filesystem cylinder groups is approximately %ld\n",
			    cylgroups);

		data_blocks_per_cyl = fsinfo.f_bsize * 8 -
					(fsinfo.f_files / 8 / cylgroups) - 3;
	}
	last_device = fileinfo.st_dev;

	width = int_log10(fsinfo.f_blocks);
	if (width > physical_width)
		physical_width = width;

	numblocks = (fileinfo.st_size + fsinfo.f_bsize - 1) / fsinfo.f_bsize;
	if (blocksize != 0)
		blk_shift = int_log2(blocksize);
	else
		blk_shift = int_log2(fsinfo.f_bsize);

	width = int_log10(numblocks);
	if (width > logical_width)
		logical_width = width;
	if (verbose)
		printf("File size of %s is %llu (%lu block%s of %d bytes)\n",
		       filename, (unsigned long long)fileinfo.st_size,
		       numblocks * fsinfo.f_bsize >> blk_shift,
		       numblocks == 1 ? "" : "s", 1 << blk_shift);

	if (force_bmap ||
	    filefrag_fiemap(fd, blk_shift, &num_extents, &fileinfo) != 0) {
		expected = filefrag_fibmap(fd, blk_shift, &num_extents,
					   &fileinfo, numblocks, is_ext2);
		if (expected < 0) {
			if (errno == EINVAL || errno == ENOTTY) {
				fprintf(stderr, "%s: FIBMAP unsupported\n",
					filename);
			} else if (errno != EPERM) {
				fprintf(stderr, "%s: FIBMAP error: %s",
					filename, strerror(errno));
			}
			goto out_close;
		}
		expected = expected / data_blocks_per_cyl + 1;
	}

	if (num_extents == 1)
		printf("%s: 1 extent found", filename);
	else
		printf("%s: %d extents found", filename, num_extents);
	if (is_ext2 && expected < num_extents)
		printf(", perfection would be %d extent%s\n", expected,
			(expected > 1) ? "s" : "");
	else
		fputc('\n', stdout);
out_close:
	close(fd);
}

static void usage(const char *progname)
{
	fprintf(stderr, "Usage: %s [-b{blocksize}] [-BeklsvxX] file ...\n",
		progname);
	exit(1);
}

int main(int argc, char**argv)
{
	char **cpp;
	int c;

	while ((c = getopt(argc, argv, "Bb::eklsvxX")) != EOF)
		switch (c) {
		case 'B':
			force_bmap++;
			force_extent = 0;
			break;
		case 'b':
			if (optarg) {
				char *end;
				blocksize = strtoul(optarg, &end, 0);
				if (end) {
					switch (end[0]) {
					case 'g':
					case 'G': blocksize *= 1024;
						/* no break */
					case 'm':
					case 'M': blocksize *= 1024;
						/* no break */
					case 'k':
					case 'K': blocksize *= 1024;
						break;
					default: break;
					}
				}
			} else { /* allow -b without argument, for compat */
				fprintf(stderr, "%s: -b needs a blocksize "
					"option, assuming 1024-byte blocks.\n",
					argv[0]);
				blocksize = 1024;
			}
			break;
		case 'e':
			force_extent++;
			if (!verbose)
				verbose++;
			force_bmap = 0;
			break;
		case 'k':
			blocksize = 1024;
			break;
		case 'l':
			device_offset++;
			break;
		case 's':
			sync_file++;
			break;
		case 'v':
			verbose++;
			break;
		case 'x':
			xattr_map++;
			break;
		case 'X':
			ext_fmt = hex_fmt;
			break;
		default:
			usage(argv[0]);
			break;
		}
	if (optind == argc)
		usage(argv[0]);
	for (cpp=argv+optind; *cpp; cpp++)
		frag_report(*cpp);
	return 0;
}
#endif
