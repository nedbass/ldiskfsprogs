#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <ext2fs/ext2fs.h>
#include <sys/stat.h>

#if defined(HAVE_SQLITE3) && defined(HAVE_SQLITE3_H)

#include <sqlite3.h>

/* e2scan.c */
extern ext2_filsys fs;
extern struct {
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
			/* number of files reported */
			ext2_ino_t nr_reported;
			time_t mtimestamp;
			time_t ctimestamp;
		} fl;
	};
} scan_data;

int block_iterate_cb(ext2_filsys fs, blk_t  *block_nr,
		     e2_blkcnt_t blockcnt,
		     blk_t ref_block EXT2FS_ATTR((unused)),
		     int ref_offset EXT2FS_ATTR((unused)),
		     void *priv_data);


static long count = 10000;

static void exec_one_sql_noreturn(sqlite3 *db, char *sqls)
{
	char *errmsg = NULL;

	if (sqlite3_exec(db, sqls, NULL, NULL, &errmsg) != SQLITE_OK) {
		fprintf(stderr, "SQL error: %s;\nrequest: %s", errmsg, sqls);
		sqlite3_free(errmsg);
		exit(1);
	}
}

static void create_sql_table(sqlite3 *db, const char *table_name,
			     const char *columns)
{
	char *sqls;

	if (asprintf(&sqls, "create table %s (%s)", table_name, columns) < 0) {
		perror("asprintf failed");
		exit(1);
	}

	exec_one_sql_noreturn(db, sqls);
	free(sqls);
}

static void begin_one_transaction(sqlite3 *db)
{
	exec_one_sql_noreturn(db, "BEGIN;");
}

static void commit_one_transaction(sqlite3 *db)
{
	exec_one_sql_noreturn(db, "COMMIT;");
}

static void batch_one_transaction(sqlite3 *db)
{
	static long num = 0;

	num ++;
	if (num % count == 0) {
		commit_one_transaction(db);
		begin_one_transaction(db);
	}
	/* else do nothing */
}

#define COLUMNS "ino, generation, parent, name, size, mtime, ctime, dtime"

static void create_full_db(const char *name, int fd)
{
	sqlite3 *db;
	int rd;
	int nr;
	char sqls[512];

	if (sqlite3_open(name, &db) != SQLITE_OK) {
		fprintf(stderr, "failed to sqlite3_open: %s\n", name);
		sqlite3_close(db);
		exit(1);
	}
	create_sql_table(db, "dirs", COLUMNS);
	create_sql_table(db, "files", COLUMNS);

	begin_one_transaction(db);

	nr = 0;
	while (1) {
		rd = read(fd, sqls, 512);
		if (rd == -1) {
			perror("read failed\n");
			exit(1);
		}
		if (rd != 512)
			break;
		nr ++;
		exec_one_sql_noreturn(db, sqls);
		batch_one_transaction(db);
	}
	commit_one_transaction(db);
	printf("database is created, %d records are inserted\n", nr);
	sqlite3_close(db);
}

/* write sql command to pipe */
#define PIPECMDLEN 512
static void write_sql_command(int fd, const char *sqls)
{
	char buf[PIPECMDLEN];

	if (strlen(sqls) + 1 > PIPECMDLEN) {
		fprintf(stderr, "too long command");
		exit(1);
	}
	strcpy(buf, sqls);
	if (write(fd, buf, PIPECMDLEN) != PIPECMDLEN) {
		perror("failed to write to pipe");
		exit(1);
	}
	scan_data.db.nr_commands ++;
}

pid_t fork_db_creation(const char *database)
{
	int p[2];
	struct stat st;
	pid_t pid;

	if (stat(database, &st) == 0) {
		fprintf(stderr, "%s exists. remove it first\n", database);
		exit(1);
	}
	if (pipe(p) == -1) {
		fprintf(stderr, "failed to pipe");
		exit(1);
	}

	pid = fork();
	if (pid == (pid_t)-1) {
		fprintf(stderr, "failed to fork");
		exit(1);
	}
	if (pid == (pid_t)0) {
		/* child, read data written by parent and write to
		 * database */
		close(p[1]);
		create_full_db(database, p[0]);
		close(p[0]);
		exit(0);
	}

	/* parent, read inodes and write them to pipe */
	close(p[0]);
	scan_data.db.fd = p[1];
	return pid;
}

void database_iscan_action(ext2_ino_t ino, struct ext2_inode *inode,
			   int fd, char *buf)
{
	char *sqls;

	if (LINUX_S_ISDIR(inode->i_mode)) {
		if (ino == EXT2_ROOT_INO) {
			sqls = sqlite3_mprintf("%s (%u,%u,%u,'%q',%u,%u,%u,%u)",
					       "insert into dirs values",
					       ino, inode->i_generation, ino, "/",
					       inode->i_size, inode->i_mtime,
					       inode->i_ctime, inode->i_dtime);
			write_sql_command(fd, sqls);
			sqlite3_free(sqls);
		}

		if (ext2fs_block_iterate2(fs, ino, 0, buf,
					  block_iterate_cb, &ino)) {
			fprintf(stderr, "ext2fs_block_iterate2 failed\n");
			exit(1);
		}
	}
}

/*
 * callback for ext2fs_dblist_dir_iterate to be called for each
 * directory entry
 */
int database_dblist_iterate_cb(ext2_ino_t dir, struct ext2_dir_entry *dirent,
			       int namelen, int fd)
{
	struct ext2_inode inode;
	char *table;
	char *sqls;
	errcode_t retval;
	char str[256];

	if (!ext2fs_fast_test_inode_bitmap2(fs->inode_map, dirent->inode))
		/* entry of deleted file? can that ever happen */
		return 0;

	retval = ext2fs_read_inode(fs, dirent->inode, &inode);
	if (retval) {
		com_err("ext2fs_read_inode", retval, "while reading inode");
		exit(1);
	}

	if (LINUX_S_ISDIR(inode.i_mode))
		table = "dirs";
	else
		table = "files";

	sprintf(str, "%.*s", namelen, dirent->name);
	sqls = sqlite3_mprintf("%s %s %s (%u,%u,%u,'%q',%u,%u,%u,%u)",
			       "insert into", table, "values",
			       dirent->inode, inode.i_generation, dir,
			       str,
			       inode.i_size, inode.i_mtime,
			       inode.i_ctime, inode.i_dtime);
	write_sql_command(fd, sqls);
	sqlite3_free(sqls);

	return 0;
}

#else

pid_t fork_db_creation(const char *database)
{
	return 0;
}

void database_iscan_action(ext2_ino_t ino, struct ext2_inode *inode,
			   int fd, char *buf)
{
	return;
}

int database_dblist_iterate_cb(ext2_ino_t dir, struct ext2_dir_entry *dirent,
			       int namelen, int fd)
{
	return 0;
}

#endif
