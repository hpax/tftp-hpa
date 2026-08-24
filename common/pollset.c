/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 * All rights reserved.
 */

#include "config.h"
#include "pollset.h"
#include "tftpsubs.h"           /* For xcalloc()/xfree() */

struct pollset *pollset_new(void)
{
    struct pollset *ptr = xcalloc(1, sizeof(struct pollset));
#ifdef HAVE_POLL_H
    /*
     * On a typical implementation, struct pollfd is 8 bytes long.
     * Typical malloc() datums are 32 or 64 bytes, so consuming 64
     * bytes at the start is pretty reasonable, and will cover the
     * vast majority of all use cases.
     */
    size_t bytes;
    ptr->size = 8;
    bytes = ptr->size * sizeof(*ptr->fds);
    ptr->fds = xmalloc(bytes);

    /* Not really necessary, but out of paranoia... */
    memset(ptr->fds, -1, bytes);
#else
    /* Handle the remote possibility that FD_ZERO() isn't all zero */
    FD_ZERO(&ptr->fdset);
#endif
    return ptr;
}

void pollset_free(struct pollset **setp)
{
    struct pollset *set = *setp;
    *setp = NULL;
#ifdef HAVE_POLL_H
    xfree(set->fds);
#endif
    xfree(set);
}

#ifdef HAVE_POLL_H

/*
 * Return the index in the pollset::fds array where either a file
 * descriptor *is*, or where it should be inserted if it isn't already
 * included.
 */
struct pollset_found {
    bool found;
    size_t i;
};

static struct pollset_found pollset_find(const struct pollset *set, int fd)
{
    size_t l = 0;               /* Lowest possible position */
    size_t h = set->nfds;       /* Highest possible position + 1 */
    struct pollset_found found;

    while (l < h) {
        size_t i = (l+h) >> 1;
        if (set->fds[i].fd < fd) {
            l = i+1;
        } else if (set->fds[i].fd == fd) {
            /* Exact match */
            found.found = true;
            found.i = i;
            return found;
        } else {
            h = i;
        }
    }

    /* No exact match, l will point to the insertion index point */
    found.found = false;
    found.i = l;
    return found;
}

struct pollset *pollset_add(struct pollset *set, int fd)
{
    struct pollset_found found;

    if (!set)
        set = pollset_new();

    if (fd < 0)                 /* Invalid file descriptor */
        return set;

    /*
     * Check for duplicates, and where in a sorted list to insert the
     * value.
     */
    found = pollset_find(set, fd);
    if (found.found)
        return set;             /* Already in set */

    if (set->nfds >= set->size) {
        size_t oldsize = set->size;
        size_t newsize = set->size = oldsize << 1;
        set->fds = xrealloc(set->fds, newsize * sizeof(*set->fds));

        /* Not really necessary, but out of paranoia... */
        memset(set->fds + oldsize, -1, (newsize-oldsize) * sizeof(*set->fds));
    }

    if (found.i < (size_t)set->nfds) {
        memmove(&set->fds[found.i+1], &set->fds[found.i],
                (set->nfds - found.i) * sizeof(*set->fds));
    }

    set->fds[found.i].fd = fd;
    set->nfds++;
    return set;
}

int pollset_next(const struct pollset *set, pollset_cursor *cursor, int *whatp)
{
    int what;
    pollset_cursor i = *cursor;

    what = whatp ? *whatp : 0;

    while (i < set->nfds) {
        if (whatp) {
            int whatflags = what & set->fds[i].revents;
            if (whatflags) {
                *whatp = whatflags;
                *cursor = i+1;
                return set->fds[i].fd;
            }
        } else {
            *cursor = i+1;
            return set->fds[i].fd;
        }
        i++;
    }

    *cursor = set->nfds;
    return -1;
}

int pollset_poll(struct pollset *set, int what, intmax_t utimeout)
{
    size_t i;

#ifdef HAVE_PPOLL
    struct timespec ts, *tsp = NULL;
    if (utimeout >= 0) {
        ts.tv_sec  =  utimeout / 1000000;
        ts.tv_nsec = (utimeout % 1000000) * 1000;
        tsp = &ts;
    }

    for (i = 0; i < set->nfds; i++)
        set->fds[i].events = what;

    return ppoll(set->fds, set->nfds, tsp, NULL);
#else
    int timeout = -1;
    if (utimeout >= 0) {
        utimeout = (utimeout + 999)/1000;
        timeout = (utimeout > INT_MAX) ? INT_MAX : utimeout;
    }

    for (i = 0; i < set->nfds; i++)
        set->fds[i].events = what;

    return poll(set->fds, set->nfds, timeout);
#endif
}

int pollset_close(struct pollset **setp)
{
    struct pollset *set = *setp;
    size_t i;
    int err = 0;

    for (i = 0; i < set->nfds; i++)
        err |= close(set->fds[i].fd);

    pollset_free(setp);

    return err;
}

bool pollset_has(const struct pollset *set, int fd)
{
    return pollset_find(set, fd).found;
}

#else /* no poll(), using select() */

struct pollset *pollset_add(struct pollset *set, int fd)
{
    if (!set)
        set = pollset_new();

    /* select() has a maximum supported file descriptor count */
    if ((unsigned int)fd >= FD_SETSIZE)
        return set;

    if (fd >= set->nfds)
        set->nfds = fd + 1;

    FD_SET(fd, &set->fdset);

    return set;
}

static int fd_what(const struct pollset *set, int fd)
{
    int what = 0;
    size_t i;

    if (fd >= set->nfds)
        return 0;

    for (i = 0; i < POLLSET_FDSETS; i++)
        if (FD_ISSET(fd, &set->rfdset[i]))
            what |= 1 << i;

    return what;
}

int pollset_next(const struct pollset *set, pollset_cursor *cursor, int *whatp)
{
    int what;
    pollset_cursor i = *cursor;

    what = whatp ? *whatp : 0;

    while (i < set->nfds) {
        if (FD_ISSET(i, &set->fdset)) {
            if (whatp) {
                int whatflags = what & fd_what(set, i);
                if (whatflags) {
                    *whatp = whatflags;
                    *cursor = i+1;
                    return i;
                }
            } else {
                *cursor = i+1;
                return i;
            }
        }
        i++;
    }

    *cursor = set->nfds;
    return -1;
}

int pollset_poll(struct pollset *set, int what, intmax_t utimeout)
{
    struct timeval tv, *tvp = NULL;
    fd_set *sets[3] = { NULL, NULL, NULL };
    size_t i;
    const size_t nsets = POLLSET_FDSETS < 3 ? POLLSET_FDSETS : 3;

    if (utimeout >= 0) {
        tv.tv_sec  = utimeout / 1000000;
        tv.tv_usec = utimeout % 1000000;
        tvp = &tv;
    }

    for (i = 0; i < nsets; i++) {
        if (what & (1 << i)) {
            sets[i] = &set->rfdset[i];
            memcpy(&set->rfdset[i], &set->fdset, sizeof set->fdset);
        } else {
            FD_ZERO(&set->rfdset[i]);
        }
    }

    return select(set->nfds, sets[0], sets[1], sets[2], tvp);
}

int pollset_close(struct pollset **setp)
{
    struct pollset *set = *setp;
    size_t i;
    int err = 0;

    for (i = 0; i < (size_t)set->nfds; i++)
        if (FD_ISSET(i, &set->fdset))
            err |= close(i);

    pollset_free(setp);

    return err;
}

#endif
