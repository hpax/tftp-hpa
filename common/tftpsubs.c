/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "tftpsubs.h"
#include "pollset.h"

/* Simple minded read-ahead/write-behind subroutines for tftp user and
   server.  Written originally with multiple buffers in mind, but current
   implementation has two buffer logic wired in.

   Todo:  add some sort of final error check so when the write-buffer
   is finally flushed, the caller can detect if the disk filled up
   (or had an i/o error) and return a nak to the other side.

			Jim Guyton 10/85
 */

#define PKTSIZE MAX_SEGSIZE+4   /* should be moved to tftp.h */

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

struct bf {
    int counter;                /* size of data in buffer, or flag */
    char buf[PKTSIZE];          /* room for data packet */
};
static struct bf bfs[2];

                                /* Values for bf.counter  */
#define BF_ALLOC -3             /* alloc'd but not yet filled */
#define BF_FREE  -2             /* free */
/* [-1 .. segsize] = size of data in the data buffer */

static int nextone;             /* index of next buffer to use */
static int current;             /* index of buffer in use */

                                /* control flags for crlf conversions */
static int newline = 0;         /* fillbuf: in middle of newline expansion */
static int prevchar = -1;       /* putbuf: previous char (cr check) */

static struct tftphdr *rw_init(int);

struct tftphdr *w_init()
{
    return rw_init(0);
}                               /* write-behind */

struct tftphdr *r_init()
{
    return rw_init(1);
}                               /* read-ahead */

/* init for either read-ahead or write-behind */
/* x == zero for write-behind, one for read-head */
static struct tftphdr *rw_init(int x)
{
    newline = 0;                /* init crlf flag */
    prevchar = -1;
    bfs[0].counter = BF_ALLOC;  /* pass out the first buffer */
    current = 0;
    bfs[1].counter = BF_FREE;
    nextone = x;                /* ahead or behind? */
    return (struct tftphdr *)bfs[0].buf;
}

/* Have emptied current buffer by sending to net and getting ack.
   Free it and return next buffer filled with data.
 */
int readit(FILE * file, struct tftphdr **dpp, int convert)
{
    struct bf *b;

    bfs[current].counter = BF_FREE;     /* free old one */
    current = !current;         /* "incr" current */

    b = &bfs[current];          /* look at new buffer */
    if (b->counter == BF_FREE)  /* if it's empty */
        read_ahead(file, convert);      /* fill it */
    /*      assert(b->counter != BF_FREE);*//* check */
    *dpp = (struct tftphdr *)b->buf;    /* set caller's ptr */
    return b->counter;
}

/*
 * fill the input buffer, doing ascii conversions if requested
 * conversions are  lf -> cr,lf  and cr -> cr, nul
 */
void read_ahead(FILE * file, int convert)
{
    int i;
    char *p;
    int c;
    struct bf *b;
    struct tftphdr *dp;

    b = &bfs[nextone];          /* look at "next" buffer */
    if (b->counter != BF_FREE)  /* nop if not free */
        return;
    nextone = !nextone;         /* "incr" next buffer ptr */

    dp = (struct tftphdr *)b->buf;

    if (convert == 0) {
        b->counter = read(fileno(file), dp->th_data, segsize);
        return;
    }

    p = dp->th_data;
    for (i = 0; i < segsize; i++) {
        if (newline) {
            if (prevchar == '\n')
                c = '\n';       /* lf to cr,lf */
            else
                c = '\0';       /* cr to cr,nul */
            newline = 0;
        } else {
            c = getc(file);
            if (c == EOF)
                break;
            if (c == '\n' || c == '\r') {
                prevchar = c;
                c = '\r';
                newline = 1;
            }
        }
        *p++ = c;
    }
    b->counter = (int)(p - dp->th_data);
}

/* Update count associated with the buffer, get new buffer
   from the queue.  Calls write_behind only if next buffer not
   available.
 */
int writeit(FILE * file, struct tftphdr **dpp, int ct, int convert)
{
    bfs[current].counter = ct;  /* set size of data to write */
    current = !current;         /* switch to other buffer */
    if (bfs[current].counter != BF_FREE)        /* if not free */
        (void)write_behind(file, convert);      /* flush it */
    bfs[current].counter = BF_ALLOC;    /* mark as alloc'd */
    *dpp = (struct tftphdr *)bfs[current].buf;
    return ct;                  /* this is a lie of course */
}

/*
 * Output a buffer to a file, converting from netascii if requested.
 * CR,NUL -> CR  and CR,LF => LF.
 * Note spec is undefined if we get CR as last byte of file or a
 * CR followed by anything else.  In this case we leave it alone.
 */
int write_behind(FILE * file, int convert)
{
    char *buf;
    int count;
    int ct;
    char *p;
    int c;                      /* current character */
    struct bf *b;
    struct tftphdr *dp;

    b = &bfs[nextone];
    if (b->counter < -1)        /* anything to flush? */
        return 0;               /* just nop if nothing to do */

    count = b->counter;         /* remember byte count */
    b->counter = BF_FREE;       /* reset flag */
    dp = (struct tftphdr *)b->buf;
    nextone = !nextone;         /* incr for next time */
    buf = dp->th_data;

    if (count <= 0)
        return -1;              /* nak logic? */

    if (convert == 0)
        return write(fileno(file), buf, count);

    p = buf;
    ct = count;
    while (ct--) {              /* loop over the buffer */
        c = *p++;               /* pick up a character */
        if (prevchar == '\r') { /* if prev char was cr */
            if (c == '\n')      /* if have cr,lf then just */
                fseek(file, -1, 1);     /* smash lf on top of the cr */
            else if (c == '\0') /* if have cr,nul then */
                goto skipit;    /* just skip over the putc */
            /* else just fall through and allow it */
        }
        putc(c, file);
      skipit:
        prevchar = c;
    }
    return count;
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
