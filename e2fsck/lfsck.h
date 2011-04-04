#undef PACKAGE
#undef VERSION
#ifndef LFSCK_H
#define LFSCK_H

#ifdef ENABLE_LFSCK
#include "e2fsck.h"
#include "../lib/ext2fs/ext2fsP.h"
#include <ext2fs/ext2_ext_attr.h>
#include <lustre/liblustreapi.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif

#include <db.h>

#ifndef LPU64
#if (__WORDSIZE == 32) || defined(__x86_64__)
# define LPU64 "%llu"
# define LPD64 "%lld"
# define LPX64 "%#llx"
# define LPSZ  "%u"
# define LPSSZ "%d"
#elif (__WORDSIZE == 64)
# define LPU64 "%lu"
# define LPD64 "%ld"
# define LPX64 "%#lx"
# define LPSZ  "%lu"
# define LPSSZ "%ld"
#endif
#endif /* !LPU64 */

/* Get O/R or O/0 dir */
#define OBJECT_DIR  "O"
#define OBJECT_DIR_V1 "R"
#define OBJECT_DIR_V2 "0"
#define LOG_DIR "1"
#define PENDING_DIR "PENDING"
#define OBJECTS "OBJECTS"
#define CATLIST "CATALOGS"
#define LAST_ID "LAST_ID"
#define LAST_RCVD "last_rcvd"
#define LOV_OBJID "lov_objid"

#ifndef EXT3_XATTR_INDEX_TRUSTED        /* temporary until we hit l28 kernel */
#define EXT3_XATTR_INDEX_TRUSTED        4
#endif
#define XATTR_LUSTRE_MDS_LOV_EA         "lov"

/* Database names */
#define MDS_HDR       "mdshdr"
#define MDS_DIRINFO   "mds_dirinfo"
#define MDS_SIZEINFO  "mds_sizeinfo"
#define MDS_OSTDB     "mds_ostdb"
#define OST_HDR       "osthdr"
#define OST_OSTDB     "ost_db"

#define MDS_MAGIC     0xDBABCD01
#define OST_MAGIC     0xDB123402

#define OBD_COMPAT_OST          0x00000002 /* this is an OST */
#define OBD_COMPAT_MDT          0x00000004 /* this is an MDT */

#define OBD_INCOMPAT_OST        0x00000002 /* this is an OST */
#define OBD_INCOMPAT_MDT        0x00000004 /* this is an MDS */

#define LOV_MAX_OSTS 2048       /* XXX - Not permanent, change */
#define LOV_EA_SIZE(lum, num) (sizeof(*lum) + num * sizeof(*lum->lmm_objects))
#define LOV_EA_MAX(lum) LOV_EA_SIZE(lum, LOV_MAX_OSTS)

/*XXX*/
#define STRTOUL strtoul
#define STRTOUL_MAX ULONG_MAX

#define HASH_SIZE 131072

struct lustre_server_data {
	__u8  lsd_uuid[40];        /* server UUID */
	__u64 lsd_last_transno;    /* last completed transaction ID */
	__u64 lsd_compat14;        /* reserved - compat with old last_rcvd */
	__u64 lsd_mount_count;     /* incarnation number */
	__u32 lsd_feature_compat;  /* compatible feature flags */
	__u32 lsd_feature_rocompat;/* read-only compatible feature flags */
	__u32 lsd_feature_incompat;/* incompatible feature flags */
	__u32 lsd_server_size;     /* size of server data area */
	__u32 lsd_client_start;    /* start of per-client data area */
	__u16 lsd_client_size;     /* size of per-client data area */
	__u16 lsd_subdir_count;    /* number of subdirectories for objects */
	__u64 lsd_catalog_oid;     /* recovery catalog object id */
	__u32 lsd_catalog_ogen;    /* recovery catalog inode generation */
	__u8  lsd_peeruuid[40];    /* UUID of LOV/OSC associated with MDS */
	__u32 lsd_ost_index;       /* index number of OST in LOV */
	__u32 lsd_mdt_index;       /* index number of MDT in LMV */
};

struct lfsck_mds_hdr {
	__u64 mds_magic;
	__u64 mds_flags;
	__u64 mds_max_files;
	__u32 mds_num_osts;
	__u32 mds_unused;
	__u64 mds_max_ost_id[LOV_MAX_OSTS];
	struct obd_uuid mds_uuid;
	struct obd_uuid mds_ost_info[LOV_MAX_OSTS];
};

struct lfsck_ost_hdr  {
	__u64 ost_magic;
	__u64 ost_flags;
	__u64 ost_num_files;
	__u64 ost_last_id;
	__u32 ost_index;
	__u32 ost_unused;
	struct obd_uuid ost_mds_uuid;
	struct obd_uuid ost_uuid;
};

struct lfsck_mds_dirent {
	__u64 mds_dirfid;
	__u64 mds_fid;
	__u32 mds_generation;
	__u32 mds_filetype;
};

struct lfsck_mds_szinfo {
	__u64 mds_fid;
	__u64 mds_group;
	__u64 mds_size;
	__u64 mds_calc_size;
	__u32 mds_stripe_size;
	__u32 mds_stripe_pattern;
	__u16 mds_stripe_count;
	__u16 mds_stripe_start;
};

struct lfsck_mds_objent {
	__u64 mds_fid;
	__u32 mds_generation;
	__u32 mds_flag;
	__u64 mds_objid;
	__u64 mds_group;
	__u32 mds_ostidx;
	__u32 mds_ostoffset;
};

struct lfsck_ost_objent {
	__u64 ost_objid;
	__u64 ost_group;
	__u64 ost_size;
	__u64 ost_flag;
	__u64 ost_bytes;
};

struct lfsck_ofile_ctx {
	DB *dbp;
	__u64 max_id;
	int have_max_id;
};

struct lfsck_outdb_info {
	__u32 ost_count;
	int have_ost_count;
	DB *mds_sizeinfo_dbp;
	struct lfsck_ofile_ctx *ofile_ctx;
};

/* pass6.c */
extern int e2fsck_lfsck_found_ea(e2fsck_t ctx, ext2_ino_t ino,
				 struct ext2_inode_large *inode,
				 struct ext2_ext_attr_entry *entry,void *value);
extern int e2fsck_lfsck_flush_ea(e2fsck_t ctx);
extern int e2fsck_lfsck_cleanupdb(e2fsck_t ctx);
extern int e2fsck_lfsck_remove_pending(e2fsck_t ctx, char *block_buf);

/* lfsck_common.c */
extern int lfsck_create_dbenv(const char *progname);
extern int lfsck_opendb(const char *fname, const char *dbname, DB **dbpp,
			int allow_dup, int keydata_size, int num_files);
extern void cputole_mds_hdr(struct lfsck_mds_hdr *mds_hdr);
extern void letocpu_mds_hdr(struct lfsck_mds_hdr *mds_hdr);
extern void cputole_ost_hdr(struct lfsck_ost_hdr *ost_hdr);
extern void letocpu_ost_hdr(struct lfsck_ost_hdr *ost_hdr);
extern void cputole_mds_dirent(struct lfsck_mds_dirent *mds_dirent);
extern void letocpu_mds_dirent(struct lfsck_mds_dirent *mds_dirent);
extern void cputole_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo);
extern void letocpu_mds_szinfo(struct lfsck_mds_szinfo *mds_szinfo);
extern void cputole_mds_objent(struct lfsck_mds_objent *mds_objent);
extern void letocpu_mds_objent(struct lfsck_mds_objent *mds_objent);
extern void cputole_ost_objent(struct lfsck_ost_objent *ost_objent);
extern void letocpu_ost_objent(struct lfsck_ost_objent *ost_objent);
extern void letocpu_lov_user_md(struct lov_user_md *lmm);

#define MDS_START_DIRENT_TABLE sizeof(struct lfsck_mds_hdr)

#define MDS_START_SZINFO_TABLE(numfiles) \
sizeof(struct lfsck_mds_hdr) + (sizeof(struct lfsck_mds_dirent) * numfiles)

#define MDS_START_OST_TABLE_OFFSET(idx, numfiles) \
sizeof(struct lfsck_mds_hdr) + (sizeof(struct lfsck_mds_dirent) * numfiles) +\
(sizeof(struct lfsck_mds_szinfo) * numfiles) +\
(sizeof(struct lfsck_mds_objent_hdr) + \
((sizeof(struct lfsck_mds_objent) * numfiles)) * (idx)) + \
sizeof(struct lfsck_mds_objent_hdr)

#define MDS_START_OST_HDR_OFFSET(idx, numfiles) \
sizeof(struct lfsck_mds_hdr) + (sizeof(struct lfsck_mds_dirent) * numfiles) +\
(sizeof(struct lfsck_mds_szinfo) * numfiles) +\
(sizeof(struct lfsck_mds_objent_hdr) + \
((sizeof(struct lfsck_mds_objent) * numfiles)) * (idx))

#define OST_START_OFFSET  sizeof(struct lfsck_ost_hdr)

#ifdef HAVE_OBJECT_SEQ
#define LMM_OBJECT_SEQ(lov) (lov)->lmm_object_seq
#define L_OBJECT_SEQ(lov) (lov)->l_object_seq
#else /* !HAVE_OBJECT_SEQ */
#define LMM_OBJECT_SEQ(lov) (lov)->lmm_object_gr
#define L_OBJECT_SEQ(lov) (lov)->l_object_gr
#endif /* HAVE_OBJECT_SEQ */

#else /* !ENABLE_LFSCK */
#define e2fsck_lfsck_found_ea(ctx, ino, inode, entry, value) (0)
#define e2fsck_lfsck_flush_ea(ctx) (0)
#define e2fsck_lfsck_cleanupdb(ctx) (0)
#define e2fsck_lfsck_remove_pending(ctx, block_buf) (0)
#endif /* ENABLE_LFSCK */

#endif /* LFSCK_H */
