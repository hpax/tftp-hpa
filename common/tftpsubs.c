/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "tftpsubs.h"
#include "pollset.h"

int segsize = SEGSIZE;          /* Default segsize */

#define TFTP_SOCKET_BUFFER_MIN	(256U * 1024U)
#define TFTP_SOCKET_BUFFER_MAX	(4U * 1024U * 1024U)
#define TFTP_SOCKET_BUFFER_WINDOWS 2

/* A somewhat conservative estimate for all packet overhead */
#define TFTP_SOCKET_BUFFER_OVERHEAD	256

void tftp_set_socket_buffers(int fd, unsigned int blocksize,
                             unsigned int windowsize, bool is_send)
{
#if defined(SO_SNDBUF) && defined(SO_RCVBUF)
    int whichbuf = is_send ? SO_SNDBUF : SO_RCVBUF;
    size_t packetsize = (size_t)blocksize + TFTP_SOCKET_BUFFER_OVERHEAD;
    unsigned int buffersize;

    buffersize = windowsize * packetsize * TFTP_SOCKET_BUFFER_WINDOWS;

    if (buffersize > TFTP_SOCKET_BUFFER_MAX)
        buffersize = TFTP_SOCKET_BUFFER_MAX;

    if (buffersize <= TFTP_SOCKET_BUFFER_MIN)
        return;

    setsockint(fd, SOL_SOCKET, whichbuf, buffersize);
#else
    (void)fd;
    (void)blocksize;
    (void)windowsize;
    (void)is_send;
#endif
}

#ifndef MSG_DONTWAIT
static int set_socket_nonblock(int fd, bool flag)
{
    int socket_flags;

#if defined(HAVE_FCNTL) && (O_NONBLOCK != 0)
    socket_flags = fcntl(fd, F_GETFL, 0);
    if (socket_flags < 0)
        return -1;

    if (flag)
        socket_flags |= O_NONBLOCK;
    else
        socket_flags &= ~O_NONBLOCK;

    return fcntl(fd, F_SETFL, socket_flags);
#else
    socket_flags = flag ? 1 : 0;
    return ioctl(fd, FIONBIO, &socket_flags);
#endif
}
#endif

/*
 * Receive a packet with a synchronous timeout.  The remaining timeout is
 * updated after interrupted polls and discarded packets so a receive attempt
 * cannot extend its deadline.
 */
int tftp_recv_time(int s, void *rbuf, int len, unsigned int flags,
                   struct sockaddr *from, socklen_t *fromlen,
                   unsigned long *timeout_us_p)
{
    struct timeval t0, t1;
    int rv, err = errno;
    intmax_t timeout_us = *timeout_us_p;
    intmax_t timeout_left, dt;
    struct pollset *set = pollset_add(NULL, s);

    gettimeofday(&t0, NULL);
    timeout_left = timeout_us;

    do {
        do {
            rv = pollset_poll(set, POLLSET_IN, timeout_left);
            err = errno;

            gettimeofday(&t1, NULL);

            dt = (t1.tv_sec - t0.tv_sec) * (intmax_t)1000000 +
                 (t1.tv_usec - t0.tv_usec);
            *timeout_us_p = timeout_left =
                (dt >= timeout_us) ? 1 : (timeout_us - dt);
        } while (rv == -1 && err == EINTR);

        if (rv == 0) {
            err = ETIMEDOUT;
            rv = -1;
            break;
        }

#ifdef MSG_DONTWAIT
        rv = recvfrom(s, rbuf, len, flags | MSG_DONTWAIT, from, fromlen);
        err = errno;
#else
        if (set_socket_nonblock(s, true) < 0) {
            err = errno;
            rv = -1;
            break;
        }
        rv = recvfrom(s, rbuf, len, flags, from, fromlen);
        err = errno;
        if (set_socket_nonblock(s, false) < 0 && rv >= 0) {
            err = errno;
            rv = -1;
        }
#endif
    } while (rv < 0 && (E_WOULD_BLOCK(err) || err == EINTR));

    pollset_free(&set);
    if (rv < 0)
        errno = err;
    return rv;
}

int pick_port_bind(int sockfd, union sock_addr *myaddr,
                   unsigned int port_range_from,
                   unsigned int port_range_to)
{
    unsigned int port, firstport;
    bool port_range;

    port_range = port_range_from != 0 && port_range_to != 0;

    firstport = port_range
        ? port_range_from + rand() % (port_range_to - port_range_from + 1)
        : 0;

    port = firstport;

    do {
        sa_set_port(myaddr, htons(port));
        if (bind(sockfd, &myaddr->sa, SOCKLEN(myaddr)) < 0) {
            /* Some versions of Linux return EINVAL instead of EADDRINUSE */
            if (!(port_range && (errno == EINVAL || errno == EADDRINUSE)))
                return -1;

            /* Normally, we shouldn't have to loop, but some situations involving
               aborted transfers make it possible. */
        } else {
            return 0;
        }

        port++;
        if (port > port_range_to)
            port = port_range_from;
    } while (port != firstport);

    return -1;
}

/*
 * Extract a sock_addr for a specific host. If "early" is set, this is
 * a socket intended to be bound as a standalone listening socket, and
 * should be bound to a specified address even if it is not yet configured
 * (e.g. due to network initialization delays.)
 */
int set_sock_addr(char *host, union sock_addr *s, char **name, bool early)
{
    struct addrinfo *addrResult;
    struct addrinfo hints;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = s->sa.sa_family;
    hints.ai_flags = AI_CANONNAME;
    hints.ai_flags |= early ? AI_PASSIVE : AI_ADDRCONFIG;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    err = getaddrinfo(strip_address(host), NULL, &hints, &addrResult);
    if (err)
        return err;
    if (addrResult == NULL)
        return EAI_NONAME;
    memcpy(s, addrResult->ai_addr, addrResult->ai_addrlen);
    if (name) {
        if (addrResult->ai_canonname)
            *name = xstrdup(addrResult->ai_canonname);
        else
            *name = xstrdup(host);
    }
    freeaddrinfo(addrResult);
    return 0;
}

#ifdef HAVE_IPV6
bool is_numeric_ipv6(const char *p)
{
    /* A numeric IPv6 address consist at least of 2 ':' and
     * it may have sequences of hex-digits and maybe contain
     * a '.' from a IPv4 mapped address and maybe is enclosed in []
     * we do not check here, if it is a valid IPv6 address
     * only if is something like a numeric IPv6 address or something else
     */
    int colon = 0;
    int dot = 0;
    bool bracket = false;
    char c;

    if (!p)
        return false;

    if (*p == '[') {
	bracket = true;
	p++;
    }

    while ((c = *p++) && c != ']') {
	switch (c) {
	case ':':
	    colon++;
	    break;
	case '.':
	    dot++;
	    break;
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
	case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
	case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
	    break;
	default:
	    return false;		/* Invalid character */
	}
    }

    if (colon < 2 || colon > 7)
	return false;

    if (dot) {
	/* An IPv4-mapped address in dot-quad form will have 3 dots */
	if (dot != 3)
	    return false;
	/* The IPv4-mapped address takes the space of one colon */
	if (colon > 6)
	    return false;
    }

    /* If bracketed, must be closed, and vice versa */
    if (bracket ^ (c == ']'))
	return false;

    /* Otherwise, assume we're okay */
    return true;
}

/* strip [] from numeric IPv6 addreses */

char *strip_address(char *addr)
{
    char *p;

    if (is_numeric_ipv6(addr) && (*addr == '[')) {
        p = addr + strlen(addr);
        p--;
        if (*p == ']') {
            *p = 0;
            addr++;
        }
    }
    return addr;
}
#endif

/*
 * Get a descriptor to /dev/null. If none is available, open one.
 * If the descriptor comes back as < 3, do it again (backfill closed
 * standard file descriptors.)
 */
static int nullfd = -2;
static void close_nullfd(void)
{
    if (nullfd >= 0)
        close(nullfd);
}

int get_nullfd(void)
{
    if (nullfd > -1)
        return nullfd;

    while (nullfd < 3) {
        nullfd = open(_PATH_DEVNULL, O_RDWR);
        if (nullfd < 0)
            return nullfd;
    }

    atexit(close_nullfd);

#ifdef FD_CLOEXEC
    {
        int flags = fcntl(nullfd, F_GETFL, 0);
        if (flags >= 0) {
            flags |= FD_CLOEXEC;
            fcntl(nullfd, F_SETFL, flags);
        }
    }
#endif

    return nullfd;
}
