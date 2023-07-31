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
 * $FreeBSD$
 */

#ifndef _AUTOFS_AUTOFS_H_
#define _AUTOFS_AUTOFS_H_

/*
 * sysctl(2) defines
 * XXX AUTOFS_INTERRUPTIBLE not implemented.
 */
#define AUTOFS_DEBUG_TUNABLE	1 /* Enable debug messages */
#define AUTOFS_MOUNT_ON_STAT	2 /* Trigger mount on stat(2) on mountpoint */
#define AUTOFS_TIMEOUT		3 /* Number of seconds to wait for automountd(8) */
#define AUTOFS_CACHE		4 /* Number of seconds to wait before reinvoking */
#define AUTOFS_RETRY_ATTEMPTS	5 /* Number of attempts before failing mount */
#define AUTOFS_RETRY_DELAY	6 /* Number of seconds before retrying */
//#define AUTOFS_INTERRUPTIBLE	7 /* Allow requests to be interrupted by signal */
#define AUTOFS_MAXID		7 /* Number of valid autofs ids */

#define AUTOFS_NAMES { \
	{ 0, 0 }, \
	{ "autofs_debug", CTLTYPE_INT }, \
	{ "autofs_mount_on_stat", CTLTYPE_INT }, \
	{ "autofs_timeout", CTLTYPE_INT }, \
	{ "autofs_cache", CTLTYPE_INT }, \
	{ "autofs_retry_attempts", CTLTYPE_INT }, \
	{ "autofs_retry_delay", CTLTYPE_INT }, \
}

#ifdef _KERNEL

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/pool.h>
#include <sys/malloc.h>
#include <sys/tree.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/proc.h>

#include "autofs_ioctl.h"

#define AUTOFS_ROOTINO	((ino_t)1)
#define VFSTOAUTOFS(mp)	((struct autofs_mount *)((mp)->mnt_data))
#define VTOI(vp)	((struct autofs_node *)((vp)->v_data))

extern struct pool	autofs_request_pool;
extern struct pool	autofs_node_pool;
extern struct autofs_softc	*autofs_softc;
extern struct taskq	*autofs_tmo_tq;

/*
 * sysctl(2) tunables
 */
extern int	autofs_debug;
extern int	autofs_mount_on_stat;
extern int	autofs_timeout;
extern int	autofs_cache;
extern int	autofs_retry_attempts;
extern int	autofs_retry_delay;
extern int	autofs_interruptible;

#define AUTOFS_DEBUG(X, ...)				\
	do {						\
		if (autofs_debug > 1)			\
			printf("%s: " X "\n",		\
			    __func__, ## __VA_ARGS__);	\
	} while (0)

#define AUTOFS_WARN(X, ...)				\
	do {						\
		if (autofs_debug > 0) {			\
			printf("WARNING: %s: " X "\n",	\
			    __func__, ## __VA_ARGS__);	\
		}					\
	} while (0)

/*
 * APRINTF is only for debugging.
 */
#define APRINTF(X, ...)					\
	printf("### %s(%s|%d): " X, __func__,		\
	    curproc->p_p->ps_comm, curproc->p_tid, ## __VA_ARGS__)

struct autofs_node {
	RB_ENTRY(autofs_node)		an_entry;
	char				*an_name;
	ino_t				an_ino;
	struct autofs_node		*an_parent;
	RB_HEAD(autofs_node_tree,
	    autofs_node)		an_children;
	struct autofs_mount		*an_mount;
	struct vnode			*an_vnode;
	struct rwlock			an_node_lock;
	struct rrwlock			an_vn_lock; /* vop */
	bool				an_cached;
	bool				an_wildcards;
	struct timeout			an_timeout;
	int				an_retries;
	struct timespec			an_ctime;
};

struct autofs_mount {
	TAILQ_ENTRY(autofs_mount)	am_next;
	struct autofs_node		*am_root;
	struct mount			*am_mp;
	struct rwlock			am_lock;
	char				am_from[AUTOFS_MAXPATHLEN];
	char				am_on[AUTOFS_MAXPATHLEN];
	char				am_options[AUTOFS_MAXPATHLEN];
	char				am_prefix[AUTOFS_MAXPATHLEN];
	ino_t				am_last_ino;
};

struct autofs_request {
	struct task			ar_tk; /* must be first */
	TAILQ_ENTRY(autofs_request)	ar_next;
	struct autofs_mount		*ar_mount;
	int				ar_id;
	bool				ar_done;
	int				ar_error;
	bool				ar_wildcards;
	bool				ar_in_progress;
	char				ar_from[AUTOFS_MAXPATHLEN];
	char				ar_path[AUTOFS_MAXPATHLEN];
	char				ar_prefix[AUTOFS_MAXPATHLEN];
	char				ar_key[AUTOFS_MAXPATHLEN];
	char				ar_options[AUTOFS_MAXPATHLEN];
	struct timeout			ar_timeout;
	volatile u_int			ar_refcount;
};

struct autofs_softc {
	char				*ident;
	struct rwlock			sc_lock;
	TAILQ_HEAD(, autofs_request)	sc_requests;
	bool				sc_dev_opened;
	pid_t				sc_dev_sid;
	int				sc_last_request_id;
};

int	autofs_trigger(struct autofs_node *anp, const char *component,
	    int componentlen);
bool	autofs_cached(struct autofs_node *anp, const char *component,
	    int componentlen);
void	autofs_flush(struct autofs_mount *amp);
bool	autofs_ignore_thread(void);
int	autofs_node_new(struct autofs_node *parent, struct autofs_mount *amp,
	    const char *name, int namelen, struct autofs_node **anpp);
int	autofs_node_find(struct autofs_node *parent,
	    const char *name, int namelen, struct autofs_node **anpp);
void	autofs_node_delete(struct autofs_node *anp);
int	autofs_node_vn(struct autofs_node *anp, struct mount *mp,
	    struct vnode **vpp);
char	*kstrdup(const char *str);
char	*kstrndup(const char *str, size_t maxlen);

static __inline void
autofs_node_cache(struct autofs_node *anp)
{
	anp->an_cached = true;
}

static __inline void
autofs_node_uncache(struct autofs_node *anp)
{
	anp->an_cached = false;
}

RB_PROTOTYPE(autofs_node_tree, autofs_node, an_entry, autofs_node_cmp);

#endif /* _KERNEL */
#endif /* !_AUTOFS_AUTOFS_H_ */
