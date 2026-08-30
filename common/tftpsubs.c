/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "tftpsubs.h"
#include "pollset.h"

#define PKTSIZE MAX_SEGSIZE+4

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

/* When an error has occurred, it is possible that the two sides
 * are out of synch.  Ie: that what I think is the other side's
 * response to packet N is really their response to packet N-1.
 *
 * So, to try to prevent that, we flush all the input queued up
 * for us on the network connection on our host.
 *
 * We return the number of packets we flushed (mostly for reporting
 * when trace is active).
 */

int synchnet(int f)
{                               /* socket to flush */
    int pktcount = 0;
    char rbuf[PKTSIZE];
    union sock_addr from;
    socklen_t fromlen;
    struct pollset *set = pollset_add(NULL, f);

    while (pollset_poll(set, POLLSET_IN, 0) > 0) {
        /* Otherwise drain the packet */
        pktcount++;
        fromlen = sizeof(from);
        if (recvfrom(f, rbuf, sizeof(rbuf), 0, &from.sa, &fromlen) < 0)
            break;
    }

    pollset_free(&set);

    return pktcount;            /* Return packets drained */
}

int pick_port_bind(int sockfd, union sock_addr *myaddr,
                   unsigned int port_range_from,
                   unsigned int port_range_to)
{
    unsigned int port, firstport;
    int port_range = 0;

    if (port_range_from != 0 && port_range_to != 0) {
        port_range = 1;
    }

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
int is_numeric_ipv6(const char *p)
{
    /* A numeric IPv6 address consist at least of 2 ':' and
     * it may have sequences of hex-digits and maybe contain
     * a '.' from a IPv4 mapped address and maybe is enclosed in []
     * we do not check here, if it is a valid IPv6 address
     * only if is something like a numeric IPv6 address or something else
     */
    int colon = 0;
    int dot = 0;
    int bracket = 0;
    char c;

    if (!p)
        return 0;

    if (*p == '[') {
	bracket = 1;
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
	    return 0;		/* Invalid character */
	}
    }

    if (colon < 2 || colon > 7)
	return 0;

    if (dot) {
	/* An IPv4-mapped address in dot-quad form will have 3 dots */
	if (dot != 3)
	    return 0;
	/* The IPv4-mapped address takes the space of one colon */
	if (colon > 6)
	    return 0;
    }

    /* If bracketed, must be closed, and vice versa */
    if (bracket ^ (c == ']'))
	return 0;

    /* Otherwise, assume we're okay */
    return 1;
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
