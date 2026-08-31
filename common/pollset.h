/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 * All rights reserved.
 */

#ifndef TFTP_POLL_H
#define TFTP_POLL_H 1

#include "config.h"

#ifdef HAVE_POLL_H

struct pollset {
    nfds_t nfds;
    nfds_t size;
    struct pollfd *fds;
};

typedef nfds_t pollset_cursor;

#define POLLSET_IN	(POLLIN | POLLHUP | POLLERR)
#define POLLSET_OUT	(POLLOUT | POLLERR)
#define POLLSET_EX	(POLLPRI)

bool pollset_has(const struct pollset *set, int fd);

#else

#define POLLSET_FDSETS 3		/* read, write, err */

struct pollset {
    int nfds;
    fd_set fdset;
    fd_set rfdset[POLLSET_FDSETS];	/* No "all" set included */
};

typedef int pollset_cursor;

#define POLLSET_IN	1
#define POLLSET_OUT	2
#define POLLSET_EX	4

static inline bool pollset_has(const struct pollset *set, int fd)
{
    return !!FD_ISSET(fd, &set->fdset);
}

#endif

static inline bool pollset_isempty(const struct pollset *set)
{
    return set->nfds <= 0;
}

struct pollset *pollset_new(void);
struct pollset *pollset_add(struct pollset *set, int fd);
int pollset_next(const struct pollset *set, pollset_cursor *cursor, int *whatp);
int pollset_poll(struct pollset *set, int what, intmax_t utimeout);

/*
 * pollset_close() is like pollset_free(), but closes file descriptors
 * in the set.
 */
int pollset_close(struct pollset **setp);
void pollset_free(struct pollset **setp);

/*
 * Signals that should be masked around the poll, to avoid possibly lost
 * EINTR.
 */
void pollset_sigmask_add(int sig);
void pollset_sigmask_clear(void);

/*
 * pollset_notify_signal() should be called at the end of a signal
 * handler to force the next pollset_poll() to return EINTR if set.
 * It may or may not return.
 */
void pollset_notify_signal(int sig);

#endif /* TFTP_POLLSET_H */
