/*
 * SPDX-License-Identifier: BSD-4-Clause-UC
 *
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (C) 2026 H. Peter Anvin <hpa@zytor.com>
 */

#include "common/tftpsubs.h"
#include "common/tftp-io.h"
#include "common/tftp-xfer.h"
#include "common/clock.h"
#include "options.h"

/*
 * TFTP User Program -- Protocol Machines
 */
#include "extern.h"

extern union sock_addr peeraddr; /* filled in by main */
extern int f;                    /* the opened socket */

static unsigned long timeout;
static sigjmp_buf timeoutbuf;
static sigjmp_buf *active_timeoutbuf = &timeoutbuf;

static void nak(int, const char *);
static int makerequest(struct tftphdr **, int *,
                       int, const char *, const char *,
                       unsigned int, unsigned int);
static bool parse_oack(const struct tftphdr *, int, unsigned int,
                       unsigned int, unsigned int *, unsigned int *);
static void printstats(const char *, uintmax_t);
static void startclock(void);
static void stopclock(void);
static void timer(int);
static void tpacket(const char *, const struct tftphdr *, int);

struct client_xfer_context {
    union sock_addr from;
    unsigned long timeout;
};

static int client_recv_time(void *packet, int length, union sock_addr *from,
                            unsigned long *timeout_us_p)
{
    socklen_t fromlen = sizeof(*from);
    int n;

    n = tftp_recv_time(f, packet, length, 0, &from->sa, &fromlen,
                       timeout_us_p);
    if (n < 0 && errno == ETIMEDOUT)
        timer(0);               /* Should not return */
    if (n >= 0)
        sa_set_port(&peeraddr, SOCKPORT(from));
    return n;
}

static int client_xfer_send(void *vctx, const void *packet, int length)
{
    (void)vctx;
    if (copt.trace)
        tpacket("sent", packet, length);
    return sendto(f, packet, length, 0, &peeraddr.sa,
                  sizeof peeraddr) == length ? 0 : -1;
}

static int client_xfer_recv(void *vctx, void *packet, int length)
{
    struct client_xfer_context *ctx = vctx;

    return client_recv_time(packet, length, &ctx->from, &ctx->timeout);
}

static void client_xfer_received(void *vctx, const struct tftphdr *packet,
                                 int length)
{
    (void)vctx;
    if (copt.trace)
        tpacket("received", packet, length);
}

static void client_xfer_retry_enter(void *vctx, sigjmp_buf *retrybuf,
                                    bool restarted)
{
    (void)vctx;
    active_timeoutbuf = retrybuf;
    if (!restarted)
        timeout = (unsigned long)copt.rexmtval * USEC_PER_SEC;
}

static void client_xfer_retry_leave(void *vctx)
{
    (void)vctx;
    active_timeoutbuf = &timeoutbuf;
}

static void client_xfer_wait_begin(void *vctx)
{
    struct client_xfer_context *ctx = vctx;

    ctx->timeout = timeout;
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
 * The maximum possible size of an OACK packet plus at least one extra
 * byte: the options we try to negotiate, plus their values; if the
 * OACK packet is larger than this it is faulty.
 *
 * This is also used to size the buffer for the options part of a
 * request packet.
 *
 * This is quite aggressively conservative since the numeric range of
 * these options is way less than the total size of an uintmax_t, but
 * the extra amount of memory is trivial and not worth worrying about.
 */
#define OPT_SPACE(x) (sizeof(x) + DIGIT_SPACE(uintmax_t) + 1)

#define TFTP_OPTION_SPACE (OPT_SPACE("blksize") + OPT_SPACE("windowsize"))

/*
 * The maximum size of a reply packet (ACK, ERROR, OACK or
 * default-sized DATA), that we care about. If an ERROR packet is
 * unreasonably long, just truncate it.
 */
#define TFTP_REPLY_MAX_PACKET_SIZE MAX(TFTP_OPTION_SPACE+2, SEGSIZE+4)

/*
 * Send the requested file.
 */
int tftp_sendfile(int fd, const char *name, const char *mode,
                  unsigned int requested_window)
{
    struct tftphdr * volatile response;
    struct tftphdr *req;
    union sock_addr from;
    FILE *file = NULL;
    struct tftp_io * volatile io = NULL;
    struct client_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    int n, size;
    volatile int err = 0;
    bool convert = !strcmp(mode, "netascii");
    unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    int optionlen;
    uint16_t ap_opcode, ap_block;
    unsigned long r_timeout;
    volatile uintmax_t amount = 0;

    response = xmalloc(TFTP_REPLY_MAX_PACKET_SIZE);

    startclock();
    file = fdopen(fd, convert ? "rt" : "rb");
    if (!file) {
        close(fd);
        err = EX_OSERR;
        goto abort;
    }

    tftp_signal(SIGALRM, timer, 0);
    size = makerequest(&req, &optionlen, WRQ, name, mode,
                       xopt.max_blksize, requested_window);
    if (size < 0) {
        fprintf(stderr, "tftp: %s: %s\n", name, strerror(errno));
        err = EX_OSERR;
        goto abort;
    }

    /* A peer which ignores options answers a WRQ with ACK 0. */
    for (;;) {
        timeout = (unsigned long)copt.rexmtval * USEC_PER_SEC;
        (void)sigsetjmp(timeoutbuf, 1);
        if (copt.trace)
            tpacket("sent", req, size);
        if (sendto(f, req, size, 0, &peeraddr.sa, sizeof peeraddr) != size) {
            perror("tftp: sendto");
            err = EX_OSERR;
            goto abort;
        }
        r_timeout = timeout;
      wait_for_reply:
        n = client_recv_time(response, TFTP_REPLY_MAX_PACKET_SIZE,
                             &from, &r_timeout);
        if (n < 0) {
            perror("tftp: recvfrom");
            err = EX_OSERR;
            goto abort;
        }
        if (n < 2)
            goto wait_for_reply;
        if (copt.trace)
            tpacket("received", response, n);
        ap_opcode = ntohs(response->th_opcode);
        if (ap_opcode == ERROR) {
            char *error_string = errpkt_to_string(response, n);
            printf("Error: %s\n", error_string);
            xfree(error_string);
            err = EX_PROTOCOL;
            goto abort;
        }
        if (optionlen && ap_opcode == OACK) {
            if (!parse_oack(response, n, xopt.max_blksize, requested_window,
                            &negotiated_block, &negotiated_window)) {
                nak(EOPTNEG, "Invalid option response");
                err = EX_PROTOCOL;
                goto abort;
            }
            segsize = (int)negotiated_block;
            window = negotiated_window;
            tftp_set_socket_buffers(f, negotiated_block,
                                    negotiated_window, true);
            break;
        }
        if (n >= 4) {
            ap_block = ntohs(response->th_block);
            if (ap_opcode == ACK && ap_block == 0) {
                segsize = SEGSIZE;
                window = 1;         /* Traditional server ignored options. */
                break;
            }
        }
        goto wait_for_reply;
    }

    xfree(req);

    io = tftp_io_reader_start(file, convert, window, window, segsize, false);
    if (!io) {
        nak(-errno, NULL);
        err = EX_OSERR;
        goto abort;
    }

    xfer.blocksize = segsize;
    xfer.windowsize = window;
    xfer.rollover = 0;
    xfer.resend_oack = requested_window != 0;
    xfer.control = response;
    xfer.control_size = TFTP_REPLY_MAX_PACKET_SIZE;
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
        nak(-result.error, NULL);
        err = EX_OSERR;
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        perror("tftp: sendto");
        err = EX_OSERR;
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        perror("tftp: recvfrom");
        err = EX_OSERR;
        break;
    case TFTP_XFER_PEER_ERROR:
    {
        char *error_string =
            errpkt_to_string(result.packet, result.packet_len);
        printf("Error: %s\n", error_string);
        xfree(error_string);
        err = EX_PROTOCOL;
        break;
    }
    default:
        break;
    }
  abort:
    tftp_io_stop(io);
    if (file)
        fclose(file);
    xfree(response);
    stopclock();
    if (amount > 0)
        printstats("Sent", amount);
    return err;
}

/*
 * Receive a file.
 */
int tftp_recvfile(int fd, const char *name, const char *mode,
                  unsigned int requested_window)
{
    union sock_addr from;
    FILE *file = NULL;
    struct tftp_io * volatile io = NULL;
    struct client_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    struct tftphdr *req = NULL;
    struct tftphdr *ack = NULL;
    struct tftphdr * volatile datapkt = NULL;
    struct tftphdr *initial_packet = NULL;
    struct tftphdr *initial_reply = NULL;
    volatile int initial_reply_len = 0;
    volatile int initial_packet_len = -1;
    int n, size;
    volatile int err = 0;
    bool convert = !strcmp(mode, "netascii");
    unsigned int window;
    unsigned int negotiated_block, negotiated_window;
    int optionlen;
    uint16_t opcode;
    unsigned long r_timeout;
    volatile uintmax_t amount = 0;

    startclock();
    file = fdopen(fd, convert ? "wt" : "wb");
    if (!file) {
        close(fd);
        err = EX_OSERR;
        goto abort;
    }
    initial_packet = xmalloc(TFTP_REPLY_MAX_PACKET_SIZE);

    size = makerequest(&req, &optionlen, RRQ, name, mode,
                       xopt.max_blksize, requested_window);
    if (size < 0) {
        fprintf(stderr, "tftp: %s: %s\n", name, strerror(errno));
        err = EX_OSERR;
        goto abort;
    }
    tftp_signal(SIGALRM, timer, 0);

    /* RFC 7440 peers answer with OACK; legacy peers start with DATA 1. */
    for (;;) {
        timeout = (unsigned long)copt.rexmtval * USEC_PER_SEC;
        (void)sigsetjmp(timeoutbuf, 1);
        if (copt.trace)
            tpacket("sent", req, size);
        if (sendto(f, req, size, 0, &peeraddr.sa, sizeof peeraddr) != size) {
            perror("tftp: sendto");
            err = EX_OSERR;
            goto abort;
        }
        r_timeout = timeout;
      wait_for_reply:
        n = client_recv_time(initial_packet, TFTP_REPLY_MAX_PACKET_SIZE,
                             &from, &r_timeout);
        if (n < 0) {
            perror("tftp: recvfrom");
            err = EX_OSERR;
            goto abort;
        }
        initial_packet_len = n;
        if (n < 2)
            goto wait_for_reply;
        opcode = ntohs(initial_packet->th_opcode);
        if (copt.trace)
            tpacket("received", initial_packet, n);
        if (opcode == ERROR) {
            char *errmsg = errpkt_to_string(initial_packet, n);
            printf("Error: %s\n", errmsg);
            xfree(errmsg);
            err = EX_PROTOCOL;
            goto abort;
        }
        if (optionlen && opcode == OACK) {
            if (!parse_oack(initial_packet, n, xopt.max_blksize,
                            requested_window,
                            &negotiated_block, &negotiated_window)) {
                nak(EOPTNEG, "Invalid option response");
                err = EX_PROTOCOL;
                goto abort;
            }
            segsize = negotiated_block;
            window = negotiated_window;
            tftp_set_socket_buffers(f, negotiated_block,
                                    negotiated_window, false);
            initial_reply = xmalloc(4);
            initial_reply->th_opcode = htons(ACK);
            initial_reply->th_block = 0;
            initial_reply_len = 4;
            xdelete(initial_packet);
            initial_packet_len = 0;
        } else if (opcode == DATA && n >= 4 &&
                   ntohs(initial_packet->th_block) == 1) {
            /* Peer ignored/rejected requested options. */
            segsize = SEGSIZE;
            window = 1;
            break;
        } else {
            goto wait_for_reply;
        }
        break;
    }

    xdelete(req);

    io = tftp_io_writer_start(file, convert, window, segsize, false);
    if (!io) {
        nak(-errno, NULL);
        err = EX_OSERR;
        goto abort;
    }

    /* +5 to allow overrun detection when MSG_TRUNC is not supported */
    datapkt = xmalloc(segsize + 5);
    xfer.blocksize = segsize;
    xfer.windowsize = window;
    xfer.rollover = 0;
    xfer.resend_oack = false;
    xfer.control = ack;
    xfer.control_size = 4;
    xfer.context = &context;
    xfer.ops = &client_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;
    tftp_xfer_recv(&xfer, datapkt, segsize + 5,
                   &initial_reply, initial_reply_len,
                   &initial_packet, initial_packet_len,
                   &result);
    amount = result.bytes;
    tftp_io_stop(io);
    io = NULL;

    switch (result.status) {
    case TFTP_XFER_BAD_DATA:
        nak(EBADOP, "Data packet too large");
        err = EX_PROTOCOL;
        break;
    case TFTP_XFER_WRITE_ERROR:
        nak(-result.error, NULL);
        err = EX_OSERR;
        break;
    case TFTP_XFER_SEND_ERROR:
        errno = result.error;
        perror("tftp: sendto");
        err = EX_OSERR;
        break;
    case TFTP_XFER_RECV_ERROR:
        errno = result.error;
        perror("tftp: recvfrom");
        err = EX_OSERR;
        break;
    case TFTP_XFER_PEER_ERROR:
        printf("Error code %d: %s\n", ntohs(result.packet->th_code),
               result.packet->th_msg);
        err = EX_PROTOCOL;
        break;
    default:
        break;
    }

  abort:
    tftp_io_stop(io);
    xdelete(datapkt);
    xdelete(initial_packet);
    xdelete(ack);
    xdelete(req);
    if (file)
        fclose(file);
    stopclock();
    if (amount > 0)
        printstats("Received", amount);
    return err;
}

static int
makerequest(struct tftphdr **pkt, int *optionlen_p,
            int request, const char *name, const char *mode,
            unsigned int requested_block, unsigned int requested_window)
{
    struct tftphdr *tp;
    char *cp;
    char optionbuf[TFTP_OPTION_SPACE];
    size_t namelen, modelen, optionlen;
    size_t pktsize;

    namelen = strlen(name) + 1;
    modelen = strlen(mode) + 1;
    cp = optionbuf;
    if (requested_block != SEGSIZE) {
        cp = mempcpy(cp, "blksize", sizeof "blksize");
        cp += snprintf(cp, sizeof optionbuf - (cp - optionbuf),
                       "%u", requested_block) + 1;
    }
    if (requested_window > 1) {
        cp = mempcpy(cp, "windowsize", sizeof "windowsize");
        cp += snprintf(cp, sizeof optionbuf - (cp - optionbuf),
                       "%u", requested_window) + 1;
    }
    optionlen = cp - optionbuf;
    if (optionlen_p)
        *optionlen_p = optionlen;

    /*
     * The request is encoded into tp as: opcode(2) name NUL mode NUL.
     * tpsize is the total size of the buffer tp points to; reject
     * anything that would not fit rather than overflowing it. Compare
     * with subtraction on the tpsize side only, so there is no risk of
     * namelen/modelen (both attacker/user-controlled) underflowing an
     * unsigned computation.
     */
    if (namelen + modelen + optionlen > MAX_SEGSIZE) {
        errno = ENAMETOOLONG;
        return -1;
    }

    pktsize = namelen + modelen + optionlen + 2;
    *pkt = tp = xmalloc(pktsize);

    tp->th_opcode = htons(request);
    cp = (char *)&(tp->th_stuff);
    cp = mempcpy(cp, name, namelen);
    cp = mempcpy(cp, mode, modelen);
    memcpy(cp, optionbuf, optionlen);
    return pktsize;
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

    length = make_errpacket(&tp, error, msg);

    if (copt.trace)
        tpacket("sent", tp, length);
    if (sendto(f, tp, length, 0, &peeraddr.sa,
               sizeof peeraddr) != length)
        perror("nak");

    xfree(tp);
}

static void tpacket(const char *s, const struct tftphdr *tp, int n)
{
    uint16_t op = ntohs(tp->th_opcode);
    const char *on;
    const char *pfx = "";

    printf("%s %s (%u): ", s, packet_type(op), op);
    switch (op) {
    case RRQ:
    case WRQ:
    {
        int fl, ml;
        const char *file, *mode;

        n -= 2;                 /* Skip header */
        file = tp->th_stuff;
        fl = n > 0 ? strnlen(file, n) : 0;
        n -= fl + 1;
        mode = file + fl + 1;
        ml = n > 0 ? strnlen(mode, n) : 0;
        n -= ml + 1;
        on = mode + ml + 1;
        printf("file \"%*s\", mode %*s", fl, file, ml, mode);
        pfx = ", ";
        goto print_options;
    }
    break;

    case DATA:
        printf("block %u, %d bytes\n", ntohs(tp->th_block), n - 4);
        break;

    case ACK:
        printf("block %u\n", ntohs(tp->th_block));
        break;

    case ERROR:
        printf("code %u, \"%*s\"\n", ntohs(tp->th_code), n-2, tp->th_msg);
        break;

    case OACK:
        n -= 2;                 /* Skip header */
        on = tp->th_stuff;
        goto print_options;

    print_options:
        while (n > 0) {
            const char *ov;
            int nl, vl;
            nl = strnlen(on, n);
            n -= nl + 1;
            ov = on + nl + 1;
            vl = n > 0 ? strnlen(ov, n) : 0;
            n -= vl + 1;
            on = ov + vl + 1;
            printf("%s%*s %*s", pfx, nl, on, vl, ov);
            pfx = ", ";
        }
        putchar('\n');
        break;

    default:
        if (n > 2)
            printf("%+d bytes\n", n-2);
        break;
    }
}

static uintmax_t tstart, tstop;

static inline void startclock(void)
{
    tstart = clock_us();
}

static inline void stopclock(void)
{
    tstop = clock_us();
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

    if (copt.verbose < 2) {
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
    if (copt.verbose) {
        double delta = (tstop - tstart) * 1.0e-6;
        bool with_suffix;

        fputs(direction, stdout);
        putchar(' ');
        if (copt.verbose > 1 || amount < 9999) {
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

    timeout <<= 1;
    if (timeout >= (unsigned long)copt.maxtimeout * USEC_PER_SEC) {
        printf("Transfer timed out.\n");
        errno = save_errno;
        siglongjmp(toplevel, EX_TEMPFAIL);
    }
    errno = save_errno;
    siglongjmp(*active_timeoutbuf, 1);
}
