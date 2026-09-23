/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "config.h"
#include "options.h"
#include "tftpsubs.h"
#include "pollset.h"
#include "clock.h"

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
 * Try to get the MTU of a connected socket, adjusted to account
 * for TFTP overhead.
 */
unsigned int tftp_max_blksize(int fd, const union sock_addr *sa)
{
    int mtu;

    if (xopt.blksize <= 0) {
        int adjust = xopt.blksize;

        mtu = -1;
        errno = 0;

        switch (sa->sa.sa_family) {
        case AF_INET:
            adjust -= 20 + 8 + 4;
#ifdef IP_MTU
            mtu = getsockint(fd, IPPROTO_IP, IP_MTU);
#endif
            break;
#ifdef HAVE_IPV6
        case AF_INET6:
            adjust -= 40 + 8 + 4;
#ifdef IPV6_MTU
            mtu = getsockint(fd, IPPROTO_IPV6, IPV6_MTU);
#endif
            break;
#endif
        default:                /* What is this?! */
            break;
        }

        /* If querying the MTU failed, guess Ethernet */
        if (errno || mtu < 0)
            mtu = 1500;

        mtu += adjust;

        /* Don't allow MTU-based size to drop below SEGSIZE */
        if (mtu < SEGSIZE)
            mtu = SEGSIZE;
    } else {
        mtu = xopt.blksize;     /* Absolute */

        /* Don't allow dropping below MIN_SEGSIZE */
        if (mtu < MIN_SEGSIZE)
            mtu = MIN_SEGSIZE;
    }

    /* Obviously cannot be beyond MAX_SEGSIZE */
    if (mtu > MAX_SEGSIZE)
        mtu = MAX_SEGSIZE;

    return mtu;
}

bool parse_blocksize_arg(const char *str, unsigned int minimum)
{
    char *vp;
    bool ok = false;
    unsigned long v;
    int blksize = 0;

    if (ascii_strncaseeq(str, "mtu", 3)) {
        switch (str[3]) {
        case '\0':
            blksize = 0;
            ok = true;
            break;
        case '-':
            v = strtoul(str+4, &vp, 10);
            blksize = -v;
            ok = !*vp && (v <= INT_MAX/2);
            break;
        default:
            ok = true;
            break;
        }
    } else if (*str) {
        blksize = v = strtoul(str, &vp, 10);
        ok = v >= minimum && v <= MAX_SEGSIZE && !*vp;
    }

    if (ok)
        xopt.blksize = blksize;

    return ok;
}

/*
 * Wrappers for getsockopt() for the case where the option is an int.
 */
int getsockint(int sockfd, int level, int optname)
{
    int val = -1;
    socklen_t optlen = sizeof val;

    if (getsockopt(sockfd, level, optname, &val, &optlen) ||
        (size_t)optlen != sizeof val)
        return -1;

    return val;
}

/*
 * Receive a packet with a synchronous timeout.  The remaining timeout is
 * updated after interrupted polls and discarded packets so a receive attempt
 * cannot extend its deadline.
 */
int tftp_recv_time(int s, void *rbuf, int len, unsigned int flags,
                   struct sockaddr *from, socklen_t *fromlen,
                   uintmax_t *timeout_us_p)
{
    uintmax_t t0, dt;
    int rv, err = errno;
    uintmax_t timeout_us = *timeout_us_p;
    uintmax_t timeout_left;
    struct pollset *set = pollset_add(NULL, s);

    t0 = clock_us();
    timeout_left = timeout_us;

    do {
        do {
            rv = pollset_poll(set, POLLSET_IN, timeout_left);
            err = errno;

            dt = clock_us() - t0;

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

int pick_port_bind(int sockfd, union sock_addr *myaddr)
{
    if (xopt.portrange_from) {
        uint16_t port, firstport;

        port = firstport = xopt.portrange_from
            + random_u32() %
            ((uint16_t)(xopt.portrange_to - xopt.portrange_from) + 1);

        do {
            sa_set_port(myaddr, htons(port));
            if (likely(!bind(sockfd, &myaddr->sa, SOCKLEN(myaddr))))
                return 0;

            /* Keep trying until a free port is found */
            port++;
            if (port > xopt.portrange_to)
                port = xopt.portrange_from;
        } while (port != firstport);

        return -1;              /* Failed to allocate a port */
    } else {
        /* Let the kernel pick. */
        sa_set_port(myaddr, 0);
        return bind(sockfd, &myaddr->sa, SOCKLEN(myaddr));
    }
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

/*
 * Similar to strcasecmp() but only for ASCII, and returns true on match
 */
bool ascii_strncaseeq(const char *s1, const char *s2, size_t n)
{
    unsigned char c1, cx;

    while (n--) {
        c1 = *s1++;
        cx = c1 ^ *s2++;

        if (unlikely(cx)) {
            if (cx != 0x20)
                return false;
            if ((unsigned char)((c1 | cx) - 'a') > (unsigned char)('z' - 'a'))
                return false;
        }

        if (!c1)
            return true;
    }

    return true;
}

bool ascii_strcaseeq(const char *s1, const char *s2)
{
    return ascii_strncaseeq(s1, s2, SIZE_MAX);
}
