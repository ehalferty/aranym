/*
 * The ARAnyM MetaDOS driver.
 *
 * 2002 STan
 *
 * Based on:
 * dosdir.c,v 1.11 2001/10/23 09:09:14 fna Exp
 *
 * This file has been modified as part of the FreeMiNT project. See
 * the file Changes.MH for details and dates.
 *
 *
 * Copyright 1990,1991,1992 Eric R. Smith.
 * Copyright 1992,1993,1994 Atari Corporation.
 * All rights reserved.
 *
 */

/* DOS directory functions */

#include "hostfs.h"
#include "mint/errno.h"
#include "mint/ctype.h"
#include "mint/assert.h"
#include "mint/credentials.h"
#include "mint/pathconf.h"
#include "mint/string.h"


long _cdecl
sys_d_free (MetaDOSDir long *buf, int d)
{
	struct proc *p = get_curproc();
	fcookie *dir;

	d = tolower(pathNameMD[0]);
	if (d >= '1' && d <= '6')
		d = d - '1' + 26;
	else
		d = d - 'a';
	if (d < 0 || d >= NUM_DRIVES)
		return ENXIO;
	dir = &p->p_cwd->root[d];
	TRACE(("Dfree(%d)", d));

	if (!dir->fs)
	{
		DEBUG(("Dfree: bad drive"));
		return ENXIO;
	}

	return xfs_dfree (dir->fs, dir, buf);
}

long _cdecl
sys_d_create (MetaDOSDir const char *path)
{
	struct proc *p = get_curproc();

	fcookie dir;
	long r;
	char temp1[PATH_MAX];
	XATTR xattr;
	ushort mode;

	TRACE(("Dcreate(%s)", path));
	assert (p->p_fd && p->p_cwd);

	r = path2cookie(p, path, temp1, &dir);
	if (r)
	{
		DEBUG(("Dcreate(%s): returning %ld", path, r));
		return r;
	}

	if (temp1[0] == '\0')
	{
		DEBUG(("Dcreate(%s): creating a NULL dir?", path));
		release_cookie(&dir);
		return EBADARG;
	}

	/* check for write permission on the directory */
	r = dir_access(p->p_cred->ucr, &dir, S_IWOTH, &mode);
	if (r)
	{
		DEBUG(("Dcreate(%s): write access to directory denied",path));
		release_cookie(&dir);
		return r;
	}

	if (mode & S_ISGID)
	{
		r = xfs_getxattr (dir.fs, &dir, &xattr);
		if (r)
		{
			DEBUG(("Dcreate(%s): file system returned %ld", path, r));
		}
		else
		{
			ushort cur_gid = p->p_cred->rgid;
			ushort cur_egid = p->p_cred->ucr->egid;
			p->p_cred->rgid = p->p_cred->ucr->egid = xattr.gid;
			r = xfs_mkdir (dir.fs, &dir, temp1,
					     (DEFAULT_DIRMODE & ~p->p_cwd->cmask)
					     | S_ISGID);
			p->p_cred->rgid = cur_gid;
			p->p_cred->ucr->egid = cur_egid;
		}
	}
	else
		r = xfs_mkdir (dir.fs, &dir, temp1,
				     DEFAULT_DIRMODE & ~p->p_cwd->cmask);
	release_cookie(&dir);
	return r;
}

long _cdecl
sys_d_delete (MetaDOSDir const char *path)
{
	struct proc *cp = get_curproc();
	struct ucred *cred = cp->p_cred->ucr;

	fcookie parentdir, targdir;
	long r;
	XATTR xattr;
	char temp1[PATH_MAX];
	ushort mode;


	TRACE(("Ddelete(%s)", path));

	r = path2cookie (cp, path, temp1, &parentdir);
	if (r)
	{
		DEBUG(("Ddelete(%s): error %lx", path, r));
		release_cookie(&parentdir);
		return r;
	}

	/* check for write permission on the directory which the target
	 * is located
	 */
	r = dir_access (cred, &parentdir, S_IWOTH, &mode);
	if (r)
	{
		DEBUG(("Ddelete(%s): access to directory denied", path));
		release_cookie (&parentdir);
		return r;
	}

	/* now get the info on the file itself */
	r = relpath2cookie (cp, &parentdir, temp1, NULL, &targdir, 0);
	if (r)
	{
bailout:
		release_cookie (&parentdir);
		DEBUG(("Ddelete: error %ld on %s", r, path));
		return r;
	}

	r = xfs_getxattr (targdir.fs, &targdir, &xattr);
	if (r)
	{
		release_cookie (&targdir);
		goto bailout;
	}

	/* check effective uid if sticky bit is set in parent */
	if ((mode & S_ISVTX) && cred->euid
	    && cred->euid != xattr.uid)
	{
		release_cookie (&targdir);
		release_cookie (&parentdir);
		DEBUG(("Ddelete: sticky bit set and not owner"));
		return EACCES;
	}

	/* if the "directory" is a symbolic link, really unlink it */
	if (S_ISLNK(xattr.mode))
	{
		release_cookie (&targdir);
		r = xfs_remove (parentdir.fs, &parentdir, temp1);
	}
	else if (!S_ISDIR(xattr.mode))
	{
		DEBUG(("Ddelete: %s is not a directory", path));
		r = ENOTDIR;
	}
	else if (strlen (temp1) == 1 && temp1[0] == '.')
	{
		DEBUG(("Ddelete: %s is not allowed to be deleted", path));
		r = EINVAL;
	}
	else
	{
		release_cookie (&targdir);
		r = xfs_rmdir (parentdir.fs, &parentdir, temp1);
	}

	release_cookie (&parentdir);
	return r;
}


/*
 * Fsfirst/next are actually implemented in terms of opendir/readdir/closedir.
 */
long _cdecl
sys_f_sfirst (MetaDOSDTA const char *path, int attrib)
{
	struct proc *p = get_curproc();

	char *s, *slash;
	FILESYS *fs;
	fcookie dir, newdir;
	DTABUF *dta;
	DIR *dirh;
	XATTR xattr;
	long r;
	int i, havelabel;
	char temp1[PATH_MAX];
	ushort mode;


	TRACE(("Fsfirst(%s, %x)", path, attrib));

	r = path2cookie (p, path, temp1, &dir);
	if (r)
	{
		DEBUG(("Fsfirst(%s): path2cookie returned %ld", path, r));
		return r;
	}

	/* we need to split the last name (which may be a pattern) off from
	 * the rest of the path, even if FS_KNOPARSE is true
	 */
	slash = 0;
	s = temp1;
	while (*s)
	{
		if (*s == '\\' || *s == '/')
			slash = s;
		s++;
	}

	if (slash)
	{
		*slash++ = 0;	/* slash now points to a name or pattern */
		r = relpath2cookie (p, &dir, temp1, follow_links, &newdir, 0);
		release_cookie (&dir);
		if (r)
		{
			DEBUG(("Fsfirst(%s): lookup returned %ld", path, r));
			return r;
		}
		dir = newdir;
	}
	else
		slash = temp1;

	/* BUG? what if there really is an empty file name?
	 */
	if (!*slash)
	{
		DEBUG(("Fsfirst: empty pattern"));
		return ENOENT;
	}

	fs = dir.fs;
	dta = dtaMD;

	/* Now, see if we can find a DIR slot for the search. We use the
	 * following heuristics to try to avoid destroying a slot:
	 * (1) if the search doesn't use wildcards, don't bother with a slot
	 * (2) if an existing slot was for the same DTA address, re-use it
	 * (3) if there's a free slot, re-use it. Slots are freed when the
	 *     corresponding search is terminated.
	 */

	for (i = 0; i < NUM_SEARCH; i++)
	{
		if (p->p_fd->srchdta[i] == dta)
		{
			dirh = &p->p_fd->srchdir[i];
			if (dirh->fc.fs)
			{
				xfs_closedir (dirh->fc.fs, dirh);
				release_cookie(&dirh->fc);
				dirh->fc.fs = 0;
			}
			p->p_fd->srchdta[i] = 0; /* slot is now free */
		}
	}

	/* copy the pattern over into dta_pat into TOS 8.3 form
	 * remember that "slash" now points at the pattern
	 * (it follows the last, if any)
	 */
	copy8_3 (dta->dta_pat, slash);

	/* if (attrib & FA_LABEL), read the volume label
	 *
	 * BUG: the label date and time are wrong. ISO/IEC 9293 14.3.3 allows this.
	 * The Desktop set also date and time to 0 when formatting a floppy disk.
	 */
	havelabel = 0;
	if (attrib & FA_LABEL)
	{
		r = xfs_readlabel (fs, &dir, dta->dta_name, TOS_NAMELEN+1);
		dta->dta_attrib = FA_LABEL;
		dta->dta_time = dta->dta_date = 0;
		dta->dta_size = 0;
		dta->magic = EVALID;
		if (r == E_OK && !pat_match (dta->dta_name, dta->dta_pat))
			r = ENOENT;
		if ((attrib & (FA_DIR|FA_LABEL)) == FA_LABEL)
			return r;
		else if (r == E_OK)
			havelabel = 1;
	}

	DEBUG(("Fsfirst(): havelabel = %d",havelabel));

	if (!havelabel && has_wild (slash) == 0)
	{
		/* no wild cards in pattern */
		r = relpath2cookie (p, &dir, slash, follow_links, &newdir, 0);
		if (r == E_OK)
		{
			r = xfs_getxattr (newdir.fs, &newdir, &xattr);
			release_cookie (&newdir);
		}
		release_cookie (&dir);
		if (r)
		{
			DEBUG(("Fsfirst(%s): couldn't get file attributes",path));
			return r;
		}

		dta->magic = EVALID;
		dta->dta_attrib = xattr.attr;
		dta->dta_size = xattr.size;

		if (fs->fsflags & FS_EXT_3)
		{
			dta_UTC_local_dos(dta,xattr,m);
		}
		else
		{
			dta->dta_time = xattr.mtime;
			dta->dta_date = xattr.mdate;
		}

		strncpy (dta->dta_name, slash, TOS_NAMELEN-1);
		dta->dta_name[TOS_NAMELEN-1] = 0;
		if (p->domain == DOM_TOS && !(fs->fsflags & FS_CASESENSITIVE))
			strupr (dta->dta_name);

		return E_OK;
	}

	/* There is a wild card. Try to find a slot for an opendir/readdir
	 * search. NOTE: we also come here if we were asked to search for
	 * volume labels and found one.
	 */
	for (i = 0; i < NUM_SEARCH; i++)
	{
		if (p->p_fd->srchdta[i] == 0)
			break;
	}

	if (i == NUM_SEARCH)
	{
		int oldest = 0;
		long oldtime = p->p_fd->srchtim[0];

		DEBUG(("Fsfirst(%s): having to re-use a directory slot!", path));
		for (i = 1; i < NUM_SEARCH; i++)
		{
			if (p->p_fd->srchtim[i] < oldtime)
			{
				oldest = i;
				oldtime = p->p_fd->srchtim[i];
			}
		}

		/* OK, close this directory for re-use */
		i = oldest;
		dirh = &p->p_fd->srchdir[i];
		if (dirh->fc.fs)
		{
			xfs_closedir (dirh->fc.fs, dirh);
			release_cookie(&dirh->fc);
			dirh->fc.fs = 0;
		}

		/* invalidate re-used DTA */
		p->p_fd->srchdta[i]->magic = EVALID;
		p->p_fd->srchdta[i] = 0;
	}

	/* check to see if we have read permission on the directory (and make
	 * sure that it really is a directory!)
	 */
	r = dir_access(p->p_cred->ucr, &dir, S_IROTH, &mode);
	if (r)
	{
		DEBUG(("Fsfirst(%s): access to directory denied (error code %ld)", path, r));
		release_cookie(&dir);
		return r;
	}

	/* set up the directory for a search */
	dirh = &p->p_fd->srchdir[i];
	dirh->fc = dir;
	dirh->index = 0;
	dirh->flags = TOS_SEARCH;

	r = xfs_opendir (dir.fs, dirh, dirh->flags);
	if (r != E_OK)
	{
		DEBUG(("Fsfirst(%s): couldn't open directory (error %ld)", path, r));
		release_cookie(&dir);
		return r;
	}

	/* mark the slot as in-use */
	p->p_fd->srchdta[i] = dta;

	/* set up the DTA for Fsnext */
	dta->index = i;
	dta->magic = SVALID;
	dta->dta_sattrib = attrib;

	/* OK, now basically just do Fsnext, except that instead of ENMFILES we
	 * return ENOENT.
	 * NOTE: If we already have found a volume label from the search above,
	 * then we skip the f_snext and just return that.
	 */
	if (havelabel)
		return E_OK;

	r = sys_f_snext(MetaDOSDTA0pass);
	if (r == ENMFILES) r = ENOENT;
	if (r)
		TRACE(("Fsfirst: returning %ld", r));

	/* release_cookie isn't necessary, since &dir is now stored in the
	 * DIRH structure and will be released when the search is completed
	 */
	return r;
}

/*
 * Counter for Fsfirst/Fsnext, so that we know which search slots are
 * least recently used. This is updated once per second by the code
 * in timeout.c.
 * BUG: 1/second is pretty low granularity
 */

long searchtime;

long _cdecl
sys_f_snext (MetaDOSDTA0)
{
	struct proc *p = get_curproc();

	char buf[TOS_NAMELEN+1];
	DTABUF *dta = dtaMD;
	FILESYS *fs;
	fcookie fc;
	ushort i;
	DIR *dirh;
	long r;
	XATTR xattr;

#if 0

	if (1) {
		char *buf[TOS_NAMELEN+1];

		static int count = 0;
		static char *test = "HOSTFS";
		while( count++ < 100 ) {
			dta->dta_attrib = 0x20;
			dta->dta_size = 100;
			dta->dta_time = 0;
			dta->dta_date = 0;
			strcpy(dta->dta_name, test);
			DEBUG (("Fsnext(%s.%ld): ", dta->dta_name, sizeof(struct dtabuf)));
			return E_OK;
		}
		return ENMFILES;
	}

#endif

	TRACE (("Fsnext"));

	if (dta->magic == EVALID)
	{
		DEBUG (("Fsnext(%p): DTA marked a failing search", dta));
		return ENMFILES;
	}

	if (dta->magic != SVALID)
	{
		DEBUG (("Fsnext(%p): dta incorrectly set up", dta));
		return ENOSYS;
	}

	i = dta->index;
	if (i >= NUM_SEARCH)
	{
		DEBUG (("Fsnext(%p): DTA has invalid index", dta));
		return EBADARG;
	}

	dirh = &p->p_fd->srchdir[i];
	p->p_fd->srchtim[i] = searchtime++;

	fs = dirh->fc.fs;
	if (!fs)
	{
		/* oops -- the directory got closed somehow */
		DEBUG (("Fsnext(%p): invalid filesystem", dta));
		return EINTERNAL;
	}

	/* BUG: f_snext and readdir should check for disk media changes
	 */
	for(;;)
	{
		r = xfs_readdir (fs, dirh, buf, TOS_NAMELEN+1, &fc);

		if (r == EBADARG)
		{
			DEBUG(("Fsnext: name too long"));
			continue;	/* TOS programs never see these names */
		}

		if (r != E_OK)
		{
baderror:
			if (dirh->fc.fs)
				(void) xfs_closedir (fs, dirh);
			release_cookie(&dirh->fc);
			dirh->fc.fs = 0;
			p->p_fd->srchdta[i] = 0;
			dta->magic = EVALID;
			if (r != ENMFILES)
				DEBUG(("Fsnext: returning %ld", r));
			return r;
		}

		if (!pat_match (buf, dta->dta_pat))
		{
			release_cookie (&fc);
			continue;	/* different patterns */
		}

		/* check for search attributes */
		r = xfs_getxattr (fc.fs, &fc, &xattr);
		if (r)
		{
			DEBUG(("Fsnext: couldn't get file attributes"));
			release_cookie (&fc);
			goto baderror;
		}

		/* if the file is a symbolic link, try to find what it's linked to */
		if (S_ISLNK(xattr.mode))
		{
			char linkedto[PATH_MAX];
			r = xfs_readlink (fc.fs, &fc, linkedto, PATH_MAX);
			release_cookie (&fc);
			if (r == E_OK)
			{
				/* the "1" tells relpath2cookie that we read a link */
				r = relpath2cookie (p, &dirh->fc, linkedto,
					follow_links, &fc, 1);
				if (r == E_OK)
				{
					r = xfs_getxattr (fc.fs, &fc, &xattr);
					release_cookie (&fc);
				}
			}
			if (r)
				DEBUG(("Fsnext: couldn't follow link: error %ld", r));
		}
		else
			release_cookie (&fc);

		/* silly TOS rules for matching attributes */
		if (xattr.attr == 0)
			break;

		if (xattr.attr & (FA_CHANGED|FA_RDONLY))
			break;

		if (dta->dta_sattrib & xattr.attr)
			break;
	}

	/* here, we have a match
	 */

	if (fs->fsflags & FS_EXT_3)
	{
		dta_UTC_local_dos(dta,xattr,m);
	}
	else
	{
		dta->dta_time = xattr.mtime;
		dta->dta_date = xattr.mdate;
	}

	dta->dta_attrib = xattr.attr;
	dta->dta_size = xattr.size;
	strcpy (dta->dta_name, buf);

	if (p->domain == DOM_TOS && !(fs->fsflags & FS_CASESENSITIVE))
		strupr (dta->dta_name);

	return E_OK;
}

long _cdecl
sys_f_attrib (MetaDOSFile const char *name, int rwflag, int attr)
{
	struct proc *p = get_curproc();
	struct ucred *cred = p->p_cred->ucr;

	fcookie fc;
	XATTR xattr;
	long r;


	DEBUG(("Fattrib(%s, %d)", name, attr));

	r = path2cookie (p, name, follow_links, &fc);
	if (r)
	{
		DEBUG(("Fattrib(%s): error %ld", name, r));
		return r;
	}

	r = xfs_getxattr (fc.fs, &fc, &xattr);
	if (r)
	{
		DEBUG(("Fattrib(%s): getxattr returned %ld", name, r));
		release_cookie (&fc);
		return r;
	}

	if (rwflag)
	{
		if (attr & ~(FA_CHANGED|FA_DIR|FA_SYSTEM|FA_HIDDEN|FA_RDONLY)
		    || (attr & FA_DIR) != (xattr.attr & FA_DIR))
		{
			DEBUG(("Fattrib(%s): illegal attributes specified",name));
			r = EACCES;
		}
		else if (cred->euid && cred->euid != xattr.uid)
		{
			DEBUG(("Fattrib(%s): not the file's owner",name));
			r = EACCES;
		}
		else if (xattr.attr & FA_LABEL)
		{
			DEBUG(("Fattrib(%s): file is a volume label", name));
			r = EACCES;
		}
		else
			r = xfs_chattr (fc.fs, &fc, attr);

		release_cookie (&fc);
		return r;
	}
	else
	{
		release_cookie (&fc);
		return xattr.attr;
	}
}

long _cdecl
sys_f_delete (MetaDOSDir const char *name)
{
	struct proc *p = get_curproc();
	struct ucred *cred = p->p_cred->ucr;

	fcookie dir, fc;
	XATTR xattr;
	long r;
	char temp1[PATH_MAX];
	ushort mode;


	TRACE(("Fdelete(%s)", name));

	/* get a cookie for the directory the file is in */
	r = path2cookie (p, name, temp1, &dir);
	if (r)
	{
		DEBUG(("Fdelete: couldn't get directory cookie: error %ld", r));
		return r;
	}

	/* check for write permission on directory */
	r = dir_access (cred, &dir, S_IWOTH, &mode);
	if (r)
	{
		DEBUG(("Fdelete(%s): write access to directory denied",name));
		release_cookie (&dir);
		return EACCES;
	}

	/* now get the file attributes */
	r = xfs_lookup (dir.fs, &dir, temp1, &fc);
	if (r)
	{
		DEBUG(("Fdelete: error %ld while looking for %s", r, temp1));
		release_cookie (&dir);
		return r;
	}

	r = xfs_getxattr (fc.fs, &fc, &xattr);
	if (r < E_OK)
	{
		release_cookie (&dir);
		release_cookie (&fc);

		DEBUG(("Fdelete: couldn't get file attributes: error %ld", r));
		return r;
	}

	/* do not allow directories to be deleted */
	if (S_ISDIR(xattr.mode))
	{
		release_cookie (&dir);
		release_cookie (&fc);

		DEBUG(("Fdelete: %s is a directory", name));
		return EISDIR;
	}

	/* check effective uid if directories sticky bit is set */
	if ((mode & S_ISVTX) && cred->euid
		&& cred->euid != xattr.uid)
	{
		release_cookie (&dir);
		release_cookie (&fc);

		DEBUG(("Fdelete: sticky bit set and not owner"));
		return EACCES;
	}

#if 0
	/* TOS domain processes can only delete files if they have write permission
	 * for them
	 */
	if (p->domain == DOM_TOS)
	{
		/* see if we're allowed to kill it */
		if (denyaccess (cred, &xattr, S_IWOTH))
		{
			release_cookie (&dir);
			release_cookie (&fc);

			DEBUG(("Fdelete: file access denied"));
			return EACCES;
		}
	}
#endif

	release_cookie (&fc);
	r = xfs_remove (dir.fs, &dir,temp1);
	release_cookie (&dir);

	return r;
}

long _cdecl
sys_f_rename (MetaDOSDir int junk, const char *old, const char *new)
{
	struct proc *p = get_curproc();
	struct ucred *cred = p->p_cred->ucr;

	fcookie olddir, newdir, oldfil;
	XATTR xattr;
	char temp1[PATH_MAX], temp2[PATH_MAX];
	long r;
	ushort mode;

	/* ignored, for TOS compatibility */
	UNUSED(junk);


	TRACE(("Frename(%s, %s)", old, new));

	r = path2cookie (p, old, temp2, &olddir);
	if (r)
	{
		DEBUG(("Frename(%s,%s): error parsing old name",old,new));
		return r;
	}

	/* check for permissions on the old file
	 * GEMDOS doesn't allow rename if the file is FA_RDONLY
	 * we enforce this restriction only on regular files; processes,
	 * directories, and character special files can be renamed at will
	 */
	r = relpath2cookie (p, &olddir, temp2, (char *)0, &oldfil, 0);
	if (r)
	{
		DEBUG(("Frename(%s,%s): old file not found",old,new));
		release_cookie (&olddir);
		return r;
	}

	r = xfs_getxattr (oldfil.fs, &oldfil, &xattr);
	release_cookie (&oldfil);
	if (r || (S_ISREG(xattr.mode)
			&& ((xattr.attr & FA_RDONLY)
				&& cred->euid
				&& (cred->euid != xattr.uid))))
	{
		/* Only SuperUser and the owner of the file are allowed to rename
		 * readonly files
		 */
		DEBUG(("Frename(%s,%s): access to old file not granted",old,new));
		release_cookie (&olddir);
		return EACCES;
	}

	r = path2cookie(p, new, temp1, &newdir);
	if (r)
	{
		DEBUG(("Frename(%s,%s): error parsing new name",old,new));
		release_cookie (&olddir);
		return r;
	}

	if (newdir.fs != olddir.fs)
	{
		DEBUG(("Frename(%s,%s): different file systems",old,new));
		release_cookie (&olddir);
		release_cookie (&newdir);

		/* cross device rename */
		return EXDEV;
	}

	/* check for write permission on both directories */
	r = dir_access (cred, &olddir, S_IWOTH, &mode);
	if (!r && (mode & S_ISVTX) && cred->euid
	    && cred->euid != xattr.uid)
		r = EACCES;

	if (!r) r = dir_access (cred, &newdir, S_IWOTH, &mode);

	if (r)
		DEBUG(("Frename(%s,%s): access to a directory denied",old,new));
	else
		r = xfs_rename (newdir.fs, &olddir, temp2, &newdir, temp1);

	release_cookie (&olddir);
	release_cookie (&newdir);

	return r;
}

/*
 * GEMDOS extension: Dpathconf(name, which)
 *
 * returns information about filesystem-imposed limits; "name" is the name
 * of a file or directory about which the limit information is requested;
 * "which" is the limit requested, as follows:
 *	-1	max. value of "which" allowed
 *	0	internal limit on open files, if any
 *	1	max. number of links to a file	{LINK_MAX}
 *	2	max. path name length		{PATH_MAX}
 *	3	max. file name length		{NAME_MAX}
 *	4	no. of bytes in atomic write to FIFO {PIPE_BUF}
 *	5	file name truncation rules
 *	6	file name case translation rules
 *
 * unlimited values are returned as 0x7fffffffL
 *
 * see also Sysconf() in dos.c
 */
long _cdecl
sys_d_pathconf (MetaDOSDir const char *name, int which)
{
	struct proc *p = get_curproc();

	fcookie dir;
	long r;

	r = path2cookie (p, name, NULL, &dir);
	if (r)
	{
		DEBUG(("Dpathconf(%s): bad path",name));
		return r;
	}

	r = xfs_pathconf (dir.fs, &dir, which);
	if (which == DP_CASE && r == ENOSYS)
	{
		/* backward compatibility with old .XFS files */
		r = (dir.fs->fsflags & FS_CASESENSITIVE) ? DP_CASESENS :
				DP_CASEINSENS;
	}

	release_cookie (&dir);
	return r;
}

/*
 * GEMDOS extension: Opendir/Readdir/Rewinddir/Closedir
 *
 * offer a new, POSIX-like alternative to Fsfirst/Fsnext,
 * and as a bonus allow for arbitrary length file names
 */
long _cdecl
sys_d_opendir (MetaDOSDir const char *name, int flag)
{
	struct proc *p = get_curproc();

	DIR *dirh = dirMD;
	fcookie dir;
	long r;
	ushort mode;

	r = path2cookie (p, name, follow_links, &dir);
	if (r)
	{
		DEBUG(("Dopendir(%s): error %ld", name, r));
		return r;
	}

	r = dir_access (p->p_cred->ucr, &dir, S_IROTH, &mode);
	if (r)
	{
		DEBUG(("Dopendir(%s): read permission denied", name));
		release_cookie (&dir);
		return r;
	}

	dirh->fc = dir;
	dirh->index = 0;
	dirh->flags = flag;
	r = xfs_opendir (dir.fs, dirh, flag);
	if (r)
	{
		DEBUG(("d_opendir(%s): opendir returned %ld", name, r));
		release_cookie (&dir);
		return r;
	}

	/* we keep a chain of open directories so that if a process
	 * terminates without closing them all, we can clean up
	 */
	dirh->next = p->p_fd->searches;
	p->p_fd->searches = dirh;

	dirh->fd = 0; /* less than MIN_OPEN */

	return (long) dirh;
}

long _cdecl
sys_d_readdir (MetaDOSDir int len, long handle, char *buf)
{
	DIR *dirh = dirMD;
	fcookie fc;
	long r;

	if (!dirh->fc.fs)
		return EBADF;

	r = xfs_readdir (dirh->fc.fs, dirh, buf, len, &fc);
	if (r == E_OK)
		release_cookie (&fc);

	DEBUG(("sys_d_readdir(): returned %ld", r));

	return r;
}

/* jr: just as d_readdir, but also returns XATTR structure (not
 * following links). Note that the return value reflects the
 * result of the Dreaddir operation, the result of the Fxattr
 * operation is stored in long *xret
 */
long _cdecl
sys_d_xreaddir (MetaDOSDir int len, long handle, char *buf, XATTR *xattr, long *xret)
{
	DIR *dirh = dirMD;
	fcookie fc;
	long r;

	if (!dirh->fc.fs)
		return EBADF;

	r = xfs_readdir (dirh->fc.fs, dirh, buf, len, &fc);
	if (r != E_OK)
		return r;

	*xret = xfs_getxattr (fc.fs, &fc, xattr);
	if ((*xret == E_OK) && (fc.fs->fsflags & FS_EXT_3))
	{
		xtime_to_local_dos(xattr, m);
		xtime_to_local_dos(xattr, a);
		xtime_to_local_dos(xattr, c);
	}

	release_cookie (&fc);
	return r;
}


long _cdecl
sys_d_rewind (MetaDOSDir long handle)
{
	DIR *dirh = dirMD;

	if (!dirh->fc.fs)
		return EBADF;

	return xfs_rewinddir (dirh->fc.fs, dirh);
}

/*
 * NOTE: there is also code in terminate() in dosmem.c that
 * does automatic closes of directory searches.
 * If you change d_closedir(), you may also need to change
 * terminate().
 */
long _cdecl
sys_d_closedir (MetaDOSDir long handle)
{
	struct proc *p = get_curproc();
	DIR *dirh = dirMD;
	DIR **where;
	long r;

	where = &p->p_fd->searches;
	while (*where && *where != dirh)
		where = &((*where)->next);

	if (!*where)
	{
		DEBUG(("Dclosedir: not an open directory"));
		return EBADF;
	}

	/* unlink the directory from the chain */
	*where = dirh->next;

	if (dirh->fc.fs)
	{
		r = xfs_closedir (dirh->fc.fs, dirh);
		release_cookie (&dirh->fc);
	}
	else
		r = E_OK;

	if (r)
		DEBUG(("Dclosedir: error %ld", r));

	return r;
}

/*
 * GEMDOS extension: Fxattr(flag, file, xattr)
 *
 * gets extended attributes for a file.
 * flag is 0 if symbolic links are to be followed (like stat),
 * flag is 1 if not (like lstat).
 */
long _cdecl
sys_f_xattr (MetaDOSFile int flag, const char *name, XATTR *xattr)
{
	struct proc *p = get_curproc();

	fcookie fc;
	long r;

	TRACE (("Fxattr(%d, %s)", flag, name));

	r = path2cookie (p, name, flag ? NULL : follow_links, &fc);
	if (r)
	{
		DEBUG (("Fxattr(%s): path2cookie returned %ld", name, r));
		return r;
	}

	r = xfs_getxattr (fc.fs, &fc, xattr);
	if (r)
	{
		DEBUG (("Fxattr(%s): returning %ld", name, r));
	}
	else if (fc.fs->fsflags & FS_EXT_3)
	{
		xtime_to_local_dos(xattr, m);
		xtime_to_local_dos(xattr, a);
		xtime_to_local_dos(xattr, c);
	}

	release_cookie (&fc);
	return r;
}

/*
 * GEMDOS extension: Flink(old, new)
 *
 * creates a hard link named "new" to the file "old".
 */
long _cdecl
sys_f_link (MetaDOSDir const char *old, const char *new)
{
	struct proc *p = get_curproc();
	
	fcookie olddir, newdir;
	char temp1[PATH_MAX], temp2[PATH_MAX];
	long r;
	ushort mode;

	TRACE(("Flink(%s, %s)", old, new));

	r = path2cookie (p, old, temp2, &olddir);
	if (r)
	{
		DEBUG(("Flink(%s,%s): error parsing old name",old,new));
		return r;
	}

	r = path2cookie (p, new, temp1, &newdir);
	if (r)
	{
		DEBUG(("Flink(%s,%s): error parsing new name",old,new));
		release_cookie(&olddir);
		return r;
	}

	if (newdir.fs != olddir.fs)
	{
		DEBUG(("Flink(%s,%s): different file systems",old,new));
		release_cookie (&olddir);
		release_cookie (&newdir);
		return EXDEV;	/* cross device link */
	}

	/* check for write permission on the destination directory
	 */
	r = dir_access (p->p_cred->ucr, &newdir, S_IWOTH, &mode);
	if (r)
		DEBUG(("Flink(%s,%s): access to directory denied",old,new));
	else
		r = xfs_hardlink (newdir.fs, &olddir, temp2, &newdir, temp1);

	release_cookie (&olddir);
	release_cookie (&newdir);

	return r;
}

/*
 * GEMDOS extension: Fsymlink(old, new)
 *
 * create a symbolic link named "new" that contains the path "old"
 */
long _cdecl
sys_f_symlink (MetaDOSDir const char *old, const char *new)
{
	struct proc *p = get_curproc();
	
	fcookie newdir;
	long r;
	char temp1[PATH_MAX];
	ushort mode;

	TRACE(("Fsymlink(%s, %s)", old, new));

	r = path2cookie(p, new, temp1, &newdir);
	if (r)
	{
		DEBUG(("Fsymlink(%s,%s): error parsing %s", old,new,new));
		return r;
	}

	r = dir_access (p->p_cred->ucr, &newdir, S_IWOTH, &mode);
	if (r)
		DEBUG(("Fsymlink(%s,%s): access to directory denied",old,new));
	else
		r = xfs_symlink (newdir.fs, &newdir, temp1, old);

	release_cookie (&newdir);
	return r;
}

/*
 * GEMDOS extension: Freadlink(buflen, buf, linkfile)
 *
 * read the contents of the symbolic link "linkfile" into the buffer
 * "buf", which has length "buflen".
 */
long _cdecl
sys_f_readlink (MetaDOSDir int buflen, char *buf, const char *linkfile)
{
	struct proc *p = get_curproc();

	fcookie file;
	long r;
	XATTR xattr;

	TRACE(("Freadlink(%s)", linkfile));

	r = path2cookie (p, linkfile, (char *)0, &file);
	if (r)
	{
		DEBUG(("Freadlink: unable to find %s", linkfile));
		return r;
	}

	r = xfs_getxattr (file.fs, &file, &xattr);
	if (r)
	{
		DEBUG(("Freadlink: unable to get attributes for %s", linkfile));
	}
	else if (S_ISLNK(xattr.mode))
	{
		r = xfs_readlink (file.fs, &file, buf, buflen);
	}
	else
	{
		DEBUG(("Freadlink: %s is not a link", linkfile));
		r = EACCES;
	}

	release_cookie (&file);
	return r;
}

/*
 * GEMDOS extension: Fchown(name, uid, gid)
 *
 * changes the user and group ownerships of a file to "uid" and "gid"
 * respectively
 */
long _cdecl
sys_f_chown (MetaDOSDir const char *name, int uid, int gid)
{
	return sys_f_chown16( MetaDOSDirpass name, uid, gid, 0 );
}

/*
 * GEMDOS extension: Fchown16(name, uid, gid, follow_symlinks)
 *
 * @param follow_symlinks set to 1 to follow or 0 to not to follow
 *                        (the other values are reserved)
 */
long _cdecl
sys_f_chown16 (MetaDOSDir const char *name, int uid, int gid, int follow_symlinks)
{
	struct proc *p = get_curproc();
#if 0
	struct ucred *cred = p->p_cred->ucr;
#endif

	fcookie fc;
#if 0
	XATTR xattr;
#endif
	long r;


	TRACE(("Fchown16(%s, %d, %d, %s)", name, uid, gid, follow_symlinks != 0 ? "follow_links" : "nofollow"));

	r = path2cookie (p, name, follow_symlinks == 1 ? follow_links : NULL, &fc);
	if (r)
	{
		DEBUG(("Fchown(%s): error %ld", name, r));
		return r;
	}

#if 0
	/* MiNT acts like _POSIX_CHOWN_RESTRICTED: a non-privileged process can
	 * only change the ownership of a file that is owned by this user, to
	 * the effective group id of the process or one of its supplementary groups
	 */
	if (cred->euid)
	{
		if (cred->egid != gid && !groupmember (cred, gid))
			r = EACCES;
		else
			r = xfs_getxattr (fc.fs, &fc, &xattr);

		if (r)
		{
			DEBUG(("Fchown(%s): unable to get file attributes",name));
			release_cookie (&fc);
			return r;
		}

		if (xattr.uid != cred->euid || xattr.uid != uid)
		{
			DEBUG(("Fchown(%s): not the file's owner",name));
			release_cookie (&fc);
			return EACCES;
		}

		r = xfs_chown (fc.fs, &fc, uid, gid);

		/* POSIX 5.6.5.2: if name refers to a regular file the set-user-ID and
		 * set-group-ID bits of the file mode shall be cleared upon successful
		 * return from the call to chown, unless the call is made by a process
		 * with the appropriate privileges.
		 * Note that POSIX leaves the behaviour unspecified for all other file
		 * types. At least for directories with BSD-like setgid semantics,
		 * these bits should be left unchanged.
		 */
		if (!r && !S_ISDIR(xattr.mode)
		    && (xattr.mode & (S_ISUID | S_ISGID)))
		{
			long s;

			s = xfs_chmode (fc.fs, &fc, xattr.mode & ~(S_ISUID | S_ISGID));
			if (!s)
				DEBUG(("Fchown: chmode returned %ld (ignored)", s));
		}
	}
	else
#endif
		r = xfs_chown (fc.fs, &fc, uid, gid);

	release_cookie (&fc);
	return r;
}

/*
 * GEMDOS extension: Fchmod (file, mode)
 *
 * changes a file's access permissions.
 */
long _cdecl
sys_f_chmod (MetaDOSDir const char *name, unsigned int mode)
{
	struct proc *p = get_curproc();
#if 0
	struct ucred *cred = p->p_cred->ucr;
#endif

	fcookie fc;
	long r;
	XATTR xattr;


	TRACE (("Fchmod(%s, %o)", name, mode));
	r = path2cookie (p, name, follow_links, &fc);
	if (r)
	{
		DEBUG (("Fchmod(%s): error %ld", name, r));
		return r;
	}

	r = xfs_getxattr (fc.fs, &fc, &xattr);
	if (r)
	{
		DEBUG (("Fchmod(%s): couldn't get file attributes",name));
	}
#if 0
	else if (cred->euid && cred->euid != xattr.uid)
	{
		DEBUG (("Fchmod(%s): not the file's owner",name));
		r = EACCES;
	}
#endif
	else
	{
		r = xfs_chmode (fc.fs, &fc, mode & ~S_IFMT);
		if (r) DEBUG(("Fchmod: error %ld", r));
	}
	release_cookie (&fc);
	return r;
}

/*
 * GEMDOS-extension: Dreadlabel(path, buf, buflen)
 *
 * original written by jr
 */
long _cdecl
sys_d_readlabel (MetaDOSDir const char *name, char *buf, int buflen)
{
	struct proc *p = get_curproc();

	fcookie dir;
	long r;

	r = path2cookie (p, name, NULL, &dir);
	if (r)
	{
		DEBUG (("Dreadlabel(%s): bad path", name));
		return r;
	}

	r = xfs_readlabel (dir.fs, &dir, buf, buflen);

	release_cookie (&dir);
	return r;
}

/*
 * GEMDOS-extension: Dwritelabel(path, newlabel)
 *
 * original written by jr
 */
long _cdecl
sys_d_writelabel (MetaDOSDir const char *name, const char *label)
{
	struct proc *p = get_curproc();

	fcookie dir;
	long r;

	r = path2cookie (p, name, NULL, &dir);
	if (r)
	{
		DEBUG (("Dwritelabel(%s): bad path",name));
		return r;
	}

	r = xfs_writelabel (dir.fs, &dir, label);

	release_cookie (&dir);
	return r;
}

/*
 * GEMDOS extension: Fstat(flag, file, stat)
 *
 * gets extended attributes in native UNIX format for a file.
 * flag is 0 if symbolic links are to be followed (like stat),
 * flag is 1 if not (like lstat).
 */
long _cdecl
sys_f_stat64 (MetaDOSDir int flag, const char *name, STAT *stat)
{
	struct proc *p = get_curproc();

	fcookie fc;
	long r;

	TRACE (("Fstat64(%d, %s)", flag, name));

	r = path2cookie (p, name, flag ? NULL : follow_links, &fc);
	if (r)
	{
		DEBUG (("Fstat64(%s): path2cookie returned %ld", name, r));
		return r;
	}

	r = xfs_stat64 (fc.fs, &fc, stat);
	if (r) DEBUG (("Fstat64(%s): returning %ld", name, r));

	release_cookie (&fc);
	return r;
}

long _cdecl sys_d_dsetpath (MetaDOSDir const char *path)
{
	struct proc *p = get_curproc();
	fcookie parentdir, targdir;
	int d;
	char temp1[PATH_MAX];
	long r;
	XATTR xattr;
	ushort mode;

	d = tolower(pathNameMD[0]);
	if (d >= '1' && d <= '6')
		d = d - '1' + 26;
	else
		d = d - 'a';
	if (d < 0 || d >= NUM_DRIVES)
		return ENXIO;
	parentdir = p->p_cwd->root[d];
	TRACE(("Dsetpath(%s)", path));

	if (!parentdir.fs)
	{
		DEBUG(("Dfree: bad drive"));
		return ENXIO;
	}

	r = path2cookie(p, path, temp1, &parentdir);
	if (r)
	{
		DEBUG(("Dsetpath(%s): returning %ld", path, r));
		return r;
	}

	r = dir_access(p->p_cred->ucr, &parentdir, S_IROTH, &mode);
	if (r)
	{
		DEBUG(("Dsetpath(%s): read access to directory denied",path));
		release_cookie(&parentdir);
		return r;
	}

	/* now get the info on the file itself */
	r = relpath2cookie (p, &parentdir, temp1, NULL, &targdir, 0);
	if (r)
	{
		release_cookie (&parentdir);
		if (r == ENOENT)
			r = ENOTDIR;
		DEBUG(("Dsetpath: error %ld on %s", r, path));
		return r;
	}

	r = xfs_getxattr (targdir.fs, &targdir, &xattr);
	if (r)
	{
		DEBUG(("Dsetpath: error %ld on %s", r, path));
	} else if (!S_ISDIR(xattr.mode))
	{
		DEBUG(("Dsetpath: %s is not a directory", path));
		r = ENOTDIR;
	} else
	{
		r = 0;
	}

	release_cookie (&targdir);
	release_cookie (&parentdir);
	return r;
}
