/*
 * Copyright (c) 2004  Hewlett-Packard Co.
 */
/*****************************************************************************
 * e2fsck extentions: code for gathering data from the OST & MDT filesystems
 * when e2fsck is run against them. The best description and knowledge of
 * the layout and information gathered is in lfsck.h where the structures
 * defining each entry in the tables are declared. Basically the ost file
 * contains one table with each entry holding the object id and size.
 * In addition there is header information at the start of the file.
 * The MDT file contains multiple tables, one per OST. Each MDT/OST table
 * contains an entry describing the MDT FID and the OST object associated
 * with this FID on an OST. In addition the MDT also contains a table
 * with the mds_fid and the FID of the containg directory. Header information
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
#include <unistd.h>

#include "ext2fs/lfsck.h"

#ifdef ENABLE_LFSCK
char *my_dirname(char *path)
{

	if (path != NULL) {
		char *tmp = strrchr(path, '/');
		if (tmp != NULL) {
			*tmp = '\0';
			return path;
		}
	}

	return ".";
}

const char *my_basename(const char *path)
{
	if (path != NULL) {
		char *tmp = strrchr(path, '/');
		if (tmp != NULL)
			return tmp + 1;
	}

	return path;
}

DB_ENV *dbenv;

u_int32_t lfsck_hash_raw_fn(const void *p)
{
	u_int32_t *c = (u_int32_t *)p;
	u_int32_t rc = 0;

	rc = (*c >> 7) & (HASH_SIZE - 1) ;

	return rc;
}


u_int32_t lfsck_hash_fn(DB *dbp, const void *p, u_int32_t len)
{
	u_int32_t rc = 0 ;

	if (len < sizeof(u_int32_t)) {
	        printf("Hash size error");
	        exit(128);
	}
	rc = lfsck_hash_raw_fn(p);

	return (rc);
}

int lfsck_create_dbenv(const char *progname)
{
	int rc;
	size_t pagesize;
	long pages;
	unsigned long cachesize;

	pagesize = getpagesize();
	pages = sysconf(_SC_AVPHYS_PAGES);

	cachesize = ((pagesize * 3) / 4) * pages;
	if (cachesize > 500UL * 1024 * 1024) {
		cachesize = 500UL * 1024 * 1024;
	} else if (cachesize < 10 * 1024 * 1024) {
		cachesize = 10 * 1024 * 1024;
	}

	if ((rc = db_env_create(&dbenv, 0)) != 0) {
		fprintf(stderr, "%s: error creating dbenv: %s\n",
			progname, db_strerror(rc));
		return (-EINVAL);
	}
	if ((rc = dbenv->set_cachesize(dbenv, 0, cachesize,  0)) != 0) {
		dbenv->err(dbenv, rc, "set_cachesize");
		dbenv->close(dbenv, 0);
		return (-EINVAL);
	}
	if ((rc = dbenv->set_data_dir(dbenv, "/")) != 0) {
		dbenv->err(dbenv, rc, "set_data_dir");
		dbenv->close(dbenv, 0);
		return (-EINVAL);
	}

	/* Open the environment with full transactional support. */
	if ((rc = dbenv->open(dbenv, "/tmp", DB_CREATE | DB_PRIVATE |
			      DB_INIT_MPOOL|DB_INIT_LOCK|DB_THREAD, 0)) != 0) {
		dbenv->err(dbenv, rc, "environment open: ");
		dbenv->close(dbenv, 0);
		return (-EINVAL);
	}
	return (0);
}

int lfsck_opendb(const char *fname, const char *dbname, DB **dbpp,
		 int allow_dup, int keydata_size, int num_files)
{
	static int dbenv_set = 0;
	DB *dbp;
	int rc;
	int pagesize = 512;
	int h_ffactor = 0;

	if (!dbenv_set) {
		if (lfsck_create_dbenv(dbname))
			return(-EIO);
		dbenv_set = 1;
	}

	rc = db_create(&dbp, dbenv, 0);
	if (rc) {
		fprintf(stderr, "%s: error db_create: %s\n",
			dbname, db_strerror(rc));
		return(EIO);
	}

	if ((rc = dbp->set_pagesize(dbp, pagesize)) != 0) {
		dbp->err(dbp, rc, "set_pagesize");
		dbp->close(dbp, 0);
		return(EIO);
	}

	if ((rc = dbp->set_lorder(dbp, 1234)) != 0 ) {
		dbp->err(dbp, rc, "set_lorder");
		dbp->close(dbp, 0);
		return (EIO);
	}

	if (keydata_size && num_files) {
		h_ffactor = (pagesize - 32) / (keydata_size + 8);
		if ((rc = dbp->set_h_ffactor(dbp, h_ffactor)) != 0) {
			dbp->err(dbp, rc, "set_h_ffactor");
		}
		if ((rc = dbp->set_h_nelem(dbp, num_files)) != 0 ) {
			dbp->err(dbp, rc, "set_h_nelem");
		}
	}

	if ((rc = dbp->set_h_hash(dbp, lfsck_hash_fn)) != 0 ) {
		dbp->err(dbp, rc, "set_h_hash");
		dbp->close(dbp, 0);
		return (EIO);
	}

	if (allow_dup) {
		if ((rc = dbp->set_flags(dbp, DB_DUPSORT)) != 0) {
			fprintf(stderr, "Failure to allow duplicates\n");
			dbp->close(dbp, 0);
			return (EIO);
		}
	}

#if (DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 1) || (DB_VERSION_MAJOR > 4)
	if ((rc = dbp->open(dbp, NULL, fname, dbname, DB_HASH,
			    DB_CREATE | DB_INIT_LOCK | DB_THREAD, 0664)) != 0)
#else
	if ((rc = dbp->open(dbp, fname, dbname, DB_HASH,
			    DB_CREATE | DB_INIT_LOCK | DB_THREAD, 0664)) != 0)
#endif
	{
		dbp->err(dbp, rc, "%s:%s\n", fname, dbname);
		dbp->close(dbp, 0);
		return (EIO);
	}
	*dbpp = dbp;
	return (0);
}

void cputole_mds_hdr(struct lfsck_mds_hdr *mds_hdr)
{
	int i, num_osts = mds_hdr->mds_num_osts;
	mds_hdr->mds_magic = ext2fs_cpu_to_le64(mds_hdr->mds_magic);
	mds_hdr->mds_flags = ext2fs_cpu_to_le64(mds_hdr->mds_flags);
	mds_hdr->mds_max_files = ext2fs_cpu_to_le64(mds_hdr->mds_max_files);
	mds_hdr->mds_num_osts = ext2fs_cpu_to_le64(mds_hdr->mds_num_osts);
	for (i = 0; i < num_osts; i++) {
		 mds_hdr->mds_max_ost_id[i] =
			      ext2fs_cpu_to_le64(mds_hdr->mds_max_ost_id[i]);
	}

}

void letocpu_mds_hdr(struct lfsck_mds_hdr *mds_hdr)
{
	int i;
	mds_hdr->mds_magic = ext2fs_le64_to_cpu(mds_hdr->mds_magic);
	mds_hdr->mds_flags = ext2fs_le64_to_cpu(mds_hdr->mds_flags);
	mds_hdr->mds_max_files = ext2fs_le64_to_cpu(mds_hdr->mds_max_files);
	mds_hdr->mds_num_osts = ext2fs_le64_to_cpu(mds_hdr->mds_num_osts);
	for (i = 0; i < mds_hdr->mds_num_osts; i ++) {
		mds_hdr->mds_max_ost_id[i] =
			     ext2fs_le64_to_cpu(mds_hdr->mds_max_ost_id[i]);
	}
}

void cputole_ost_hdr(struct lfsck_ost_hdr *ost_hdr)
{
	ost_hdr->ost_magic = ext2fs_cpu_to_le64(ost_hdr->ost_magic);
	ost_hdr->ost_flags = ext2fs_cpu_to_le64(ost_hdr->ost_flags);
	ost_hdr->ost_num_files = ext2fs_cpu_to_le64(ost_hdr->ost_num_files);
	ost_hdr->ost_last_id = ext2fs_cpu_to_le64(ost_hdr->ost_last_id);
}

void letocpu_ost_hdr(struct lfsck_ost_hdr *ost_hdr)
{
	ost_hdr->ost_magic = ext2fs_le64_to_cpu(ost_hdr->ost_magic);
	ost_hdr->ost_flags = ext2fs_le64_to_cpu(ost_hdr->ost_flags);
	ost_hdr->ost_num_files = ext2fs_le64_to_cpu(ost_hdr->ost_num_files);
	ost_hdr->ost_last_id = ext2fs_le64_to_cpu(ost_hdr->ost_last_id);
}

void cputole_fid(struct lu_fid *fid)
{
	fid->f_seq = ext2fs_cpu_to_le64(fid->f_seq);
	fid->f_oid = ext2fs_cpu_to_le32(fid->f_oid);
	fid->f_ver = ext2fs_cpu_to_le32(fid->f_ver);
}

void letocpu_fid(struct lu_fid *fid)
{
	fid->f_seq = ext2fs_le64_to_cpu(fid->f_seq);
	fid->f_oid = ext2fs_le32_to_cpu(fid->f_oid);
	fid->f_ver = ext2fs_le32_to_cpu(fid->f_ver);
}

void cputole_oi(struct ost_id *oi)
{
	oi->oi_id = ext2fs_cpu_to_le64(oi->oi_id);
	oi->oi_seq = ext2fs_cpu_to_le64(oi->oi_seq);
}

void letocpu_oi(struct ost_id *oi)
{
	oi->oi_id = ext2fs_le64_to_cpu(oi->oi_id);
	oi->oi_seq = ext2fs_le64_to_cpu(oi->oi_seq);
}

void cputole_mds_dirent(struct lfsck_mds_dirent *mds_dirent)
{
	cputole_fid(&mds_dirent->mds_dirfid);
	cputole_fid(&mds_dirent->mds_fid);
}

void letocpu_mds_dirent(struct lfsck_mds_dirent *mds_dirent)
{
	letocpu_fid(&mds_dirent->mds_dirfid);
	letocpu_fid(&mds_dirent->mds_fid);
}

void cputole_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo)
{
	mds_szinfo->mds_fid = ext2fs_cpu_to_le64(mds_szinfo->mds_fid);
	mds_szinfo->mds_seq = ext2fs_cpu_to_le64(mds_szinfo->mds_seq);
	mds_szinfo->mds_size = ext2fs_cpu_to_le64(mds_szinfo->mds_size);
	mds_szinfo->mds_calc_size =
		ext2fs_cpu_to_le64(mds_szinfo->mds_calc_size);
	mds_szinfo->mds_stripe_size =
		ext2fs_cpu_to_le32(mds_szinfo->mds_stripe_size);
	mds_szinfo->mds_stripe_pattern =
		    ext2fs_cpu_to_le32(mds_szinfo->mds_stripe_pattern);
	mds_szinfo->mds_stripe_count =
		ext2fs_cpu_to_le16(mds_szinfo->mds_stripe_count);
	mds_szinfo->mds_stripe_start =
		ext2fs_cpu_to_le16(mds_szinfo->mds_stripe_start);
}

void letocpu_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo)
{
	mds_szinfo->mds_fid = ext2fs_le64_to_cpu(mds_szinfo->mds_fid);
	mds_szinfo->mds_seq = ext2fs_le64_to_cpu(mds_szinfo->mds_seq);
	mds_szinfo->mds_size = ext2fs_le64_to_cpu(mds_szinfo->mds_size);
	mds_szinfo->mds_calc_size =
		ext2fs_le64_to_cpu(mds_szinfo->mds_calc_size);
	mds_szinfo->mds_stripe_size =
		ext2fs_le32_to_cpu(mds_szinfo->mds_stripe_size);
	mds_szinfo->mds_stripe_pattern =
		ext2fs_le32_to_cpu(mds_szinfo->mds_stripe_pattern);
	mds_szinfo->mds_stripe_count =
		ext2fs_le16_to_cpu(mds_szinfo->mds_stripe_count);
	mds_szinfo->mds_stripe_start =
		ext2fs_le16_to_cpu(mds_szinfo->mds_stripe_start);
}

void cputole_mds_objent(struct lfsck_mds_objent *mds_objent)
{
	cputole_fid(&mds_objent->mds_fid);
	cputole_oi(&mds_objent->mds_oi);
	mds_objent->mds_ostidx = ext2fs_cpu_to_le32(mds_objent->mds_ostidx);
	mds_objent->mds_ostoffset=ext2fs_cpu_to_le32(mds_objent->mds_ostoffset);
}

void letocpu_mds_objent(struct lfsck_mds_objent *mds_objent)
{
	letocpu_fid(&mds_objent->mds_fid);
	letocpu_oi(&mds_objent->mds_oi);
	mds_objent->mds_ostidx = ext2fs_le32_to_cpu(mds_objent->mds_ostidx);
	mds_objent->mds_ostoffset=ext2fs_le32_to_cpu(mds_objent->mds_ostoffset);
}

void cputole_ost_objent(struct lfsck_ost_objent *ost_objent)
{
	cputole_oi(&ost_objent->ost_oi);
	ost_objent->ost_size = ext2fs_cpu_to_le64(ost_objent->ost_size);
	ost_objent->ost_bytes = ext2fs_cpu_to_le64(ost_objent->ost_bytes);
}

void letocpu_ost_objent(struct lfsck_ost_objent *ost_objent)
{
	letocpu_oi(&ost_objent->ost_oi);
	ost_objent->ost_oi.oi_id = ext2fs_le64_to_cpu(ost_objent->ost_oi.oi_id);
	ost_objent->ost_oi.oi_seq=ext2fs_le64_to_cpu(ost_objent->ost_oi.oi_seq);
	ost_objent->ost_size = ext2fs_le64_to_cpu(ost_objent->ost_size);
	ost_objent->ost_bytes = ext2fs_le64_to_cpu(ost_objent->ost_bytes);
}

void letocpu_lov_user_md(struct lov_user_md *lmm)
{
	struct lov_user_ost_data_v1 *loi;
	int i;

	lmm->lmm_magic = ext2fs_le32_to_cpu(lmm->lmm_magic);
	lmm->lmm_pattern = ext2fs_le32_to_cpu(lmm->lmm_pattern);
	letocpu_oi((struct ost_id *)&lmm->lmm_object_id);
	lmm->lmm_stripe_size = ext2fs_le32_to_cpu(lmm->lmm_stripe_size);
	lmm->lmm_stripe_count = ext2fs_le16_to_cpu(lmm->lmm_stripe_count);
	/* No swabbing needed for the lov_user_md_v3 lmm_pool_name */

	if (lmm->lmm_magic == LOV_USER_MAGIC_V3)
		loi = ((struct lov_user_md_v3 *)lmm)->lmm_objects;
	else /* if (lmm->lmm_magic == LOV_USER_MAGIC_V1) */
		loi = lmm->lmm_objects;
	/* If there is a bad magic, this will be found immediately in the
	 * call to lfsck_check_lov_ea() following this function. */

	for (i = 0; i < lmm->lmm_stripe_count; i++, loi++) {
		letocpu_oi((struct ost_id *)&loi->l_object_id);
		loi->l_ost_gen = ext2fs_le32_to_cpu(loi->l_ost_gen);
		loi->l_ost_idx = ext2fs_le32_to_cpu(loi->l_ost_idx);
	}
}

int lfsck_get_fid(ext2_filsys fs, ino_t ino, struct lu_fid *fid)
{
	struct ext2_inode *inode;
	errcode_t rc;
	int size;
	struct lustre_mdt_attrs lma;

	rc = ext2fs_get_mem(EXT2_INODE_SIZE(fs->super), &inode);
	if (rc) {
		com_err("ext2fs_get_mem", rc, "allocating %d bytes\n",
			EXT2_INODE_SIZE(fs->super));
		return rc;
	}
	rc = ext2fs_read_inode_full(fs, ino, inode, EXT2_INODE_SIZE(fs->super));
	if (rc) {
		com_err("ext2fs_read_inode_full", rc,
			"reading inode %lu\n", ino);
		ext2fs_free_mem(&inode);
		return rc;
	}
	rc = ext2fs_attr_get(fs, inode, EXT2_ATTR_INDEX_TRUSTED, "lma",
			     (char *)&lma, sizeof(lma), &size);
	if (rc) {
		if (rc != EXT2_ET_EA_NAME_NOT_FOUND &&
		    rc != EXT2_ET_EA_BAD_MAGIC) {
			ext2fs_free_mem(&inode);
			return rc;
		}
		/* compose igif */
		fid->f_seq = ino;
		fid->f_oid = inode->i_generation;
		fid->f_ver = 0;
	} else {
		*fid = lma.lma_self_fid;
	}
	ext2fs_free_mem(&inode);
	return 0;
}

int lfsck_is_dirfid_root(const struct lu_fid *dirfid)
{
	if (dirfid->f_seq == EXT2_ROOT_INO &&
	    dirfid->f_oid == 0 && dirfid->f_ver == 0)
		return 1;
	return 0;
}

int lfsck_fidcmp(const struct lu_fid *fid1, const struct lu_fid *fid2)
{
	if (fid_is_igif(fid1) && fid_is_igif(fid2)) {
		/* do not compare f_ver for comparing igif-s */
		if (fid1->f_seq == fid2->f_seq && fid1->f_oid == fid2->f_oid)
			return 0;
		return 1;
	}
	if (!fid_is_igif(fid1) && !fid_is_igif(fid2)) {
		if (fid1->f_seq == fid2->f_seq && fid1->f_oid == fid2->f_oid &&
			fid1->f_ver == fid2->f_ver)
			return 0;
		return 1;
	}
	return 1;
}
#endif
