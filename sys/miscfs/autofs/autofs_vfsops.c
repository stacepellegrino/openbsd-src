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
#include <sys/sysctl.h>

static int	autofs_statfs(struct mount *, struct statfs *, struct proc *p);

static int
autofs_init(struct vfsconf *vfsp)
{

	KASSERT(!autofs_softc);

	pool_init(&autofs_request_pool, sizeof(struct autofs_request), 0,
	    IPL_NONE, PR_WAITOK, "autofs_request", NULL);
	pool_init(&autofs_node_pool, sizeof(struct autofs_node), 0,
	    IPL_NONE, PR_WAITOK, "autofs_node", NULL);

	autofs_softc = malloc(sizeof(*autofs_softc), M_TEMP, M_WAITOK | M_ZERO);
	KASSERT(autofs_softc);

	TAILQ_INIT(&autofs_softc->sc_requests);
	autofs_softc->ident = "autofswait";
	rw_init(&autofs_softc->sc_lock, "autofssclock");
	autofs_softc->sc_dev_opened = false;

	autofs_tmo_tq = taskq_create("autofstmo", 1, IPL_NONE, 0);
	KASSERT(autofs_tmo_tq);

	return 0;
}

static int
autofs_mount(struct mount *mp, const char *path, void *data,
    struct nameidata *ndp, struct proc *p)
{
	struct autofs_args *args = data;
	struct autofs_mount *amp = VFSTOAUTOFS(mp);
	struct statfs *sbp = &mp->mnt_stat;
	int error;

	if (mp->mnt_flag & MNT_UPDATE) {
		if (amp == NULL)
			return EIO;
		autofs_flush(amp);
		return 0;
	}

	if (!args)
		return EINVAL;
	if (amp != NULL)
		return EBUSY;

	/*
	 * Copy-in ->f_mntfromname string.
	 */
	memset(sbp->f_mntfromname, 0, sizeof(sbp->f_mntfromname));
	error = copyinstr(args->from, sbp->f_mntfromname,
	    sizeof(sbp->f_mntfromname), NULL);
	if (error)
		goto fail;
	strlcpy(sbp->f_mntonname, path, sizeof(sbp->f_mntonname));

	/*
	 * Allocate the autofs mount.
	 */
	amp = malloc(sizeof(*amp), M_TEMP, M_WAITOK | M_ZERO);
	mp->mnt_data = amp;
	amp->am_mp = mp;
	strlcpy(amp->am_from, sbp->f_mntfromname, sizeof(amp->am_from));
	strlcpy(amp->am_on, sbp->f_mntonname, sizeof(amp->am_on));

	/*
	 * Copy-in master_options string.
	 */
	error = copyinstr(args->master_options, amp->am_options,
	    sizeof(amp->am_options), NULL);
	if (error)
		goto fail;

	/*
	 * Copy-in master_prefix string.
	 */
	error = copyinstr(args->master_prefix, amp->am_prefix,
	    sizeof(amp->am_prefix), NULL);
	if (error)
		goto fail;

	/*
	 * Initialize the autofs mount.
	 */
	rw_init(&amp->am_lock, "autofsamlock");
	amp->am_last_ino = AUTOFS_ROOTINO;

	rw_enter_write(&amp->am_lock);
	error = autofs_node_new(NULL, amp, ".", -1, &amp->am_root);
	rw_exit_write(&amp->am_lock);
	KASSERT(!error);
	KASSERT(amp->am_root->an_ino == AUTOFS_ROOTINO);

	autofs_statfs(mp, sbp, NULL);
	vfs_getnewfsid(mp);

	return 0;
fail:
	mp->mnt_data = NULL;
	free(amp, M_TEMP, sizeof(*amp));

	return error;
}

static int
autofs_unmount(struct mount *mp, int mntflags, struct proc *p)
{
	struct autofs_mount *amp = VFSTOAUTOFS(mp);
	int error, flags;

	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;
	error = vflush(mp, NULL, flags);
	if (error) {
		AUTOFS_WARN("vflush failed with error %d", error);
		return error;
	}

	/*
	 * All vnodes are gone, and new one will not appear - so,
	 * no new triggerings.
	 */
	for (;;) {
		struct autofs_request *ar;
		int dummy;
		bool found;

		found = false;
		rw_enter_write(&autofs_softc->sc_lock);
		TAILQ_FOREACH(ar, &autofs_softc->sc_requests, ar_next) {
			if (ar->ar_mount != amp)
				continue;
			ar->ar_error = ENXIO;
			ar->ar_done = true;
			ar->ar_in_progress = false;
			found = true;
		}
		if (found == false) {
			rw_exit_write(&autofs_softc->sc_lock);
			break;
		}

		wakeup(autofs_softc->ident);
		rw_exit_write(&autofs_softc->sc_lock);

		tsleep(&dummy, PCATCH, "autofs_unmount", hz);
	}

	rw_enter_write(&amp->am_lock);
	while (!RB_EMPTY(&amp->am_root->an_children)) {
		struct autofs_node *anp;
		/*
		 * Force delete all nodes when more than one level of
		 * directories are created via indirect map. Autofs doesn't
		 * support rmdir(2), thus this is the only way to get out.
		 */
		anp = RB_MIN(autofs_node_tree, &amp->am_root->an_children);
		while (!RB_EMPTY(&anp->an_children))
			anp = RB_MIN(autofs_node_tree, &anp->an_children);
		autofs_node_delete(anp);
	}
	autofs_node_delete(amp->am_root);

	mp->mnt_data = NULL;
	rw_exit_write(&amp->am_lock);

	free(amp, M_TEMP, sizeof(*amp));

	return 0;
}

static int
autofs_start(struct mount *mp, int flags, struct proc *p)
{

	return 0;
}

static int
autofs_root(struct mount *mp, struct vnode **vpp)
{
	struct autofs_mount *amp;
	struct autofs_node *anp;

	amp = VFSTOAUTOFS(mp);
	anp = amp->am_root;

	rw_enter_write(&anp->an_node_lock);
	return autofs_node_vn(anp, mp, vpp);
}

static int
autofs_statfs(struct mount *mp, struct statfs *sbp, struct proc *p)
{

	sbp->f_bsize = S_BLKSIZE;
	sbp->f_iosize = 0;
	sbp->f_blocks = 0;
	sbp->f_bfree = 0;
	sbp->f_bavail = 0;
	sbp->f_files = 0;
	sbp->f_ffree = 0;
	sbp->f_favail = 0;

	copy_statfs_info(sbp, mp);

	return 0;
}

static int
autofs_sync(struct mount *mp, int waitfor, int stall, struct ucred *cred,
    struct proc *p)
{

	return 0;
}

static int
autofs_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen, struct proc *p)
{

	/* all sysctl names at this level are terminal */
	if (namelen != 1)
		return ENOTDIR;		/* overloaded */

	switch (name[0]) {
	case AUTOFS_DEBUG_TUNABLE:
		return sysctl_int(oldp, oldlenp, newp, newlen, &autofs_debug);
	case AUTOFS_MOUNT_ON_STAT:
		return sysctl_int(oldp, oldlenp, newp, newlen,
		    &autofs_mount_on_stat);
	case AUTOFS_TIMEOUT:
		return sysctl_int(oldp, oldlenp, newp, newlen, &autofs_timeout);
	case AUTOFS_CACHE:
		return sysctl_int(oldp, oldlenp, newp, newlen, &autofs_cache);
	case AUTOFS_RETRY_ATTEMPTS:
		return sysctl_int(oldp, oldlenp, newp, newlen,
		    &autofs_retry_attempts);
	case AUTOFS_RETRY_DELAY:
		return sysctl_int(oldp, oldlenp, newp, newlen,
		    &autofs_retry_delay);
	default:
		return EOPNOTSUPP;
	}
	return EINVAL;
}

struct vfsops autofs_vfsops = {
	autofs_mount,		/* vfs_mount */
	autofs_start,		/* vfs_start */
	autofs_unmount,		/* vfs_unmount */
	autofs_root,		/* vfs_root */
	(void *)eopnotsupp,	/* vfs_quotactl */
	autofs_statfs,		/* vfs_statfs */
	autofs_sync,		/* vfs_sync */
	(void *)eopnotsupp,	/* vfs_vget */
	(void *)eopnotsupp,	/* vfs_fhtovp */
	(void *)eopnotsupp,	/* vfs_vptofh */
	autofs_init,		/* vfs_init */
	autofs_sysctl,		/* vfs_sysctl */
	(void *)eopnotsupp,	/* vfs_checkexp */
};
