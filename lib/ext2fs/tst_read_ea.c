/*
 * tst_getsize.c --- this function tests the getsize function
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */

#include <stdio.h>
#include <string.h>
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mntent.h>
#include <assert.h>
#if HAVE_ERRNO_H
#include <errno.h>
#endif

#include "ext2_fs.h"
#include "ext2fs.h"

#define NR_XATTRS 256
char tmpvalue[NR_XATTRS + 1];

struct ea {
	char *name;
	char *value;
};

struct ea *ea_table;

static void init_ea_table(void)
{
	int i;

	ea_table = malloc(sizeof(struct ea) * NR_XATTRS);
	if (ea_table == NULL) {
		perror("maloc failed");
		exit(1);
	}
	for (i = 0; i < NR_XATTRS; i ++) {
		ea_table[i].name = malloc(i + 2 + strlen("user."));
		if (ea_table[i].name == NULL) {
			perror("malloc failed");
			exit(1);
		}
		strcpy(ea_table[i].name, "user.");
		memset(ea_table[i].name + strlen("user."), 'X', i + 1);
		ea_table[i].name[i + 1 + strlen("user.")] = 0;

		ea_table[i].value = malloc(NR_XATTRS - i + 1);
		if (ea_table[i].value == NULL) {
			perror("malloc failed");
			exit(1);
		}
		memset(ea_table[i].value, 'Y', NR_XATTRS - i);
		ea_table[i].value[NR_XATTRS - i] = 0;
	}
}

static int set_xattrs(int fd)
{
	int i;

	for (i = 0; i < NR_XATTRS; i ++) {
		if (fsetxattr(fd, ea_table[i].name, ea_table[i].value,
			      NR_XATTRS - i + 1, XATTR_CREATE) == -1) {
			if (errno != ENOSPC) {
				perror("fsetxattr failed");
				exit(1);
			}
			break;
		}
	}
	printf("\t%d xattrs are set\n", i);
	return i;
}

void get_xattrs1(int fd, int nr)
{
	int i;
	ssize_t size;

	printf("\ttesting fgetxattr .. "); fflush(stdout);

	for (i = 0; i < nr; i ++) {
		size = fgetxattr(fd, ea_table[i].name, tmpvalue,
				 NR_XATTRS - i + 1);
		if (size == -1) {
			perror("fgetxattr failed");
			exit(1);
		}
		if (memcmp(ea_table[i].value, tmpvalue, nr - i + 1)) {
			fprintf(stderr, "value mismatch");
			exit(1);
		}
	}

	printf("%d xattrs are checked, ok\n", i);
}

void get_xattrs2(const char *device, ext2_ino_t ino, int nr)
{
	ext2_filsys fs;
	int i;
	struct ext2_inode *inode;
	errcode_t err;
	int size;

	printf("\ttesting ext2fs_attr_get .. "); fflush(stdout);

	err = ext2fs_open(device, 0, 0, 0, unix_io_manager, &fs);
	assert(err == 0);

	err = ext2fs_get_mem(EXT2_INODE_SIZE(fs->super), &inode);
	if (err) {
		com_err("get_xattrs2", err, "allocating memory");
		exit(1);
	}

	err = ext2fs_read_inode_full(fs, ino, inode,
				     EXT2_INODE_SIZE(fs->super));
	if (err) {
		com_err("get_xattrs2", err, "reading inode");
		exit(1);
	}
	for (i = 0; i < nr; i ++) {
		err = ext2fs_attr_get(fs, inode, EXT2_ATTR_INDEX_USER,
				      ea_table[i].name + strlen("user."),
				      tmpvalue, sizeof(tmpvalue), &size);
		if (err) {
			com_err("get_xattrs2", err, "getting xattr");
			exit(1);
		}
		assert(size == (NR_XATTRS - i + 1));

		if (memcmp(ea_table[i].value, tmpvalue, size)) {
			fprintf(stderr, "value mismatch");
			exit(1);
		}
	}
	ext2fs_close(fs);

	printf("%d xattrs are checked, ok\n", i);
}

int main(int argc, const char *argv[])
{
	ext2_filsys fs;
	FILE *f;
	struct mntent *mnt;
	char *name;
	int fd;
	errcode_t err;
	struct stat st;
	int nr;
	int tested = 0;

	initialize_ext2_error_table();

	init_ea_table();

	f = setmntent(MOUNTED, "r");
	if (!f) {
		fprintf(stderr, "failed to setmntent\n");
		return 1;
	}

	while ((mnt = getmntent(f)) != NULL) {
		if (hasmntopt(mnt, "user_xattr") == NULL)
			continue;
		err = ext2fs_open(mnt->mnt_fsname, 0, 0, 0,
				  unix_io_manager, &fs);
		if (err) {
			com_err("tst_read_ea", err,
				"opening fs %s:%s",
				mnt->mnt_fsname, mnt->mnt_type);
			continue;
		}
		ext2fs_close(fs);

		printf("(%s)%s:%s\n", mnt->mnt_type, mnt->mnt_fsname, mnt->mnt_dir);

		asprintf(&name, "%s/readeaXXXXXX", mnt->mnt_dir);
		fd = mkstemp(name);
		if (fd == -1) {
			perror("mkstemp failed");
			exit(1);
		}
		if (fstat(fd, &st)) {
			perror("fstat failed");
			exit(1);
		}
		nr = set_xattrs(fd);

		sync();
		get_xattrs1(fd, nr);
		close(fd);

		get_xattrs2(mnt->mnt_fsname, st.st_ino, nr);

		unlink(name);
		free(name);
		tested = 1;
	}
	endmntent(f);

	if (!tested)
		fprintf(stderr,
			"\tno ext2 based filesystems mounted with user_xattr\n"
			"\thope it is ok\n");
	return 0;
}
