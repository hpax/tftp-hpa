/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#include "tftp-xfer.h"

static void set_result(struct tftp_xfer_result *result,
                       enum tftp_xfer_status status, int io_result,
                       int error, uintmax_t bytes)
{
    result->status = status;
    result->io_result = io_result;
    result->error = error;
    result->bytes = bytes;
}

static void finish(const struct tftp_xfer *xfer,
                   struct tftp_xfer_result *result,
                   enum tftp_xfer_status status, int io_result, int error,
                   uintmax_t bytes)
{
    xfer->ops->retry_leave(xfer->context);
    set_result(result, status, io_result, error, bytes);
}

static uint16_t next_block(uint16_t block, uint16_t rollover)
{
    if (!++block)
        block = rollover;
    return block;
}

void tftp_xfer_send(const struct tftp_xfer *xfer,
                    struct tftp_xfer_result *result)
{
    struct tftphdr *dp;
    struct tftphdr *ap;
    char *packets;
    int *lengths;
    size_t packetsize;
    sigjmp_buf retrybuf;
    volatile uint16_t block = 1;
    volatile uintmax_t bytes = 0;
    volatile int packet_count;
    volatile int final;
    int n;
    int size;
    int restarted;
    uint16_t opcode;
    uint16_t packet_block;
    uint16_t expected_ack;

    result->last_block = 0;
    packetsize = ((size_t)xfer->blocksize + 5) & ~(size_t)1;
    packets = xcalloc(xfer->windowsize, packetsize);
    lengths = xcalloc(xfer->windowsize, sizeof(*lengths));
    dp = r_init();

    for (;;) {
        packet_count = 0;
        final = 0;
        do {
            size = readit(xfer->file, &dp, xfer->convert);
            if (size < 0) {
                finish(xfer, result, TFTP_XFER_READ_ERROR, size, errno,
                       bytes);
                goto out;
            }
            dp->th_opcode = htons((uint16_t)DATA);
            dp->th_block = htons(block);
            memcpy(packets + (size_t)packet_count * packetsize, dp,
                   (size_t)size + 4);
            lengths[packet_count++] = size + 4;
            read_ahead(xfer->file, xfer->convert);
            if (size != (int)xfer->blocksize)
                final = 1;
            block = next_block(block, xfer->rollover);
        } while (packet_count < (int)xfer->windowsize && !final);

        restarted = sigsetjmp(retrybuf, 1);
        xfer->ops->retry_enter(xfer->context, &retrybuf, restarted);
      resend_window:
        for (n = 0; n < packet_count; n++) {
            dp = (struct tftphdr *)(packets + (size_t)n * packetsize);
            if (xfer->ops->send(xfer->context, dp, lengths[n]) < 0) {
                finish(xfer, result, TFTP_XFER_SEND_ERROR, -1, errno,
                       bytes);
                goto out;
            }
        }

        xfer->ops->wait_begin(xfer->context);
        for (;;) {
            n = xfer->ops->recv(xfer->context, xfer->control,
                                xfer->control_size);
            if (n < 0) {
                finish(xfer, result, TFTP_XFER_RECV_ERROR, n, errno,
                       bytes);
                goto out;
            }
            if (n < 4)
                continue;
            ap = xfer->control;
            xfer->ops->received(xfer->context, ap, n);
            opcode = ntohs(ap->th_opcode);
            packet_block = ntohs(ap->th_block);
            if (opcode == ERROR) {
                finish(xfer, result, TFTP_XFER_PEER_ERROR, n, 0, bytes);
                goto out;
            }
            expected_ack = ntohs(((struct tftphdr *)
                                  (packets + (size_t)(packet_count - 1) *
                                   packetsize))->th_block);
            if (opcode == ACK && packet_block == expected_ack)
                break;
            if (opcode == OACK && xfer->resend_oack)
                goto resend_window;
            if (opcode == ACK)
                xfer->ops->drain(xfer->context);
        }
        for (n = 0; n < packet_count; n++)
            bytes += lengths[n] - 4;
        if (final) {
            finish(xfer, result, TFTP_XFER_OK, 0, 0, bytes);
            goto out;
        }
    }

  out:
    xfree(lengths);
    xfree(packets);
}

void tftp_xfer_recv(const struct tftp_xfer *xfer, struct tftphdr *ack,
                    const struct tftphdr *initial_reply,
                    int initial_reply_len, struct tftphdr *initial_packet,
                    int initial_packet_len, struct tftp_xfer_result *result)
{
    struct tftphdr *dp;
    const struct tftphdr *reply;
    sigjmp_buf retrybuf;
    volatile uint16_t block = 1;
    volatile uint16_t last_acked = 0;
    uint16_t opcode;
    volatile uint16_t packet_block;
    volatile uintmax_t bytes = 0;
    int packets_in_window = 0;
    int initial_reply_pending = initial_reply != NULL;
    volatile int initial_packet_pending = initial_packet != NULL;
    int reply_len;
    int n;
    int size;
    int final;
    int restarted;

    result->last_block = 0;
    dp = w_init();
    if (initial_packet_pending)
        dp = initial_packet;

    for (;;) {
        restarted = sigsetjmp(retrybuf, 1);
        xfer->ops->retry_enter(xfer->context, &retrybuf, restarted);
        if (initial_reply_pending || restarted) {
            if (initial_reply_pending) {
                reply = initial_reply;
                reply_len = initial_reply_len;
            } else {
                ack->th_opcode = htons((uint16_t)ACK);
                ack->th_block = htons(last_acked);
                reply = ack;
                reply_len = 4;
            }
            if (xfer->ops->send(xfer->context, reply, reply_len) < 0) {
                finish(xfer, result, TFTP_XFER_SEND_ERROR, -1, errno,
                       bytes);
                return;
            }
            xfer->ops->flush(xfer->context);
        }

        if (initial_packet_pending) {
            n = initial_packet_len;
            initial_packet_pending = 0;
        } else {
            xfer->ops->wait_begin(xfer->context);
            for (;;) {
                n = xfer->ops->recv(xfer->context, dp,
                                    MAX_SEGSIZE + 4);
                if (n < 0) {
                    finish(xfer, result, TFTP_XFER_RECV_ERROR, n, errno,
                           bytes);
                    return;
                }
                if (n < 4)
                    continue;
                xfer->ops->received(xfer->context, dp, n);
                opcode = ntohs(dp->th_opcode);
                packet_block = ntohs(dp->th_block);
                if (opcode == ERROR) {
                    finish(xfer, result, TFTP_XFER_PEER_ERROR, n, 0,
                           bytes);
                    return;
                }
                if (opcode != DATA)
                    continue;
                if (packet_block == block)
                    break;
                if (packet_block == last_acked) {
                    ack->th_opcode = htons((uint16_t)ACK);
                    ack->th_block = htons(last_acked);
                    (void)xfer->ops->send(xfer->context, ack, 4);
                }
            }
        }

        initial_reply_pending = 0;
        if (n - 4 > (int)xfer->blocksize) {
            finish(xfer, result, TFTP_XFER_BAD_DATA, n, 0, bytes);
            return;
        }
        size = writeit(xfer->file, &dp, n - 4, xfer->convert);
        if (size != n - 4) {
            finish(xfer, result, TFTP_XFER_WRITE_ERROR, size, errno,
                   bytes);
            return;
        }
        bytes += size;
        final = size != (int)xfer->blocksize;
        packets_in_window++;
        block = next_block(block, xfer->rollover);
        if (final || packets_in_window == (int)xfer->windowsize) {
            last_acked = packet_block;
            packets_in_window = 0;
            ack->th_opcode = htons((uint16_t)ACK);
            ack->th_block = htons(last_acked);
            if (xfer->ops->send(xfer->context, ack, 4) < 0) {
                finish(xfer, result, TFTP_XFER_SEND_ERROR, -1, errno,
                       bytes);
                return;
            }
            xfer->ops->flush(xfer->context);
            if (final) {
                result->last_block = last_acked;
                finish(xfer, result, TFTP_XFER_OK, 0, 0, bytes);
                return;
            }
        }
    }
}
