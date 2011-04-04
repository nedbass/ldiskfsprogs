/*
 * Copyright (c) 2004  Hewlett-Packard Co.
 */
/*****************************************************************************
 * e2fsck extentions: code for gathering data from the ost & mds filesystems
 * when e2fsck is run against them. The best description and knowledge of
 * the layout and information gathered is in lfsck.h where the structures
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
#include <unistd.h>

#include "lfsck.h"

#include "problem.h"

#ifdef ENABLE_LFSCK

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
			dbp->close(dbp, 0);
			return (EIO);
		}
		if ((rc = dbp->set_h_nelem(dbp, num_files)) != 0 ) {
			dbp->err(dbp, rc, "set_h_nelem");
			dbp->close(dbp, 0);
			return (EIO);
		}
	}

	if ((rc = dbp->set_h_hash(dbp, lfsck_hash_fn)) != 0 ) {
		dbp->err(dbp, rc, "set_h_hash");
		dbp->close(dbp, 0);
		return (EIO);
	}

	if (allow_dup) {
		if((rc = dbp->set_flags(dbp, DB_DUPSORT)) != 0) {
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

void cputole_mds_dirent(struct lfsck_mds_dirent *mds_dirent)
{
	mds_dirent->mds_dirfid = ext2fs_cpu_to_le32(mds_dirent->mds_dirfid);
	mds_dirent->mds_fid = ext2fs_cpu_to_le32(mds_dirent->mds_fid);
	mds_dirent->mds_filetype = ext2fs_cpu_to_le32(mds_dirent->mds_filetype);
}

void letocpu_mds_dirent(struct lfsck_mds_dirent *mds_dirent)
{
	mds_dirent->mds_dirfid = ext2fs_le32_to_cpu(mds_dirent->mds_dirfid);
	mds_dirent->mds_fid = ext2fs_le32_to_cpu(mds_dirent->mds_fid);
	mds_dirent->mds_filetype = ext2fs_le32_to_cpu(mds_dirent->mds_filetype);
}

void cputole_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo)
{
	mds_szinfo->mds_group = ext2fs_cpu_to_le64(mds_szinfo->mds_group);
	mds_szinfo->mds_stripe_size =
		ext2fs_cpu_to_le32(mds_szinfo->mds_stripe_size);
	mds_szinfo->mds_stripe_start =
		ext2fs_cpu_to_le16(mds_szinfo->mds_stripe_start);
	mds_szinfo->mds_size = ext2fs_cpu_to_le64(mds_szinfo->mds_size);
	mds_szinfo->mds_calc_size =
		ext2fs_cpu_to_le64(mds_szinfo->mds_calc_size);
	mds_szinfo->mds_stripe_pattern =
		    ext2fs_cpu_to_le32(mds_szinfo->mds_stripe_pattern);
}

void letocpu_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo)
{
	mds_szinfo->mds_group = ext2fs_le64_to_cpu(mds_szinfo->mds_group);
	mds_szinfo->mds_stripe_size =
		ext2fs_le32_to_cpu(mds_szinfo->mds_stripe_size);
	mds_szinfo->mds_stripe_start =
		ext2fs_le16_to_cpu(mds_szinfo->mds_stripe_start);
	mds_szinfo->mds_size = ext2fs_le64_to_cpu(mds_szinfo->mds_size);
	mds_szinfo->mds_calc_size =
		ext2fs_le64_to_cpu(mds_szinfo->mds_calc_size);
	mds_szinfo->mds_stripe_pattern =
		    ext2fs_le32_to_cpu(mds_szinfo->mds_stripe_pattern);
}

void cputole_mds_objent(struct lfsck_mds_objent *mds_objent)
{
	mds_objent->mds_fid = ext2fs_cpu_to_le32(mds_objent->mds_fid);
	mds_objent->mds_ostidx = ext2fs_cpu_to_le32(mds_objent->mds_ostidx);
	mds_objent->mds_flag = ext2fs_cpu_to_le64(mds_objent->mds_flag);
	mds_objent->mds_objid = ext2fs_cpu_to_le64(mds_objent->mds_objid);
	mds_objent->mds_ostoffset = ext2fs_cpu_to_le32(mds_objent->mds_ostoffset);
}

void letocpu_mds_objent(struct lfsck_mds_objent *mds_objent)
{
	mds_objent->mds_fid = ext2fs_le32_to_cpu(mds_objent->mds_fid);
	mds_objent->mds_ostidx = ext2fs_le32_to_cpu(mds_objent->mds_ostidx);
	mds_objent->mds_flag = ext2fs_le64_to_cpu(mds_objent->mds_flag);
	mds_objent->mds_objid = ext2fs_le64_to_cpu(mds_objent->mds_objid);
	mds_objent->mds_ostoffset = ext2fs_le32_to_cpu(mds_objent->mds_ostoffset);
}

void cputole_ost_objent(struct lfsck_ost_objent *ost_objent)
{
	ost_objent->ost_objid = ext2fs_cpu_to_le64(ost_objent->ost_objid);
	ost_objent->ost_group = ext2fs_cpu_to_le64(ost_objent->ost_group);
	ost_objent->ost_size = ext2fs_cpu_to_le64(ost_objent->ost_size);
	ost_objent->ost_flag = ext2fs_cpu_to_le64(ost_objent->ost_flag);
	ost_objent->ost_bytes = ext2fs_cpu_to_le64(ost_objent->ost_bytes);
}

void letocpu_ost_objent(struct lfsck_ost_objent *ost_objent)
{
	ost_objent->ost_objid = ext2fs_le64_to_cpu(ost_objent->ost_objid);
	ost_objent->ost_group = ext2fs_le64_to_cpu(ost_objent->ost_group);
	ost_objent->ost_size = ext2fs_le64_to_cpu(ost_objent->ost_size);
	ost_objent->ost_flag = ext2fs_le64_to_cpu(ost_objent->ost_flag);
	ost_objent->ost_bytes = ext2fs_le64_to_cpu(ost_objent->ost_bytes);
}

void letocpu_lov_user_md(struct lov_user_md *lmm)
{
	struct lov_user_ost_data_v1 *loi;
	int i;

	lmm->lmm_magic = ext2fs_le32_to_cpu(lmm->lmm_magic);
	lmm->lmm_pattern = ext2fs_le32_to_cpu(lmm->lmm_pattern);
	lmm->lmm_object_id = ext2fs_le64_to_cpu(lmm->lmm_object_id);
	LMM_OBJECT_SEQ(lmm) = ext2fs_le64_to_cpu(LMM_OBJECT_SEQ(lmm));
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
		loi->l_object_id = ext2fs_le64_to_cpu(loi->l_object_id);
		L_OBJECT_SEQ(loi) = ext2fs_le64_to_cpu(L_OBJECT_SEQ(loi));
		loi->l_ost_gen = ext2fs_le32_to_cpu(loi->l_ost_gen);
		loi->l_ost_idx = ext2fs_le32_to_cpu(loi->l_ost_idx);
	}
}
#endif
