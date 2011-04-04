#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE		/* for getdate */
#define _XOPEN_SOURCE_EXTENDED	/* for getdate */

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <ext2fs/ext2fs.h>
#include <string.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/errno.h>

ext2_filsys fs;
const char *database = "e2scan.db";
int readahead_groups = 1; /* by default readahead one group inode table */
FILE *outfile;

void usage(char *prog)
{
	fprintf(stderr,
#if defined(HAVE_SQLITE3) && defined(HAVE_SQLITE3_H)
		"\nUsage: %s {-l | -f} [ options ] device-filename\nModes:"
		"\t-f: create file database\n"
#else
		"\nUsage: %s -l [ options ] device-filename\nModes:"
#endif
		"\t-l: list recently changed files\n"
		"Options:\n"
		"\t-a groups: readahead 'groups' inode tables (default %d)\n"
		"\t-b blocks: buffer 'blocks' inode table blocks\n"
		"\t-C chdir: list files relative to 'chdir' in filesystem\n"
		"\t-d database: output database filename (default %s)\n"
		"\t-D: list not only files, but directories as well\n"
		"\t-n filename: list files newer than 'filename'\n"
		"\t-N date: list files newer than 'date' (default 1 day, "
							 "0 for all files)\n"
		"\t-o outfile: output file list to 'outfile'\n",
		prog, readahead_groups, database);
	exit(1);
}

#define SM_NONE 0
#define SM_DATABASE 1 /* -f */
#define SM_FILELIST 2 /* -l */

struct {
	int mode;
	int nr;
	union {
		struct {
			int fd;
			int nr_commands;
		} db;
		struct {
			/* number of files newer than specified time */
			ext2_ino_t nr_files;
			ext2_ino_t nr_dirs;
			/* number of files reported */
			ext2_ino_t nr_reported;
			time_t mtimestamp;
			time_t ctimestamp;
			int with_dirs;
		} fl;
	};
} scan_data = { .mode = SM_FILELIST, };

/* db.c */
pid_t fork_db_creation(const char *database);
void database_iscan_action(ext2_ino_t ino,
			   struct ext2_inode *inode, int fd, char *buf);
int database_dblist_iterate_cb(ext2_ino_t dir, struct ext2_dir_entry *dirent,
			       int namelen, int fd);

/* filelist.c */
void filelist_iscan_action(ext2_ino_t ino,
			   struct ext2_inode *inode, char *buf);
int filelist_dblist_iterate_cb(ext2_ino_t dirino,
			       struct ext2_dir_entry *dirent,
			       int namelen);
int create_root_dentries(char *root);
void report_root(void);


static void get_timestamps(const char *filename)
{
	struct stat st;

	if (stat(filename, &st) == -1) {
		perror("failed to stat file");
		exit(1);
	}
	scan_data.fl.mtimestamp = st.st_mtime;
	scan_data.fl.ctimestamp = st.st_ctime;
}

/*
 * callback for ext2fs_block_iterate2, it adds directory leaf blocks
 * to dblist
 */
int block_iterate_cb(ext2_filsys fs, blk_t  *block_nr,
		     e2_blkcnt_t blockcnt,
		     blk_t ref_block EXT2FS_ATTR((unused)),
		     int ref_offset EXT2FS_ATTR((unused)),
		     void *priv_data)
{
	int ret;
	ext2_ino_t *ino;

	if ((int) blockcnt < 0)
		/* skip indirect blocks */
		return 0;
	ret = 0;
	ino = priv_data;
	if (ext2fs_add_dir_block(fs->dblist, *ino, *block_nr, (int) blockcnt))
		ret |= BLOCK_ABORT;

	return ret;
}

/*
 * done_group callback for inode scan.
 * When i-th group of inodes is scanned over, readahead for i+2-th
 * group is issued. Inode table readahead for two first groups is
 * issued before scan begin.
 */
errcode_t done_group_callback(ext2_filsys fs, ext2_inode_scan scan,
			      dgrp_t group, void *vp)
{
	dgrp_t ra_group;
	unsigned long ra_start;
	int ra_size;

	if (readahead_groups <= 0)
		return 0;

	if (((group + 1) % readahead_groups) != 0)
		return 0;

	ra_group = group + 1 + readahead_groups;
	if (ra_group >= fs->group_desc_count)
		return 0;

	ra_start = fs->group_desc[ra_group].bg_inode_table;
	if (ra_group + readahead_groups > fs->group_desc_count)
		ra_size = fs->group_desc_count - ra_group;
	else
		ra_size = readahead_groups;

	ra_size *= fs->inode_blocks_per_group;
	io_channel_readahead(fs->io, ra_start, ra_size);
	return 0;
}

#define DEFAULT_CHUNK_SIZE 16
__u32 chunk_size; /* in blocks */
int nr_chunks;

struct chunk {
	__u32 start;
	__u32 covered;
} *cur_chunk, *ra_chunk, *chunks;

/* callback for ext2fs_dblist_iterate */
static int fill_chunks(ext2_filsys fs, struct ext2_db_entry *db_info,
		       void *priv_data)
{
	__u32 cur;

	cur = db_info->blk / chunk_size;
	if (cur_chunk == NULL || cur != cur_chunk->start) {
		/* new sweep starts */
		if (cur_chunk == NULL)
			cur_chunk = chunks;
		else
			cur_chunk ++;

		cur_chunk->start = cur;
		cur_chunk->covered = 1;
	} else
		cur_chunk->covered ++;

	return 0;
}

/* callback for ext2fs_dblist_iterate */
static int count_chunks(ext2_filsys fs, struct ext2_db_entry *db_info,
			void *priv_data)
{
	__u32 cur;
	static __u32 prev = (__u32)-1;

	cur = db_info->blk / chunk_size;
	if (cur != prev) {
		nr_chunks ++;
		prev = cur;
	}
	return 0;
}

/* create list of chunks, readahead two first of them */
static void make_chunk_list(ext2_dblist dblist)
{
	chunk_size = readahead_groups * DEFAULT_CHUNK_SIZE;
	if (chunk_size == 0)
		return;

	ext2fs_dblist_iterate(dblist, count_chunks, NULL);
	chunks = malloc(sizeof(struct chunk) * nr_chunks);
	if (chunks == NULL) {
		fprintf(stderr, "malloc failed\n");
		exit(1);
	}
	ext2fs_dblist_iterate(dblist, fill_chunks, NULL);

	/* start readahead for two first chunks */
	ra_chunk = chunks;
	cur_chunk = NULL;

	io_channel_readahead(fs->io,
			     ra_chunk->start * chunk_size,
			     chunk_size);
	ra_chunk ++;
	if (ra_chunk < chunks + nr_chunks)
		io_channel_readahead(fs->io,
				     ra_chunk->start * chunk_size,
				     chunk_size);
}

/*
 * this is called for each directory block when it is read by dblist
 * iterator
 */
static int dblist_readahead(void *vp)
{
	if (chunk_size == 0)
		return 0;
	if (cur_chunk == NULL)
		cur_chunk = chunks;
	if (--cur_chunk->covered == 0) {
		/*
		 * last block of current chunk is read, readahead
		 * chunk is under I/O, get new readahead chunk, move
		 * current chunk
		 */
		cur_chunk ++;
		ra_chunk ++;
		if (ra_chunk < chunks + nr_chunks)
			io_channel_readahead(fs->io,
					     ra_chunk->start * chunk_size,
					     chunk_size);
	}
	return 0;
}

/*
 * callback for ext2fs_dblist_dir_iterate to be called for each
 * directory entry, perform actions common for both database and
 * filelist modes, call specific functions depending on the mode
 */
static int dblist_iterate_cb(ext2_ino_t dirino, int entry,
			     struct ext2_dir_entry *dirent,
			     int offset EXT2FS_ATTR((unused)),
			     int blocksize EXT2FS_ATTR((unused)),
			     char *buf EXT2FS_ATTR((unused)),
			     void *private)
{
	int namelen;

	if (offset == 0) {
		/* new directory block is read */
		scan_data.nr ++;
		dblist_readahead(NULL);
	}

	if (dirent->inode == 0)
		return 0;

	namelen = (dirent->name_len & 0xFF);
	if (namelen == 2 && !strncmp(dirent->name, "..", 2))
		return 0;

	if (namelen == 1 && !strncmp(dirent->name, ".", 1))
		return 0;

	if (dirent->inode > fs->super->s_inodes_count) {
		fprintf(stderr, "too big ino %u (%.*s)\n",
			dirent->inode, namelen, dirent->name);
		exit(1);
	}

	if (scan_data.mode == SM_DATABASE)
		return database_dblist_iterate_cb(dirino, dirent, namelen,
						  scan_data.db.fd);

	return filelist_dblist_iterate_cb(dirino, dirent, namelen);
}

int main(int argc, char **argv)
{
	char *root = "/";
	int inode_buffer_blocks = 0;
	errcode_t retval;
	char *block_buf;
	ext2_inode_scan scan;
	struct ext2_inode inode;
	ext2_ino_t ino;
	dgrp_t nr;
	time_t t;
	int c;
	pid_t pid;

	/*
	 * by default find for files which are modified less than one
	 * day ago
	 */
	scan_data.fl.mtimestamp = time(NULL) - 60 * 60 * 24;
	scan_data.fl.ctimestamp = scan_data.fl.mtimestamp;
	outfile = stdout;

	opterr = 0;
#if defined(HAVE_SQLITE3) && defined(HAVE_SQLITE3_H)
#define OPTF "f"
#else
#define OPTF ""
#endif
	while ((c = getopt(argc, argv, "a:b:C:d:D"OPTF"hln:N:o:")) != EOF) {
		char *end;

		switch (c) {
		case 'a':
			if (optarg == NULL)
				usage(argv[0]);
			readahead_groups = strtoul(optarg, &end, 0);
			if (*end) {
				fprintf(stderr, "%s: bad -a argument '%s'\n",
					argv[0], optarg);
				usage(argv[0]);
			}
			break;
		case 'b':
			inode_buffer_blocks = strtoul(optarg, &end, 0);
			if (*end) {
				fprintf(stderr, "%s: bad -b argument '%s'\n",
					argv[0], optarg);
				usage(argv[0]);
			}
			break;
		case 'C':
			root = optarg;
			break;
		case 'd':
			database = optarg;
			break;
		case 'D':
			scan_data.fl.with_dirs = 1;
			break;
		case 'f':
#if !defined(HAVE_SQLITE3) || !defined(HAVE_SQLITE3_H)
			fprintf(stderr,
				"%s: sqlite3 was not detected on configure, "
				"database creation is not supported\n",argv[0]);
			return 1;
#endif
			scan_data.mode = SM_DATABASE;
			break;
		case 'l':
			scan_data.mode = SM_FILELIST;
			break;
		case 'n':
			get_timestamps(optarg);
			break;
		case 'N': {
			const char *fmts[] = {"%c", /*date/time current locale*/
					      "%Ec",/*date/time alt. locale*/
					      "%a%t%b%t%d,%t%Y%t%H:%M:%S",
					      "%a,%t%d%t%b%t%Y%t%H:%M:%S",
					      "%a%t%b%t%d%t%H:%M:%S%t%Z%t%Y",
					      "%a%t%b%t%d%t%H:%M:%S%t%Y",
					      "%b%t%d%t%H:%M:%S%t%Z%t%Y",
					      "%b%t%d%t%H:%M:%S%t%Y",
					      "%x%t%X",/*date time*/
					      "%Ex%t%EX",/*alternate date time*/
					      "%F", /*ISO 8601 date*/
					      "%+", /*`date` format*/
					      "%s", /*seconds since epoch */
					      NULL,
					    };
			const char **fmt;
			char *ret;
			struct tm tmptm, *tm = NULL;
			time_t now = time(0);

			tmptm = *localtime(&now);

			for (fmt = &fmts[0]; *fmt != NULL; fmt++) {
				if (strptime(optarg, *fmt, &tmptm) != NULL) {
					tm = &tmptm;
					break;
				}
			}

			if (tm == NULL) {
				fprintf(stderr, "%s: bad -N argument '%s'\n",
					argv[0], optarg);
				usage(argv[0]);
			}
			scan_data.fl.mtimestamp = mktime(tm);
			scan_data.fl.ctimestamp = scan_data.fl.mtimestamp;
			break;
			}
		case 'o':
			outfile = fopen(optarg, "w");
			if (outfile == NULL) {
				fprintf(stderr, "%s: can't open '%s': %s\n",
					argv[0], optarg, strerror(errno));
				usage(argv[0]);
			}
			break;
		default:
			fprintf(stderr, "%s: unknown option '-%c'\n",
				argv[0], optopt);
		case 'h':
			usage(argv[0]);
		}
	}

	if (scan_data.mode == SM_NONE || argv[optind] == NULL)
		usage(argv[0]);


	fprintf(stderr, "generating list of files with\n"
		"\tmtime newer than %s"
		"\tctime newer than %s",
		ctime(&scan_data.fl.mtimestamp),
		ctime(&scan_data.fl.ctimestamp));

	retval = ext2fs_open(argv[optind], EXT2_FLAG_SOFTSUPP_FEATURES,
			     0, 0, unix_io_manager, &fs);
	if (retval != 0) {
		com_err("ext2fs_open", retval, "opening %s\n", argv[optind]);
		return 1;
	}

	t = time(NULL);

	for (nr = 0; nr < fs->group_desc_count; nr ++)
		io_channel_readahead(fs->io,
				     fs->group_desc[nr].bg_inode_bitmap, 1);
	retval = ext2fs_read_inode_bitmap(fs);
	if (retval) {
		com_err("ext2fs_read_inode_bitmap", retval,
			"opening inode bitmap on %s\n", argv[optind]);
		exit(1);
	}
	fprintf(stderr, "inode bitmap is read, %ld seconds\n", time(NULL) - t);


	if (inode_buffer_blocks == 0)
		inode_buffer_blocks = fs->inode_blocks_per_group;

	retval = ext2fs_open_inode_scan(fs, inode_buffer_blocks, &scan);
	if (retval) {
		com_err("ext2fs_open_inode_scan", retval,
			"opening inode scan on %s\n", argv[optind]);
		fprintf(stderr, "failed to open inode scan\n");
		exit(1);
	}
	ext2fs_set_inode_callback(scan, done_group_callback, NULL);

	retval = ext2fs_init_dblist(fs, NULL);
	if (retval) {
		com_err("ext2fs_init_dblist", retval,
			"initializing dblist\n");
		exit(1);
	}

	block_buf = (char *)malloc(fs->blocksize * 3);
	if (block_buf == NULL) {
		fprintf(stderr, "%s: failed to allocate memory for block_buf\n",
			argv[0]);
		exit(1);
	}
	memset(block_buf, 0, fs->blocksize * 3);

	switch (scan_data.mode) {
	case SM_DATABASE:
		pid = fork_db_creation(database);
		break;

	case SM_FILELIST:
		c = create_root_dentries(root);
		if (c == ENOENT && strncmp(root, "/ROOT", 5) != 0) {
			/* Try again with prepending "/ROOT" */
			char newroot[PATH_MAX];
			if (snprintf(newroot, PATH_MAX, "/ROOT/%s", root) >=
			    PATH_MAX) {
				fprintf(stderr, "%s: root path '%s' too long\n",
					argv[0], root);
				exit(1);
			}
			if (create_root_dentries(newroot) == 0)
				c = 0;
		}
		if (c == ENOENT)
			fprintf(stderr,
				"%s: visible filesystem root '%s' not found\n",
				argv[0], root);
		else if (c == EIO)
			fprintf(stderr,
				"%s: error reading visible root: '%s'\n",
				argv[0], root);
		else if (c == ENOTDIR)
			fprintf(stderr,
			       "%s: visible root '%s' not a directory\n",
			       argv[0], root);
		if (c)
			exit(1);
		break;
	default:
		break;
	}

	t = time(NULL);
	fprintf(stderr, "scanning inode tables .. ");
	scan_data.nr = 0;

	done_group_callback(fs, scan, -readahead_groups * 2, NULL);
	done_group_callback(fs, scan, -readahead_groups, NULL);
	while (ext2fs_get_next_inode(scan, &ino, &inode) == 0) {
		if (ino == 0)
			break;

		scan_data.nr ++;
		if (ext2fs_fast_test_inode_bitmap(fs->inode_map, ino) == 0)
			/* deleted - always skip for now */
			continue;
		switch (scan_data.mode) {
		case SM_DATABASE:
			database_iscan_action(ino, &inode, scan_data.db.fd,
					      block_buf);
			break;

		case SM_FILELIST:
			filelist_iscan_action(ino, &inode, block_buf);
			break;

		default:
			break;
		}
	}

	switch (scan_data.mode) {
	case SM_DATABASE:
		fprintf(stderr,
			"done\n\t%d inodes, %ld seconds\n",
			scan_data.nr, time(NULL) - t);
		break;

	case SM_FILELIST:
		fprintf(stderr, "done\n\t%d inodes, %ld seconds, %d files, "
			"%d dirs to find\n",
			scan_data.nr, time(NULL) - t, scan_data.fl.nr_files,
			scan_data.fl.nr_dirs);
		if (scan_data.fl.nr_files == 0 && scan_data.fl.nr_dirs == 0) {
			ext2fs_close_inode_scan(scan);
			ext2fs_close(fs);
			free(block_buf);
			return 0;
		}
		break;

	default:
		break;
	}

	t = time(NULL);
	fprintf(stderr, "scanning directory blocks (%u).. ",
		ext2fs_dblist_count(fs->dblist));

	/* root directory does not have name, handle it separately */
	report_root();
	/*
	 * we have a list of directory leaf blocks, blocks are sorted,
	 * but can be not very sequential. If such blocks are close to
	 * each other, read throughput can be improved if blocks are
	 * read not sequentially, but all at once in a big
	 * chunk. Create list of those chunks, it will be then used to
	 * issue readahead
	 */
	make_chunk_list(fs->dblist);

	scan_data.nr = 0;
	retval = ext2fs_dblist_dir_iterate(fs->dblist,
					   DIRENT_FLAG_INCLUDE_EMPTY,
					   block_buf,
					   dblist_iterate_cb, NULL);
	if (retval) {
		com_err("ext2fs_dblist_dir_iterate", retval,
			"dir iterating dblist\n");
		exit(1);
	}
	if (chunk_size)
		free(chunks);

	switch (scan_data.mode) {
	case SM_DATABASE:
	{
		int status;

		fprintf(stderr,
			"done\n\t%d blocks, %ld seconds, "
			"%d records sent to database\n",
			scan_data.nr, time(NULL) - t, scan_data.db.nr_commands);
		close(scan_data.db.fd);
		waitpid(pid, &status, 0);
		if (WIFEXITED(status))
			fprintf(stderr, "database creation exited with %d\n",
				WEXITSTATUS(status));
		break;
	}

	case SM_FILELIST:
		fprintf(stderr,
			"done\n\t%d blocks, %ld seconds, %d files reported\n",
			scan_data.nr, time(NULL) - t, scan_data.fl.nr_reported);
		break;

	default:
		break;
	}

	ext2fs_close_inode_scan(scan);
	ext2fs_close(fs);
	free(block_buf);

	return 0;
}
