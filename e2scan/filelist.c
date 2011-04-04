#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/errno.h>
#include <search.h>
#include <ext2fs/ext2fs.h>

/* e2scan.c */
extern ext2_filsys fs;
extern FILE *outfile;
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
			ext2_ino_t nr_dirs;
			/* number of files reported */
			ext2_ino_t nr_reported;
			time_t mtimestamp;
			time_t ctimestamp;
			int with_dirs;
		} fl;
	};
} scan_data;

ext2_ino_t visible_root_ino;

int block_iterate_cb(ext2_filsys fs, blk_t  *block_nr,
		     e2_blkcnt_t blockcnt,
		     blk_t ref_block EXT2FS_ATTR((unused)),
		     int ref_offset EXT2FS_ATTR((unused)),
		     void *priv_data);


/*
create root dentry
    root->connected_to_root = 1
    root->d_path = "/"
for each directory block:
    if (directory is not in memory)
        create new directory dentry
        set directory->connected_to_root = 0
    for each entry found in directory block:
        if (entry is a subdirectory)
            if (subdir is in memory)
                subdir->d_parent = directory
                if (directory->connected_to_root)
                    recurse for each subsubdir
			subsubdir->connected_to_root = 1
			subsubdir->d_parent = subdir
			subsubdir->d_path = subdir->d_path + name
			for each non-directory entry on subdir
			    generate full pathname and output
			    drop filename entry from RAM
            else
                create new subdir dentry
                subdir->connected_to_root = directory->connected_to_root
                subdir->d_parent = directory
                if (directory->connected_to_root)
		    subdir->d_path = directory->d_path + name
        else if (file is interesting)
            if (directory->connected_to_root)
                generate full pathname and output
            else
		create filename entry
                attach filename to directory
*/

struct e2scan_dentry {
	ext2_ino_t ino;
	struct e2scan_dentry *d_parent;
	char *name;
	struct e2scan_dentry *d_child; /* pointer to first of subdirs */
	struct e2scan_dentry *d_next; /* pointer to next directory */
	unsigned connected_to_root:1;
	unsigned is_file:1;
	unsigned is_dir:1;
	unsigned not_in_root:1;
	unsigned is_printed:1;
};

static void *dentry_tree = NULL;

static int compare_ino(const void *a, const void *b)
{
	const struct e2scan_dentry *d1;
	const struct e2scan_dentry *d2;

	d1 = a;
	d2 = b;
	if (d1->ino > d2->ino)
		return 1;
	if (d1->ino < d2->ino)
		return -1;
	return 0;
}

static struct e2scan_dentry *find_dentry(ext2_ino_t ino)
{
	struct e2scan_dentry tmp, **pdentry;

	tmp.ino = ino;
	pdentry = tfind(&tmp, &dentry_tree, compare_ino);
	return (pdentry == NULL) ? NULL : *pdentry;
}

static struct e2scan_dentry *find_or_create_dentry(ext2_ino_t ino, int *created)
{
	struct e2scan_dentry **dentry, *new;

	new = calloc(1, sizeof(struct e2scan_dentry));
	if (new == NULL) {
		fprintf(stderr, "malloc failed");
		exit(1);
	}
	new->ino = ino;

	dentry = tsearch(new, &dentry_tree, compare_ino);
	if (dentry == NULL) {
		fprintf(stderr, "tsearch failed");
		exit(1);
	}
	if (*dentry != new) {
		*created = 0;
		free(new);
	} else {
		*created = 1;
	}

	return *dentry;
}

static int is_file_interesting(ext2_ino_t ino)
{
	return ext2fs_fast_test_inode_bitmap(fs->inode_map, ino);
}

static void link_to_parent(struct e2scan_dentry *parent,
			   struct e2scan_dentry *child)
{
	child->d_next = parent->d_child;
	parent->d_child = child;
	child->d_parent = parent;
}

static void dentry_attach_name(struct e2scan_dentry *dentry, int namelen,
			       const char *name)
{
	if (dentry->name) {
		if (namelen == 1 && (!strcmp(name, ".") || !strcmp(name, "/")))
			return;
		fprintf(stderr, "dentry name: %s, name %.*s\n",
			dentry->name, namelen, name);
		exit(1);
	}
	asprintf(&dentry->name, "%.*s", namelen, name);
}

/*
  - look up $ROOT in the filesystem
  - build dentry for each component of the path, starting at /
  - for each component of the path except the last, mark dentry "not_in_root"
*/
int create_root_dentries(char *root)
{
	int created;
	char *name;
	ext2_ino_t ino;
	struct e2scan_dentry *child, *parent;
	struct ext2_inode inode;
	char *copy, *p;

	copy = p = strdup(root);

	ino = EXT2_ROOT_INO;
	name = "/";
	parent = NULL;
	do {
		child = find_or_create_dentry(ino, &created);
		dentry_attach_name(child, strlen(name), name);
		child->connected_to_root = 1;
		child->not_in_root = 1;
		if (parent != NULL)
			link_to_parent(parent, child);
		parent = child;

		name = strtok(copy, "/");
		if (name == NULL)
			break;
		copy = NULL;

		if (ext2fs_lookup(fs, ino, name, strlen(name), NULL, &ino))
			return ENOENT;
	} while (1);

	if (ext2fs_read_inode(fs, ino, &inode))
		return EIO;

	if (!LINUX_S_ISDIR(inode.i_mode)) {
		return ENOTDIR;
	}
	child->not_in_root = 0;
	visible_root_ino = ino;
	fprintf(stderr, "visible root: \"%s\"\n", root);

	free(p);

	return 0;
}

static inline void output_dot(void)
{
	fprintf(outfile, ".");
}

static inline void output_dot_newline(void)
{
	fprintf(outfile, ".\n");
}

static inline void output_dir_name(const char *dirname)
{
	fprintf(outfile, "/%s", dirname);
}

static inline void output_file_name(const char *filename, int len)
{
	fprintf(outfile, "/%.*s\n", len, filename);
}

static int up_path(struct e2scan_dentry *dentry)
{
	int len;

	len = 0;
	while (dentry->ino != visible_root_ino) {
		if (dentry->ino == EXT2_ROOT_INO)
			return -1;
		dentry = dentry->d_parent;
		len ++;
	}
	return len;
}

static void revert_dir_name(int path_length, struct e2scan_dentry *dentry)
{
	if (path_length > 0) {
		path_length --;
		revert_dir_name(path_length, dentry->d_parent);
		output_dir_name(dentry->name);
	}
	return;
}

static void report_file_name(struct e2scan_dentry *dentry, ext2_ino_t ino,
			     const char *name, int namelen)
{
	int path_up_length;

	ext2fs_fast_unmark_inode_bitmap(fs->inode_map, ino);

	if (ino == visible_root_ino) {
		/* visible root is to be reported */
		output_dot_newline();
		scan_data.fl.nr_reported ++;
		return;
	}

	path_up_length = up_path(dentry);
	if (path_up_length == -1)
		/* file is not in visible root */
		return;

	output_dot();
	revert_dir_name(path_up_length, dentry);
	/* the file is under visible root */
	scan_data.fl.nr_reported ++;
	output_file_name(name, namelen);
}

void report_root(void)
{
	if (EXT2_ROOT_INO == visible_root_ino &&
	    is_file_interesting(EXT2_ROOT_INO)) {
		output_dot_newline();
		scan_data.fl.nr_reported ++;
	}
}

static struct e2scan_dentry *connect_subtree_to_root(struct e2scan_dentry *dir,
					      int not_in_root)
{
	struct e2scan_dentry *subdir, *prev, *p;

	assert(!dir->is_file);
	dir->connected_to_root = 1;
	dir->not_in_root = not_in_root;

	subdir = dir->d_child;
	prev = NULL;
	while (subdir) {
		if (subdir->is_file) {
			/* report filename and release dentry */
			report_file_name(dir, subdir->ino, subdir->name,
					 strlen(subdir->name));

			if (prev == NULL)
				dir->d_child = subdir->d_next;
			else
				prev->d_next = subdir->d_next;

			p = tdelete(subdir, &dentry_tree, compare_ino);
			assert(p != NULL);

			free(subdir->name);
			p = subdir->d_next;
			free(subdir);
			subdir = p;
			continue;
		}
		if (subdir->is_dir && subdir->is_printed == 0) {
			/* report directory name */
			report_file_name(dir, subdir->ino, subdir->name,
					 strlen(subdir->name));
			subdir->is_printed = 1;
		}
		connect_subtree_to_root(subdir, not_in_root);
		prev = subdir;
		subdir = subdir->d_next;
	}
	return NULL;
}

void filelist_iscan_action(ext2_ino_t ino,
			   struct ext2_inode *inode, char *buf)
{
	int created;
	struct e2scan_dentry *dentry;
	int to_be_listed;

	if (!LINUX_S_ISDIR(inode->i_mode) &&
	    (inode->i_flags & EXT2_NODUMP_FL)) {
		/* skip files which are not to be backuped */
		ext2fs_fast_unmark_inode_bitmap(fs->inode_map, ino);
		return;
	}

	to_be_listed = (inode->i_ctime < scan_data.fl.ctimestamp &&
			inode->i_mtime < scan_data.fl.mtimestamp) ? 0 : 1;
	if (LINUX_S_ISDIR(inode->i_mode)) {
		dentry = find_or_create_dentry(ino, &created);

		if (ext2fs_block_iterate2(fs, ino, 0, buf,
					  block_iterate_cb, &ino)) {
			fprintf(stderr, "ext2fs_block_iterate2 failed\n");
			exit(1);
		}
		dentry->is_dir = to_be_listed;
	}
	if (!to_be_listed)
		/* too old files are not interesting */
		ext2fs_fast_unmark_inode_bitmap(fs->inode_map, ino);
	else {
		/* files and directories to find names of */
		if (LINUX_S_ISDIR(inode->i_mode)) {
			if (scan_data.fl.with_dirs)
				scan_data.fl.nr_dirs++;
			else
				ext2fs_fast_unmark_inode_bitmap(fs->inode_map,
								ino);
		} else
			scan_data.fl.nr_files++;
	}
}

int filelist_dblist_iterate_cb(ext2_ino_t dirino,
			       struct ext2_dir_entry *dirent,
			       int namelen)
{
	struct e2scan_dentry *dir, *subdir;
	int created;
	int ret;
	struct ext2_dir_entry_2 *dirent2;
	int is_dirname;

	dir = find_dentry(dirino);
	assert(dir != NULL);

	dirent2 = (struct ext2_dir_entry_2 *)dirent;
	is_dirname = (dirent2->file_type == EXT2_FT_DIR) ? 1 : 0;
	if (is_dirname) {
		subdir = find_dentry(dirent->inode);
		if (subdir == NULL)
			/* new name is encountered, skip it */
			return 0;

		if (subdir->d_parent == NULL) {
			dentry_attach_name(subdir, namelen, dirent->name);
			link_to_parent(dir, subdir);

			if (dir->connected_to_root)
				/*
				 * go down and connect all subdirs to
				 * root recursively
				 */
				connect_subtree_to_root(subdir,
							dir->not_in_root);
		}
	}
	if (is_file_interesting(dirent->inode)) {
		if (dir->connected_to_root) {
			if (is_dirname && subdir->is_printed == 0) {
				report_file_name(dir, dirent->inode,
						 dirent->name, namelen);
				subdir->is_printed = 1;
			} else
				report_file_name(dir, dirent->inode,
						 dirent->name, namelen);
		} else {
			subdir = find_or_create_dentry(dirent->inode, &created);
			if (created) {
				dentry_attach_name(subdir,namelen,dirent->name);

				link_to_parent(dir, subdir);
				subdir->is_file = 1;
			} else {
				/*
				 * dentry exists already, hard link
				 * encountered, nothing to do about it
				 */
				;
			}
		}
	}
	ret = 0;
	if (scan_data.fl.nr_reported ==
	    (scan_data.fl.nr_files + scan_data.fl.nr_dirs))
		/*
		 * names of all recently modified files are
		 * generated, break dblist iteration
		 */
		ret |= DIRENT_ABORT;
	return ret;
}
