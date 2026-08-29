/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#ifndef TFTP_XFER_H
#define TFTP_XFER_H

#include "tftpsubs.h"

enum tftp_xfer_status {
    TFTP_XFER_OK,
    TFTP_XFER_SEND_ERROR,
    TFTP_XFER_RECV_ERROR,
    TFTP_XFER_READ_ERROR,
    TFTP_XFER_WRITE_ERROR,
    TFTP_XFER_BAD_DATA,
    TFTP_XFER_PEER_ERROR
};

struct tftp_xfer_result {
    enum tftp_xfer_status status;
    int io_result;
    int error;
    uintmax_t bytes;
    uint16_t last_block;
    const struct tftphdr *packet;
};

/*
 * The transport callbacks keep packet I/O, timeout implementation, tracing,
 * and error reporting local to the client or daemon.  recv() is responsible
 * for its own timeout mechanism.  retry_enter() supplies its jump buffer to
 * that mechanism, and is called again after it transfers control there.
 */
struct tftp_xfer_ops {
    int (*send)(void *, const void *, int);
    int (*recv)(void *, void *, int);
    void (*received)(void *, const struct tftphdr *, int);
    void (*drain)(void *);
    void (*retry_enter)(void *, sigjmp_buf *, int);
    void (*retry_leave)(void *);
    void (*wait_begin)(void *);
    void (*flush)(void *);
};

struct tftp_xfer {
    FILE *file;
    int convert;
    unsigned int blocksize;
    unsigned int windowsize;
    uint16_t rollover;
    int resend_oack;
    void *control;
    int control_size;
    void *context;
    const struct tftp_xfer_ops *ops;
};

/*
 * Send a file after any request or OACK exchange is complete.
 */
void tftp_xfer_send(const struct tftp_xfer *, struct tftp_xfer_result *);

/*
 * Receive a file after any request or OACK exchange is complete.  ack is
 * the reusable ACK buffer.  initial_reply, when supplied, is sent before
 * waiting for DATA 1 (typically OACK or ACK 0).  initial_packet is DATA 1
 * that the request exchange has already received.
 */
void tftp_xfer_recv(const struct tftp_xfer *, struct tftphdr *ack,
                    const struct tftphdr *initial_reply,
                    int initial_reply_len, struct tftphdr *initial_packet,
                    int initial_packet_len, struct tftp_xfer_result *);

#endif /* TFTP_XFER_H */
