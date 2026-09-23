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

struct peer {
    union sock_addr addr;       /* will be modified with transfer ID */
    socklen_t addrlen;
    union sock_addr myaddr;     /* local address */
    int sock;                   /* socket file descriptor */
    bool connected;             /* connected to a transfer ID */
};
static struct peer peer = { .sock = -1 };

static uintmax_t timeout;
static sigjmp_buf timeoutbuf;
static sigjmp_buf *active_timeoutbuf = &timeoutbuf;

struct optreq {
    unsigned int options;       /* Bitmap of options present */
    unsigned int window;
    unsigned int blksize;
    off_t tsize;
};

/* Initialize a struct optreq with the default (no-option) values */
static inline struct optreq *optreq_init(struct optreq *optreq)
{
    optreq->options = 0;
    optreq->tsize   = -1;
    optreq->window  = 1;
    optreq->blksize = SEGSIZE;
    return optreq;
}

static int nak(const char *, int, const char *);
static int makerequest(struct tftphdr **, struct optreq *,
                       int, const char *, const char *);
static bool
parse_oack(struct optreq *optack, const struct optreq *optreq,
           const struct tftphdr *tp, int length);
static void printstats(const char *, uintmax_t);
static void startclock(void);
static void stopclock(void);
static void timer(int);
static void tpacket(const char *, const struct tftphdr *, int);

struct client_xfer_context {
    union sock_addr from;
    uintmax_t timeout;
};

static bool address_match(const union sock_addr *a, const union sock_addr *b)
{
    if (a->sa.sa_family != b->sa.sa_family)
        return false;

    switch (a->sa.sa_family) {
    case AF_INET:
        return a->si.sin_addr.s_addr == b->si.sin_addr.s_addr;
#ifdef HAVE_IPV6
    case AF_INET6:
        return !!IN6_ARE_ADDR_EQUAL(&a->s6.sin6_addr, &b->s6.sin6_addr);
#endif
    default:
        return false;           /* Not supported */
    }
}

static int client_recv_time(void *packet, int length, union sock_addr *from,
                            uintmax_t *timeout_us_p)
{
    socklen_t fromlen = sizeof(*from);
    int n;

    do {
        n = tftp_recv_time(peer.sock, packet, length, 0, &from->sa, &fromlen,
                           timeout_us_p);
        if (n < 0) {
            if (errno == ETIMEDOUT)
                timer(0);               /* Should not return */
            return n;
        }
    } while (n < 2 ||
             !(address_match(&peer.addr, from) ||
               (!peer.connected && copt.unsafe)));

    if (!peer.connected) {
        peer.addr = *from;
        peer.addrlen = fromlen;
        if (!connect(peer.sock, &from->sa, fromlen))
            peer.connected = true;
    }

    return n;
}

static int client_xfer_send(void *vctx, const void *packet, int length)
{
    (void)vctx;
    if (copt.trace)
        tpacket("sent", packet, length);
    return sendto(peer.sock, packet, length, 0, &peer.addr.sa,
                  peer.addrlen) == length ? 0 : -1;
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
        timeout = xopt.rexmtval;
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
 * Printing error messages
 */
static void print_server_error(const char *file,
                               const struct tftphdr *packet, int packet_len)
{
    char *error_str = errpkt_to_string(packet, packet_len);
    fprintf(stderr, "%s: %s: server error: %s\n",
            _progname, file, error_str);
    xfree(error_str);
}

static void print_os_error(const char *file, const char *what, int err)
{
    fprintf(stderr, "%s: %s: %s%s%s%s\n",
            _progname, file,
            what ? what : "",
            what ? " error" : "error",
            err ? ": " : "",
            err ? strerror(err) : "");
}

static void print_proto_error(const char *file, const char *what)
{
    fprintf(stderr, "%s: %s: server sent an invalid %s\n",
            _progname, file, what);
}

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

#define TFTP_OPTION_SPACE \
    (OPT_SPACE("blksize") + OPT_SPACE("windowsize") + OPT_SPACE("tsize"))

/*
 * The maximum size of a reply packet (ACK, ERROR, OACK or
 * default-sized DATA), that we care about. If an ERROR packet is
 * unreasonably long, just truncate it.
 */
#define TFTP_REPLY_MAX_PACKET_SIZE MAX(TFTP_OPTION_SPACE+2, SEGSIZE+4)

static int open_socket(const char *name);
static inline void close_socket(void)
{
    if (peer.sock >= 0) {
        close(peer.sock);
        peer.sock = -1;
    }
}

static int open_socket(const char *name)
{
    if (!serv.connected) {
        fprintf(stderr, "%s: %s%sno server host selected\n",
                _progname, name ? name : "", name ? ": " : "");
        return EX_USAGE;
    }

    close_socket();

    /*
     * Since the return port ("transfer ID") is unknown, the socket
     * cannot be connected at this point; set the peer.addr port
     * to 0 to reflect that.
     */
    peer.addr = serv.addr;
    peer.addrlen = serv.addrlen;
    peer.connected = false;

    xzero(peer.myaddr);
    peer.myaddr.sa.sa_family = peer.addr.sa.sa_family;

    peer.sock = socket(peer.myaddr.sa.sa_family, SOCK_DGRAM, 0);
    if (peer.sock < 0) {
        print_os_error(name, "socket", errno);
        return EX_OSERR;
    }

    if (pick_port_bind(peer.sock, &peer.myaddr)) {
        print_os_error(name, "bind", errno);
        close_socket();
        return EX_OSERR;
    }

    return 0;
}

/*
 * Send the requested file.
 */
int tftp_sendfile(int fd, const char *name, const char *mode)
{
    struct tftphdr * volatile response;
    struct tftphdr *req;
    union sock_addr from;
    FILE * volatile file = NULL;
    struct tftp_io * volatile io = NULL;
    struct client_xfer_context context;
    struct tftp_xfer xfer;
    struct tftp_xfer_result result;
    int n, size;
    volatile int err = 0;
    bool convert = !strcmp(mode, "netascii");
    struct optreq optreq, optack;
    uint16_t ap_opcode, ap_block;
    unsigned long r_timeout;
    volatile uintmax_t amount = 0;
    struct stat st;

    optreq_init(&optreq);
    optreq_init(&optack);

    err = open_socket(name);
    if (err)
        goto abort;

    if (!convert && !fstat(fd, &st) && S_ISREG(st.st_mode))
        optreq.tsize = st.st_size;

    response = xmalloc(TFTP_REPLY_MAX_PACKET_SIZE);

    startclock();
    file = fdopen(fd, convert ? "rt" : "rb");
    if (!file) {
        print_os_error(name, "fdopen", errno);
        close(fd);
        err = EX_OSERR;
        goto abort;
    }

    tftp_signal(SIGALRM, timer, 0);
    size = makerequest(&req, &optreq, WRQ, name, mode);
    if (size < 0) {
        print_os_error(name, "request", errno);
        err = EX_OSERR;
        goto abort;
    }

    /* A peer which ignores options answers a WRQ with ACK 0. */
    for (;;) {
        timeout = xopt.rexmtval;
        (void)sigsetjmp(timeoutbuf, 1);
        if (copt.trace)
            tpacket("sent", req, size);
        if (sendto(peer.sock, req, size, 0, &peer.addr.sa, peer.addrlen)
            != size) {
            print_os_error(name, "send", errno);
            err = EX_OSERR;
            goto abort;
        }
        r_timeout = timeout;
      wait_for_reply:
        n = client_recv_time(response, TFTP_REPLY_MAX_PACKET_SIZE,
                             &from, &r_timeout);
        if (n < 0) {
            print_os_error(name, "receive", errno);
            err = EX_OSERR;
            goto abort;
        }
        if (n < 2)
            goto wait_for_reply;
        if (copt.trace)
            tpacket("received", response, n);
        ap_opcode = ntohs(response->th_opcode);
        if (ap_opcode == ERROR) {
            print_server_error(name, response, n);
            err = EX_PROTOCOL;
            goto abort;
        }
        if (ap_opcode == OACK) {
            if (!parse_oack(&optack, &optreq, response, n)) {
                print_proto_error(name, "option response");
                nak(name, EOPTNEG, "Invalid option response");
                err = EX_PROTOCOL;
                goto abort;
            }
            break;
        }
        if (n >= 4) {
            ap_block = ntohs(response->th_block);
            if (ap_opcode == ACK && ap_block == 0) {
                /* Traditional server ignored options. */
                break;
            }
        }
        goto wait_for_reply;
    }

    xfree(req);
    tftp_set_socket_buffers(peer.sock, optack.blksize, optack.window, true);

    io = tftp_io_reader_start(file, convert, optack.window,
                              optack.window, optack.blksize, false);
    if (!io) {
        nak(name, -errno, NULL);
        err = EX_OSERR;
        goto abort;
    }

    xzero(xfer);
    xfer.blocksize = optack.blksize;
    xfer.windowsize = optack.window;
    xfer.max_bytes = UINTMAX_MAX;
    xfer.resend_oack = optack.window; /* THIS CAN'T BE RIGHT CHECK AGAIN */
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
        print_os_error(name, "file read", result.error);
        nak(name, -result.error, NULL);
        err = EX_OSERR;
        break;
    case TFTP_XFER_SEND_ERROR:
        print_os_error(name, "send", result.error);
        err = EX_OSERR;
        break;
    case TFTP_XFER_RECV_ERROR:
        print_os_error(name, "receive", result.error);
        err = EX_OSERR;
        break;
    case TFTP_XFER_PEER_ERROR:
    {
        print_server_error(name, result.packet, result.packet_len);
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
    close_socket();
    return err;
}

/*
 * Receive a file.
 */
int tftp_recvfile(int fd, const char *name, const char *mode)
{
    union sock_addr from;
    FILE * volatile file = NULL;
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
    struct optreq optreq, optack;
    uint16_t opcode;
    unsigned long r_timeout;
    volatile uintmax_t amount = 0;

    optreq_init(&optreq);
    optreq.tsize = 0;

    optreq_init(&optack);

    err = open_socket(name);
    if (err)
        goto abort;

    startclock();
    file = fdopen(fd, convert ? "wt" : "wb");
    if (!file) {
        print_os_error(name, "fdopen", errno);
        close(fd);
        err = EX_OSERR;
        goto abort;
    }
    initial_packet = xmalloc(TFTP_REPLY_MAX_PACKET_SIZE);

    size = makerequest(&req, &optreq, RRQ, name, mode);
    if (size < 0) {
        print_os_error(name, "request", errno);
        err = EX_OSERR;
        goto abort;
    }
    tftp_signal(SIGALRM, timer, 0);

    /* RFC 7440 peers answer with OACK; legacy peers start with DATA 1. */
    for (;;) {
        timeout = xopt.rexmtval;
        (void)sigsetjmp(timeoutbuf, 1);
        if (copt.trace)
            tpacket("sent", req, size);
        if (sendto(peer.sock, req, size, 0, &peer.addr.sa, peer.addrlen) != size) {
            print_os_error(name, "send", errno);
            err = EX_OSERR;
            goto abort;
        }
        r_timeout = timeout;
      wait_for_reply:
        n = client_recv_time(initial_packet, TFTP_REPLY_MAX_PACKET_SIZE,
                             &from, &r_timeout);
        if (n < 0) {
            print_os_error(name, "receive", errno);
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
            print_server_error(name, initial_packet, n);
            err = EX_PROTOCOL;
            goto abort;
        }
        if (opcode == OACK) {
            if (!parse_oack(&optack, &optreq, initial_packet, n)) {
                print_proto_error(name, "option response");
                nak(name, EOPTNEG, "Invalid option response");
                err = EX_PROTOCOL;
                goto abort;
            }
            initial_reply = initial_packet; /* Reuse buffer */
            initial_packet = NULL;
            initial_packet_len = 0;
            initial_reply->th_opcode = htons(ACK);
            initial_reply->th_block = 0;
            initial_reply_len = 4;
        } else if (opcode == DATA && n >= 4 &&
                   ntohs(initial_packet->th_block) == 1) {
            /* Peer ignored/rejected requested options. */
            break;
        } else {
            goto wait_for_reply;
        }
        break;
    }

    xdelete(req);
    tftp_set_socket_buffers(peer.sock, optack.blksize, optack.window, false);

    io = tftp_io_writer_start(file, convert, optack.window, optack.blksize,
                              false);
    if (!io) {
        nak(name, -errno, NULL);
        err = EX_OSERR;
        goto abort;
    }

    /* +5 to allow overrun detection when MSG_TRUNC is not supported */
    datapkt = xmalloc(optack.blksize + 5);

    xzero(xfer);
    xfer.blocksize = optack.blksize;
    xfer.windowsize = optack.window;
    xfer.max_bytes = UINTMAX_MAX;
    xfer.context = &context;
    xfer.ops = &client_xfer_ops;
    xfer.io_context = io;
    xfer.io_ops = &tftp_io_xfer_ops;
    tftp_xfer_recv(&xfer, datapkt, optack.blksize + 5,
                   &initial_reply, initial_reply_len,
                   &initial_packet, initial_packet_len,
                   &result);
    amount = result.bytes;
    tftp_io_stop(io);
    io = NULL;

    switch (result.status) {
    case TFTP_XFER_BAD_DATA:
        print_proto_error(name, "data packet");
        nak(name, EBADOP, "Data packet too large");
        err = EX_PROTOCOL;
        break;
    case TFTP_XFER_WRITE_ERROR:
        print_os_error(name, "file write", result.error);
        nak(name, -result.error, NULL);
        err = EX_OSERR;
        break;
    case TFTP_XFER_SEND_ERROR:
        print_os_error(name, "send", result.error);
        err = EX_OSERR;
        break;
    case TFTP_XFER_RECV_ERROR:
        print_os_error(name, "receive", result.error);
        err = EX_OSERR;
        break;
    case TFTP_XFER_PEER_ERROR:
        print_server_error(name, result.packet, result.packet_len);
        err = EX_PROTOCOL;
        break;
    default:
        break;
    }

    if (optack.tsize >= 0 && amount != (uintmax_t)optack.tsize) {
        fprintf(stderr, "%s: %s: warning: expected %"PRIuMAX" bytes "
                "received %"PRIuMAX"\n",
                _progname, name, (uintmax_t)optack.tsize, amount);
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
    close_socket();
    return err;
}

static int
makerequest(struct tftphdr **pkt, struct optreq *optreq,
            int request, const char *name, const char *mode)
{
    struct tftphdr *tp;
    char *cp;
    char optionbuf[TFTP_OPTION_SPACE];
    size_t namelen, modelen, optionlen;
    size_t pktsize;

    namelen = strlen(name) + 1;
    modelen = strlen(mode) + 1;
    cp = optionbuf;

    if (copt.no_options) {
        /* Reset all options */
        optreq_init(optreq);
    } else {
        /* Set default or configured options */
        optreq->options = 0;
        optreq->blksize = tftp_max_blksize(peer.sock, &peer.addr);
        optreq->window  = xopt.max_windowsize;

        if (optreq->blksize != SEGSIZE) {
            optreq->options |= 1 << PO_BLKSIZE;
            cp = mempcpy(cp, "blksize", sizeof "blksize");
            cp += snprintf(cp, sizeof optionbuf - (cp - optionbuf),
                           "%u", optreq->blksize) + 1;
        }
        if (optreq->window > 1) {
            optreq->options |= 1 << PO_WINDOWSIZE;
            cp = mempcpy(cp, "windowsize", sizeof "windowsize");
            cp += snprintf(cp, sizeof optionbuf - (cp - optionbuf),
                           "%u", optreq->window) + 1;
        } else {
            optreq->window = 1;
        }
        if (optreq->tsize >= 0) {
            optreq->options |= 1 << PO_TSIZE;
            cp = mempcpy(cp, "tsize", sizeof "tsize");
            cp += snprintf(cp, sizeof optionbuf - (cp - optionbuf),
                           "%"PRIuMAX, (uintmax_t)optreq->tsize) + 1;
        }
    }

    optionlen = cp - optionbuf;

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
parse_oack(struct optreq *optack, const struct optreq *optreq,
           const struct tftphdr *tp, int length)
{
    const char *cp, *end, *nul;
    char *value_end;
    uintmax_t value;

    optreq_init(optack);

    if (length <= 2 || ntohs(tp->th_opcode) != OACK)
        return false;

    cp = (const char *)&tp->th_stuff;
    end = (const char *)tp + length;
    while (cp < end) {
        /* Bitmask of repeated or non-requested options */
        const unsigned int badopts = optack->options | ~optreq->options;
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
        value = strtoumax(cp, &value_end, 10);
        if (errno || value_end != nul)
            return false;
        if (ascii_strcaseeq(option, "blksize")) {
            if ((badopts & (1 << PO_BLKSIZE)) ||
                value < MIN_SEGSIZE || value > optreq->blksize)
                return false;
            optack->blksize = value;
            optack->options |= 1 << PO_BLKSIZE;
        } else if (ascii_strcaseeq(option, "windowsize")) {
            if ((badopts & (1 << PO_WINDOWSIZE)) ||
                value < 1 || value > optreq->window)
                return false;
            optack->window  = value;
            optack->options |= 1 << PO_WINDOWSIZE;
        } else if (ascii_strcaseeq(option, "tsize")) {
            if (badopts & (1 << PO_TSIZE))
                return false;
            optack->tsize = value;
            optack->options = 1 << PO_TSIZE;
        } else {
            /* Server sent us a unknown, unrequested option response... */
            return false;
        }
        cp = nul + 1;
    }

    /*
     * This allows a null OACK packet, which is technically wrong, but
     * there really isn't any reason to disallow it, either.
     */
    return true;
}

/*
 * Send a nak packet (error message).
 *
 * Error code passed in is one of the standard TFTP codes, or a
 * negative errno value.
 */
static int nak(const char *name, int error, const char *msg)
{
    struct tftphdr *tp;
    int length;
    int err = 0;

    length = make_errpacket(&tp, error, msg);

    if (copt.trace)
        tpacket("sent", tp, length);
    if (sendto(peer.sock, tp, length, 0,
               &peer.addr.sa, peer.addrlen) != length) {
        err = errno;
        if (name)
            print_os_error(name, "send", err);
    }

    xfree(tp);
    return err;
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
    if (timeout > copt.maxtimeout) {
        errno = save_errno;
        siglongjmp(toplevel, EX_TEMPFAIL);
    }
    errno = save_errno;
    siglongjmp(*active_timeoutbuf, 1);
}
