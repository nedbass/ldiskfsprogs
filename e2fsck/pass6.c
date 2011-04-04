/*
 * Copyright (c) 2004  Hewlett-Packard Co.
 */
/*****************************************************************************
 * e2fsck extentions: code for gathering data from the ost & mds filesystems
 * when e2fsck is run against them. The best description and knowledge of the
 * layout and information gathered is in lfsck.h where the structures
 * defining each entry in the tables are declared. Basically the ost file
 * contains one table with each entry holding the object id and size.
 * In addition there is header information at the start of the file.
 * The mds file contains multiple tables one per ost. Each mds/ost table
 * contains an entry describing the mds fid and the ost object associated
 * with this fid on an ost. In addition the mds also contains a table
 * with the mds_fid and the fid of the containg directory. Header information
 * for each table is also included.
 * lfsck is run afterwards where the data gathered and stored here is cross
 * checked to ensure consistency and correctness
 *
 *****************************************************************************/
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <assert.h>
#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"

#ifdef ENABLE_LFSCK
#include "lfsck.h"
#include "problem.h"
//#define LOG_REMOVAL

#define VERBOSE(ctx, fmt, args...) \
do { if (ctx->options & E2F_OPT_VERBOSE) printf(fmt, ##args); } while (0)

#define DEBUG(ctx, fmt, args...) \
do { if (ctx->options & E2F_OPT_DEBUG) printf(fmt, ##args); } while (0)

struct lfsck_mds_ctx {
	e2fsck_t	ctx;
	DB		*outdb;
	ext2_ino_t	dot;
	ext2_ino_t	dotdot;
	int		numfiles;
};

struct lfsck_ost_ctx {
	e2fsck_t	ctx;
	DB		*outdb;
	ext2_ino_t	dirinode;
	int		numfiles;
	int		status;
	__u64		max_objid;
};

int e2fsck_lfsck_cleanupdb(e2fsck_t ctx)
{
	int i;
	int rc = 0;
	DB *dbp;

	if (ctx->lfsck_oinfo == NULL) {
		return (0);
	}

	for (i = 0; i < ctx->lfsck_oinfo->ost_count; i++) {
		if (ctx->lfsck_oinfo->ofile_ctx[i].dbp != NULL) {
			dbp = ctx->lfsck_oinfo->ofile_ctx[i].dbp;
			rc += dbp->close(dbp, 0);
			ctx->lfsck_oinfo->ofile_ctx[i].dbp = NULL;
		}
	}
	if (ctx->lfsck_oinfo->mds_sizeinfo_dbp != NULL) {
		dbp = ctx->lfsck_oinfo->mds_sizeinfo_dbp;
		rc += dbp->close(dbp, 0);
		ctx->lfsck_oinfo->mds_sizeinfo_dbp = NULL;
	}
	if (ctx->lfsck_oinfo->ofile_ctx)
		ext2fs_free_mem(ctx->lfsck_oinfo->ofile_ctx);
	ext2fs_free_mem(&ctx->lfsck_oinfo);

	return(rc);
}

/* What is the last object id for the OST on the MDS */
int e2fsck_get_lov_objids(e2fsck_t ctx, struct lfsck_outdb_info *outdb)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t inode;
	ext2_file_t e2_file;
	__u64 *lov_objids = NULL;
	unsigned int got;
	char *block_buf;
	int i, rc = 0;

	block_buf = e2fsck_allocate_memory(ctx, fs->blocksize * 3,
					   "block iterate buffer");

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, LOV_OBJID,
			   strlen(LOV_OBJID), block_buf, &inode);
	if (rc)
		goto out;

	lov_objids = e2fsck_allocate_memory(ctx,
					    sizeof(*lov_objids) * LOV_MAX_OSTS,
					    "lov_objids array");
	if (lov_objids == NULL) {
		rc = ENOMEM;
		goto out;
	}

	rc = ext2fs_file_open(fs, inode, 0, &e2_file);
	if (rc)
		goto out;

	rc = ext2fs_file_read(e2_file, lov_objids,
			      sizeof(*lov_objids) * LOV_MAX_OSTS, &got);
	rc = ext2fs_file_close(e2_file);

	outdb->ost_count = got / sizeof(*lov_objids);
	for (i = 0; i < outdb->ost_count; i++) {
		VERBOSE(ctx,"MDS: ost_idx %d max_id "LPU64"\n",i,lov_objids[i]);
		outdb->ofile_ctx[i].max_id = lov_objids[i];
		outdb->ofile_ctx[i].have_max_id = 1;
		outdb->have_ost_count = 1;
	}

out:
	ext2fs_free_mem(&block_buf);
	if (lov_objids)
		ext2fs_free_mem(&lov_objids);
	if (rc)
		VERBOSE(ctx, "MDS: unable to read lov_objids: rc %d\n", rc);
	else
		VERBOSE(ctx, "MDS: got %d bytes = %d entries in lov_objids\n",
			got, outdb->ost_count);
	return (rc);
}

static int lfsck_write_mds_hdrinfo(e2fsck_t ctx, struct lfsck_outdb_info *outdb)
{
	struct lfsck_mds_hdr mds_hdr;
	ext2_filsys fs = ctx->fs;
	char *mds_hdrname;
	DB *mds_hdrdb = NULL;
	DBT key, data;
	int rc = 0;
	int i;

	mds_hdrname = e2fsck_allocate_memory(ctx, PATH_MAX,
					   "mds_hdr filename");
	sprintf(mds_hdrname, "%s.mdshdr",ctx->lustre_mdsdb);

	if (unlink(mds_hdrname)) {
		if (errno != ENOENT) {
			fprintf(stderr, "Failure to remove old db file %s\n",
				mds_hdrname);
			ctx->flags |= E2F_FLAG_ABORT;
			return -EINVAL;
		}
	}

	rc = lfsck_opendb(mds_hdrname, MDS_HDR, &mds_hdrdb, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "failure to open database for mdsdhr "
			"info%s: %s\n", MDS_HDR, db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		ext2fs_free_mem(&mds_hdrname);
		return(rc);
	}

	/* read in e2fsck_lfsck_save_ea() already if we opened read/write */
	if (ctx->lfsck_oinfo->ost_count == 0)
		e2fsck_get_lov_objids(ctx, ctx->lfsck_oinfo);

	memset(&mds_hdr, 0, sizeof(mds_hdr));
	mds_hdr.mds_magic = MDS_MAGIC;
	mds_hdr.mds_flags = ctx->options & E2F_OPT_READONLY;
	mds_hdr.mds_max_files = fs->super->s_inodes_count -
			    fs->super->s_free_inodes_count;
	VERBOSE(ctx, "MDS: max_files = "LPU64"\n", mds_hdr.mds_max_files);
	mds_hdr.mds_num_osts = ctx->lfsck_oinfo->ost_count;
	VERBOSE(ctx, "MDS: num_osts = %u\n", mds_hdr.mds_num_osts);
	for (i = 0; i < mds_hdr.mds_num_osts; i++) {
		mds_hdr.mds_max_ost_id[i] =
			ctx->lfsck_oinfo->ofile_ctx[i].max_id;
	}

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = &mds_hdr.mds_magic;
	key.size = sizeof(mds_hdr.mds_magic);
	cputole_mds_hdr(&mds_hdr);
	data.data = &mds_hdr;
	data.size = sizeof(mds_hdr);
	rc = mds_hdrdb->put(mds_hdrdb, NULL, &key, &data, 0);
	if (rc != 0) {
		fprintf(stderr, "error: db put %s: %s\n", MDS_HDR,
			db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

out:
	mds_hdrdb->close(mds_hdrdb, 0);
	ext2fs_free_mem(&mds_hdrname);
	if (rc == 0) {
		printf("mds info db file written \n");
		fflush(stdout);

	}
	return (rc);
}

static int e2fsck_lfsck_save_ea(e2fsck_t ctx, ext2_ino_t ino, __u32 generation,
				struct lov_user_md *lmm)
{
	ext2_filsys fs = ctx->fs;
	struct lfsck_mds_szinfo szinfo;
	struct lov_user_ost_data_v1 *loi;
	__u64 mds_fid;
	int rc, i;
	DBT key, data;
	DB *dbp;
	__u32 numfiles = fs->super->s_inodes_count -
			 fs->super->s_free_inodes_count;

	if (!ctx->lfsck_oinfo) {
		/* remove old db file */
		if (unlink(ctx->lustre_mdsdb)) {
			rc = errno;
			if (rc != ENOENT) {
				fprintf(stderr,"Error removing old db %s: %s\n",
					ctx->lustre_mdsdb, strerror(rc));
				ctx->flags |= E2F_FLAG_ABORT;
				return rc;
			}
		}

		rc = ext2fs_get_mem(sizeof(struct lfsck_outdb_info),
				    &ctx->lfsck_oinfo);
		if (rc) {
			ctx->lfsck_oinfo = NULL;
			ctx->flags |= E2F_FLAG_ABORT;
			return rc;
		}
		memset(ctx->lfsck_oinfo, 0, sizeof(struct lfsck_outdb_info));
		rc = ext2fs_get_mem(sizeof(struct lfsck_ofile_ctx)*LOV_MAX_OSTS,
				    &ctx->lfsck_oinfo->ofile_ctx);
		if (rc) {
			ext2fs_free_mem(&ctx->lfsck_oinfo);
			ctx->flags |= E2F_FLAG_ABORT;
			return rc;
		}
		memset(ctx->lfsck_oinfo->ofile_ctx, 0,
		       sizeof(struct lfsck_ofile_ctx) * LOV_MAX_OSTS);
		if (lfsck_opendb(ctx->lustre_mdsdb, MDS_SIZEINFO,
				 &ctx->lfsck_oinfo->mds_sizeinfo_dbp, 0,
				 sizeof(mds_fid) + sizeof(szinfo), numfiles)) {
			fprintf(stderr, "Failed to open db file %s\n",
				MDS_SIZEINFO);
			ctx->flags |= E2F_FLAG_ABORT;
			return (EIO);
		}

		if (ctx->options & E2F_OPT_READONLY) {
			e2fsck_get_lov_objids(ctx, ctx->lfsck_oinfo);
			lfsck_write_mds_hdrinfo(ctx, ctx->lfsck_oinfo);
		}
	}
	if (lmm->lmm_magic == LOV_USER_MAGIC_V3)
		loi = ((struct lov_user_md_v3 *)lmm)->lmm_objects;
	else /* if (lmm->lmm_magic == LOV_USER_MAGIC_V1) */
		loi = lmm->lmm_objects;

	szinfo.mds_fid = ino;
	/* XXX: We don't save the layout type here.  This doesn't matter for
	 *      now, we don't really need the pool information for lfsck, but
	 *      in the future we may need it for RAID-1 and other layouts. */
	szinfo.mds_group = LMM_OBJECT_SEQ(lmm);
	szinfo.mds_stripe_size = lmm->lmm_stripe_size;
	szinfo.mds_stripe_start = loi->l_ost_idx;
	szinfo.mds_stripe_count = lmm->lmm_stripe_count;
	szinfo.mds_size = 0; /* XXX */
	szinfo.mds_calc_size = 0;
	szinfo.mds_stripe_pattern = lmm->lmm_pattern;
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	mds_fid = szinfo.mds_fid;
	key.data = &mds_fid;
	assert(sizeof(szinfo.mds_fid) == sizeof(mds_fid));
	key.size = sizeof(mds_fid);
	cputole_mds_szinfo(&szinfo);
	data.data = &szinfo;
	data.size = sizeof(szinfo);
	dbp = ctx->lfsck_oinfo->mds_sizeinfo_dbp;
#ifdef CHECK_SIZE
	if ((rc = dbp->put(dbp, NULL, &key, &data, 0)) != 0) {
		dbp->err(ctx->lfsck_oinfo->mds_sizeinfo_dbp, rc,
			 "db->put failed\n");
		e2fsck_lfsck_cleanupdb(ctx);
		ctx->flags |= E2F_FLAG_ABORT;
		return (EIO);
	}
#endif
	for (i = 0; i < lmm->lmm_stripe_count; i++, loi++) {
		int ost_idx = loi->l_ost_idx;
		struct lfsck_mds_objent mds_ent;
		struct lfsck_ofile_ctx *ofile_ctx =
					 &ctx->lfsck_oinfo->ofile_ctx[ost_idx];
		__u64 objid = loi->l_object_id;

		if (ost_idx >= LOV_MAX_OSTS) {
			fprintf(stderr, "invalid OST index %u ino %u[%d]\n",
				ost_idx, ino, i);
			continue;
		}

		if (ost_idx + 1 > ctx->lfsck_oinfo->ost_count) {
			if (ctx->lfsck_oinfo->have_ost_count) {
				fprintf(stderr, "bad OST index %u ino %u[%d]\n",
					ost_idx, ino, i);
				continue;
			}
			ctx->lfsck_oinfo->ost_count = ost_idx + 1;
		}

		if (ofile_ctx->dbp == NULL) {
			char dbname[256];
			memset(dbname, 0, 256);
			sprintf(dbname, "%s.%d", MDS_OSTDB, ost_idx);
			rc = lfsck_opendb(ctx->lustre_mdsdb, dbname,
					  &ofile_ctx->dbp, 1,
					  sizeof(objid) + sizeof(mds_ent),
					  numfiles);
			if (rc) {
				e2fsck_lfsck_cleanupdb(ctx);
				ctx->flags |= E2F_FLAG_ABORT;
				return (EIO);
			}
		}

		if (objid > ofile_ctx->max_id) {
			if (ofile_ctx->have_max_id) {
				DEBUG(ctx,
				      "[%d] skip obj "LPU64" > max "LPU64"\n",
				      ost_idx, objid, ofile_ctx->max_id);
				continue;
			}
			ofile_ctx->max_id = objid;
		}
		mds_ent.mds_fid = ino;
		mds_ent.mds_generation = generation;
		mds_ent.mds_flag = 0;
		mds_ent.mds_objid = objid;
		mds_ent.mds_ostidx = ost_idx;
		mds_ent.mds_ostoffset = i;
		memset(&key, 0, sizeof(key));
		memset(&data, 0, sizeof(data));
		key.data = &objid;
		assert(sizeof(objid) == sizeof(mds_ent.mds_objid));
		key.size = sizeof(objid);
		cputole_mds_objent(&mds_ent);
		data.data = &mds_ent;
		data.size = sizeof(mds_ent);
		dbp = ofile_ctx->dbp;
		if ((rc = dbp->put(dbp, NULL, &key, &data, 0)) != 0) {
			dbp->err(dbp, rc, "db->put failed\n");
			e2fsck_lfsck_cleanupdb(ctx);
			ctx->flags |= E2F_FLAG_ABORT;
			/* XXX - Free lctx memory */
			return (EIO);
		}
	}
	return (0);
}

int lfsck_check_lov_ea(e2fsck_t ctx, struct lov_user_md *lmm)
{
	if (lmm->lmm_magic != LOV_USER_MAGIC_V1 &&
	    lmm->lmm_magic != LOV_USER_MAGIC_V3) {
		VERBOSE(ctx, "error: only handle v1/v3 LOV EAs, not %08x\n",
			lmm->lmm_magic);
		return(-EINVAL);
	}

	if (LMM_OBJECT_SEQ(lmm) != 0 ) {
		VERBOSE(ctx, "error: only handle group 0 not "LPU64"\n",
			LMM_OBJECT_SEQ(lmm));
		return(-EINVAL);
	}

	return 0;
}

/*
 * e2fsck pass1 has found a file with an EA let's save the information in
 * the correct table(s).  This is only called for an MDS search.
 */
int e2fsck_lfsck_found_ea(e2fsck_t ctx, ext2_ino_t ino,
			  struct ext2_inode_large *inode,
			  struct ext2_ext_attr_entry *entry, void *value)
{
	/* This ensures that we don't open the file here if traversing an OST */
	if ((ctx->lustre_devtype & LUSTRE_TYPE) != LUSTRE_MDS)
		return 0;

	if (!LINUX_S_ISREG(inode->i_mode))
		return 0;

	if (entry->e_name_index == EXT3_XATTR_INDEX_TRUSTED &&
	    !strncmp(entry->e_name,XATTR_LUSTRE_MDS_LOV_EA,entry->e_name_len)){
		struct lov_user_md *lmm = value;
		letocpu_lov_user_md(lmm);

		if (lfsck_check_lov_ea(ctx, lmm)) {
			ctx->flags |= E2F_FLAG_ABORT;
			return -EINVAL;
		}

		return e2fsck_lfsck_save_ea(ctx, ino, inode->i_generation, lmm);
	}

	return 0;
}

/* make sure that the mds data is on file */
int e2fsck_lfsck_flush_ea(e2fsck_t ctx)
{
	int i, rc = 0;
	DB *dbp;

	if ((ctx->lustre_devtype & LUSTRE_TYPE) != LUSTRE_MDS)
		return (0);

	if (ctx->lfsck_oinfo == 0)
		return (0);

	for (i = 0; i < ctx->lfsck_oinfo->ost_count; i++) {
		if (ctx->lfsck_oinfo->ofile_ctx == NULL)
			break;

		if (ctx->lfsck_oinfo->ofile_ctx[i].dbp != NULL) {
			dbp = ctx->lfsck_oinfo->ofile_ctx[i].dbp;
			rc += dbp->close(dbp, 0);
			ctx->lfsck_oinfo->ofile_ctx[i].dbp = NULL;
		}
	}
	if (ctx->lfsck_oinfo->mds_sizeinfo_dbp != NULL) {
		dbp = ctx->lfsck_oinfo->mds_sizeinfo_dbp;
		rc += dbp->close(dbp, 0);
		ctx->lfsck_oinfo->mds_sizeinfo_dbp = NULL;
	}

	if (rc)
		ctx->flags |= E2F_FLAG_ABORT;

	return(rc);
}

/* From debugfs.c for file removal */
static int lfsck_release_blocks_proc(ext2_filsys fs, blk_t *blocknr,
			       int blockcnt, void *private)
{
	blk_t   block;

	block = *blocknr;
	ext2fs_block_alloc_stats(fs, block, -1);
	return 0;
}

static void lfsck_kill_file_by_inode(ext2_filsys fs, ext2_ino_t inode)
{
	struct ext2_inode inode_buf;

	if (ext2fs_read_inode(fs, inode, &inode_buf))
		return;

	inode_buf.i_dtime = time(NULL);
	if (ext2fs_write_inode(fs, inode, &inode_buf))
		return;

	ext2fs_block_iterate(fs, inode, 0, NULL,
			     lfsck_release_blocks_proc, NULL);
	ext2fs_inode_alloc_stats2(fs, inode, -1,
				  LINUX_S_ISDIR(inode_buf.i_mode));
}

/*
 * remove a file. Currently this removes the lov_objids file
 * since otherwise the automatic deletion of precreated objects on
 * mds/ost connection could potentially remove objects with
 * data - this would be especially the case if the mds has being
 * restored from backup
 */
static int lfsck_rm_file(e2fsck_t ctx, ext2_ino_t dir, char *name)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t ino;
	struct ext2_inode inode;
	int rc;

	rc = ext2fs_lookup(fs, dir, name, strlen(name),
			   NULL, &ino);
	if (rc)
		return (0);

	if (ext2fs_read_inode(fs, ino, &inode))
		return(-EINVAL);

	--inode.i_links_count;

	if (ext2fs_write_inode(fs, ino, &inode))
		return (-EINVAL);

	if (ext2fs_unlink(fs, dir, name, ino, 0))
		return (-EIO);

	if (inode.i_links_count == 0)
		lfsck_kill_file_by_inode(fs, ino);

	return(0);
}

/* called for each ost object - save the object id and size */
static int lfsck_list_objs(ext2_ino_t dir, int entry,
			   struct ext2_dir_entry *dirent, int offset,
			   int blocksize, char *buf, void *priv_data)
{
	struct lfsck_ost_ctx *lctx = priv_data;
	struct lfsck_ost_objent objent;
	struct ext2_inode inode;
	DBT key, data;
	DB *dbp;
	char name[32]; /* same as filter_fid2dentry() */
	__u64  objid;

	if (!ext2fs_check_directory(lctx->ctx->fs, dirent->inode)) {
		return (0);
	}
	memset(name, 0, 32);
	strncpy(name, dirent->name, dirent->name_len & 0xFF);
	objid = STRTOUL(name, NULL, 10);
	if (objid == STRTOUL_MAX) {
		lctx->status = 1;
		lctx->ctx->flags |= E2F_FLAG_ABORT;
		return(DIRENT_ABORT);
	}

	if (ext2fs_read_inode(lctx->ctx->fs, dirent->inode, &inode)) {
		lctx->status = 1;
		lctx->ctx->flags |= E2F_FLAG_ABORT;
		return(DIRENT_ABORT);
	}

	objent.ost_objid = objid;
	objent.ost_flag = 0;
	if (LINUX_S_ISREG(inode.i_mode))
		objent.ost_size = EXT2_I_SIZE(&inode);
	else
		objent.ost_size = inode.i_size;
	objent.ost_bytes = (__u64)inode.i_blocks * 512;

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = &objid;
	key.size = sizeof(objid);
	cputole_ost_objent(&objent);
	data.data = &objent;
	data.size = sizeof(objent);
	dbp = lctx->outdb;
	if (dbp->put(dbp, NULL, &key, &data, 0) != 0) {
		fprintf(stderr, "Failure to put data into db\n");
		lctx->ctx->flags |= E2F_FLAG_ABORT;
		return(DIRENT_ABORT);
	}
	if (objid > lctx->max_objid)
		lctx->max_objid = objid;

	lctx->numfiles ++;
	return (0);
}

/* For each file on the mds save the fid and the containing directory */
static int lfsck_mds_dirs(ext2_ino_t dir, int entry,
			  struct ext2_dir_entry *dirent, int offset,
			  int blocksize, char *buf, void *priv_data)
{
	struct ext2_dir_entry_2 *dirent2 = (struct ext2_dir_entry_2 *)dirent;
	struct lfsck_mds_ctx  *lctx = priv_data;
	struct lfsck_mds_ctx lctx2;
	struct lfsck_mds_dirent mds_dirent;
	DBT key, data;
	DB *dbp = lctx->outdb;
	__u64 mds_fid;
	int rc = 0;

	if (dirent->inode == lctx->dot || dirent->inode == lctx->dotdot)
		return (0);

	if (dirent2->file_type == EXT2_FT_DIR ||
	    dirent2->file_type == EXT2_FT_REG_FILE)
		mds_dirent.mds_filetype = dirent2->file_type;
	else
		return (0);

	lctx->numfiles++;
	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	mds_dirent.mds_fid = dirent->inode;
	mds_dirent.mds_dirfid = lctx->dot;
	mds_fid = mds_dirent.mds_fid;
	assert(sizeof(mds_dirent.mds_fid) == sizeof(mds_fid));
	key.data = &mds_fid;
	key.size = sizeof(mds_fid);

	cputole_mds_dirent(&mds_dirent);

	data.data = &mds_dirent;
	data.size = sizeof(mds_dirent);
	if ((rc = dbp->put(dbp, NULL, &key, &data, 0)) != 0) {
		if (rc != DB_KEYEXIST) {
			fprintf(stderr,
				"error adding MDS inode %.*s (inum %u): %s\n",
				dirent->name_len & 0xFF, dirent->name, dirent->inode,
				db_strerror(rc));
			lctx->ctx->flags |= E2F_FLAG_ABORT;
			return (DIRENT_ABORT);
		}
	}
	if (dirent2->file_type == EXT2_FT_DIR) {
		lctx2 = *lctx;
		lctx2.dot = dirent->inode;
		lctx2.dotdot = lctx->dot;
		if (ext2fs_dir_iterate2(lctx->ctx->fs, dirent->inode, 0, NULL,
					lfsck_mds_dirs, &lctx2)) {
			return (DIRENT_ABORT);
		}
		lctx->numfiles = lctx2.numfiles;
	}
	return(0);
}

/* For each directory get the objects and save the data */
static int lfsck_iterate_obj_dirs(ext2_ino_t dir, int entry,
				  struct ext2_dir_entry *dirent, int offset,
				  int blocksize, char *buf, void *priv_data)
{
	struct lfsck_ost_ctx *lctx = priv_data;

	if (ext2fs_check_directory(lctx->ctx->fs, dirent->inode))
		return (0);

	/* Traverse the d* directories */
	if (*dirent->name != 'd')
		return (0);

	ext2fs_dir_iterate2(lctx->ctx->fs, dirent->inode, 0, NULL,
			    lfsck_list_objs, priv_data);
	if (lctx->status != 0)
		return (DIRENT_ABORT);

	return(0);
}

/* Get the starting point of where the objects reside */
static int lfsck_get_object_dir(e2fsck_t ctx, char *block_buf,ext2_ino_t *inode)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t  tinode;
	int rc;

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, OBJECT_DIR, strlen(OBJECT_DIR),
			   block_buf, &tinode);
	if (rc) {
		fprintf(stderr, "error looking up OST object parent dir\n");
		return (ENOENT);
	}
	rc = ext2fs_check_directory(fs, tinode);
	if (rc) {
		return(ENOENT);
	}

	rc = ext2fs_lookup(fs, tinode, OBJECT_DIR_V1, strlen(OBJECT_DIR_V1),
			   block_buf, inode);
	if (rc) {
		rc = ext2fs_lookup(fs, tinode, OBJECT_DIR_V2,
				   strlen(OBJECT_DIR_V2), block_buf, inode);
		if (rc) {
			fprintf(stderr, "error looking up OST object subdir\n");
			return (-ENOENT);
		}
	}
	rc = ext2fs_check_directory(fs, *inode);
	if (rc) {
		return(-ENOENT);
	}
	return(0);
}

/* What is the last object id for the OST */
static int lfsck_get_last_id(e2fsck_t ctx, __u64 *last_id)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t  inode, tinode;
	ext2_file_t  e2_file;
	char *block_buf;
	unsigned int got;
	int rc;

	block_buf = e2fsck_allocate_memory(ctx, fs->blocksize * 3,
					   "lookup buffer");

	rc = lfsck_get_object_dir(ctx, block_buf, &inode);
	if (rc)
		goto out;

	rc = ext2fs_lookup(fs, inode, LAST_ID,
			   strlen(LAST_ID), block_buf, &tinode);
	if (rc)
		goto out;

	rc = ext2fs_file_open(fs, tinode, 0, &e2_file);
	if (rc)
		goto out;

	rc = ext2fs_file_read(e2_file, last_id, sizeof(__u64), &got);
	if (rc) {
		ext2fs_file_close(e2_file);
		goto out;
	}

	if (got != sizeof(__u64)) {
		rc = EIO;
		ext2fs_file_close(e2_file);
		goto out;
	}

	rc = ext2fs_file_close(e2_file);

	*last_id = ext2fs_le64_to_cpu(*last_id);
out:
	ext2fs_free_mem(&block_buf);
	return (rc);
}

int lfsck_set_last_id(e2fsck_t ctx,  __u64 last_id)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t  inode, tinode;
	ext2_file_t  e2_file;
	char *block_buf;
	unsigned int written;
	int rc;

	block_buf = e2fsck_allocate_memory(ctx, fs->blocksize * 3,
					   "lookup buffer");

	rc = lfsck_get_object_dir(ctx, block_buf, &inode);
	if (rc)
		goto out;

	rc = ext2fs_lookup(fs, inode, LAST_ID,
			   strlen(LAST_ID), block_buf, &tinode);
	if (rc)
		goto out;

	rc = ext2fs_file_open(fs, tinode, EXT2_FILE_WRITE, &e2_file);
	if (rc)
		goto out;

	last_id = ext2fs_cpu_to_le64(last_id);

	rc = ext2fs_file_write(e2_file, &last_id, sizeof(__u64), &written);
	if (rc) {
		fprintf(stderr, "Failure to update last id on file\n");
		ext2fs_file_close(e2_file);
		goto out;
	}

	if (written != sizeof(__u64)) {
		rc = EIO;
		fprintf(stderr, "Failure to update last id on file\n");
		ext2fs_file_close(e2_file);
		goto out;
	}

	rc = ext2fs_file_close(e2_file);

out:
	ext2fs_free_mem(&block_buf);
	return (rc);
}

int e2fsck_get_last_rcvd_info(e2fsck_t ctx, struct obd_uuid *local_uuid,
			      struct obd_uuid *peer_uuid, __u32 *subdircount,
			      __u32 *index, __u32 *compat, __u32 *rocompat,
			      __u32 *incompat)
{
	ext2_filsys fs = ctx->fs;
	ext2_ino_t inode;
	ext2_file_t e2_file;
	struct lustre_server_data *lsd = NULL;
	unsigned int got;
	char *block_buf;
	__u32 cmp, inc;
	int rc = 0;

	block_buf = e2fsck_allocate_memory(ctx, fs->blocksize * 3,
					   "block iterate buffer");

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, LAST_RCVD, strlen(LAST_RCVD),
			   block_buf, &inode);
	if (rc)
		goto out;

	rc = ext2fs_file_open(fs, inode, 0, &e2_file);
	if (rc)
		goto out;

	lsd = e2fsck_allocate_memory(ctx, sizeof(*lsd), "lustre server data");
	if (lsd == NULL) {
		rc = ENOMEM;
		goto out;
	}

	rc = ext2fs_file_read(e2_file, lsd, sizeof(*lsd), &got);
	if (rc)
		goto out;
	if (got != sizeof(*lsd)) {
		rc = EIO;
		goto out;
	}

	if (local_uuid)
		memcpy(local_uuid, &lsd->lsd_uuid, sizeof(lsd->lsd_uuid));

	if (peer_uuid)
		memcpy(peer_uuid, &lsd->lsd_peeruuid,sizeof(lsd->lsd_peeruuid));

	if (subdircount)
		*subdircount = ext2fs_le16_to_cpu(lsd->lsd_subdir_count);

	if (compat == NULL)
		compat = &cmp;
	*compat = ext2fs_le32_to_cpu(lsd->lsd_feature_compat);
	if (rocompat)
		*rocompat = ext2fs_le32_to_cpu(lsd->lsd_feature_rocompat);
	if (incompat == NULL)
		incompat = &inc;
	*incompat = ext2fs_le32_to_cpu(lsd->lsd_feature_incompat);
	if (index) {
		if (*compat & OBD_COMPAT_OST || *incompat & OBD_INCOMPAT_OST)
			*index = ext2fs_le32_to_cpu(lsd->lsd_ost_index);
		else if (*compat & OBD_COMPAT_MDT||*incompat & OBD_INCOMPAT_MDT)
			*index = ext2fs_le32_to_cpu(lsd->lsd_mdt_index);
		else
			*index = -1;
	}

	rc = ext2fs_file_close(e2_file);

out:
	ext2fs_free_mem(&block_buf);
	if (lsd)
		ext2fs_free_mem(&lsd);
	return (rc);
}

int lfsck_rm_log(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
		 int offset, int blocksize, char *buf, void *priv_data)
{
	struct lfsck_ost_ctx *lctx = priv_data;
	char name[EXT2_NAME_LEN + 1];

	if (!ext2fs_check_directory(lctx->ctx->fs, dirent->inode))
		return (0);

	strncpy(name, dirent->name, dirent->name_len & 0xFF);
	name[EXT2_NAME_LEN] = '\0';
	if (memcmp(name, LAST_ID, strlen(LAST_ID)) == 0)
		return (0);


	if (lfsck_rm_file(lctx->ctx, lctx->dirinode, name))
		return(DIRENT_ABORT);

	return(0);
}

/* Not 100% sure that this is correct so not activated yet */
int lfsck_remove_ost_logs(e2fsck_t ctx, char *block_buf)
{
	ext2_filsys fs = ctx->fs;
	struct lfsck_ost_ctx lctx;
	ext2_ino_t inode;
	ext2_ino_t  tinode;
	int rc;

	if (lfsck_rm_file(ctx, EXT2_ROOT_INO, CATLIST)) {
		ctx->flags |= E2F_FLAG_ABORT;
		return -EINVAL;
	}

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, OBJECT_DIR, strlen(OBJECT_DIR),
			   block_buf, &tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return (-ENOENT);
	}
	rc = ext2fs_check_directory(fs,tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return(-ENOENT);
	}

	rc = ext2fs_lookup(fs, tinode, LOG_DIR, strlen(LOG_DIR),
			   block_buf, &inode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return (-ENOENT);
	}
	rc = ext2fs_check_directory(fs, inode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return(-ENOENT);
	}
	lctx.ctx   = ctx;
	lctx.dirinode = inode;

	if (ext2fs_dir_iterate2(fs, inode, 0, block_buf, lfsck_rm_log, &lctx)) {
		ctx->flags |= E2F_FLAG_ABORT;
		return(-EIO);
	}
	return (0);
}

/* Remove files from PENDING dir - this needs to be done before getting ea from
 * blocks but we need the inode_map bitmap loaded beforehand so load write any
 * changes then remove references
 */
int e2fsck_lfsck_remove_pending(e2fsck_t ctx, char *block_buf)
{
	ext2_filsys fs = ctx->fs;
	struct lfsck_ost_ctx lctx;
	ext2_ino_t  tinode;
	int rc = 0;

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, PENDING_DIR, strlen(PENDING_DIR),
			   block_buf, &tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return (-ENOENT);
	}
	rc = ext2fs_check_directory(fs,tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return(-ENOENT);
	}

	lctx.ctx   = ctx;
	lctx.dirinode = tinode;

	e2fsck_read_bitmaps(ctx);

	if (ext2fs_dir_iterate2(fs, tinode, 0, block_buf, lfsck_rm_log, &lctx)){
		ctx->flags |= E2F_FLAG_ABORT;
		rc = -EIO;
	}
	e2fsck_write_bitmaps(ctx);
	ext2fs_free_inode_bitmap(fs->inode_map);
	ext2fs_free_block_bitmap(fs->block_map);
	fs->inode_map = NULL;
	fs->block_map = NULL;
	return (rc);
}

/* partially using code from debugfs do_write() */
int lfsck_create_objid(e2fsck_t ctx, __u64 objid)
{
	int rc = 0;
	char dirname[32];
	char name[32];
	int len, dirlen;
	__u32 compat, incompat, subdircount;
	ext2_ino_t  inode, tinode, cinode;
	struct ext2_inode ext2inode;
	char *block_buf;

	block_buf = e2fsck_allocate_memory(ctx, ctx->fs->blocksize * 3,
					   "lookup buffer");

	memset(name, 0, 32);
	memset(dirname, 0, 32);

	len = sprintf(name, LPU64, objid);

	fprintf(stderr, "creating %s\n", name);

	rc = e2fsck_get_last_rcvd_info(ctx, NULL, NULL, &subdircount, NULL,
				       &compat, NULL, &incompat);
	if (rc) {
		fprintf(stderr, "Error: reading OST last_rcvd file\n");
		rc = EINVAL;
		goto out;
	}

	if (compat & OBD_COMPAT_MDT || incompat & OBD_INCOMPAT_MDT) {
		fprintf(stderr, "Error: MDS last_rcvd file doing OST check\n");
		rc = EINVAL;
		goto out;
	}

	if (lfsck_get_object_dir(ctx, block_buf, &inode)) {
		rc = EINVAL;
		goto out;
	}

	dirlen = sprintf(dirname, "d%u", (int)objid & (subdircount - 1));

	rc = ext2fs_lookup(ctx->fs, inode, dirname,
			   dirlen, block_buf, &tinode);
	if (rc) {
		rc = EINVAL;
		goto out;
	}

	if (ext2fs_namei(ctx->fs, EXT2_ROOT_INO, tinode, name, &cinode) == 0) {
		fprintf(stderr, "Failure to create obj\n");
		rc = EINVAL;
		goto out;
	}

	rc = ext2fs_new_inode(ctx->fs, tinode, 010755, 0, &cinode);
	if (rc) {
		fprintf(stderr, "Failure to create obj\n");
		rc = EINVAL;
		goto out;
	}

	rc = ext2fs_link(ctx->fs, tinode, name, cinode, EXT2_FT_REG_FILE);
	if (rc) {
		fprintf(stderr, "Failure to create obj\n");
		rc = EINVAL;
		goto out;
	}

	if (ext2fs_test_inode_bitmap(ctx->fs->inode_map, cinode)) {
		fprintf(stderr, "Warning: inode already set");
	}
	ext2fs_inode_alloc_stats2(ctx->fs, cinode, +1, 0);
	memset(&ext2inode, 0, sizeof(ext2inode));
	ext2inode.i_mode = LINUX_S_IFREG;
	ext2inode.i_atime = ext2inode.i_ctime = ext2inode.i_mtime = time(NULL);
	ext2inode.i_links_count = 1;
	ext2inode.i_size = 0;
	if (ext2fs_write_inode(ctx->fs, cinode, &ext2inode)) {
		fprintf(stderr, "Failure to create obj\n");
		rc = EINVAL;
		goto out;
	}

out:
	ext2fs_free_mem((void *)&(block_buf));
	return (rc);
}

/*
 * For on ost iterate for the direcories and save the object information.
 */
void e2fsck_pass6_ost(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	struct lfsck_ost_ctx lctx;
	struct lfsck_ost_hdr ost_hdr;
	struct lfsck_mds_hdr mds_hdr;
	struct lfsck_ost_objent objent;
	DB *outdb = NULL;
	DB *mds_hdrdb = NULL;
	DB *osthdr = NULL;
	DBT key, data;
	ext2_ino_t dir;
	__u32 compat, rocompat, incompat;
	int i, rc;
	char *block_buf = NULL;

	if (unlink(ctx->lustre_ostdb)) {
		if (errno != ENOENT) {
			fprintf(stderr, "Failure to remove old db file %s\n",
				ctx->lustre_ostdb);
			ctx->flags |= E2F_FLAG_ABORT;
			return;
		}
	}

	block_buf = e2fsck_allocate_memory(ctx, fs->blocksize * 3,
					   "block iterate buffer");

	rc = lfsck_opendb(ctx->lustre_mdsdb, MDS_HDR, &mds_hdrdb, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "failure to open database %s: %s\n",
			MDS_HDR, db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	mds_hdr.mds_magic = MDS_MAGIC;
	key.data = &mds_hdr.mds_magic;
	key.size = sizeof(mds_hdr.mds_magic);
	data.data = &mds_hdr;
	data.size = sizeof(mds_hdr);
	data.ulen = sizeof(mds_hdr);
	data.flags = DB_DBT_USERMEM;
	rc = mds_hdrdb->get(mds_hdrdb, NULL, &key, &data, 0);
	if (rc) {
		fprintf(stderr,"error getting mds_hdr ("LPU64":%u) in %s: %s\n",
			mds_hdr.mds_magic, (int)sizeof(mds_hdr.mds_magic),
			ctx->lustre_mdsdb, db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	assert(data.size == sizeof(mds_hdr));
	memcpy(&mds_hdr, data.data, sizeof(mds_hdr));
	letocpu_mds_hdr(&mds_hdr);

	rc = lfsck_opendb(ctx->lustre_ostdb, OST_HDR, &osthdr, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "failure to open database %s: %s\n",
			OST_HDR, db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	rc = lfsck_opendb(ctx->lustre_ostdb, OST_OSTDB, &outdb, 0,
			  sizeof(objent.ost_objid) + sizeof(objent),
			  fs->super->s_inodes_count -
			  fs->super->s_free_inodes_count);
	if (rc != 0) {
		fprintf(stderr, "error getting ost_hdr in %s: %s\n",
			ctx->lustre_ostdb, db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	if (e2fsck_get_last_rcvd_info(ctx, &ost_hdr.ost_uuid,
				      &ost_hdr.ost_mds_uuid, NULL,
				      &ost_hdr.ost_index,
				      &compat, &rocompat, &incompat)) {
		fprintf(stderr, "Failure to read OST last_rcvd file\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	VERBOSE(ctx, "OST: '%s' ost idx %u: compat %#x rocomp %#x incomp %#x\n",
		(char *)&ost_hdr.ost_uuid.uuid, ost_hdr.ost_index,
		compat, rocompat, incompat);

	if (compat & OBD_COMPAT_MDT) {
		fprintf(stderr, "Found MDS last_rcvd file doing OST check\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	/*
	 * Get /O/R or /O/0 directory
	 * for each entry scan all the dirents and get the object id
	 */
	if (lfsck_get_object_dir(ctx, block_buf, &dir)) {
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	/*
	 * Okay so we have the containing directory so let's iterate over the
	 * containing d* dirs and then iterate again inside
	 */
	lctx.ctx = ctx;
	lctx.outdb = outdb;
	lctx.status = 0;
	lctx.numfiles = 0;
	lctx.max_objid = 0;
	lctx.status = ext2fs_dir_iterate2(fs, dir, 0, block_buf,
					  lfsck_iterate_obj_dirs, &lctx);
	if (lctx.status) {
		fprintf(stderr, "Failure in iterating object dirs\n");
		ctx->flags |= E2F_FLAG_ABORT;
		return;
	}

	ost_hdr.ost_magic = OST_MAGIC;
	ost_hdr.ost_flags = ctx->options & E2F_OPT_READONLY;
	ost_hdr.ost_num_files = lctx.numfiles;
	VERBOSE(ctx, "OST: num files = %u\n", lctx.numfiles);

	if (lfsck_get_last_id(ctx, &ost_hdr.ost_last_id)) {
		fprintf(stderr, "Failure to get last id for objects\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}
	VERBOSE(ctx, "OST: last_id = "LPU64"\n", ost_hdr.ost_last_id);

	/* Update the last_id value on the OST if necessary/possible to the
	 * MDS value if larger.  Otherwise we risk creating duplicate objects.
	 * If running read-only, we skip this so new objects are ignored. */
	ost_hdr.ost_last_id = lctx.max_objid;
	if (!(ctx->options & E2F_OPT_READONLY) &&
	    !(mds_hdr.mds_flags & E2F_OPT_READONLY)) {
		for (i = 0; i < mds_hdr.mds_num_osts; i++) {
			if (strcmp((char *)mds_hdr.mds_ost_info[i].uuid,
				   (char *)ost_hdr.ost_uuid.uuid) == 0 &&
			    mds_hdr.mds_max_ost_id[i] >= ost_hdr.ost_last_id)
				ost_hdr.ost_last_id=mds_hdr.mds_max_ost_id[i]+1;
		}

	        if (lfsck_set_last_id(ctx, ost_hdr.ost_last_id)) {
		        fprintf(stderr, "Failure to set last id\n");
		        ctx->flags |= E2F_FLAG_ABORT;
		        goto out;
	        }

#ifdef LOG_REMOVAL
		if (lfsck_remove_ost_logs(ctx, block_buf))
			ctx->flags |= E2F_FLAG_ABORT;
#endif
	}

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = &ost_hdr.ost_magic;
	key.size = sizeof(ost_hdr.ost_magic);
	cputole_ost_hdr(&ost_hdr);
	data.data = &ost_hdr;
	data.size = sizeof(ost_hdr);
	if (osthdr->put(osthdr, NULL, &key, &data, 0)) {
		fprintf(stderr, "Failed to db_put data\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

out:
	if (mds_hdrdb)
		mds_hdrdb->close(mds_hdrdb, 0);
	if (outdb)
		outdb->close(outdb, 0);
	if (osthdr)
		osthdr->close(osthdr, 0);
	if (block_buf)
		ext2fs_free_mem((void *)&(block_buf));
	return;
}

int lfsck_remove_mds_logs(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	struct lfsck_ost_ctx lctx;
	ext2_ino_t  tinode;
	int rc = 0;

	if (lfsck_rm_file(ctx, EXT2_ROOT_INO, CATLIST)) {
		ctx->flags |= E2F_FLAG_ABORT;
		return -EINVAL;
	}

	rc = ext2fs_lookup(fs, EXT2_ROOT_INO, OBJECTS, strlen(OBJECTS),
			   NULL, &tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return (-ENOENT);
	}
	rc = ext2fs_check_directory(fs,tinode);
	if (rc) {
		ctx->flags |= E2F_FLAG_ABORT;
		return(-ENOENT);
	}

	lctx.ctx   = ctx;
	lctx.dirinode = tinode;

	if (ext2fs_dir_iterate2(fs, tinode, 0, NULL, lfsck_rm_log, &lctx)) {
		ctx->flags |= E2F_FLAG_ABORT;
		rc = -EIO;
	}
	return (rc);
}


/*
 * On the mds save the fid and directory information for each file.
 * The mds ost tables have already been populated by pass1
 */
void e2fsck_pass6_mds(e2fsck_t ctx)
{
	ext2_filsys fs = ctx->fs;
	struct problem_context pctx;
	struct lfsck_mds_ctx lctx;
	struct lfsck_mds_dirent mds_dirent;
	struct lfsck_mds_hdr mds_hdr;
	DBT key, data;
	DB *outdb = NULL, *dbhdr = NULL;
	__u32 compat, rocompat, incompat, index;
	int rc, i;

	clear_problem_context(&pctx);

	lctx.ctx = ctx;

	/* Found no files with EA on filesystem - empty */
	if (ctx->lfsck_oinfo == NULL) {
		if (unlink(ctx->lustre_mdsdb)) {
			if (errno != ENOENT) {
				fprintf(stderr, "Failure to remove old "
					"db file %s\n", ctx->lustre_mdsdb);
				ctx->flags |= E2F_FLAG_ABORT;
				goto out;
			}
		}
		rc = ext2fs_get_mem(sizeof(struct lfsck_outdb_info),
				    &ctx->lfsck_oinfo);
		if (rc) {
			ctx->lfsck_oinfo = NULL;
			ctx->flags |= E2F_FLAG_ABORT;
			goto out;
		}
		memset(ctx->lfsck_oinfo, 0, sizeof(struct lfsck_outdb_info));
		rc = ext2fs_get_mem(sizeof(struct lfsck_ofile_ctx)*LOV_MAX_OSTS,
				    &ctx->lfsck_oinfo->ofile_ctx);
		if (rc) {
			ext2fs_free_mem(&ctx->lfsck_oinfo);
			ctx->lfsck_oinfo = NULL;
			ctx->flags |= E2F_FLAG_ABORT;
			goto out;
		}
		memset(ctx->lfsck_oinfo->ofile_ctx, 0,
		       sizeof(struct lfsck_ofile_ctx) * LOV_MAX_OSTS);
	}

	if (!(ctx->options & E2F_OPT_READONLY)) {
		 lfsck_write_mds_hdrinfo(ctx, ctx->lfsck_oinfo);
	}

	if (lfsck_opendb(ctx->lustre_mdsdb, MDS_DIRINFO, &outdb, 1,
			 sizeof(mds_dirent.mds_fid) + sizeof(mds_dirent),
			 fs->super->s_inodes_count -
			 fs->super->s_free_inodes_count)) {
		fprintf(stderr, "failure to open database %s\n", MDS_DIRINFO);
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	lctx.outdb = outdb;
	lctx.numfiles = 0;
	lctx.dot = EXT2_ROOT_INO;
	lctx.dotdot = EXT2_ROOT_INO;

	rc = ext2fs_dir_iterate2(fs, EXT2_ROOT_INO,0,NULL,lfsck_mds_dirs,&lctx);
	if (rc != 0) {
		fprintf(stderr, "Error iterating directories: %d\n", rc);
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	/* read in e2fsck_lfsck_save_ea() already if we opened read/write */
	if (ctx->lfsck_oinfo->ost_count == 0)
		e2fsck_get_lov_objids(ctx, ctx->lfsck_oinfo);

	memset(&mds_hdr, 0, sizeof(mds_hdr));
	mds_hdr.mds_magic = MDS_MAGIC;
	mds_hdr.mds_flags = ctx->options & E2F_OPT_READONLY;
	mds_hdr.mds_max_files = fs->super->s_inodes_count -
			    fs->super->s_free_inodes_count;
	VERBOSE(ctx, "MDS: max_files = "LPU64"\n", mds_hdr.mds_max_files);
	mds_hdr.mds_num_osts = ctx->lfsck_oinfo->ost_count;
	VERBOSE(ctx, "MDS: num_osts = %u\n", mds_hdr.mds_num_osts);
	for (i = 0; i < mds_hdr.mds_num_osts; i++) {
		mds_hdr.mds_max_ost_id[i] =
			ctx->lfsck_oinfo->ofile_ctx[i].max_id;
	}

	if (e2fsck_get_last_rcvd_info(ctx, &mds_hdr.mds_uuid, NULL, NULL,
				      &index, &compat, &rocompat, &incompat)) {
		fprintf(stderr, "Failure to read MDS last_rcvd file\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	VERBOSE(ctx, "MDS: '%s' mdt idx %u: compat %#x rocomp %#x incomp %#x\n",
		(char *)&mds_hdr.mds_uuid.uuid, index,compat,rocompat,incompat);

	if (compat & OBD_COMPAT_OST || incompat & OBD_INCOMPAT_OST) {
		fprintf(stderr, "Found OST last_rcvd file doing MDS check\n");
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	if (!(ctx->options & E2F_OPT_READONLY)) {
		if (lfsck_rm_file(ctx, EXT2_ROOT_INO, LOV_OBJID)) {
			ctx->flags |= E2F_FLAG_ABORT;
			goto out;
		}
#ifdef LOG_REMOVAL
		if (lfsck_remove_mds_logs(ctx)) {
			ctx->flags |= E2F_FLAG_ABORT;
			return;
		}
#endif
	}

	rc = lfsck_opendb(ctx->lustre_mdsdb, MDS_HDR, &dbhdr, 0, 0, 0);
	if (rc != 0) {
		fprintf(stderr, "failure to open database %s: %s\n", MDS_HDR,
			db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}

	memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	key.data = &mds_hdr.mds_magic;
	key.size = sizeof(mds_hdr.mds_magic);
	cputole_mds_hdr(&mds_hdr);
	data.data = &mds_hdr;
	data.size = sizeof(mds_hdr);
	rc = dbhdr->put(dbhdr, NULL, &key, &data, 0);
	if (rc != 0) {
		fprintf(stderr, "error: db put %s: %s\n", MDS_HDR,
			db_strerror(rc));
		ctx->flags |= E2F_FLAG_ABORT;
		goto out;
	}
out:
	if (dbhdr)
		dbhdr->close(dbhdr, 0);
	if (outdb)
		outdb->close(outdb, 0);
}

/* If lfsck checking requested then gather the data */
void e2fsck_pass6(e2fsck_t ctx)
{
	if (ctx->lustre_devtype == LUSTRE_NULL)
		return;

	printf("Pass 6: Acquiring information for lfsck\n");
	fflush(stdout);

	if (ctx->lustre_devtype & LUSTRE_OST)
		e2fsck_pass6_ost(ctx);
	else if (ctx->lustre_devtype & LUSTRE_MDS)
		e2fsck_pass6_mds(ctx);
	else
		fprintf(stderr, "Invalid lustre dev %x\n", ctx->lustre_devtype);

	return;
}
#endif /* ENABLE_LFSCK */
