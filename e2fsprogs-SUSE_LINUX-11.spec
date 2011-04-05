#
# spec file for package e2fsprogs (Version 1.41.9)
#
# Copyright (c) 2009 SUSE LINUX Products GmbH, Nuernberg, Germany.
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

# norootforbuild


Name:           e2fsprogs
License:        GPL v2 or later
Group:          System/Filesystems
Supplements:    filesystem(ext2) filesystem(ext3) filesystem(ext4)
BuildRequires:  libblkid-devel libuuid-devel pkg-config libdb-4_5-devel
PreReq:         %install_info_prereq
AutoReqProv:    on
# bug437293
%ifarch ppc64
Obsoletes:      e2fsprogs-64bit
%endif
#
Version:        1.41.90.wc1
Release:        0%{_vendor}
Summary:        Utilities for the Second Extended File System
Url:            http://e2fsprogs.sourceforge.net
Source:         %{name}-%{version}.tar.gz
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

%description
Utilities needed to create and maintain ext2 and ext3 file systems
under Linux. Included in this package are: chattr, lsattr, mke2fs,
mklost+found, tune2fs, e2fsck, resize2fs, and badblocks.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%package devel
License:        GPL v2 or later
Summary:        Dummy development package
Group:          Development/Libraries/C and C++
AutoReqProv:    on
# bug437293
%ifarch ppc64
Obsoletes:      e2fsprogs-devel-64bit
%endif
#
Requires:       libext2fs-devel = %version libblkid-devel libuuid-devel

%description devel
Dummy development package for backwards compatibility.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%package -n libext2fs2
License:        GPL v2 or later
Summary:        Ext2fs libray
Group:          System/Filesystems
AutoReqProv:    on

%description -n libext2fs2
The basic Ext2fs library.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%package -n libext2fs-devel
License:        GPL v2 or later
Summary:        Development files for libext2fs
Group:          Development/Libraries/C and C++
AutoReqProv:    on
Requires:       libext2fs2 = %version libcom_err-devel

%description -n libext2fs-devel
Development files for libext2fs.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%package -n libcom_err2
License:        GPL v2 or later
Summary:        E2fsprogs error reporting library
Group:          System/Filesystems
# bug437293
%ifarch ppc64
Obsoletes:      libcom_err-64bit
Obsoletes:      libcom_err2-64bit
%endif
#
Provides:       libcom_err = %{version}
Obsoletes:      libcom_err <= 1.40
AutoReqProv:    on

%description -n libcom_err2
com_err is an error message display library.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%package -n libcom_err-devel
License:        GPL v2 or later
Summary:        Development files for libcom_err
Group:          Development/Libraries/C and C++
AutoReqProv:    on
# bug437293
%ifarch ppc64
Obsoletes:      libcom_err-devel-64bit
%endif
#
Requires:       libcom_err2 = %version

%description -n libcom_err-devel
Development files for the com_err error message display library.



Authors:
--------
    Remy Card <card@masi.ibp.fr>
    Theodore Ts'o <tytso@mit.edu>

%prep
%setup -q
# e2fsprogs patches
patch -p1 < patches/sles/11/rpm/e2fsprogs-1.41.1-splash_support.patch
# libcom_err patches
patch -p1 < patches/sles/11/rpm/libcom_err-compile_et_permissions.patch
cp patches/sles/11/rpm/README.SUSE .
cp patches/sles/11/rpm/%{name}-1.41.4.de.po po/de.po

%build
autoreconf --force --install
./configure --prefix=%{_prefix} \
  --with-root-prefix=''   \
  --mandir=%{_mandir} \
  --infodir=%{_infodir} \
  --libdir=%{_libdir} \
  --enable-elf-shlibs \
  --disable-evms \
  --disable-libblkid \
  --disable-libuuid \
  --disable-uuidd \
  --disable-fsck \
  %{?extra_config_flags:%extra_config_flags} CFLAGS="$RPM_OPT_FLAGS"
make V=1

%install
make install install-libs DESTDIR=$RPM_BUILD_ROOT ELF_INSTALL_DIR=/%{_lib}
%{find_lang} %{name}
rm $RPM_BUILD_ROOT%{_libdir}/e2initrd_helper
rm -f $RPM_BUILD_ROOT/sbin/mkfs.ext4dev
rm -f $RPM_BUILD_ROOT/sbin/fsck.ext4dev
rm -f $RPM_BUILD_ROOT/usr/share/man/man8/mkfs.ext4dev.8*
rm -f $RPM_BUILD_ROOT/usr/share/man/man8/fsck.ext4dev.8*

%clean
rm -rf $RPM_BUILD_ROOT

%post
/sbin/ldconfig
%install_info --info-dir=%{_infodir} %{_infodir}/libext2fs.info.gz

%postun
/sbin/ldconfig
%install_info_delete --info-dir=%{_infodir} %{_infodir}/libext2fs.info.gz

%post -n libext2fs2
/sbin/ldconfig

%postun -n libext2fs2
/sbin/ldconfig

%post -n libcom_err2
/sbin/ldconfig

%postun -n libcom_err2
/sbin/ldconfig

%files -f %{name}.lang
%defattr(-, root, root)
%doc RELEASE-NOTES README
%config /etc/mke2fs.conf
/sbin/badblocks
/sbin/debugfs
/sbin/dumpe2fs
/sbin/e2undo
/sbin/e2fsck
/sbin/e2label
/sbin/fsck.ext2
/sbin/fsck.ext3
/sbin/fsck.ext4
/sbin/mke2fs
/sbin/mkfs.ext2
/sbin/mkfs.ext3
/sbin/mkfs.ext4
/sbin/resize2fs
/sbin/tune2fs
/sbin/e2image
/sbin/logsave
/usr/bin/chattr
/usr/bin/lsattr
/usr/sbin/mklost+found
/usr/sbin/filefrag
/usr/sbin/e2freefrag
/usr/sbin/e2scan
/usr/sbin/lfsck
%{_infodir}/libext2fs.info.gz
%{_mandir}/man1/chattr.1.gz
%{_mandir}/man1/lsattr.1.gz
%{_mandir}/man5/e2fsck.conf.5.gz
%{_mandir}/man5/mke2fs.conf.5.gz
%{_mandir}/man8/*.8.gz

%files devel
%defattr(-,root,root)
%doc README.SUSE

%files -n libext2fs2
%defattr(-, root, root)
/%{_lib}/libext2fs.so.*
/%{_lib}/libe2p.so.*

%files -n libext2fs-devel
%defattr(-, root, root)
%{_libdir}/libext2fs.so
%{_libdir}/libext2fs.a
%{_libdir}/libe2p.a
%{_libdir}/libe2p.so
/usr/include/ext2fs
/usr/include/e2p
%_libdir/pkgconfig/e2p.pc
%_libdir/pkgconfig/ext2fs.pc

%files -n libcom_err2
%defattr(-, root, root)
/%{_lib}/libcom_err.so.*
/%{_lib}/libss.so.*

%files -n libcom_err-devel
%defattr(-, root, root)
%_bindir/compile_et
%_bindir/mk_cmds
%{_libdir}/libcom_err.so
%{_libdir}/libcom_err.a
%{_libdir}/libss.a
%{_libdir}/libss.so
%_libdir/pkgconfig/com_err.pc
%_libdir/pkgconfig/ss.pc
%_includedir/et
%_includedir/ss
%_datadir/et
%_datadir/ss
%{_mandir}/man1/compile_et.1.gz
%{_mandir}/man1/mk_cmds.1.gz
%{_mandir}/man3/com_err.3.gz

%changelog
* Thu Nov 19 2009 hvogel@suse.de
- Update to version 1.41.9 [FATE#305340]
* Thu Oct  8 2009 crrodriguez@suse.de
- fsck during boot up fails with Too many open files [bnc#503008]
* Thu Sep 10 2009 coolo@novell.com
- fix the super block even if someone mounted the file system
  in wrong timezone in between (bnc#537542)
* Thu Sep  3 2009 coolo@novell.com
- update to 1.41.9:
  "All users of e2fsprogs are urged to upgrade to the 1.41.9
    version as soon as possible"
  * Fix a bug in e2fsck routines for reallocating an inode table which
  could cause it to loop forever on an ext4 filesystem with the FLEX_BG
  filesystem feature with a relatively rare (and specific) filesystem
  corruption.  This fix causes e2fsck to try to find space for a new
  portion of the inode table in the containing flex_bg, and if that
  fails, the new portion of the inode table will be allocated in any
  free space available in the filesystem.
  * Make e2fsck less annoying by only asking for permission to relocate a
  block group's inode table once, instead of for every overlapping
  block.  Similarly, only ask once to recompute the block group
  checksums, instead of once for each corrupted block group's checksum.
  see more changes in RELEASE-NOTES
* Mon Aug  3 2009 aschnell@suse.de
- added Supplements for ext4
* Mon Jul 13 2009 coolo@novell.com
- look for libreadline.so.6 too
- refresh patches to avoid fuzz
* Mon Jul 13 2009 kay.sievers@novell.com
- update to 1.41.8
  - Fix resize2fs's online resizing, fixing a regression which in
    e2fpsrogs 1.41.7.
  - Fix resize2fs bugs when shrinking ext4 filesystems
  - If the resize2fs operation fails, the user will be told to fix up
    the filesystem using e2fsck -fy.
  - do not install disabled uuid binary and man page
  - Fix filefrag program for files that have more than 144 extents.
  - allow V=1 to disable silent build
- enable verbose build again (V=1 merged upstream)
- move awk scripts from libcom_err2 to libcom_err2-devel
* Wed Jul  8 2009 meissner@suse.de
- moved baselibs.conf changes to util-linux.
* Mon Jun 29 2009 kay.sievers@novell.com
- update to 1.41.7
- disable libuuid and libblkid packages (moved to util-linux)
- drop libvolume_id support (util-linux's libblkid will work)
- removed patches:
    e2fsprogs-libvolume_id-support.patch
    e2fsprogs-no_cmd_hiding.patch
    e2fsprogs-base_devt.patch
    e2fsprogs-mdraid.patch
* Mon Mar  9 2009 pth@suse.de
- Fix errors in German messages.
* Fri Feb 20 2009 coolo@suse.de
- ext4dev is now ext4 (2.6.29)
- don't call autoconf as long as it works without
* Tue Feb  3 2009 mkoenig@suse.de
- update to version 1.41.4:
  debugfs:
  * enhance the "ncheck" command
  * enhance "hash" command
  * fix a potential memory leak
  * fix the usage message for logdump command
  * fix ncheck command so that it prints all of the names of
    hardlinks in the same directory
  * e2fsprogs 1.41 broke debugfs's logdump command for normal
    ext3/4 filesystems with 32-bit block numbers, when the headers
    for 64-bit block numbers was added.  This regression has been fixed
  * ncheck command has been fixed to avoid printing garbage
    characters at the end of file names
  e2fsck:
  * don't accidentally print the translation file's
    header when asking the user a custom question
  * print the correct inode number for uinit_bg related problems
  * will now offer to clear the test_fs flag if the ext4 filesystem
    is available on linux
  * fix a bug where in preen mode, if there are disk I/O errors
    while trying to close a filesystem can lead to infinite loops
  * no longer abort an fsck run if block group has an errant
    INODE_UNINIT flag
  * distinguish between fragmented directories and fragmented
    files in verbose mode statistics and in the fragcheck report
  * fix a bug which caused it double count non-contiguous
    extent-based inodes
  * e2fsck will leave some slack space when repacking directories
    to allow room for a few directory entries to be added without
    causing leaf nodes to be split right away
  * fix a bug which caused e2fsck to crash when it comes across a
    corrupted interior node in an extent tree
  * e2fsck problem descriptions involving the journal are no longer
    referred to as "ext3" problems, since ext4 filesystems also have
    journals
  * fix a long-standing bug in e2fsck which would cause it to crash
    when replying journals for filesystems with block sizes greater
    than 8k
  badblocks:
  * support for normal files which are greater than 2GB
  * display the time and percentage complete when in verbose mode
  resize2fs:
  * fix a potential memory corruption problem
  * fix a bug in resize2fs where passing in a bogus new size of
    0 blocks will cause resize2fs to drop into an infinite loop
  * fix resize2fs for ext4 filesystems
  tune2fs:
  * now updates the block group checksums when changing the UUID
    to avoid causing e2fsck to complain vociferously at the next reboot
  * inode size resizing algorithms have been fixed so it is not
    vastly inefficient for moderate-to-large filesystems
  * fix inode resizing algorithm so it will not corrupt filesystems
    laid out for RAID filesystems; in addition, tune2fs will refuse
    to change the inode size for filesystems that have the flex_bg
    feature enabled
  dumpe2fs:
  * fix bug which caused dumpe2fs to abort with an error if run on a
    filesystem that contained an external journal
  mke2fs:
  * new option -U, which allows the user to specify the UUID that
    should be used for the new filesystem
  * treat devices that are exactly 16TB as if they were 16TB minus
    one block
  blkid:
  * fix a file descriptor leak in libblkid
  * correctly detect whether the ext4 and ext4dev filesystems
    are available, so that the ext4dev->ext4 fallback code works
    correctly
  * fixed a bug which could sometimes cause blkid to return an
    exit value of zero for a non-existent device
  * recognize ext3 filesystems that have the test_fs flag
    set as ext3 filesystems
  * recognize btrfs filesystems and swap devices currently used
    by user-level software suspend
  libext2fs:
  * add a check in the Unix I/O functions in libext2fs so that
    when a device is opened read/write, return an error if the
    device is read-only using the BLKROGET ioctl
- the libcom_err patches for bnc#66534 have been removed because
  git commit d7f45af802330a0e1450afa05185d3722e77a76c
  should fix the problem
- remove patches
  e2fsprogs-1.41.1-e2fsck_fix_automatic_blocksize_detection.patch
  e2fsprogs-1.41.1-function_signature_fix.patch
  e2fsprogs-1.41.1-link_fix.patch
  libcom_err-disable_test.patch
  libcom_err-mutex.patch
  libcom_err-no-init_error_table.patch
* Tue Jan 13 2009 olh@suse.de
- obsolete old -XXbit packages (bnc#437293)
* Thu Dec  4 2008 mkoenig@suse.de
- send bootsplash messages in one write call
* Tue Oct 28 2008 mkoenig@suse.de
- fix function signature to avoid compiler warning [bnc#439129]
* Tue Oct  7 2008 mkoenig@suse.de
- e2fsck: fix e2fsck automatic blocksize detetion
* Mon Sep 29 2008 mkoenig@suse.de
- e2fsck: shut off splash screen when check is needed [bnc#237283]
* Mon Sep 15 2008 mkoenig@suse.de
- remove recommends of uuid-runtime from libuuid [bnc#426278]
- move uuid hints README.SUSE.uuidd to uuid-runtime package
* Fri Sep  5 2008 mkoenig@suse.de
- update to version 1.41.1
  * mke2fs
    + issues now a warning if there is no definition in
    /etc/mke2fs.conf for the filesystem to be created
    + creates now the journal in the middle of the filesystem
    + now avoids allocating an extra block to the journal
    + will correctly enforce the prohibition against features
    in revision 0 filesystems
    + previously would occasionaly create some slightly non-optimally
    placed inode tables; this bug has been fixed
    + will now set the creation timestamp on the lost+found directory
    and the root directory
  * blkid
    + recognize MacOS hfsx filesystems, and correctly extract the
    label and uuid for hfs, hfsx, and hfsplus filesystems
    + improved detection of JFS and HPFS
    + more efficient handling of devicemapper devices
    + fix cache validation bugs
    + The blkid program will now print out a user-friendly listing
    of all of the block devices in the system and what they
    contain when given the -L option
  * resize2fs
    + will now correctly handle filesystems with extents and/or
    uninitialized block groups correctly when file/directory blocks
    need to relocated
    + support for on-line resizing ext4 filesystem with the flex_bg
    filesystem feature.  The method for doing so is not optimal,
    but to do a better job will require kernel support
    + is now correctly managing the directory in-use counts when
    shrinking filesystems and directory inodes needed to be moved
    from one block group to another
  * e2fsck
    + now correctly calculates ind/dind/tind statistics in the
    presence of extent-based files
    + now prints the depth of corrupt htree directories
  * debugfs
    + htree command now correctly understands extent-based
    directories
    + new command which will print the supported features
  * Add support for setting the default hash algorithm used in b-tree
    directories in tune2fs (from a command-line option) or mke2fs (via
    mke2fs.conf).  In addition, change the default hash algorithm to
    half_md4, since it is faster and better
  * Fix support for empty directory blocks in ext4 filesystems with
    64k blocksize filesystems
  * The filefrag program now has a more accurate calculation for the
    number of ideal extents
- fix linking of blkid
  e2fsprogs-1.41.1-link_fix.patch
- remove patches
  e2fsprogs-1.41.0-fix_messages.patch
  e2fsprogs-1.41.0-tst_link_fix.patch
* Fri Aug 29 2008 kay.sievers@novell.com
- update libvolume_id patch to work with libvolume_id.so.1
* Thu Aug 21 2008 pth@suse.de
- Add current german messages.
- Fix e2fsprogs-base_devt.patch and e2fsprogs-libvolume_id-support.patch
  so that the package tools work.
- Add missing space to two messages and resync message
  catalogs by configuring with --enable-maintainer-mode.
* Wed Aug 20 2008 mkoenig@suse.de
- add uuid related manpages to uuid-runtime subpackage [bnc#418568]
* Mon Aug 18 2008 mkoenig@suse.de
- currently do not install *.ext4 links for mkfs and fsck
  tools, but only the *.ext4dev links.
* Wed Jul 16 2008 mkoenig@suse.de
- update to version 1.41.0
  * add support for ext4 filesystem features:
    extents, uninit_bg, flex_bg, huge_file, dir_nlink
  * support for checking journal checksums
  * tune2fs supports migrating fs from 128 byte inode to 256 byte
  * add support for "undo"
  * e2fsck now performs more extensive and careful checks of extended
    attributes stored in the inod
- fix e2fsck make check
* Wed Jul  2 2008 schwab@suse.de
- Remove doubleplusungood -fsigned-char.
* Tue Jun 24 2008 mkoenig@suse.de
- update to version 1.40.11
  most important changes since 1.40.8:
  * Mke2fs will not allow the logically incorect combination of
    resize_inode and meta_bg, which had previously caused mke2fs
    to create a corrupt fileystem
  * Fix mke2fs's creation of resize inode when there is a
    non-standard s_first_data_block setting
  * Teach fsck to treat "ext4" and "ext4dev" as ext* filesystems
  * Fix fsck so that progress information is sent back correctly
  * Add detection for ZFS volumes to the libblkid library
- remove e2fsprogs-1.40.7.de.po, updated upstream
- remove patches
  e2fsprogs-1.40.7-uuidd_security.patch
  e2fsprogs-1.40.8-e2fsck_recovery_fix.patch
  e2fsprogs-1.40.8-fix_ext2fs_swap_inode_full.patch
  e2fsprogs-1.40.8-missing_init.patch
* Tue May 27 2008 ro@suse.de
- fix baselibs.conf to not generate unresolvable deps
* Wed May 21 2008 cthiel@suse.de
- fix baselibs.conf
* Wed May 21 2008 mkoenig@suse.de
- e2fsck: Fix potential data corruptor bug in journal recovery
  [bnc#393095]
* Tue May 13 2008 mkoenig@suse.de
- libuuid: do not use unintialized variable [bnc#386649]
* Wed May  7 2008 coolo@suse.de
- fix provides/obsoletes for rename
* Thu Apr 10 2008 ro@suse.de
- added baselibs.conf file to build xxbit packages
  for multilib support
* Tue Mar 18 2008 pth@suse.de
- Readd the current de.po just submitted upstream to the TP robot.
* Fri Mar 14 2008 mkoenig@suse.de
- update to version 1.40.8
  * Fixed e2image -I so it works on image files which are larger than 2GB
  * Fixed e2fsck's handling of directory inodes with a corrupt size field
  * Fixed e2fsck handling of pass 2 "should never happen error"
  * Fixed Resize2fs bug resizing large inodes with extended attributes
- update README.SUSE: give some hints on enabling uuidd, since it has
  been decided to not enable it by default [bnc#354398]
- removed
  de.po (updated upstream)
* Tue Mar  4 2008 mkoenig@suse.de
- update to version 1.40.7
  * Remove support for clearing the SPARSE_SUPER feature from tune2fs, and
    depreciate the -s option, since it can result in filesystems which
    e2fsck can't fix easily.  There are very good reasons for wanting to
    disable sparse_super; users who wants to turn off sparse_super can use
    debugfs.
  * Add missing options to mke2fs's usage message
  * Fix bug in resize2fs when large (greater than 128 byte) inodes are
    moved when a filesystem is shrunk
  * E2fsck now prints an explicit message when the bad block inode is
    updated, to avoid confusion about why the filesystem was modified.
  * Allow mke2fs and tune2fs manipulate the large_file feature.
    Previously we just let the kernel and e2fsck do this automatically,
    but e2fsck will no longer automatically clear the large_file feature
  * Suppress message about an old-style fstab if the fstab file is empty
  * Change e2fsck to no longer clear the LARGE_FILES feature flag
    automatically, when there are no more > 2GB files in the filesystem.
  * Fix bug which could cause libblkid to seg fault if a device mapper
    volume disappears while it is being probed.
  * Enhance e2fsck's reporting of unsupported filesystem feature flags
  * Fix option syntax in dumpe2fs for explicit superblock and blocksize
    parameters
  * Add support to tune2fs to clear the resize_inode feature
  * Teach blkid to detect LVM2 physical volumes
  * Add support for setting RAID stride and stripe-width via mke2fs and
    tune2fs.  Teach dumpe2fs to print the RAID parameters
  * Add support for setting new superblock fields to debugfs's
    set_super_value
  * Add support for printing "mostly-printable" extended attributes in
    Debugfs
  * Add support for the -M option to fsck, which causes it to ignore
    mounted filesystem
  * Fix uuidd so that it creates the pid file with the correct pid number
- The -M option is now used upstream to ignore mounted filesystems,
  this has previously been in SuSE with -m. This has to be changed
  since lower case characters are reserved for filesystem specific
  checker options. The "like mount" behaviour previously in SuSE
  with -M has been removed.
- add patch from Eric Sandeen to fix the loss of extended attributes
  in large inodes upon resize
- removed patches
  fsck-ignore-mounted.patch
* Wed Feb 27 2008 mkoenig@suse.de
- update to version 1.40.6
  * Add support for returning labels for UDF filesystems in the blkid
    library
  * Fix bug in the blkid library where cached filesystems was not being
    flushed
  * Added logic to the blkid library to automatically choose whether a
    filesystem should be mounted as ext4 or ext4dev, as appropriate
  * Allow tune2fs to set and clear the test_fs flag on ext4 filesystems
- removed patches:
  e2fsprogs-1.40.5-blkid_cache_file_env.patch (merged)
  libcom_err-no-static-buffer.patch (fixed upstream with TLS)
* Tue Jan 29 2008 mkoenig@suse.de
- update to version 1.40.5:
  * Fix a potential overflow big in e2image
  * Mke2fs will now create new filesystems with 256 byte inodes and the
    ext_attr feature flag by default
  * Teach e2fsck to ignore certain "safe" filesystem features which are
    set automatically by the kernel
  * Add support in tune2fs and mke2fs for making a filesystem as being "ok
    to be used with test kernel code"
  * Change e2fsck -fD so that it sorts non-htree directories by inode
    numbers instead of by name, since that optimizes performances much
    more significantly
  * If e2image fills the disk, fix it so it exits right away
  * If ftruncate64() is not available for resize2fs, let it use ftrucate()
    instead
  * Add support for detecting HFS+ filesystems in the blkid library
  * Add supprt in the blkid library for ext4/ext4dev filesystems
  * Fix a bug in blkid where it could die on a floating point exception
    when presented with a corrupt reiserfs image
  * Fix blkid's handling of ntfs UUID's so that leading zeros are printed
    such that UUID string is a fixed length
  * Fix debugfs's 'lsdel' command so it uses ext2fs_block_iterate2 so it
    will work with large files
  * Allow the debugfs 'undel' command to undelete an inode without linking
    it to a specific destination directory
- enhance init script
- add sysconfig parameter UUIDD_ON_DEMAND_ONLY setting the startup
  policy for uuidd
- remove merged and obsolete patches:
  close.patch
  e2fsprogs-1.33-codecleanup.diff
  e2fsprogs-1.35-libdir.diff
  e2fsprogs-1.39-resize2fs_manpage.patch
  e2fsprogs-1.40.4-uuid_null.patch
  e2fsprogs-blkid.diff
  e2fsprogs-blkid_probe_hfsplus.patch
  e2fsprogs-strncat.patch
  elf.diff
  e2fsprogs-mkinstalldirs.patch
  e2fsprogs-special_make_targets.patch
  e2fsprogs-probe_reiserfs-fpe.patch
  e2fsprogs-1.33-fsckdevdisplay.diff
  e2fsprogs-uninitialized.diff
* Wed Jan 16 2008 mkoenig@suse.de
- update to version 1.40.4:
  * Fix potential security vulnerability (CVE-2007-5497)
  * Fix big-endian byte-swapping bug in ext2fs_swap_inode_full()
  * Fix a bug in ext2fs_initialize() which causes mke2fs to fail while
    allocating inode tables for some relatively rare odd disk sizes.
  * Fix bug in ext2fs_check_desc() which will cause e2fsck to complain
    about (valid) filesystems where the inode table extends to the last
    block of the block group
  * Change e2fsck so it will not complain if a file has blocks reallocated
    up to the next multiple of a system's page size
  * Change e2fsck so it uses sscanf() instead of atoi() so it non-numeric
    arguments are detected as such and the parse error is reported to the
    user
  * Make the e2fsprogs program more robust so that they will not crash
    when opening a corrupt filesystem where s_inode_size is zero.
  * Fix e2fsck so that if the superblock is corrupt, but still looks
    vaguely like an ext2/3/4 superblock, that it automatically tries to
    fall back to the backup superblock, instead of failing with a hard
    error
  * Fix fsck to ignore /etc/fstab entries for bind mounts
  * E2fsck will no longer mark a filesystem as invalid if it has time
    errors and the user refuses to fix the problem.
  * Enhance blkid's detection of FAT filesystems
  * Enhance e2fsck so it will force the backup superblocks to be backed up
    if the filesystem is consistent and key constants have been changed
    (i.e., by an on-line resize) or by e2fsck in the course of its
    operations.
  * Enhance the blkid library so it will recognize squashfs filesystems
  * Fix e2image so that in raw mode it does not create an image file which
    is one byte too large
  * Fix heuristics in blkid which could cause a disk without partitions to
    be incorrectly skipped when a loopback device is present
  * Avoid division by zero error when probing an invalid FAT filesystem in
    the blkid library
  * Fix sign-extension problem on 64-bit systems in in the com_err
    library
  * Format control characters and characters with the high eighth bit set
    when printing the contents of the blkid cache, to prevent filesystems
    with garbage labels from sending escape sequences
  * Fix fsck to only treat the '#' character as a comment at the beginning
    of the line in /etc/fstab
  * Filter out the NEEDS_RECOVERY feature flag when writing out the backup
    superblocks
  * Improve time-based UUID generation.  A new daemon uuidd, is started
    automatically by libuuid if necessary
- new subpackage: uuid-runtime
  containing uuidd and uuidgen
- removed obsolete patches
  e2fsprogs-1.39-uuid_duplicates.patch
  e2fsprogs-1.40.2-open_fix.patch
  e2fsprogs-1.40-be_swap_fix.patch
* Mon Nov 26 2007 mkoenig@suse.de
- fix build: missing third argument to open
- do not remove buildroot in install section
* Fri Jul 27 2007 mkoenig@suse.de
- fix typo in specfile
* Thu Jul 26 2007 mkoenig@suse.de
- Fix big-endian byte-swapping bug in ext2fs_swap_inode_full()
  e2fsprogs-1.40-be_swap_fix.patch
* Wed Jul 25 2007 bk@suse.de
- e2fsprogs requires libext2fs2 of the same version number to work
- enable make check and make gcc-wall in %%check (executed last)
- shut up bogus gcc warning for use of uninitialised variables
* Wed Jul 25 2007 mkoenig@suse.de
- remove e2fsprogs-blkid_probe_ext4.patch
  broken and it is way too early to support
* Wed Jul 18 2007 mkoenig@suse.de
- update to version 1.40.2
  bugfix release
* Mon Jul  9 2007 mkoenig@suse.de
- update to version 1.40.1:
  * Bugfix release
- removed patch (merged upstream)
  e2fsprogs-1.39-cleanup.patch
* Wed Jul  4 2007 mkoenig@suse.de
- update to version 1.40
- branch off libraries:
  libblkid1
  libuuid1
  libext2fs2
- renaming libcom_err to libcom_err2
* Tue Jun 19 2007 mkoenig@suse.de
- fix e2fsprogs-1.39-uuid_duplicates.patch [#189640]
  * detach shm segment after use
  * set SEM_UNDO for semaphore operations, otherwise we do not
    get a clean state after interruption by a signal
* Wed Apr 25 2007 pth@suse.de
- Fix German translations.
* Wed Apr 11 2007 mkoenig@suse.de
- blkid: fix hfsplus probing to detect HFS+ volumes embedded
  in a HFS volume
* Wed Apr  4 2007 mkoenig@suse.de
- add Supplements line [FATE#301966]
* Fri Mar 30 2007 mkoenig@suse.de
- update to current hg version from 29-03-2007
  * Fixes a lot of memory leaks and other bugs
- remove merged patch:
  e2fsprogs-1.39-check_fs_open-in-dump_unused.patch
* Wed Mar 28 2007 mkoenig@suse.de
- blkid: add hfsplus volume detection (FATE#302071)
- blkid: add experimental detection of ext4dev (FATE#301897)
* Thu Jan 25 2007 mkoenig@suse.de
- fix segfault in debugfs when using without open fs [#238140]
* Mon Jan 22 2007 mkoenig@suse.de
- don't chmod -w headers in compile_et to avoid build
  problems with some packages
* Fri Jan 19 2007 mkoenig@suse.de
- update to version 1.40-WIP-1114 (FATE#301897)
  * support for ext4
  * several bugfixes
- remove ext2resize from package, because the online resizing
  functionality has been merged into resize2fs since version 1.39
  and ext2resize is unmaintained.
* Tue Dec 19 2006 meissner@suse.de
- fixed build
* Wed Nov  8 2006 ro@suse.de
- provide libcom_err-devel in libcom_err
* Thu Oct 19 2006 mkoenig@suse.de
- fix bug in uuid patch
* Mon Oct 16 2006 mkoenig@suse.de
- fix build of shared lib
* Thu Oct 12 2006 mkoenig@suse.de
- fix uuid bug [#189640]
- fix blkid problem with empty FAT labels [#211110]
- fix small typo in resize2fs man page
* Tue Sep 26 2006 mkoenig@suse.de
- fix bug in fsck udev/libvolume_id patch [#205671]
* Wed Sep 20 2006 mkoenig@suse.de
- update to version 1.39:
  * Fix 32-bit cleanliness
  * Change mke2fs to use /etc/mke2fs.conf
  * Mke2fs will now create filesystems hash trees and
    on-line resizing enabled by default
  * The e2fsprogs tools (resize2fs, e2fsck, mke2fs) will open the
    filesystem device node in exclusive mode
  * Add support for on-line resizing to resize2fs.
  * The blkid library will now store the UUID of the external
    journal used by ext3 filesystems
  * E2fsck will now consult a configuration file, /etc/e2fsck.conf
  * E2fsck will detect if the superblock's last mount field or
    last write field is in the future, and offer to fix if so.
  * E2fsck now checks to see if the superblock hint for the
    location of the external journal is incorrect
  * Resize2fs will now automatically determine the RAID stride
    parameter that had been used to create the filesystem
  * Fix mke2fs so that it correctly creates external journals on
    big-endian machines
  * Fix a bug in the e2p library
  * Add a new debugfs command, set_current_time
  * Fix debugfs commands
  * Fix mklost+found so that it creates a full-sized directory on
    filesystems with larger block sizes.
  * Fix a file descriptor leak in blkid library
  * Allow fractional percentages to the -m option in mke2fs and tune2fs
  * Add support for device mapper library to the blkid library
  * Fix the blkid library so that it notices when an ext2 filesystem
    is upgraded to ext3.
  * Improve the blkid's library VFAT/FAT detectio
  * Add support for the reiser4 and software suspend partitions
    to the blkid library.
- update ext2resize to version 1.1.19:
  * Add support for ext3 online resizing
  * Support LARGEFILE compat flag
  * Make the resize inode part of the fs struct
  * Add the FL_IOCTL flag
  * Bugfixes
* Fri Aug 11 2006 pth@suse.de
- Fix to comply with gettex 0.15.
- Move ext2resize sources to toplevel directory.
- Fix use of MKINSTALLDIRS.
* Fri Aug  4 2006 kay.sievers@suse.de
- update libvolume_id integration to match util-linux
* Fri Jun 16 2006 ro@suse.de
- added libvolume_id-devel to BuildRequires
- updated e2fsprogs-udev.patch to match header rename
* Wed Feb  8 2006 hare@suse.de
- Fix fsck -m (#146606) to really check filesystems.
* Mon Jan 30 2006 hvogel@suse.de
- Document -I inode-size [#145445]
* Sun Jan 29 2006 hvogel@suse.de
- fix hares patch
* Fri Jan 27 2006 hare@suse.de
- Add option to not return an error code for mounted
  filesystems (#145400).
* Wed Jan 25 2006 mls@suse.de
- converted neededforbuild to BuildRequires
* Fri Jan 20 2006 hvogel@suse.de
- Support ext2/ext3 online resize
* Mon Dec 12 2005 hvogel@suse.de
- remove lib/et/test_cases/imap_err* from the tarball because
  they are not distributeable.
* Tue Dec  6 2005 pth@suse.de
- remove unnecessary type-punning
- reduce compiler warnings
* Tue Nov 15 2005 jblunck@suse.de
- added close.patch provided by Ted Tso (IBM) to fix bug #132708
* Mon Nov 14 2005 hare@suse.de
- Use devt when comparing devices
- fsck: Use information provided by udev for detecting devices
* Wed Oct  5 2005 hvogel@suse.de
- fix too few arguments to a *printf function
- require libcom_err on e2fsprogs-devel
* Fri Sep  9 2005 hvogel@suse.de
- add gross hack to avoid divide by zero in probe_reiserfs
  [#115827]
* Mon Aug  8 2005 hvogel@suse.de
- added environment variable BLKID_SKIP_CHECK_MDRAID to work around
  broken software raid detection [Bug #100530]
* Tue Jul  5 2005 hvogel@suse.de
- update to version 1.38
- mt reworked his patches a bit. See Bug #66534
* Thu Jun 23 2005 hvogel@suse.de
- call ldconfig in post/postun
- add version to devel package dependencie
- readd missing patch (7)
* Thu Apr 28 2005 hvogel@suse.de
- update to version 1.37
- mt reworked one libcom_err patch a bit to provide more
  meaningfull error handling
- fix retuen value in inode.c
* Thu Mar 31 2005 hvogel@suse.de
- split libcom_err to a subpackage
- add mutex synchronization to e2fsprogs/lib/et
- removed usage of a static buffer in error_message()
- disabled init_error_table function
- disabled build of unused e2fsck.static
* Fri Mar 18 2005 hvogel@suse.de
- fix endian unsafeness in getopt (#73855)
* Tue Feb  8 2005 hvogel@suse.de
- Update to 1.36 final
* Tue Aug 10 2004 pth@suse.de
- Update to 1.35 RC5
* Wed Mar 17 2004 pth@suse.de
- Don't build the EVMS plugin because it's out of date for
  EVMS in kernel 2.6.
* Thu Mar  4 2004 pth@suse.de
- Add patch from Olaf Hering that makes fsck read a different
  blkid file via BLKID_FILE environment var (#35156)
* Thu Feb 19 2004 kukuk@suse.de
- Remove obsolete recode call
* Mon Jan 12 2004 ro@suse.de
- removed run_ldconfig again
* Sat Jan 10 2004 adrian@suse.de
- add %%run_ldconfig
* Thu Oct  2 2003 pthomas@suse.de
- Add patch from Kurt Garloff to make e2fsprogs compile
  with latest kernel headers (SCSI_BLK_MAJOR undefined).
* Mon Sep 15 2003 pthomas@suse.de
- Fix czech message catalog which has been transformed twice
  from latin2 to utf-8 and add an iconv call to the spec file
  that will make building fail if the file is corrected upstream.
* Sat Aug 30 2003 agruen@suse.de
- Fix an endianness bug in ext2fs_swap_inode: Fast symlinks that
  have extended attributes are acidentally byte swapped on
  big-endian machines.
* Fri Aug  1 2003 pthomas@suse.de
- Apply patch from Ted T'so for badblocks.
* Thu Jul 31 2003 pthomas@suse.de
- Update to 1.34.
- Various fixes to libcom_err to make it really compatible
  to the heimdal version.
- Fix int<->pointer casts.
- Fix places that may break strict aliasing.
* Fri Jun 20 2003 ro@suse.de
- added directory to filelist
* Wed May 14 2003 pthomas@suse.de
- Use %%defattr
- Include all installed files.
* Tue Apr 29 2003 mfabian@suse.de
- add libblkid.so* and libblkid.a to file lists
* Thu Apr 24 2003 pthomas@suse.de
- Update to 1.33 and adapt patches.
- Add missing headers where necessary.
* Thu Apr 24 2003 ro@suse.de
- fix install_info --delete call and move from preun to postun
* Fri Feb  7 2003 ro@suse.de
- added install_info macros
* Tue Oct  1 2002 garloff@suse.de
- Fix segfault in display of real device in presence of volume
  name. #20422
* Tue Sep  3 2002 mls@suse.de
- remove duplicate evms scan (already included in 1.28)
- fix volume group scan bug
* Mon Sep  2 2002 agruen@suse.de
- Update to 1.28. Includes very minor fixes in htree, which we have
  disabled anyway, one fix that we had in a separate patch, and
  has additional release notes.
* Mon Aug 19 2002 agruen@suse.de
- Update to 1.28-WIP-0817, which includes Extended Attribute
  and several smaller fixes. We disable htree support and don't
  install the evms library for now.
- Remove `make gcc-wall' for now (as it does a `make clean' in
  doc/).
* Thu Aug 15 2002 mls@suse.de
- support jfs, reiserfs, evms in label/uuid scan (code stolen
  from util-linux:mount)
* Sun Jul 28 2002 kukuk@suse.de
- Remove unused tetex from neededforbuild
* Fri Jul 19 2002 olh@suse.de
- use a better method for finding missed filelist entries
* Fri Apr 12 2002 sf@suse.de
- added %%{_libdir}
- added fix for lib/lib64
* Thu Mar 28 2002 bk@suse.de
- fix man pages, filelist and add check for missing files in packs
* Wed Mar 27 2002 bk@suse.de
- Update to 1.27, fixes resource limit problem for other archs and
  merges many patches
* Thu Mar  7 2002 pthomas@suse.de
- Add patch from Ted T'so to keep e2fsck from dumping
  core when the journal inode is missing.
* Mon Mar  4 2002 pthomas@suse.de
- Fix implicit function declarations and some other gcc warnings.
- Include patch from Kurt Garloff to make e2fsck display the
  device name in addition to the volume label. Adapt it to 1.26.
- Adapt BSD_disklables.diff to new code.
- Set LC_CTYPE in addition to LC_MESSAGES.
- Repack with bzip2.
* Fri Mar  1 2002 bk@suse.de
- Update to 1.26. This release has a number of critical bug
  fixes over the previous release, version 1.25:
* Fri Feb 15 2002 pthomas@suse.de
- Use %%{_lib} and %%{_libdir}.
* Wed Feb 13 2002 pthomas@suse.de
- Make heimdal-devel conflict e2fsprogs-devel.
  Temporary solution for bug #13145
* Thu Dec 13 2001 pthomas@suse.de
- Add mkfs.ext2.8 because mkfs.8 from util-linux references it.
  Fixes bug #12613.
* Fri Nov 23 2001 pthomas@suse.de
- Add accidently left out e2image to file list. Fixes bug
  [#12009]
* Wed Oct 31 2001 ro@suse.de
- fix for axp: should malloc buffer _before_ use
* Wed Oct 10 2001 pthomas@suse.de
- Update to 1.25.
- Remove patches not needed anymore.
- Change mke2fs to output warnings to stderr not stdout.
- Repack as bz2.
* Mon Sep 24 2001 olh@suse.de
- replace ext2fs_d
* Fri Sep 21 2001 pthomas@suse.de
- Add patch for mke2fs from 1.25 as that bug seems to be the
  reason for the mk_initrd warning.
* Wed Sep 12 2001 pthomas@suse.de
- Update to 1.24a:
  - Fix brown-paper bug in mke2fs which caused it to segfault.
  - Revert the BLKGETSIZE64 support as this ioctl has been used
    by an unofficial kernel patch to update the last sector on
    the disk, and this was causing disk corruption problems as a
    result.
  - Mke2fs is now more careful about zapping swap space signatures
    and other filesystem/raid superblock magic values so.
  - E2fsck will no longer complain if the the mode of
    EXT2_RESIZE_INO is a regular file
  - mke2fs and tune2fs will allow the use of UUID= or LABEL=
    specifiers when specifying the external journal device.
    tune2fs will also search devices looking for the external
    journal device when removing.
* Fri Aug 17 2001 ro@suse.de
- update to 1.23 to enable external journals on ext3
* Wed Aug 15 2001 pthomas@suse.de
- Update to 1.22.
- Drop fsck Patch as code changed.
- Use LD_LIBRARY_PATH to run test programs.
* Fri Jun  8 2001 pthomas@suse.de
- Remove incorrect use of AC_REQUIRE (fails with autoconf 2.5)
- Recompress tarball with bzip2.
* Thu Jan 18 2001 schwab@suse.de
- Add Obsoletes: ext2fs_d and Requires: e2fsprogs to devel
  subpackage.
* Mon Nov  6 2000 pthomas@suse.de
- use _mandir and _infodir more consistently in spec file.
* Sun Nov  5 2000 ro@suse.de
- renamed packages to e2fsprogs/e2fsprogs-devel
* Fri Jun  9 2000 kasal@suse.cz
- Build dynamic libraries.  Partition Surprise requires them.
- Make /usr/lib/*.so symlinks relative.
* Thu Mar 23 2000 kukuk@suse.de
- Don't erase BSD labels on Alpha
- Add Y2K fix to debugfs
* Wed Mar 22 2000 kukuk@suse.de
- Fix ifdefs for gcc 2.95.2
* Tue Feb 22 2000 garloff@suse.de
- Bugfix for the change ...
- ... and change version no of fsck to 1.18a to reflect the change.
* Sun Feb 20 2000 garloff@suse.de
- Patch to be more clever WRT to basenames (used to find out wheter
  a fsck for this device is already running).
- Give better message in case fsck fails, to tell the user what to
  do. (e2fsck only displays the label, nowadays :-( )
* Thu Feb 10 2000 kukuk@suse.de
- Use autoconf to create new configure
* Wed Jan 19 2000 ro@suse.de
- man,info -> /usr/share
* Mon Jan 17 2000 ro@suse.de
- fixed to build on kernels >= 2.3.39
* Sat Nov 13 1999 kukuk@suse.de
- Update to e2fsprogs 1.18
- Create new sub-package ext2fs_d which includes libs and headers
* Mon Nov  8 1999 ro@suse.de
-fixed coredump in e2fsck
* Fri Oct 29 1999 ro@suse.de
-e2fsprogs: 1.17 vital bugfix in e2fsck
* Sun Oct 24 1999 ro@suse.de
- e2fsprogs: update to 1.16, sparse_super is default on when
  called on a > 2.2 kernel, can be overridden with -O none
* Fri Oct 15 1999 garloff@suse.de
- Disabled flushb again. (Moved to ddrescue.)
* Mon Sep 13 1999 bs@suse.de
- ran old prepare_spec on spec file to switch to new prepare_spec.
* Wed Sep  1 1999 ro@suse.de
- mke2fs: sparse superblocks default back to "off"
* Tue Aug 31 1999 ro@suse.de
- update to 1.15
- cleanup diff and specfile
* Sun Jul 18 1999 garloff@suse.de
- Enabled flushb compilation
* Sat Jun 26 1999 kukuk@suse.de
- Add fix for fsck core dump from beta list
* Tue Mar 16 1999 ro@suse.de
- fixed configure call
* Fri Mar 12 1999 ro@suse.de
- update to 1.14
* Thu Oct 29 1998 ro@suse.de
- respect change in 2.1.126 SCSI_DISK_MAJOR
* Tue Sep  1 1998 ro@suse.de
- update to 1.12
* Sat Apr 26 1997 florian@suse.de
- update to new version 1.10
* Sun Apr 13 1997 florian@suse.de
- update to new version 1.08
- do not include ext2 libs and include files as they are only used by dump
* Thu Jan  2 1997 florian@suse.de
- update to new version 1.06
* Thu Jan  2 1997 florian@suse.de
- update to newer version
- use now static libs instead of 4 small shared libs
* Thu Jan  2 1997 florian@suse.de
  update to version 1.0.4
