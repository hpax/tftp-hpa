/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 */

#include "common/tftpsubs.h"
#include "common/tftp-io.h"
#include "common/tftp-xfer.h"

/*
 * TFTP User Program -- Protocol Machines
 */
#include "extern.h"

extern union sock_addr peeraddr; /* filled in by main */
extern int f;                    /* the opened socket */
extern bool trace;
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
static char ackbuf[REQBUFSIZE];
static int timeout;
static sigjmp_buf timeoutbuf;
static sigjmp_buf *active_timeoutbuf = &timeoutbuf;

static void nak(int, const char *);
static int makerequest(int, const char *, struct tftphdr *, const char *,
                       unsigned int, unsigned int, size_t);
static bool parse_oack(const struct tftphdr *, int, unsigned int,
                       unsigned int, unsigned int *, unsigned int *);
static void printstats(const char *, uintmax_t);
static void startclock(void);
static void stopclock(void);
static void timer(int);
static void tpacket(const char *, const struct tftphdr *, int);

struct client_xfer_context {
    union sock_addr from;
};

static int client_xfer_send(void *vctx, const void *packet, int length)
{
    (void)vctx;
    if (trace)
        tpacket("sent", packet, length);
    return sendto(f, packet, length, 0, &peeraddr.sa,
                  SOCKLEN(&peeraddr)) == length ? 0 : -1;
}

static int client_xfer_recv(void *vctx, void *packet, int length)
{
    struct client_xfer_context *ctx = vctx;
    socklen_t fromlen = sizeof(ctx->from);
    int n;

    alarm(rexmtval);
    n = recvfrom(f, packet, length, 0, &ctx->from.sa, &fromlen);
    alarm(0);
    if (n >= 0)
        sa_set_port(&peeraddr, SOCKPORT(&ctx->from));
    return n;
}

static void client_xfer_received(void *vctx, const struct tftphdr *packet,
                                 int length)
{
    (void)vctx;
    if (trace)
        tpacket("received", packet, length);
}

static void client_xfer_retry_enter(void *vctx, sigjmp_buf *retrybuf,
                                    bool restarted)
{
    (void)vctx;
    active_timeoutbuf = retrybuf;
    if (!restarted)
        timeout = 0;
}

static void client_xfer_retry_leave(void *vctx)
{
    (void)vctx;
    active_timeoutbuf = &timeoutbuf;
}

static void client_xfer_wait_begin(void *vctx)
{
    (void)vctx;
}

static const struct tftp_xfer_ops client_xfer_ops = {
    client_xfer_send,
    client_xfer_recv,
    client_xfer_received,
    client_xfer_retry_enter,
    client_xfer_retry_leave,
    client_xfer_wait_begin
};

/*
 * Send the requested file.
 */
void tftp_sendfile(int fd, const char *name, const char *mode,
                   unsigned int requested_window)
{
    struct tftphdr *ap;
    char response[REQBUFSIZE];
    const struct tftphdr *rp = (const struct tftphdr *)response;
    union sock_addr from;
    socklen_t fromlen;
    FILE *file = NULL;
    struct tftp_io * volatile io = NULL;
    struct client_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    int n, size;
    bool convert = !strcmp(mode, "netascii");
    unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    bool requested_options = blocksize != SEGSIZE || requested_window;
    uint16_t ap_opcode, ap_block;
    volatile uintmax_t amount = 0;

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
        goto abort;
    }

    /* A peer which ignores options answers a WRQ with ACK 0. */
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
        n = recvfrom(f, response, sizeof(response), 0, &from.sa, &fromlen);
        alarm(0);
        if (n < 0) {
            perror("tftp: recvfrom");
            goto abort;
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
            tftp_set_socket_buffers(f, negotiated_block,
                                    negotiated_window, true);
            break;
        }
        if (ap_opcode == ACK && ap_block == 0) {
            segsize = SEGSIZE;
            window = 1;         /* Traditional server ignored options. */
            break;
        }
    }

    io = tftp_io_reader_start(file, convert, window, window, segsize, false);
    if (!io) {
        nak(errno + 100, NULL);
        goto abort;
    }

    xfer.blocksize = segsize;
    xfer.windowsize = window;
    xfer.rollover = 0;
    xfer.resend_oack = requested_window != 0;
    xfer.control = ackbuf;
    xfer.control_size = sizeof(ackbuf);
    xfer.context = &context;
    xfer.ops = &client_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;
    tftp_xfer_send(&xfer, &result);
    amount = result.bytes;
    tftp_io_stop(io);
    io = NULL;

    switch (result.status) {
    case TFTP_XFER_READ_ERROR:
        nak(result.error + 100, NULL);
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        perror("tftp: sendto");
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        perror("tftp: recvfrom");
        break;
    case TFTP_XFER_PEER_ERROR:
        printf("Error code %d: %s\n", ntohs(ap->th_code), ap->th_msg);
        break;
    default:
        break;
    }
  abort:
    tftp_io_stop(io);
    if (file)
        fclose(file);
    stopclock();
    if (amount > 0)
        printstats("Sent", amount);
}

/*
 * Receive a file.
 */
void tftp_recvfile(int fd, const char *name, const char *mode,
                   unsigned int requested_window)
{
    struct tftphdr *ap;
    union sock_addr from;
    socklen_t fromlen;
    FILE *file = NULL;
    struct tftp_io * volatile io = NULL;
    struct client_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    struct tftphdr *initial_packet = NULL;
    const struct tftphdr * volatile initial_reply = NULL;
    volatile int initial_reply_len = 0;
    volatile int initial_packet_len = -1;
    int n, size;
    bool convert = !strcmp(mode, "netascii");
    unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    bool requested_options = blocksize != SEGSIZE || requested_window;
    uint16_t opcode;
    volatile uintmax_t amount = 0;

    startclock();
    file = fdopen(fd, convert ? "wt" : "wb");
    if (!file)
        goto abort;
    initial_packet = xmalloc(TFTP_XFER_MAX_PACKET_SIZE);
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
        n = recvfrom(f, initial_packet, TFTP_XFER_MAX_PACKET_SIZE, 0,
                     &from.sa, &fromlen);
        alarm(0);
        if (n < 0) {
            perror("tftp: recvfrom");
            goto abort;
        }
        sa_set_port(&peeraddr, SOCKPORT(&from));
        if (n < 2)
            continue;
        opcode = ntohs(initial_packet->th_opcode);
        if (trace)
            tpacket("received", initial_packet, n);
        if (opcode == ERROR) {
            printf("Error code %d: %s\n",
                   ntohs(initial_packet->th_code), initial_packet->th_msg);
            goto abort;
        }
        if (requested_options && opcode == OACK) {
            if (!parse_oack(initial_packet, n, blocksize, requested_window,
                            &negotiated_block, &negotiated_window)) {
                nak(EOPTNEG, "Invalid option response");
                goto abort;
            }
            segsize = (int)negotiated_block;
            window = negotiated_window;
            tftp_set_socket_buffers(f, negotiated_block,
                                    negotiated_window, false);
            ap->th_opcode = htons((uint16_t)ACK);
            ap->th_block = 0;
            initial_reply = ap;
            initial_reply_len = 4;
        } else if (opcode == DATA && n >= 4 &&
                   ntohs(initial_packet->th_block) == 1) {
            segsize = SEGSIZE;
            window = 1;         /* Peer ignored requested options. */
            initial_packet_len = n;
            break;
        } else {
            continue;
        }
        break;
    }

    io = tftp_io_writer_start(file, convert, window, segsize, false);
    if (!io) {
        nak(errno + 100, NULL);
        goto abort;
    }

    xfer.blocksize = segsize;
    xfer.windowsize = window;
    xfer.rollover = 0;
    xfer.resend_oack = false;
    xfer.control = ackbuf;
    xfer.control_size = sizeof(ackbuf);
    xfer.context = &context;
    xfer.ops = &client_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;
    tftp_xfer_recv(&xfer, ap, initial_packet,
                   TFTP_XFER_MAX_PACKET_SIZE,
                   initial_reply, initial_reply_len,
                   initial_packet_len < 0 ? NULL : initial_packet,
                   initial_packet_len,
                   &result);
    amount = result.bytes;
    tftp_io_stop(io);
    io = NULL;

    switch (result.status) {
    case TFTP_XFER_BAD_DATA:
        nak(EBADOP, "Data packet too large");
        break;
    case TFTP_XFER_WRITE_ERROR:
        nak(result.error + 100, NULL);
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        perror("tftp: sendto");
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        perror("tftp: recvfrom");
        break;
    case TFTP_XFER_PEER_ERROR:
        printf("Error code %d: %s\n", ntohs(result.packet->th_code),
               result.packet->th_msg);
        break;
    default:
        break;
    }

    xfree(initial_packet);
    initial_packet = NULL;
  abort:
    tftp_io_stop(io);
    xfree(initial_packet);
    if (file) {
        fclose(file);
    }
    stopclock();
    if (amount > 0)
        printstats("Received", amount);
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

    tp->th_opcode = htons((uint16_t) request);
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
static bool
parse_oack(const struct tftphdr *tp, int length, unsigned int requested_block,
           unsigned int requested_window, unsigned int *negotiated_block,
           unsigned int *negotiated_window)
{
    const char *cp, *end, *nul;
    char *value_end;
    unsigned long value;
    bool found = false;
    bool block_found = false;
    bool window_found = false;

    if (length <= 2 || ntohs(tp->th_opcode) != OACK)
        return false;
    *negotiated_block = SEGSIZE;
    *negotiated_window = 1;
    cp = (const char *)&tp->th_stuff;
    end = (const char *)tp + length;
    while (cp < end) {
        const char *option = cp;

        nul = memchr(cp, '\0', (size_t)(end - cp));
        if (!nul || nul == cp)
            return false;
        cp = nul + 1;
        if (cp >= end)
            return false;
        nul = memchr(cp, '\0', (size_t)(end - cp));
        if (!nul || nul == cp)
            return false;
        errno = 0;
        value = strtoul(cp, &value_end, 10);
        if (errno || value_end != nul)
            return false;
        if (!strcasecmp(option, "blksize")) {
            if (block_found || requested_block == SEGSIZE ||
                value < 8 || value > requested_block)
                return false;
            *negotiated_block = (unsigned int)value;
            block_found = true;
        } else if (!strcasecmp(option, "windowsize")) {
            if (window_found || !requested_window ||
                value < 1 || value > requested_window)
                return false;
            *negotiated_window = (unsigned int)value;
            window_found = true;
        } else {
            return false;
        }
        found = true;
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
    tp->th_opcode = htons((uint16_t) ERROR);
    tp->th_code = htons((uint16_t) error);

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

    tp->th_code = htons((uint16_t) error);

    length = strlen(msg) + 1;
    memcpy(tp->th_msg, msg, length);
    length += 4;                /* Add space for header */

    if (trace)
        tpacket("sent", tp, length);
    if (sendto(f, ackbuf, length, 0, &peeraddr.sa,
               SOCKLEN(&peeraddr)) != length)
        perror("nak");
}

static void tpacket(const char *s, const struct tftphdr *tp, int n)
{
    static const char *const opcodes[] =
        { "#0", "RRQ", "WRQ", "DATA", "ACK", "ERROR", "OACK" };
    const char *cp, *file;
    uint16_t op = ntohs((uint16_t) tp->th_opcode);

    if (op < RRQ || op > OACK)
        printf("%s opcode=%x ", s, op);
    else
        printf("%s %s ", s, opcodes[op]);
    switch (op) {

    case RRQ:
    case WRQ:
        n -= 2;
        file = cp = (const char *)&(tp->th_stuff);
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

static struct timeval tstart;
static struct timeval tstop;

static void startclock(void)
{
    (void)gettimeofday(&tstart, NULL);
}

static void stopclock(void)
{

    (void)gettimeofday(&tstop, NULL);
}

#define PWS_BINARY 1
#define PWS_EXACT  2
static bool print_with_suffix(double val, unsigned int flags)
{
    static const char *const dsuffixes[] = {
        "", "k", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q", NULL
    };
    static const char *const bsuffixes[] = {
        "", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi", "Yi", "Ri", "Qi", NULL
    };
    const char *const *suffix = flags & PWS_BINARY ? bsuffixes : dsuffixes;
    double divisor = (flags & PWS_BINARY) ? 1024.0 : 1000.0;
    int decimals;
    bool with_suffix = false;

    if (verbose < 2) {
        while (val >= divisor && suffix[1]) {
            suffix++;
            val /= divisor;
            flags &= ~PWS_EXACT; /* Not exact once divided down */
            with_suffix = true;
        }
    }

    decimals = 0;
    if (!(flags & PWS_EXACT)) {
        if (val < 9.995)
            decimals = 2;
        else if (val < 99.95)
            decimals = 1;
    }

    printf("%0.*f %s", decimals, val, *suffix);
    return with_suffix;
}

static void printstats(const char *direction, uintmax_t amount)
{
    if (verbose) {
        double delta = (tstop.tv_sec - tstart.tv_sec) +
            ((int32_t)tstop.tv_usec - (int32_t)tstart.tv_usec) * 1.0e-6;
        bool with_suffix;

        fputs(direction, stdout);
        putchar(' ');
        if (verbose > 1 || amount < 9999) {
            printf("%" PRIuMAX " bytes", amount);
        } else {
            with_suffix = print_with_suffix(amount, PWS_EXACT);
            putchar('B');
            if (with_suffix) {
                fputs(" (", stdout);
                print_with_suffix(amount, PWS_EXACT|PWS_BINARY);
                fputs("B)", stdout);
            }
        }
        printf(" in %.3f s [", delta);
        print_with_suffix((amount << 3)/delta, 0);
        fputs("bit/s]\n", stdout);
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
    siglongjmp(*active_timeoutbuf, 1);
}
