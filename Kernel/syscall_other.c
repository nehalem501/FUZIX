#include <kernel.h>
#include <version.h>
#include <kdata.h>
#include <printf.h>

/*
 *	More obscure syscalls that it might be useful to pull out of the main
 *	flow in case we ever need to bank any of them. Avoid putting anything
 *	that may sleep in here. We want this to potentially be pageable syscall
 *	helper. Right now its about 3.5K of material
 */


/*******************************************
rename (src, dst)                Function 59
char *src;
char *dst;

Rename a file within a filesystem, can be
a directory. Don't allow an object to be
put inside itself!
********************************************/

#define src (char *)udata.u_argn
#define dst (char *)udata.u_argn1

arg_t _rename(void)
{
#ifndef CONFIG_LEVEL_0
	staticfast inoptr srci, srcp, dsti, dstp;
	char fname[FILENAME_LEN + 1];
	arg_t ret;

	srci = n_open(src, &srcp);
	/* Source must exist */
	if (!srci) {
		if (srcp)
			i_deref(srcp);
		return -1;
	}
	/* n_open will wipe u_rename if it walks that inode
	   so it tells us whether we are trying to create a loop */
	udata.u_rename = srci;
	/* Destination maybe does not exist, but parent must */
	filename(dst, fname);

	dsti = n_open(dst, &dstp);
	/* Same file - do nothing */
	if (dsti == srci) {
		ret = 0;
		goto nogood3;
	}

	ret = -1;

	/* Destination not found, but neither is the directory to
	   put the new node in -> so fail */
	if (dstp == NULLINODE)
		goto nogood3;
	/* Can't rename between devices */
	if (srci->c_dev != dstp->c_dev) {
		udata.u_error = EXDEV;
		goto nogood;
	}
	/* Can't rename a directory yet.. need to fix this but to do so we
	   must update .. in the child. Probably a LEVEL 2 functionality ? */
	if (getmode(srci) == MODE_R(F_DIR)) {
		udata.u_error = EISDIR;
		goto nogood;
	}
	/* Can't rename one dir into another */
	if (udata.u_rename == NULL) {
		/* dst is within src */
		udata.u_error = EINVAL;
		goto nogood;
	}
	/* Check we can write the src dir, we don't want to fail on
	   ch_link */
	if (!(getperm(srcp) & OTH_WR) && esuper()) {
		udata.u_error = EACCES;
		goto nogood;
	}
	/* Destination exists ? If so we must remove it if possible */
	if (dsti) {
		/* Can't overwrite a directory */
		if (getmode(dsti) == MODE_R(F_DIR)) {
			udata.u_error = EISDIR;
			goto nogood;
		}
		if (unlinki(dsti, dstp, fname) == -1)
			goto nogood;
		/* Drop the reference to the unlinked file */
		i_deref(dsti);
	}
	/* Ok we may proceed: we set up fname earlier */
	if (!ch_link(dstp, "", fname, srci))
		goto nogood2;
	filename(src, fname);
	/* A fail here is bad */
	if (!ch_link(srcp, fname, "", NULLINODE)) {
		kputs("WARNING: rename: unlink fail\n");
		goto nogood2;
	}
	/* get it onto disk - probably overkill */
	wr_inode(dstp);
	wr_inode(srcp);
	sync();
	ret = 0;
      nogood2:
	i_deref(dstp);
      nogood3:
	i_deref(srci);
	i_deref(srcp);
	return ret;
      nogood:
	if (dsti)
		i_deref(dsti);
	goto nogood2;
#else
	udata.u_error = -ENOSYS;
	return -1;
#endif		
}

#undef src
#undef dst

/*******************************************
  mkdir (path, mode)	        Function 51
  char *path;
  int16_t mode;
 ********************************************/

#define name (char *)udata.u_argn
#define mode (int16_t)udata.u_argn1

arg_t _mkdir(void)
{
#ifndef CONFIG_LEVEL_0
	inoptr ino;
	inoptr parent;
	char fname[FILENAME_LEN + 1];

	if ((ino = n_open(name, &parent)) != NULL) {
		udata.u_error = EEXIST;
		goto nogood;
	}

	if (!parent) {
		udata.u_error = ENOENT;
		return (-1);
	}

	if (parent->c_node.i_nlink == 0xFFFF) {
		udata.u_error = EMLINK;
		goto nogood2;
	}

	filename(name, fname);

	i_ref(parent);		/* We need it again in a minute */
	if (!(ino = newfile(parent, fname))) {
		i_deref(parent);
		goto nogood2;	/* parent inode is derefed in newfile. */
	}

	/* Initialize mode and dev */
	ino->c_node.i_mode = F_DIR | 0200;	/* so ch_link is allowed */
	setftime(ino, A_TIME | M_TIME | C_TIME);
	if (ch_link(ino, "", ".", ino) == 0 ||
	    ch_link(ino, "", "..", parent) == 0)
		goto cleanup;

	/* Link counts and permissions */
	ino->c_node.i_nlink = 2;
	parent->c_node.i_nlink++;
	ino->c_node.i_mode = ((mode & ~udata.u_mask) & MODE_MASK) | F_DIR;
	i_deref(parent);
	wr_inode(ino);
	i_deref(ino);
	return (0);

      cleanup:
	if (!ch_link(parent, fname, "", NULLINODE))
		kprintf("_mkdir: bad rec\n");
	/* i_deref will put the blocks */
	ino->c_node.i_nlink = 0;
	wr_inode(ino);
      nogood:
	i_deref(ino);
      nogood2:
	i_deref(parent);
	return (-1);
#else
	udata.u_error = -ENOSYS;
	return -1;
#endif		
}

#undef name
#undef mode

/*******************************************
rmdir (path)                     Function 52
char *path;
********************************************/
#define path (char *)udata.u_argn

arg_t _rmdir(void)
{
#ifndef CONFIG_LEVEL_0
	inoptr ino;
	inoptr parent;
	char fname[FILENAME_LEN + 1];

	ino = n_open(path, &parent);

	/* It and its parent must exist */
	if (!(parent && ino)) {
		if (parent)	/* parent exist */
			i_deref(parent);
		udata.u_error = ENOENT;
		return (-1);
	}

	/* Fixme: check for rmdir of /. - ditto for unlink ? */

	/* Not a directory */
	if (getmode(ino) != MODE_R(F_DIR)) {
		udata.u_error = ENOTDIR;
		goto nogood;
	}

	/* Busy */
	if (ino->c_node.i_nlink != 2) {
		udata.u_error = ENOTEMPTY;
		goto nogood;
	}

	/* Parent must be writable */
	if (!(getperm(parent) & OTH_WR))
		goto nogood;

	/* Remove the directory entry */
	filename(path, fname);
	if (!ch_link(parent, fname, "", NULLINODE))
		goto nogood;

	/* We are unused, parent is now one link down (removal of ..) */
	ino->c_node.i_nlink = 0;

	/* Decrease the link count of the parent inode */
	if (!(parent->c_node.i_nlink--)) {
		parent->c_node.i_nlink += 2;
		kprintf("_rmdir: bad nlink\n");
	}
	setftime(ino, C_TIME);
	wr_inode(parent);
	wr_inode(ino);
	i_deref(parent);
	i_deref(ino);
	return (0);

      nogood:
	i_deref(parent);
	i_deref(ino);
	return (-1);
#else
	udata.u_error = -ENOSYS;
	return -1;
#endif		

}

#undef path


/*******************************************
  mount (spec, dir, flags)       Function 33
  char *spec;
  char *dir;
  int  flags;
 ********************************************/
#define spec (char *)udata.u_argn
#define dir (char *)udata.u_argn1
#define flags (int)udata.u_argn2

arg_t _mount(void)
{
	inoptr sino, dino;
	uint16_t dev;

	if (esuper()) {
		return (-1);
	}

	if (!(sino = n_open(spec, NULLINOPTR)))
		return (-1);

	if (!(dino = n_open(dir, NULLINOPTR))) {
		i_deref(sino);
		return (-1);
	}

	if (getmode(sino) != MODE_R(F_BDEV)) {
		udata.u_error = ENOTBLK;
		goto nogood;
	}

	if (getmode(dino) != MODE_R(F_DIR)) {
		udata.u_error = ENOTDIR;
		goto nogood;
	}

	dev = (int) sino->c_node.i_addr[0];

	if (!validdev(dev) || d_open(dev, (flags & MS_RDONLY) ? O_RDONLY : O_RDWR)) {
		udata.u_error = ENXIO;
		goto nogood;
	}

	if (fs_tab_get(dev) || dino->c_refs != 1 || dino->c_num == ROOTINODE) {
		udata.u_error = EBUSY;
		goto nogood;
	}

	sync();

	if (fmount(dev, dino, flags)) {
		udata.u_error = EBUSY;
		goto nogood;
	}

	i_deref(dino);
	i_deref(sino);
	return (0);

      nogood:
	i_deref(dino);
	i_deref(sino);
	return (-1);
}

#undef spec
#undef dir
#undef flags


/*******************************************
  _umount (spec)                   Function 34
  char *spec;
 ********************************************/

#define spec (char *)udata.u_argn
#define flags (uint16_t)udata.u_argn1

static int do_umount(uint16_t dev)
{
	regptr struct mount *mnt;
	uint8_t rm = flags & MS_REMOUNT;
	regptr inoptr ptr;

	mnt = fs_tab_get(dev);
	if (mnt == NULL) {
		udata.u_error = EINVAL;
		return -1;
	}

	/* If anything on this file system is open for write then you
	   can't remount it read only */
	if (flags & (MS_RDONLY|MS_REMOUNT) == (MS_RDONLY|MS_REMOUNT)) {
		for (ptr = i_tab ; ptr < i_tab + ITABSIZE; ++ptr) {
			if (ptr->c_dev == dev && !isdevice(ptr)) {
			/* Files being written block the remount ro, but so
			   do files that when closed will be deleted */
				if (ptr->c_writers ||
					!ptr->c_node.i_nlink) {
					udata.u_error = EBUSY;
					return -1;
				}
			}
		}
	}

	/* Sweep the inode table. If unmounting look for any references
	   and if so fail. If remounting update the CRDONLY flags */
	for (ptr = i_tab; ptr < i_tab + ITABSIZE; ++ptr) {
		if (ptr->c_dev == dev) {
			if (rm) {
				ptr->c_flags &= ~CRDONLY;
				if (flags & MS_RDONLY)
					ptr->c_flags |= CRDONLY;
				else
					ptr->c_flags &= ~CRDONLY;
			} else if (ptr->c_refs) {
				udata.u_error = EBUSY;
				return -1;
			}
		}
	}

	if (!rm)
		mnt->m_fs.s_fmod = FMOD_GO_CLEAN;

	sync();

	if (rm) {
		mnt->m_flags &= ~(MS_RDONLY|MS_NOSUID);
		mnt->m_flags |= flags & (MS_RDONLY|MS_NOSUID);
		/* You can choose to remount a corrupt fs r/o in which case
		   it gets marked clean. We may want to rethink that FIXME */
		if (mnt->m_flags & MS_RDONLY)
			mnt->m_fs.s_fmod = FMOD_GO_CLEAN;
		return 0;
	}

	i_deref(mnt->m_fs.s_mntpt);
	/* Vanish the entry */
	mnt->m_dev = NO_DEVICE;
	return 0;
}

arg_t _umount(void)
{
	inoptr sino;
	uint16_t dev;
	arg_t ret = -1;

	if (esuper())
		return -1;

	if (!(sino = n_open(spec, NULLINOPTR)))
		return -1;

	if (getmode(sino) != MODE_R(F_BDEV)) {
		udata.u_error = ENOTBLK;
		goto nogood;
	}

	dev = (int) sino->c_node.i_addr[0];
	if (!validdev(dev)) {
		udata.u_error = ENXIO;
		goto nogood;
	}
	ret = do_umount(dev);
nogood:
	i_deref(sino);
	return ret;
}

#undef spec
#undef flags


/*******************************************
profil (samples, size, offset, scale) Function 56
char *samples;
usize_t offset;
usize_t size;
uint16_t scale;
********************************************/

#define samples (char *)udata.u_argn
#define offset 	(usize_t)udata.u_argn1
#define size 	(usize_t)udata.u_argn2
#define scale	(uint16_t)udata.u_argn3

arg_t _profil(void)
{
#ifdef CONFIG_PROFIL
	/* For performance reasons scale as
	   passed to the kernel is a shift value
	   not a divider */
	regptr ptptr p = udata.u_ptab;

	if (scale == 0) {
		p->p_profscale = scale;
		return 0;
	}

	if (!valaddr(samples, size >> (scale - 1)))
		return -1;

	p->p_profscale = scale;
	p->p_profbuf = samples;
	p->p_profsize = size;
	p->p_profoff = offset;
	return 0;
#else
	udata.u_error = ENOSYS;
	return -1;
#endif
}

#undef samples
#undef offset
#undef size
#undef scale

/*******************************************
uadmin(cmd, func, ptr)           Function 57
int16_t cmd;
int16_t func;
char *ptr;
********************************************/
#define cmd (int16_t)udata.u_argn
#define func (int16_t)udata.u_argn1
#define ptr  (char *)udata.u_argn2

arg_t _uadmin(void)
{
	if (esuper())
		return -1;
	if (func != AD_NOSYNC)
		sync();
	/* Wants moving into machine specific files */
	if (cmd == A_SHUTDOWN || cmd == A_DUMP)
		trap_monitor();
	if (cmd == A_REBOOT)
		trap_reboot();

	/* We don't do SWAPCTL yet */
	udata.u_error = EINVAL;
	return -1;
}


/*******************************************
nice (pri)                     Function 58
int16_t pri;
********************************************/
#define pri (int16_t)udata.u_argn

arg_t _nice(void)
{
	regptr ptptr p = udata.u_ptab;
	int16_t np;

	if (pri < 0 && !esuper())
		return -1;
	np = p->p_nice + pri;
	if (np < -20)
		np = -20;
	if (np > 19)
		np = 19;

	p->p_nice = np;
	p->p_priority = MAXTICKS - (np >> 2);
	return 20 + np;		/* avoid errno confusion */
}

#undef pri
