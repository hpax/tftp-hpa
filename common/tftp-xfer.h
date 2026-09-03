/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#ifndef TFTP_XFER_H
#define TFTP_XFER_H

#include "tftpsubs.h"

#define TFTP_XFER_MAX_PACKET_SIZE (MAX_SEGSIZE + 4)

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
    int error;
    uintmax_t bytes;
    uint16_t last_block;
    /* Caller-owned control or input packet storage. */
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
    void (*retry_enter)(void *, sigjmp_buf *, bool);
    void (*retry_leave)(void *);
    void (*wait_begin)(void *);
};

/*
 * The file I/O adapter.  The transfer engine owns the protocol state, while
 * the adapter can pipeline file I/O independently.  A read window remains
 * valid until read_release() and a write packet remains reserved until
 * write_publish().  write_finish() is called after draining only the final
 * receive window and before its ACK.  All callbacks return -1 and set errno
 * on failure, except for callbacks declared void.  write_reserve() must
 * provide room for a TFTP header and xfer->blocksize bytes of data.
 */
struct tftp_xfer_io_ops {
    int (*read_window)(void *, unsigned int *, bool *);
    struct tftphdr *(*read_packet)(void *, unsigned int);
    int (*read_length)(void *, unsigned int);
    void (*read_release)(void *);
    struct tftphdr *(*write_reserve)(void *);
    int (*write_publish)(void *, int);
    int (*write_drain)(void *);
    int (*write_finish)(void *);
};

struct tftp_xfer {
    unsigned int blocksize;
    unsigned int windowsize;
    uint16_t rollover;
    bool resend_oack;
    void *control;
    int control_size;
    void *context;
    const struct tftp_xfer_ops *ops;
    void *io_context;
    const struct tftp_xfer_io_ops *io_ops;
};

/*
 * Send a file after any request or OACK exchange is complete.
 */
void tftp_xfer_send(const struct tftp_xfer *, struct tftp_xfer_result *);

/*
 * Receive a file after any request or OACK exchange is complete.  ack is
 * the reusable ACK buffer.  input is caller-owned storage, separate from the
 * I/O adapter, with a capacity of at least TFTP_XFER_MAX_PACKET_SIZE;
 * received data is validated and copied to the adapter's reserved storage.
 * input must not overlap initial_reply and must remain valid through any use
 * of result.packet.  initial_reply, when supplied, is sent before waiting
 * for DATA 1 (typically OACK or ACK 0).  initial_packet is DATA 1 that the
 * request exchange has already received.
 */
void tftp_xfer_recv(const struct tftp_xfer *, struct tftphdr *ack,
                    struct tftphdr *input, int input_size,
                    const struct tftphdr *initial_reply,
                    int initial_reply_len, struct tftphdr *initial_packet,
                    int initial_packet_len, struct tftp_xfer_result *);

#endif /* TFTP_XFER_H */
