/*-
 * Copyright (c) 2018 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2017 The NetBSD Foundation, Inc.
 * Copyright (c) 2016 The DragonFly Project
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Tomohiro Kusumi <kusumi.tomohiro@gmail.com>.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include "autofs.h"

#include <sys/stat.h>
#include <sys/dirent.h>
#include <sys/namei.h>
#include <sys/stdint.h>

static int	autofs_trigger_vn(struct vnode *vp, const char *path,
		    int pathlen, struct vnode **newvp, struct proc *p);

static int
autofs_access(void *v)
{
	struct vop_access_args /* {
		struct vnode *a_vp;
		int a_mode;
		struct ucred *a_cred;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp __unused = ap->a_vp;

	KASSERT(VOP_ISLOCKED(vp));
	/*
	 * Nothing to do here; the only kind of access control
	 * needed is in autofs_mkdir().
	 */
	return 0;
}

static int
autofs_getattr(void *v)
{
	struct vop_getattr_args /* {
		struct vnode *a_vp;
		struct vattr *a_vap;
		struct ucred *a_cred;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp = ap->a_vp;
	struct vattr *vap = ap->a_vap;
	struct autofs_node *anp = VTOI(vp);

	KASSERT(vp->v_type == VDIR);

	/*
	 * The reason we must do this is that some tree-walking software,
	 * namely fts(3), assumes that stat(".") results will not change
	 * between chdir("subdir") and chdir(".."), and fails with ENOENT
	 * otherwise.
	 */
	if (autofs_mount_on_stat &&
	    autofs_cached(anp, NULL, 0) == false &&
	    autofs_ignore_thread() == false) {
		struct vnode *newvp = NULL;
		int error = autofs_trigger_vn(vp, "", 0, &newvp, ap->a_p);
		if (error)
			return error;
		/*
		 * Already mounted here.
		 */
		if (newvp) {
			error = VOP_GETATTR(newvp, vap, ap->a_cred, ap->a_p);
			vput(newvp);
			return error;
		}
	}

	vattr_null(vap);

	vap->va_type = VDIR;
	vap->va_mode = 0755;
	vap->va_nlink = 3;
	vap->va_uid = 0;
	vap->va_gid = 0;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = anp->an_ino;
	vap->va_size = S_BLKSIZE;
	vap->va_blocksize = S_BLKSIZE;
	vap->va_mtime = anp->an_ctime;
	vap->va_atime = anp->an_ctime;
	vap->va_ctime = anp->an_ctime;
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = 0;
	vap->va_bytes = S_BLKSIZE;
	vap->va_filerev = 0;
	vap->va_vaflags = 0;
	vap->va_spare = 0;

	return 0;
}

/*
 * Unlock the vnode, request automountd(8) action, and then lock it back.
 * If anything got mounted on top of the vnode, return the new filesystem's
 * root vnode in 'newvp', locked.  A caller needs to vput() the 'newvp'.
 */
static int
autofs_trigger_vn(struct vnode *vp, const char *path, int pathlen,
    struct vnode **newvp, struct proc *p)
{
	struct autofs_node *anp;
	int error, lock_flags;

	anp = vp->v_data;

	/*
	 * Release the vnode lock, so that other operations, in partcular
	 * mounting a filesystem on top of it, can proceed.  Increase use
	 * count, to prevent the vnode from being deallocated and to prevent
	 * filesystem from being unmounted.
	 */
	lock_flags = VOP_ISLOCKED(vp);
	vref(vp);
	VOP_UNLOCK(vp, p);

	rw_enter_write(&autofs_softc->sc_lock);

	/*
	 * Workaround for mounting the same thing multiple times; revisit.
	 */
	if (vp->v_mountedhere) {
		error = 0;
		goto mounted;
	}

	error = autofs_trigger(anp, path, pathlen);
mounted:
	rw_exit_write(&autofs_softc->sc_lock);
	vn_lock(vp, lock_flags | LK_RETRY, p);
	vrele(vp);

	if (error)
		return error;

	if (!vp->v_mountedhere) {
		*newvp = NULL;
		return 0;
	} else {
		/*
		 * If the operation that succeeded was mount, then mark
		 * the node as non-cached.  Otherwise, if someone unmounts
		 * the filesystem before the cache times out, we will fail
		 * to trigger.
		 */
		autofs_node_uncache(anp);
	}

	error = VFS_ROOT(vp->v_mountedhere, newvp);
	if (error) {
		AUTOFS_WARN("VFS_ROOT() failed with error %d", error);
		return error;
	}

	return 0;
}

static int
autofs_lookup(void *v)
{
	struct vop_lookup_args /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */ *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct autofs_mount *amp = VFSTOAUTOFS(dvp->v_mount);
	struct autofs_node *anp, *child;
	int error;
	const int lastcn = (cnp->cn_flags & ISLASTCN) != 0;
	const int lockparent = (cnp->cn_flags & LOCKPARENT) != 0;

	KASSERT(VOP_ISLOCKED(dvp));

	cnp->cn_flags &= ~PDIRUNLOCK;
	anp = VTOI(dvp);
	*vpp = NULL;

	/* Check accessibility of directory. */
	KASSERT(!VOP_ACCESS(dvp, VEXEC, cnp->cn_cred, curproc));

	/*
	 * Only creation of directories is allowed.
	 * Once created, directories can't be renamed or deleted.
	 */
	KASSERT(cnp->cn_nameiop != RENAME);
	KASSERT(cnp->cn_nameiop != DELETE);

	/*
	 * Avoid doing a linear scan of the directory if the requested
	 * directory/name couple is already in the cache.
	 */
	error = cache_lookup(dvp, vpp, cnp);
	if (error == ENOENT) {
		return error;
	} else if (error != -1) {
		KASSERT(error >= 0);
		return 0;
	}

	if (cnp->cn_flags & ISDOTDOT) {
		/*
		 * Lookup of ".." case.
		 */
		if (!anp->an_parent) {
			error = ENOENT;
			goto out;
		}

		/*
		 * Lock the parent an_node_lock before releasing the vnode
		 * lock, and thus prevents parent from disappearing.
		 */
		rw_enter_write(&anp->an_node_lock);
		VOP_UNLOCK(dvp, curproc);
		cnp->cn_flags |= PDIRUNLOCK;

		/*
		 * Get a vnode of the '..' entry and re-acquire the lock.
		 * The vnode lock can be recursive.
		 */
		error = autofs_node_vn(anp, amp->am_mp, vpp);
		if (error) {
			if (vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, curproc) == 0)
				cnp->cn_flags &= ~PDIRUNLOCK;
			return error;
		}

		if (lockparent && lastcn) {
			error = vn_lock(dvp, LK_EXCLUSIVE | LK_RETRY, curproc);
			if (error) {
				vput(*vpp);
				return error;
			}
			cnp->cn_flags &= ~PDIRUNLOCK;
		}
		goto out;
	} else if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		/*
		 * Lookup of "." case.
		 */
		vref(dvp);
		*vpp = dvp;
		error = 0;
		goto done;
	}

	if (autofs_cached(anp, cnp->cn_nameptr, cnp->cn_namelen) == false &&
	    autofs_ignore_thread() == false) {
		struct vnode *newvp = NULL;
		error = autofs_trigger_vn(dvp, cnp->cn_nameptr, cnp->cn_namelen,
		    &newvp, curproc);
		if (error)
			return error;
		/*
		 * Already mounted here.
		 */
		if (newvp) {
			error = VOP_LOOKUP(newvp, vpp, cnp);
			vput(newvp);
			return error;
		}
	}

	rw_enter_read(&amp->am_lock);
	error = autofs_node_find(anp, cnp->cn_nameptr, cnp->cn_namelen, &child);
	if (error) {
		if (lastcn && cnp->cn_nameiop == CREATE) {
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, curproc);
			if (error)
				goto out;
			rw_exit_read(&amp->am_lock);
			/*
			 * We are creating an entry in the file system, so
			 * save its name for further use.
			 */
			cnp->cn_flags |= SAVENAME;
			if (!lockparent) {
				VOP_UNLOCK(dvp, curproc);
				cnp->cn_flags |= PDIRUNLOCK;
			}
			return EJUSTRETURN;
		}

		rw_exit_read(&amp->am_lock);
		error = ENOENT;
		goto done;
	}

	/*
	 * Dropping the node here is ok, because we never remove nodes.
	 */
	rw_exit_read(&amp->am_lock);

	/* Get a vnode for the matching entry. */
	rw_enter_write(&child->an_node_lock);
	error = autofs_node_vn(child, amp->am_mp, vpp);
done:
	/*
	 * Cache the result, unless request was for creation (as it does
	 * not improve the performance).
	 */
	if ((cnp->cn_flags & MAKEENTRY) && cnp->cn_nameiop != CREATE)
		cache_enter(dvp, *vpp, cnp);
out:
	/*
	 * If (1) we succeded, (2) found a distinct vnode to return and (3) were
	 * either not explicitly told to keep the parent locked or are in the
	 * middle of a lookup, unlock the parent vnode.
	 */
	if ((error == 0 || error == EJUSTRETURN) && /* (1) */
	    *vpp != dvp &&			    /* (2) */
	    (!lockparent || !lastcn)) {		    /* (3) */
		VOP_UNLOCK(dvp, curproc);
		cnp->cn_flags |= PDIRUNLOCK;
	} else {
		KASSERT(VOP_ISLOCKED(dvp));
	}

	KASSERT((*vpp && VOP_ISLOCKED(*vpp)) || error);

	return error;
}

static int
autofs_open(void *v)
{
	struct vop_open_args /* {
		struct vnode *a_vp;
		int a_mode;
		struct ucred *a_cred;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp __unused = ap->a_vp;

	KASSERT(VOP_ISLOCKED(vp));
	return 0;
}

static int
autofs_close(void *v)
{
	struct vop_close_args /* {
		struct vnode *a_vp;
		int a_fflag;
		struct ucred *a_cred;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp __unused = ap->a_vp;

	KASSERT(VOP_ISLOCKED(vp));
	return 0;
}

static int
autofs_fsync(void *v)
{
	struct vop_fsync_args /* {
		struct vnode *a_vp;
		struct ucred *a_cred;
		int a_waitfor;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp __unused = ap->a_vp;

	KASSERT(VOP_ISLOCKED(vp));
	return 0;
}

static int
autofs_mkdir(void *v)
{
	struct vop_mkdir_args /* {
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
		struct vattr *a_vap;
	} */ *ap = v;
	struct vnode *dvp = ap->a_dvp;
	struct vnode **vpp = ap->a_vpp;
	struct componentname *cnp = ap->a_cnp;
	struct autofs_mount *amp = VFSTOAUTOFS(dvp->v_mount);
	struct autofs_node *anp = VTOI(dvp);
	struct autofs_node *child = NULL;
	int error;

	KASSERT(ap->a_vap->va_type == VDIR);

	/*
	 * Do not allow mkdir() if the calling thread is not
	 * automountd(8) descendant.
	 */
	if (autofs_ignore_thread() == false) {
		vput(dvp);
		return EPERM;
	}

	rw_enter_write(&amp->am_lock);
	error = autofs_node_new(anp, amp, cnp->cn_nameptr, cnp->cn_namelen,
	    &child);
	if (error) {
		rw_exit_write(&amp->am_lock);
		vput(dvp);
		return error;
	}
	rw_exit_write(&amp->am_lock);

	rw_enter_write(&child->an_node_lock);
	error = autofs_node_vn(child, amp->am_mp, vpp);
	vput(dvp);

	return error;
}

static int
autofs_print(void *v)
{
	struct vop_print_args /* {
		struct vnode *a_vp;
	} */ *ap = v;
	struct vnode *vp = ap->a_vp;
	struct autofs_node *anp = VTOI(vp);

	printf("tag VT_AUTOFS, node %p, ino %jd, name %s, cached %d, "
	    "retries %d, wildcards %d",
	    anp, (intmax_t)anp->an_ino, anp->an_name, anp->an_cached,
	    anp->an_retries, anp->an_wildcards);
	printf("\n");

	return 0;
}

static int
autofs_readdir_one(struct uio *uio, const char *name, ino_t ino,
    size_t *reclenp)
{
	struct dirent dirent;

	dirent.d_fileno = ino;
	dirent.d_type = DT_DIR;
	strlcpy(dirent.d_name, name, sizeof(dirent.d_name));
	dirent.d_namlen = strlen(dirent.d_name);
	dirent.d_reclen = DIRENT_SIZE(&dirent);

	if (reclenp)
		*reclenp = dirent.d_reclen;

	if (!uio)
		return 0;

	if (uio->uio_resid < dirent.d_reclen)
		return EINVAL;

	return uiomove(&dirent, dirent.d_reclen, uio);
}

static size_t
autofs_dirent_reclen(const char *name)
{
	size_t reclen;

	autofs_readdir_one(NULL, name, -1, &reclen);

	return reclen;
}

static int
autofs_readdir(void *v)
{
	struct vop_readdir_args /* {
		struct vnode *a_vp;
		struct uio *a_uio;
		struct ucred *a_cred;
		int *a_eofflag;
	} */ *ap = v;
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	size_t initial_resid = ap->a_uio->uio_resid;
	struct autofs_mount *amp = VFSTOAUTOFS(vp->v_mount);
	struct autofs_node *anp = VTOI(vp);
	struct autofs_node *child;
	size_t reclen, reclens;
	int error;

	KASSERT(VOP_ISLOCKED(vp));

	if (vp->v_type != VDIR)
		return ENOTDIR;

	if (autofs_cached(anp, NULL, 0) == false &&
	    autofs_ignore_thread() == false) {
		struct vnode *newvp = NULL;
		error = autofs_trigger_vn(vp, "", 0, &newvp, curproc);
		if (error)
			return error;
		/*
		 * Already mounted here.
		 */
		if (newvp) {
			error = VOP_READDIR(newvp, ap->a_uio, ap->a_cred,
			    ap->a_eofflag);
			vput(newvp);
			return error;
		}
	}

	if (uio->uio_offset < 0)
		return EINVAL;

	if (ap->a_eofflag)
		*ap->a_eofflag = 0;

	/*
	 * Write out the directory entry for ".".
	 */
	if (uio->uio_offset == 0) {
		error = autofs_readdir_one(uio, ".", anp->an_ino, &reclen);
		if (error)
			goto out;
	}
	reclens = autofs_dirent_reclen(".");

	/*
	 * Write out the directory entry for "..".
	 */
	if (uio->uio_offset <= reclens) {
		if (uio->uio_offset != reclens)
			return EINVAL;
		error = autofs_readdir_one(uio, "..",
		    (anp->an_parent ? anp->an_parent->an_ino : anp->an_ino),
		    &reclen);
		if (error)
			goto out;
	}
	reclens += autofs_dirent_reclen("..");

	/*
	 * Write out the directory entries for subdirectories.
	 */
	rw_enter_read(&amp->am_lock);
	RB_FOREACH(child, autofs_node_tree, &anp->an_children) {
		/*
		 * Check the offset to skip entries returned by previous
		 * calls to getdents().
		 */
		if (uio->uio_offset > reclens) {
			reclens += autofs_dirent_reclen(child->an_name);
			continue;
		}

		/*
		 * Prevent seeking into the middle of dirent.
		 */
		if (uio->uio_offset != reclens) {
			rw_exit_read(&amp->am_lock);
			return EINVAL;
		}

		error = autofs_readdir_one(uio, child->an_name, child->an_ino,
		    &reclen);
		reclens += reclen;
		if (error) {
			rw_exit_read(&amp->am_lock);
			goto out;
		}
	}
	rw_exit_read(&amp->am_lock);

	if (ap->a_eofflag)
		*ap->a_eofflag = 1;

	return 0;
out:
	/*
	 * Return error if the initial buffer was too small to do anything.
	 */
	if (uio->uio_resid == initial_resid)
		return error;

	/*
	 * Don't return an error if we managed to copy out some entries.
	 */
	if (uio->uio_resid < reclen)
		return 0;

	return error;
}

static int
autofs_inactive(void *v)
{
	struct vop_inactive_args /* {
		struct vnode *a_vp;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp = ap->a_vp;

	KASSERT(VOP_ISLOCKED(vp));

	VOP_UNLOCK(vp, ap->a_p);

	/*
	 * We do not reclaim autofs_node here; instead we are
	 * destroying them in autofs_node_delete().
	 */
	return 0;
}

static int
autofs_reclaim(void *v)
{
	struct vop_reclaim_args /* {
		struct vnode *a_vp;
		struct proc *a_p;
	} */ *ap = v;
	struct vnode *vp = ap->a_vp;
	struct autofs_node *anp = VTOI(vp);

	/*
	 * We do not free autofs_node here; instead we are
	 * destroying them in autofs_node_delete().
	 */
	rw_enter_write(&anp->an_node_lock);
	anp->an_vnode = NULL;
	vp->v_data = NULL;
	rw_exit_write(&anp->an_node_lock);

	return 0;
}

static int
autofs_lock(void *v)
{
	struct vop_lock_args /* {
		struct vnode *a_vp;
		int a_flags;
		struct proc *a_p;
	} */ *ap = v;
	struct autofs_node *anp = VTOI(ap->a_vp);

	return rrw_enter(&anp->an_vn_lock, ap->a_flags & LK_RWFLAGS);
}

static int
autofs_unlock(void *v)
{
	struct vop_unlock_args /* {
		struct vnode *a_vp;
		struct proc *a_p;
	} */ *ap = v;
	struct autofs_node *anp = VTOI(ap->a_vp);

	rrw_exit(&anp->an_vn_lock);

	return 0;
}

static int
autofs_islocked(void *v)
{
	struct vop_islocked_args /* {
		struct vnode *a_vp;
	} */ *ap = v;
	struct autofs_node *anp = VTOI(ap->a_vp);

	return rrw_status(&anp->an_vn_lock);
}

static struct vops autofs_vops = {
	.vop_lookup	= autofs_lookup,
	.vop_open	= autofs_open,
	.vop_close	= autofs_close,
	.vop_access	= autofs_access,
	.vop_getattr	= autofs_getattr,
	.vop_fsync	= autofs_fsync,
	.vop_mkdir	= autofs_mkdir,
	.vop_readdir	= autofs_readdir,
	.vop_inactive	= autofs_inactive,
	.vop_reclaim	= autofs_reclaim,
	.vop_lock	= autofs_lock,
	.vop_unlock	= autofs_unlock,
	.vop_print	= autofs_print,
	.vop_islocked	= autofs_islocked,
};

static void
autofs_node_timeout(void *context)
{
	struct autofs_node *anp = context;

	autofs_node_uncache(anp);
}

int
autofs_node_new(struct autofs_node *parent, struct autofs_mount *amp,
    const char *name, int namelen, struct autofs_node **anpp)
{
	struct autofs_node *anp;

	rw_assert_wrlock(&amp->am_lock);

	if (parent) {
		rw_assert_wrlock(&parent->an_mount->am_lock);
		KASSERT(autofs_node_find(parent, name, namelen, NULL) ==
		    ENOENT);
	}

	anp = pool_get(&autofs_node_pool, PR_WAITOK);
	anp->an_name = kstrndup(name, namelen);
	anp->an_ino = amp->am_last_ino++;
	timeout_set(&anp->an_timeout, autofs_node_timeout, anp);
	rw_init(&anp->an_node_lock, "autofsvnlock");
	rrw_init_flags(&anp->an_vn_lock, "autofsvoplock", RWL_DUPOK);
	getnanotime(&anp->an_ctime);
	anp->an_parent = parent;
	anp->an_mount = amp;
	anp->an_vnode = NULL;
	anp->an_cached = false;
	anp->an_wildcards = false;
	anp->an_retries = 0;

	if (parent)
		RB_INSERT(autofs_node_tree, &parent->an_children, anp);
	RB_INIT(&anp->an_children);

	*anpp = anp;
	return 0;
}

int
autofs_node_find(struct autofs_node *parent, const char *name,
    int namelen, struct autofs_node **anpp)
{
	struct autofs_node *anp, find;
	int error;

	rw_assert_anylock(&parent->an_mount->am_lock);

	if (namelen >= 0)
		find.an_name = kstrndup(name, namelen);
	else
		find.an_name = kstrdup(name);

	anp = RB_FIND(autofs_node_tree, &parent->an_children, &find);
	if (anp) {
		error = 0;
		if (anpp)
			*anpp = anp;
	} else {
		error = ENOENT;
	}

	free(find.an_name, M_TEMP, strlen(find.an_name) + 1);

	return error;
}

void
autofs_node_delete(struct autofs_node *anp)
{

	rw_assert_wrlock(&anp->an_mount->am_lock);
	KASSERT(RB_EMPTY(&anp->an_children));

	timeout_del(&anp->an_timeout);

	if (anp->an_parent)
		RB_REMOVE(autofs_node_tree, &anp->an_parent->an_children, anp);

	free(anp->an_name, M_TEMP, strlen(anp->an_name) + 1);
	pool_put(&autofs_node_pool, anp);
}

/*
 * A caller must have acquired the node lock.
 * The node lock is released when returning from this function.
 */
int
autofs_node_vn(struct autofs_node *anp, struct mount *mp, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

again:
	rw_assert_wrlock(&anp->an_node_lock);

	if ((vp = anp->an_vnode) != NULL) {
		error = vget(vp, LK_EXCLUSIVE, curproc);
		rw_exit_write(&anp->an_node_lock);
		if (error == ENOENT) {
			rw_enter_write(&anp->an_node_lock);
			goto again;
		}

		*vpp = vp;
		return error;
	}

	error = getnewvnode(VT_AUTOFS, mp, &autofs_vops, &vp);
	if (error) {
		rw_exit_write(&anp->an_node_lock);
		return error;
	}

	uvm_vnp_setsize(vp, 0);
	vp->v_type = VDIR;
	if (anp->an_parent == NULL)
		vp->v_flag |= VROOT;
	vp->v_data = anp;
	anp->an_vnode = vp;
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, curproc);
	rw_exit_write(&anp->an_node_lock);

	KASSERT(VOP_ISLOCKED(vp));
	*vpp = vp;

	return 0;
}
