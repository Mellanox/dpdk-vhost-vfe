/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#ifndef _FD_MAN_H_
#define _FD_MAN_H_
#include <pthread.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/queue.h>

#define MAX_FDS 2048

typedef void (*fd_cb)(int fd, void *dat, int *remove);

struct fdentry {
	int fd;		/* -1 indicates this entry is empty */
	fd_cb rcb;	/* callback when this fd is readable. */
	fd_cb wcb;	/* callback when this fd is writeable.*/
	void *dat;	/* fd context */
	int busy;	/* whether this entry is being used in cb. */
	bool check_timeout; /* whether to check connection timeout */
	LIST_ENTRY(fdentry) next;
};

struct fdset {
	char name[PATH_MAX];
	int epfd;
	struct fdentry fd[MAX_FDS];
	LIST_HEAD(, fdentry) fdlist;
	int next_free_idx;
	pthread_t tid;
	pthread_mutex_t fd_mutex;
	bool destroy;
};

int fdset_init(struct fdset *fdset, const char *name);

void fdset_destroy(struct fdset *fdset);

int fdset_add(struct fdset *pfdset, int fd,
	fd_cb rcb, fd_cb wcb, void *dat, bool check_timeout);

void fdset_del(struct fdset *pfdset, int fd);

int fdset_try_del(struct fdset *pfdset, int fd);

#endif
