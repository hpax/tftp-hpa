/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "common/tftpsubs.h"

/*
 * TFTP User Program -- Protocol Machines
 */
#include "extern.h"

extern union sock_addr peeraddr; /* filled in by main */
extern int f;                    /* the opened socket */
extern int trace;
extern int verbose;
extern int rexmtval;
extern int maxtimeout;
extern unsigned int blocksize;
extern unsigned int windowsize;

/*
 * Size of the client's own request-encoding buffer (ackbuf). This is
 * intentionally named differently from the identically-purposed
 * PKTSIZE macro in common/tftpsubs.c and tftpd/tftpd.c, which is
 * MAX_SEGSIZE+4: this client never negotiates a larger block size, so
 * ackbuf holds requests, option acknowledgments, and small control
 * packets.  It needs to accommodate requests containing RFC 2347 options.
 */
#define REQBUFSIZE MAX_SEGSIZE+4
char ackbuf[REQBUFSIZE];
int timeout;
static sigjmp_buf timeoutbuf;

static void nak(int, const char *);
static int makerequest(int, const char *, struct tftphdr *, const char *,
                       unsigned int, unsigned int, size_t);
static int parse_oack(const struct tftphdr *, int, unsigned int,
                      unsigned int, unsigned int *, unsigned int *);
static void printstats(const char *, unsigned long);
static void startclock(void);
static void stopclock(void);
static void timer(int);
static void tpacket(const char *, struct tftphdr *, int);

/*
 * Send the requested file.
 */
void tftp_sendfile(int fd, const char *name, const char *mode,
                   unsigned int requested_window)
{
    struct tftphdr *dp, *ap;
    char response[REQBUFSIZE];
    struct tftphdr *rp = (struct tftphdr *)response;
    union sock_addr from;
    socklen_t fromlen;
    FILE *file = NULL;
    char * volatile packets = NULL;
    int * volatile lengths = NULL;
    int n, size;
    size_t packetsize;
    volatile int packet_count, final;
    int convert = !strcmp(mode, "netascii");
    volatile unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    int requested_options = blocksize != SEGSIZE || requested_window;
    volatile u_short block = 1;
    u_short ap_opcode, ap_block, expected_ack;
    volatile unsigned long amount = 0;

    startclock();
    file = fdopen(fd, convert ? "rt" : "rb");
    if (!file)
        goto abort;
    ap = (struct tftphdr *)ackbuf;

    tftp_signal(SIGALRM, timer, 0);
    size = makerequest(WRQ, name, ap, mode, blocksize, requested_window,
                       sizeof(ackbuf));
    if (size < 0) {
        fprintf(stderr, "tftp: %s: %s\n", name, strerror(errno));
        goto abort_packets;
    }

    /* A peer which ignores options answers a WRQ with ACK 0. */
    for (;;) {
        timeout = 0;
        (void)sigsetjmp(timeoutbuf, 1);
        if (trace)
            tpacket("sent", ap, size);
        if (sendto(f, ap, size, 0, &peeraddr.sa, SOCKLEN(&peeraddr)) != size) {
            perror("tftp: sendto");
            goto abort_packets;
        }
        alarm(rexmtval);
        fromlen = sizeof(from);
        n = recvfrom(f, response, sizeof(response), 0, &from.sa, &fromlen);
        alarm(0);
        if (n < 0) {
            perror("tftp: recvfrom");
            goto abort_packets;
        }
        sa_set_port(&peeraddr, SOCKPORT(&from));
        if (n < 2)
            continue;
        if (trace)
            tpacket("received", rp, n);
        ap_opcode = ntohs(rp->th_opcode);
        ap_block = ntohs(rp->th_block);
        if (ap_opcode == ERROR) {
            printf("Error code %d: %s\n", ap_block, rp->th_msg);
            goto abort;
        }
        if (requested_options && ap_opcode == OACK) {
            if (!parse_oack(rp, n, blocksize, requested_window,
                            &negotiated_block, &negotiated_window)) {
                nak(EOPTNEG, "Invalid option response");
                goto abort;
            }
            segsize = (int)negotiated_block;
            window = negotiated_window;
            break;
        }
        if (ap_opcode == ACK && ap_block == 0) {
            segsize = SEGSIZE;
            window = 1;         /* Traditional server ignored options. */
            break;
        }
    }

    dp = r_init();
    packetsize = ((size_t)segsize + 5) & ~(size_t)1;
    packets = xcalloc(window, packetsize);
    lengths = xcalloc(window, sizeof(*lengths));
    for (;;) {
        packet_count = 0;
        final = 0;
        do {
            size = readit(file, &dp, convert);
            if (size < 0) {
                nak(errno + 100, NULL);
                goto abort_packets;
            }
            dp->th_opcode = htons((u_short)DATA);
            dp->th_block = htons(block);
            memcpy(packets + (size_t)packet_count * packetsize,
                   dp, (size_t)size + 4);
            lengths[packet_count] = size + 4;
            read_ahead(file, convert);
            packet_count++;
            if (size != segsize)
                final = 1;
            block++;
        } while (packet_count < (int)window && !final);

        timeout = 0;
        (void)sigsetjmp(timeoutbuf, 1);
      resend_window:
        for (n = 0; n < packet_count; n++) {
            dp = (struct tftphdr *)(packets +
                                    (size_t)n * packetsize);
            if (trace)
                tpacket("sent", dp, lengths[n]);
            if (sendto(f, dp, lengths[n], 0, &peeraddr.sa,
                       SOCKLEN(&peeraddr)) != lengths[n]) {
                perror("tftp: sendto");
                goto abort_packets;
            }
        }
        for (;;) {
            alarm(rexmtval);
            fromlen = sizeof(from);
            n = recvfrom(f, ackbuf, sizeof(ackbuf), 0, &from.sa, &fromlen);
            alarm(0);
            if (n < 0) {
                perror("tftp: recvfrom");
                goto abort_packets;
            }
            sa_set_port(&peeraddr, SOCKPORT(&from));
            if (n < 4)
                continue;
            if (trace)
                tpacket("received", ap, n);
            ap_opcode = ntohs(ap->th_opcode);
            ap_block = ntohs(ap->th_block);
            if (ap_opcode == ERROR) {
                printf("Error code %d: %s\n", ap_block, ap->th_msg);
                goto abort_packets;
            }
            expected_ack = ntohs(((struct tftphdr *)(packets +
                                  (size_t)(packet_count - 1) *
                                  packetsize))->th_block);
            if (ap_opcode == ACK && ap_block == expected_ack)
                break;
            if (ap_opcode == OACK && requested_window)
                goto resend_window;
            if (ap_opcode == ACK)
                (void)synchnet(f);
        }
        for (n = 0; n < packet_count; n++)
            amount += lengths[n] - 4;
        if (final)
            break;
    }

  abort_packets:
    xfree(lengths);
    xfree(packets);
  abort:
    if (file)
        fclose(file);
    stopclock();
    if (amount > 0)
        printstats("Sent", (unsigned long)amount);
}

/*
 * Receive a file.
 */
void tftp_recvfile(int fd, const char *name, const char *mode,
                   unsigned int requested_window)
{
    struct tftphdr *ap, *dp;
    union sock_addr from;
    socklen_t fromlen;
    FILE *file = NULL;
    volatile int n, size, packets_in_window = 0, first_data = 0, final;
    int convert = !strcmp(mode, "netascii");
    volatile unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    int requested_options = blocksize != SEGSIZE || requested_window;
    volatile u_short block = 1, last_acked = 0;
    u_short opcode, packet_block;
    volatile unsigned long amount = 0;

    startclock();
    file = fdopen(fd, convert ? "wt" : "wb");
    if (!file)
        goto abort;
    dp = w_init();
    ap = (struct tftphdr *)ackbuf;
    size = makerequest(RRQ, name, ap, mode, blocksize, requested_window,
                       sizeof(ackbuf));
    if (size < 0) {
        fprintf(stderr, "tftp: %s: %s\n", name, strerror(errno));
        goto abort;
    }
    tftp_signal(SIGALRM, timer, 0);

    /* RFC 7440 peers answer with OACK; legacy peers start with DATA 1. */
    for (;;) {
        timeout = 0;
        (void)sigsetjmp(timeoutbuf, 1);
        if (trace)
            tpacket("sent", ap, size);
        if (sendto(f, ap, size, 0, &peeraddr.sa, SOCKLEN(&peeraddr)) != size) {
            perror("tftp: sendto");
            goto abort;
        }
        alarm(rexmtval);
        fromlen = sizeof(from);
        n = recvfrom(f, dp, MAX_SEGSIZE + 4, 0, &from.sa, &fromlen);
        alarm(0);
        if (n < 0) {
            perror("tftp: recvfrom");
            goto abort;
        }
        sa_set_port(&peeraddr, SOCKPORT(&from));
        if (n < 2)
            continue;
        opcode = ntohs(dp->th_opcode);
        if (trace)
            tpacket("received", dp, n);
        if (opcode == ERROR) {
            printf("Error code %d: %s\n", ntohs(dp->th_code), dp->th_msg);
            goto abort;
        }
        if (requested_options && opcode == OACK) {
            if (!parse_oack(dp, n, blocksize, requested_window,
                            &negotiated_block, &negotiated_window)) {
                nak(EOPTNEG, "Invalid option response");
                goto abort;
            }
            segsize = (int)negotiated_block;
            window = negotiated_window;
            ap->th_opcode = htons((u_short)ACK);
            ap->th_block = 0;
            size = 4;
            last_acked = 0;
        } else if (opcode == DATA && n >= 4 && ntohs(dp->th_block) == 1) {
            segsize = SEGSIZE;
            window = 1;         /* Peer ignored requested options. */
            first_data = 1;
            break;
        } else {
            continue;
        }
        /* An RRQ OACK is acknowledged with ACK 0 before DATA 1. */
        if (trace)
            tpacket("sent", ap, size);
        if (sendto(f, ap, size, 0, &peeraddr.sa, SOCKLEN(&peeraddr)) != size) {
            perror("tftp: sendto");
            goto abort;
        }
        write_behind(file, convert);
        break;
    }

    for (;;) {
        int timedout;

        timeout = 0;
        timedout = sigsetjmp(timeoutbuf, 1);
        /*
         * On timeout, ACK the last completed window.  This is also ACK 0
         * immediately after an OACK, which makes OACK loss recoverable.
         */
        if (timedout && !first_data) {
            ap->th_opcode = htons((u_short)ACK);
            ap->th_block = htons(last_acked);
            if (trace)
                tpacket("sent", ap, 4);
            if (sendto(f, ap, 4, 0, &peeraddr.sa,
                       SOCKLEN(&peeraddr)) != 4) {
                perror("tftp: sendto");
                goto abort;
            }
            write_behind(file, convert);
        }
        if (!first_data) {
            alarm(rexmtval);
            fromlen = sizeof(from);
            n = recvfrom(f, dp, MAX_SEGSIZE + 4, 0, &from.sa, &fromlen);
            alarm(0);
            if (n < 0) {
                perror("tftp: recvfrom");
                goto abort;
            }
            if (trace)
                tpacket("received", dp, n);
        }
        first_data = 0;
        if (n < 4)
            continue;
        opcode = ntohs(dp->th_opcode);
        packet_block = ntohs(dp->th_block);
        if (opcode == ERROR) {
            printf("Error code %d: %s\n", packet_block, dp->th_msg);
            goto abort;
        }
        if (opcode != DATA || n < 4)
            continue;
        if (packet_block != block) {
            if (packet_block == last_acked) {
                ap->th_opcode = htons((u_short)ACK);
                ap->th_block = htons(last_acked);
                if (trace)
                    tpacket("sent", ap, 4);
                (void)sendto(f, ap, 4, 0, &peeraddr.sa,
                             SOCKLEN(&peeraddr));
            }
            continue;
        }
        if (n - 4 > segsize) {
            nak(EBADOP, "Data packet too large");
            goto abort;
        }
        size = writeit(file, &dp, n - 4, convert);
        if (size < 0) {
            nak(errno + 100, NULL);
            goto abort;
        }
        amount += size;
        packets_in_window++;
        block++;
        final = size != segsize;
        if (final || packets_in_window == (int)window) {
            ap->th_opcode = htons((u_short)ACK);
            ap->th_block = htons(packet_block);
            last_acked = packet_block;
            packets_in_window = 0;
            if (trace)
                tpacket("sent", ap, 4);
            if (sendto(f, ap, 4, 0, &peeraddr.sa,
                       SOCKLEN(&peeraddr)) != 4) {
                perror("tftp: sendto");
                goto abort;
            }
            write_behind(file, convert);
            if (final)
                break;
        }
    }

  abort:
    if (file) {
        write_behind(file, convert);
        fclose(file);
    }
    stopclock();
    if (amount > 0)
        printstats("Received", (unsigned long)amount);
}

static int
makerequest(int request, const char *name,
            struct tftphdr *tp, const char *mode,
            unsigned int requested_block, unsigned int requested_window,
            size_t tpsize)
{
    char *cp;
    char block_value[sizeof(unsigned int) * CHAR_BIT / 3 + 2];
    char window_value[sizeof(unsigned int) * CHAR_BIT / 3 + 2];
    size_t namelen, modelen, optionlen = 0;

    namelen = strlen(name);
    modelen = strlen(mode);
    if (requested_block != SEGSIZE) {
        (void)snprintf(block_value, sizeof(block_value), "%u",
                       requested_block);
        optionlen += sizeof("blksize") + strlen(block_value);
    }
    if (requested_window) {
        (void)snprintf(window_value, sizeof(window_value), "%u",
                       requested_window);
        optionlen += sizeof("windowsize") + strlen(window_value);
    }

    /*
     * The request is encoded into tp as: opcode(2) name NUL mode NUL.
     * tpsize is the total size of the buffer tp points to; reject
     * anything that would not fit rather than overflowing it. Compare
     * with subtraction on the tpsize side only, so there is no risk of
     * namelen/modelen (both attacker/user-controlled) underflowing an
     * unsigned computation.
     */
    if (tpsize < 4 || namelen > tpsize - 4 ||
        modelen > tpsize - 4 - namelen ||
        optionlen > tpsize - 4 - namelen - modelen) {
        errno = ENAMETOOLONG;
        return -1;
    }

    tp->th_opcode = htons((u_short) request);
    cp = (char *)&(tp->th_stuff);
    memcpy(cp, name, namelen + 1);
    cp += namelen + 1;
    memcpy(cp, mode, modelen + 1);
    cp += modelen + 1;
    if (requested_block != SEGSIZE) {
        memcpy(cp, "blksize", sizeof("blksize"));
        cp += sizeof("blksize");
        memcpy(cp, block_value, strlen(block_value) + 1);
        cp += strlen(block_value) + 1;
    }
    if (requested_window) {
        memcpy(cp, "windowsize", sizeof("windowsize"));
        cp += sizeof("windowsize");
        memcpy(cp, window_value, strlen(window_value) + 1);
        cp += strlen(window_value) + 1;
    }
    return (cp - (char *)tp);
}

/*
 * RFC 2347 requires an OACK to contain only requested options.  The
 * client accepts only options it requested, and rejects malformed,
 * duplicate, or unexpected option pairs.
 */
static int
parse_oack(const struct tftphdr *tp, int length, unsigned int requested_block,
           unsigned int requested_window, unsigned int *negotiated_block,
           unsigned int *negotiated_window)
{
    const char *cp, *end, *nul;
    char *value_end;
    unsigned long value;
    int found = 0, block_found = 0, window_found = 0;

    if (length <= 2 || ntohs(tp->th_opcode) != OACK)
        return 0;
    *negotiated_block = SEGSIZE;
    *negotiated_window = 1;
    cp = (const char *)&tp->th_stuff;
    end = (const char *)tp + length;
    while (cp < end) {
        const char *option = cp;

        nul = memchr(cp, '\0', (size_t)(end - cp));
        if (!nul || nul == cp)
            return 0;
        cp = nul + 1;
        if (cp >= end)
            return 0;
        nul = memchr(cp, '\0', (size_t)(end - cp));
        if (!nul || nul == cp)
            return 0;
        errno = 0;
        value = strtoul(cp, &value_end, 10);
        if (errno || value_end != nul)
            return 0;
        if (!strcasecmp(option, "blksize")) {
            if (block_found || requested_block == SEGSIZE ||
                value < 8 || value > requested_block)
                return 0;
            *negotiated_block = (unsigned int)value;
            block_found = 1;
        } else if (!strcasecmp(option, "windowsize")) {
            if (window_found || !requested_window ||
                value < 1 || value > requested_window)
                return 0;
            *negotiated_window = (unsigned int)value;
            window_found = 1;
        } else {
            return 0;
        }
        found = 1;
        cp = nul + 1;
    }
    return found;
}

static const char *const errmsgs[] = {
    "Undefined error code",     /* 0 - EUNDEF */
    "File not found",           /* 1 - ENOTFOUND */
    "Access denied",            /* 2 - EACCESS */
    "Disk full or allocation exceeded", /* 3 - ENOSPACE */
    "Illegal TFTP operation",   /* 4 - EBADOP */
    "Unknown transfer ID",      /* 5 - EBADID */
    "File already exists",      /* 6 - EEXISTS */
    "No such user",             /* 7 - ENOUSER */
    "Failure to negotiate RFC2347 options"      /* 8 - EOPTNEG */
};

#define ERR_CNT (sizeof(errmsgs)/sizeof(const char *))

/*
 * Send a nak packet (error message).
 * Error code passed in is one of the
 * standard TFTP codes, or a UNIX errno
 * offset by 100.
 */
static void nak(int error, const char *msg)
{
    struct tftphdr *tp;
    int length;

    tp = (struct tftphdr *)ackbuf;
    tp->th_opcode = htons((u_short) ERROR);
    tp->th_code = htons((u_short) error);

    if (error >= 100) {
        /* This is a Unix errno+100 */
        if (!msg)
            msg = strerror(error - 100);
        error = EUNDEF;
    } else {
        if ((unsigned)error >= ERR_CNT)
            error = EUNDEF;

        if (!msg)
            msg = errmsgs[error];
    }

    tp->th_code = htons((u_short) error);

    length = strlen(msg) + 1;
    memcpy(tp->th_msg, msg, length);
    length += 4;                /* Add space for header */

    if (trace)
        tpacket("sent", tp, length);
    if (sendto(f, ackbuf, length, 0, &peeraddr.sa,
               SOCKLEN(&peeraddr)) != length)
        perror("nak");
}

static void tpacket(const char *s, struct tftphdr *tp, int n)
{
    static const char *opcodes[] =
        { "#0", "RRQ", "WRQ", "DATA", "ACK", "ERROR", "OACK" };
    char *cp, *file;
    u_short op = ntohs((u_short) tp->th_opcode);

    if (op < RRQ || op > OACK)
        printf("%s opcode=%x ", s, op);
    else
        printf("%s %s ", s, opcodes[op]);
    switch (op) {

    case RRQ:
    case WRQ:
        n -= 2;
        file = cp = (char *)&(tp->th_stuff);
        cp = strchr(cp, '\0');
        printf("<file=%s, mode=%s>\n", file, cp + 1);
        break;

    case DATA:
        printf("<block=%d, %d bytes>\n", ntohs(tp->th_block), n - 4);
        break;

    case ACK:
        printf("<block=%d>\n", ntohs(tp->th_block));
        break;

    case ERROR:
        printf("<code=%d, msg=%s>\n", ntohs(tp->th_code), tp->th_msg);
        break;

    case OACK:
        printf("<options>\n");
        break;
    }
}

struct timeval tstart;
struct timeval tstop;

static void startclock(void)
{
    (void)gettimeofday(&tstart, NULL);
}

static void stopclock(void)
{

    (void)gettimeofday(&tstop, NULL);
}

static void printstats(const char *direction, unsigned long amount)
{
    double delta;

    delta = (tstop.tv_sec + (tstop.tv_usec / 100000.0)) -
        (tstart.tv_sec + (tstart.tv_usec / 100000.0));
    if (verbose) {
        printf("%s %lu bytes in %.1f seconds", direction, amount, delta);
        printf(" [%.0f bit/s]", (amount * 8.) / delta);
        putchar('\n');
    }
}

static void timer(int sig)
{
    int save_errno = errno;

    (void)sig;                  /* Shut up unused warning */

    timeout += rexmtval;
    if (timeout >= maxtimeout) {
        printf("Transfer timed out.\n");
        errno = save_errno;
        siglongjmp(toplevel, -1);
    }
    errno = save_errno;
    siglongjmp(timeoutbuf, 1);
}
