/*
 * Helper functions for multiple mount protection(MMP).
 *
 * Copyright (C) 2006, 2007 by Kalpak Shah <kalpak@clusterfs.com>
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"

static int mmp_pagesize(void)
{
#ifdef _SC_PAGESIZE
	int sysval = sysconf(_SC_PAGESIZE);
	if (sysval > 0)
		return sysval;
#endif /* _SC_PAGESIZE */
#ifdef HAVE_GETPAGESIZE
	return getpagesize();
#else
	return 4096;
#endif
}

#define ptr_align(ptr, size)	(void *)(((unsigned long)(ptr) + (size) - 1) & \
					 ~((unsigned long)(size) - 1))
#ifndef O_DIRECT
#define O_DIRECT 0
#endif

errcode_t ext2fs_mmp_read(ext2_filsys fs, blk_t mmp_blk, void *buf)
{
	struct mmp_struct *mmp_cmp;
	errcode_t retval = 0;

	if ((mmp_blk <= fs->super->s_first_data_block) ||
	    (mmp_blk >= fs->super->s_blocks_count))
		return EXT2_ET_MMP_BAD_BLOCK;

	if (fs->mmp_cmp == NULL) {
		/* O_DIRECT in linux 2.4: page aligned
		 * O_DIRECT in linux 2.6: sector aligned
		 * A filesystem cannot be created with blocksize < sector size,
		 * or with blocksize > page_size. */
		int bufsize = fs->blocksize;

		if (bufsize < mmp_pagesize())
			bufsize = mmp_pagesize();
		retval = ext2fs_get_mem(bufsize * 2, &fs->mmp_unaligned_buf);
		if (retval)
			return retval;
		fs->mmp_cmp = ptr_align(fs->mmp_unaligned_buf, bufsize);
	}

	/* ext2fs_open reserves fd0,1,2 to avoid stdio collision */
	if (fs->mmp_fd <= 0) {
		fs->mmp_fd = open(fs->device_name, O_RDWR | O_DIRECT);
		if (fs->mmp_fd < 0) {
			retval = EXT2_ET_MMP_OPEN_DIRECT;
			goto out;
		}
	}

	if (ext2fs_llseek(fs->mmp_fd, mmp_blk * fs->blocksize, SEEK_SET) !=
	    mmp_blk * fs->blocksize) {
		retval = EXT2_ET_LLSEEK_FAILED;
		goto out;
	}

	if (read(fs->mmp_fd, fs->mmp_cmp, fs->blocksize) != fs->blocksize) {
		retval = EXT2_ET_SHORT_READ;
		goto out;
	}

	mmp_cmp = fs->mmp_cmp;
#ifdef EXT2FS_ENABLE_SWAPFS
	if (fs->flags & EXT2_FLAG_SWAP_BYTES)
		ext2fs_swap_mmp(mmp_cmp);
#endif

	if (buf != NULL && buf != fs->mmp_cmp)
		memcpy(buf, fs->mmp_cmp, fs->blocksize);

	if (mmp_cmp->mmp_magic != EXT2_MMP_MAGIC) {
		retval = EXT2_ET_MMP_MAGIC_INVALID;
		goto out;
	}

out:
	return retval;
}

errcode_t ext2fs_mmp_write(ext2_filsys fs, blk_t mmp_blk, void *buf)
{
	struct mmp_struct *mmp_s = buf;
	struct timeval tv;
	errcode_t retval = 0;

	gettimeofday(&tv, 0);
	mmp_s->mmp_time = tv.tv_sec;
	fs->mmp_last_written = tv.tv_sec;

#ifdef EXT2FS_ENABLE_SWAPFS
	if (fs->super->s_magic == ext2fs_swab16(EXT2_SUPER_MAGIC))
		ext2fs_swap_mmp(mmp_s);
#endif

	/* I was tempted to make this use O_DIRECT and the mmp_fd, but
	 * this caused no end of grief, while leaving it as-is works. */
	retval = io_channel_write_blk(fs->io, mmp_blk, -fs->blocksize, buf);

#ifdef EXT2FS_ENABLE_SWAPFS
	if (fs->super->s_magic == ext2fs_swab16(EXT2_SUPER_MAGIC))
		ext2fs_swap_mmp(mmp_s);
#endif

	/* Make sure the block gets to disk quickly */
	io_channel_flush(fs->io);
	return retval;
}

#ifdef HAVE_SRANDOM
#define srand(x) 	srandom(x)
#define rand() 		random()
#endif

unsigned ext2fs_mmp_new_seq()
{
	unsigned new_seq;
	struct timeval tv;

	gettimeofday(&tv, 0);
	srand((getpid() << 16) ^ getuid() ^ tv.tv_sec ^ tv.tv_usec);

	gettimeofday(&tv, 0);
	/* Crank the random number generator a few times */
	for (new_seq = (tv.tv_sec ^ tv.tv_usec) & 0x1F; new_seq > 0; new_seq--)
		rand();

	do {
		new_seq = rand();
	} while (new_seq > EXT2_MMP_SEQ_MAX);

	return new_seq;
}

static errcode_t ext2fs_mmp_reset(ext2_filsys fs)
{
	struct mmp_struct *mmp_s = NULL;
	errcode_t retval = 0;

	if (fs->mmp_buf == NULL) {
		retval = ext2fs_get_mem(fs->blocksize, &fs->mmp_buf);
		if (retval)
			goto out;
	}

	memset(fs->mmp_buf, 0, fs->blocksize);
	mmp_s = fs->mmp_buf;

	mmp_s->mmp_magic = EXT2_MMP_MAGIC;
	mmp_s->mmp_seq = EXT2_MMP_SEQ_CLEAN;
	mmp_s->mmp_time = 0;
#if _BSD_SOURCE || _XOPEN_SOURCE >= 500
	gethostname(mmp_s->mmp_nodename, sizeof(mmp_s->mmp_nodename));
#else
	mmp_s->mmp_nodename[0] = '\0';
#endif
	strncpy(mmp_s->mmp_bdevname, fs->device_name,
		sizeof(mmp_s->mmp_bdevname));

	mmp_s->mmp_check_interval = fs->super->s_mmp_update_interval;
	if (mmp_s->mmp_check_interval < EXT2_MMP_MIN_CHECK_INTERVAL)
		mmp_s->mmp_check_interval = EXT2_MMP_MIN_CHECK_INTERVAL;

	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_buf);
out:
	return retval;
}

errcode_t ext2fs_mmp_clear(ext2_filsys fs)
{
	struct mmp_struct *mmp_s = NULL;
	errcode_t retval = 0;

	if (!(fs->super->s_feature_incompat & EXT4_FEATURE_INCOMPAT_MMP) ||
	    !(fs->flags & EXT2_FLAG_RW))
		return 0;

	retval = ext2fs_mmp_reset(fs);

	return retval;
}

errcode_t ext2fs_mmp_init(ext2_filsys fs)
{
	struct ext2_super_block *sb = fs->super;
	struct mmp_struct *mmp_s = NULL;
	blk_t mmp_block;
	errcode_t retval;

	if (fs->mmp_buf == NULL) {
		retval = ext2fs_get_mem(fs->blocksize, &fs->mmp_buf);
		if (retval)
			goto out;
	}

	retval = ext2fs_alloc_block(fs, 0, fs->mmp_buf, &mmp_block);
	if (retval)
		goto out;

	sb->s_mmp_block = mmp_block;
	sb->s_mmp_update_interval = EXT2_MMP_UPDATE_INTERVAL;

	retval = ext2fs_mmp_reset(fs);
	if (retval)
		goto out;

out:
	return retval;
}

/*
 * Make sure that the fs is not mounted or being fsck'ed while opening the fs.
 */
errcode_t ext2fs_mmp_start(ext2_filsys fs)
{
	struct mmp_struct *mmp_s;
	unsigned seq;
	unsigned int mmp_check_interval;
	errcode_t retval = 0;

	if (fs->mmp_buf == NULL) {
		retval = ext2fs_get_mem(fs->blocksize, &fs->mmp_buf);
		if (retval)
			goto mmp_error;
	}

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	mmp_s = fs->mmp_buf;

	mmp_check_interval = fs->super->s_mmp_update_interval;
	if (mmp_check_interval < EXT2_MMP_MIN_CHECK_INTERVAL)
		mmp_check_interval = EXT2_MMP_MIN_CHECK_INTERVAL;

	seq = mmp_s->mmp_seq;
	if (seq == EXT2_MMP_SEQ_CLEAN)
		goto clean_seq;
	if (seq == EXT2_MMP_SEQ_FSCK) {
		retval = EXT2_ET_MMP_FSCK_ON;
		goto mmp_error;
	}

	if (seq > EXT2_MMP_SEQ_FSCK) {
		retval = EXT2_ET_MMP_UNKNOWN_SEQ;
		goto mmp_error;
	}

	/*
	 * If check_interval in MMP block is larger, use that instead of
	 * check_interval from the superblock.
	 */
	if (mmp_s->mmp_check_interval > mmp_check_interval)
		mmp_check_interval = mmp_s->mmp_check_interval;

	sleep(2 * mmp_check_interval + 1);

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	if (seq != mmp_s->mmp_seq) {
		retval = EXT2_ET_MMP_FAILED;
		goto mmp_error;
	}

clean_seq:
	if (!(fs->flags & EXT2_FLAG_RW))
		goto mmp_error;

	mmp_s->mmp_seq = seq = ext2fs_mmp_new_seq();
#if _BSD_SOURCE || _XOPEN_SOURCE >= 500
	gethostname(mmp_s->mmp_nodename, sizeof(mmp_s->mmp_nodename));
#else
	strcpy(mmp_s->mmp_nodename, "unknown host");
#endif
	strncpy(mmp_s->mmp_bdevname, fs->device_name,
		sizeof(mmp_s->mmp_bdevname));

	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	sleep(2 * mmp_check_interval + 1);

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	if (seq != mmp_s->mmp_seq) {
		retval = EXT2_ET_MMP_FAILED;
		goto mmp_error;
	}

	mmp_s->mmp_seq = EXT2_MMP_SEQ_FSCK;
	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	return 0;

mmp_error:
	return retval;
}

/*
 * Clear the MMP usage in the filesystem.  If this function returns an
 * error EXT2_ET_MMP_CHANGE_ABORT it means the filesystem was modified
 * by some other process while in use, and changes should be dropped, or
 * risk filesystem corruption.
 */
errcode_t ext2fs_mmp_stop(ext2_filsys fs)
{
	struct mmp_struct *mmp, *mmp_cmp;
	errcode_t retval = 0;

	if (!(fs->super->s_feature_incompat & EXT4_FEATURE_INCOMPAT_MMP) ||
	    !(fs->flags & EXT2_FLAG_RW) || (fs->flags & EXT2_FLAG_SKIP_MMP))
		goto mmp_error;

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		goto mmp_error;

	/* Check if the MMP block is not changed. */
	mmp = fs->mmp_buf;
	mmp_cmp = fs->mmp_cmp;
	if (memcmp(mmp, mmp_cmp, sizeof(*mmp_cmp))) {
		retval = EXT2_ET_MMP_CHANGE_ABORT;
		goto mmp_error;
	}

check_skipped:
	mmp_cmp->mmp_seq = EXT2_MMP_SEQ_CLEAN;
	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_cmp);

mmp_error:
	if (fs->mmp_fd > 0) {
		close(fs->mmp_fd);
		fs->mmp_fd = -1;
	}
	if (fs->mmp_buf) {
		ext2fs_free_mem(&fs->mmp_buf);
		fs->mmp_buf = NULL;
	}
	if (fs->mmp_unaligned_buf) {
		ext2fs_free_mem(&fs->mmp_unaligned_buf);
		fs->mmp_unaligned_buf = NULL;
		fs->mmp_cmp = NULL;
	}

	return retval;
}

#define EXT2_MIN_MMP_UPDATE_INTERVAL 60

/*
 * Update the on-disk mmp buffer, after checking that it hasn't been changed.
 */
errcode_t ext2fs_mmp_update(ext2_filsys fs)
{
	struct mmp_struct *mmp, *mmp_cmp;
	struct timeval tv;
	errcode_t retval = 0;

	if (!(fs->super->s_feature_incompat & EXT4_FEATURE_INCOMPAT_MMP) ||
	    !(fs->flags & EXT2_FLAG_RW) || (fs->flags & EXT2_FLAG_SKIP_MMP))
		return 0;

	gettimeofday(&tv, 0);
	if (tv.tv_sec - fs->mmp_last_written < EXT2_MIN_MMP_UPDATE_INTERVAL)
		return 0;

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, NULL);
	if (retval)
		goto mmp_error;

	mmp = fs->mmp_buf;
	mmp_cmp = fs->mmp_cmp;

	if (memcmp(mmp, mmp_cmp, sizeof(*mmp_cmp)))
		return EXT2_ET_MMP_CHANGE_ABORT;

	mmp->mmp_time = tv.tv_sec;
	mmp->mmp_seq = EXT2_MMP_SEQ_FSCK;
	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_buf);

mmp_error:
	return retval;
}
