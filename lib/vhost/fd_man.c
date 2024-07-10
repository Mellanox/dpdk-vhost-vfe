/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_string_fns.h>
#include <rte_thread.h>

#include "fd_man.h"
#include "vhost.h"
#include "vdpa_driver.h"

#define RTE_LOGTYPE_VHOST_FDMAN RTE_LOGTYPE_USER1

static void *fdset_event_dispatch(void *arg);

int
fdset_init(struct fdset *fdset, const char *name)
{
	int i;

	rte_strscpy(fdset->name, name, PATH_MAX);

	pthread_mutex_init(&fdset->fd_mutex, NULL);

	for (i = 0; i < (int)RTE_DIM(fdset->fd); i++) {
		fdset->fd[i].fd = -1;
		fdset->fd[i].dat = NULL;
	}
	LIST_INIT(&fdset->fdlist);

	/*
	 * Any non-zero value would work (see man epoll_create),
	 * but pass MAX_FDS for consistency.
	 */
	fdset->epfd = epoll_create(2);
	if (fdset->epfd < 0) {
		RTE_LOG(ERR, VHOST_FDMAN, "failed to create epoll for %s fdset\n", name);
		goto err_free;
	}

	if (pthread_create(&fdset->tid, NULL, fdset_event_dispatch, fdset)) {
		RTE_LOG(ERR, VHOST_FDMAN, "Failed to create %s event dispatch thread\n",
				fdset->name);
		goto err_epoll;
	}

	rte_thread_setname(fdset->tid, fdset->name);
	return 0;

err_epoll:
	close(fdset->epfd);
err_free:
	rte_free(fdset);
	return -1;
}

void
fdset_destroy(struct fdset *fdset)
{
	fdset->destroy = true;
	pthread_cancel(fdset->tid);
	pthread_join(fdset->tid, NULL);
	close(fdset->epfd);
}

static int
fdset_insert_entry(struct fdset *pfdset, int fd, fd_cb rcb, fd_cb wcb, void *dat, bool check_timeout)
{
	struct fdentry *pfdentry;

	if (pfdset->next_free_idx >= (int)RTE_DIM(pfdset->fd))
		return -1;

	pfdentry = &pfdset->fd[pfdset->next_free_idx];
	pfdentry->fd  = fd;
	pfdentry->rcb = rcb;
	pfdentry->wcb = wcb;
	pfdentry->dat = dat;
	pfdentry->check_timeout = check_timeout;

	LIST_INSERT_HEAD(&pfdset->fdlist, pfdentry, next);

	/* Find next free slot */
	pfdset->next_free_idx++;
	for (; pfdset->next_free_idx < (int)RTE_DIM(pfdset->fd); pfdset->next_free_idx++) {
		if (pfdset->fd[pfdset->next_free_idx].fd != -1)
			continue;
		break;
	}

	return 0;
}

static void
fdset_remove_entry(struct fdset *pfdset, struct fdentry *pfdentry)
{
	int entry_idx;

	pfdentry->fd = -1;
	pfdentry->rcb = pfdentry->wcb = NULL;
	pfdentry->dat = NULL;

	entry_idx = pfdentry - pfdset->fd;
	if (entry_idx < pfdset->next_free_idx)
		pfdset->next_free_idx = entry_idx;

	LIST_REMOVE(pfdentry, next);
}

static struct fdentry *
fdset_find_entry_locked(struct fdset *pfdset, int fd)
{
	struct fdentry *pfdentry;

	LIST_FOREACH(pfdentry, &pfdset->fdlist, next) {
		if (pfdentry->fd != fd)
			continue;
		return pfdentry;
	}

	return NULL;
}

/**
 * Register the fd in the fdset with read/write handler and context.
 */
int
fdset_add(struct fdset *pfdset, int fd, fd_cb rcb, fd_cb wcb, void *dat, bool check_timeout)
{
	struct epoll_event ev;
	struct fdentry *pfdentry;
	int ret = 0;

	if (pfdset == NULL || fd == -1) {
		ret = -1;
		goto out;
	}

	pthread_mutex_lock(&pfdset->fd_mutex);
	ret = fdset_insert_entry(pfdset, fd, rcb, wcb, dat, check_timeout);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_FDMAN, "failed to insert fdset entry\n");
		pthread_mutex_unlock(&pfdset->fd_mutex);
		goto out;
	}
	pthread_mutex_unlock(&pfdset->fd_mutex);

	ev.events = EPOLLERR;
	ev.events |= rcb ? EPOLLIN : 0;
	ev.events |= wcb ? EPOLLOUT : 0;
	ev.data.fd = fd;

	ret = epoll_ctl(pfdset->epfd, EPOLL_CTL_ADD, fd, &ev);
	if (ret < 0) {
		RTE_LOG(ERR, VHOST_FDMAN, "could not add %d fd to %d epfd: %s\n",
			fd, pfdset->epfd, strerror(errno));
		goto out_remove;
	}

	return 0;
out_remove:
	pthread_mutex_lock(&pfdset->fd_mutex);
	pfdentry = fdset_find_entry_locked(pfdset, fd);
	if (pfdentry)
		fdset_remove_entry(pfdset, pfdentry);
	pthread_mutex_unlock(&pfdset->fd_mutex);
out:
	return ret;
}

static void
fdset_del_locked(struct fdset *pfdset, struct fdentry *pfdentry)
{
	if (epoll_ctl(pfdset->epfd, EPOLL_CTL_DEL, pfdentry->fd, NULL) == -1)
		RTE_LOG(WARNING, VHOST_FDMAN, "could not remove %d fd from %d epfd: %s\n",
			pfdentry->fd, pfdset->epfd, strerror(errno));

	fdset_remove_entry(pfdset, pfdentry);
}

void
fdset_del(struct fdset *pfdset, int fd)
{
	struct fdentry *pfdentry;

	if (pfdset == NULL || fd == -1)
		return;

	do {
		pthread_mutex_lock(&pfdset->fd_mutex);
		pfdentry = fdset_find_entry_locked(pfdset, fd);
		if (pfdentry != NULL && pfdentry->busy == 0) {
			fdset_del_locked(pfdset, pfdentry);
			pfdentry = NULL;
		}
		pthread_mutex_unlock(&pfdset->fd_mutex);
	} while (pfdentry != NULL);
}

/**
 *  Unregister the fd from the fdset.
 *
 *  If parameters are invalid, return directly -2.
 *  And check whether fd is busy, if yes, return -1.
 *  Otherwise, try to delete the fd from fdset and
 *  return true.
 */
int
fdset_try_del(struct fdset *pfdset, int fd)
{
	struct fdentry *pfdentry;

	if (pfdset == NULL || fd == -1)
		return -2;

	pthread_mutex_lock(&pfdset->fd_mutex);
	pfdentry = fdset_find_entry_locked(pfdset, fd);
	if (pfdentry != NULL && pfdentry->busy != 0) {
		pthread_mutex_unlock(&pfdset->fd_mutex);
		return -1;
	}

	if (pfdentry != NULL)
		fdset_del_locked(pfdset, pfdentry);

	pthread_mutex_unlock(&pfdset->fd_mutex);
	return 0;
}

/**
 * This functions runs in infinite blocking loop until there is no fd in
 * pfdset. It calls corresponding r/w handler if there is event on the fd.
 *
 * Before the callback is called, we set the flag to busy status; If other
 * thread(now rte_vhost_driver_unregister) calls fdset_del concurrently, it
 * will wait until the flag is reset to zero(which indicates the callback is
 * finished), then it could free the context after fdset_del.
 */
static void *
fdset_event_dispatch(void *arg)
{
	int i;
	fd_cb rcb, wcb;
	void *dat;
	int fd, numfds;
	int remove1, remove2;
	struct fdset *pfdset = arg;
	struct rte_vdpa_device *vdpa_dev;
	struct timeval time;
	double time_passed;
	struct vhost_user_socket *vsock;

	if (pfdset == NULL)
		return NULL;

	while (1) {
		struct epoll_event events[MAX_FDS];
		struct fdentry *pfdentry;

		numfds = epoll_wait(pfdset->epfd, events, RTE_DIM(events), 1000);
		if (numfds < 0)
			continue;

		for (i = 0; i < numfds; i++) {
			pthread_mutex_lock(&pfdset->fd_mutex);

			fd = events[i].data.fd;
			pfdentry = fdset_find_entry_locked(pfdset, fd);
			if (pfdentry == NULL) {
				pthread_mutex_unlock(&pfdset->fd_mutex);
				continue;
			}

			if (pfdentry->check_timeout) {
				vsock = (struct vhost_user_socket *)pfdentry->dat;
				if (vsock->timeout_enabled) {
					gettimeofday(&time, NULL);
					time_passed = (time.tv_sec - vsock->timestamp.tv_sec) * 1e6;
					time_passed = (time_passed + (time.tv_usec - vsock->timestamp.tv_usec)) * 1e-6;
					if (time_passed > VHOST_SOCK_TIME_OUT) {
						pthread_mutex_lock(&vsock->vdpa_dev_mutex);
						vdpa_dev = vsock->vdpa_dev;
						if (vdpa_dev)
							vdpa_dev->ops->mem_tbl_cleanup(vdpa_dev);
						pthread_mutex_unlock(&vsock->vdpa_dev_mutex);
						vsock->timeout_enabled = false;
					}
				}
			}

			remove1 = remove2 = 0;

			rcb = pfdentry->rcb;
			wcb = pfdentry->wcb;
			dat = pfdentry->dat;
			pfdentry->busy = 1;

			if (pfdentry->check_timeout) {
				vsock = (struct vhost_user_socket *)pfdentry->dat;
				vsock->timeout_enabled = false;
			}

			pthread_mutex_unlock(&pfdset->fd_mutex);

			if (rcb && events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP))
				rcb(fd, dat, &remove1);
			if (wcb && events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
				wcb(fd, dat, &remove2);
			pfdentry->busy = 0;
			/*
			 * fdset_del needs to check busy flag.
			 * We don't allow fdset_del to be called in callback
			 * directly.
			 */
			/*
			 * A concurrent fdset_del may have been waiting for the
			 * fdentry not to be busy, so we can't call
			 * fdset_del_locked().
			 */
			if (remove1 || remove2)
				fdset_del(pfdset, fd);
		}

		if (pfdset->destroy)
			break;
	}

	return NULL;
}
