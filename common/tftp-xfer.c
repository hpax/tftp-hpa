/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#include "tftp-xfer.h"

static void set_result(struct tftp_xfer_result *result,
                       enum tftp_xfer_status status, int error,
                       uintmax_t bytes)
{
    result->status = status;
    result->error = error;
    result->bytes = bytes;
}

static void finish(const struct tftp_xfer *xfer,
                   struct tftp_xfer_result *result,
                   enum tftp_xfer_status status, int error, uintmax_t bytes)
{
    xfer->ops->retry_leave(xfer->context);
    set_result(result, status, error, bytes);
}

static uint16_t next_block(uint16_t block, uint16_t rollover)
{
    if (!++block)
        block = rollover;
    return block;
}

static uintmax_t time_us(void)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (uintmax_t)now.tv_sec * 1000000 + now.tv_usec;
}

static unsigned long update_round_trip_time(unsigned long round_trip_time,
                                            uintmax_t sample)
{
    uintmax_t average;

    average = (uintmax_t)round_trip_time * FAST_ACK_MEMORY +
        sample * (FAST_ACK_MEMORY_SCALE - FAST_ACK_MEMORY);
    average /= FAST_ACK_MEMORY_SCALE;
    return average > ULONG_MAX ? ULONG_MAX : (unsigned long)average;
}

unsigned long tftp_fast_ack_timeout(unsigned long round_trip_time,
                                    unsigned long default_timeout)
{
    unsigned long threshold;
    unsigned long fast_timeout;

    threshold = default_timeout / (FAST_ACK_MAX * FAST_ACK_TIME);
    if (round_trip_time >= threshold)
        return 0;

    fast_timeout = round_trip_time * FAST_ACK_TIME;
    if (fast_timeout < default_timeout / FAST_ACK_MIN)
        fast_timeout = default_timeout / FAST_ACK_MIN;
    return fast_timeout;
}

void tftp_xfer_send(const struct tftp_xfer *xfer,
                    struct tftp_xfer_result *result)
{
    struct tftphdr * volatile dp;
    struct tftphdr *ap;
    sigjmp_buf retrybuf;
    volatile uint16_t block = 1;
    volatile uintmax_t bytes = 0;
    unsigned int packet_count;
    bool final;
    int n;
    int size;
    int resend_start;
    bool restarted;
    volatile unsigned long round_trip_time;
    volatile uintmax_t burst_started;
    uint16_t opcode;
    uint16_t packet_block;
    uint16_t expected_ack;

    result->last_block = 0;
    result->packet = NULL;
    round_trip_time = xfer->default_timeout;
    if (!xfer->io_ops) {
        errno = EINVAL;
        finish(xfer, result, TFTP_XFER_READ_ERROR, errno, 0);
        return;
    }

    for (;;) {
        if (xfer->io_ops->read_window(xfer->io_context, &packet_count,
                                      &final) < 0 ||
            !packet_count || packet_count > xfer->windowsize) {
            if (!errno)
                errno = EIO;
            finish(xfer, result, TFTP_XFER_READ_ERROR, errno, bytes);
            return;
        }
        for (n = 0; n < (int)packet_count; n++) {
            dp = xfer->io_ops->read_packet(xfer->io_context,
                                            (unsigned int)n);
            if (!dp) {
                if (!errno)
                    errno = EIO;
                finish(xfer, result, TFTP_XFER_READ_ERROR, errno,
                       bytes);
                return;
            }
            dp->th_opcode = htons((uint16_t)DATA);
            dp->th_block = htons(block);
            block = next_block(block, xfer->rollover);
        }

        restarted = !!sigsetjmp(retrybuf, 1);
        xfer->ops->retry_enter(xfer->context, &retrybuf, restarted);
        resend_start = 0;
      resend_window:
        burst_started = time_us();
        for (n = resend_start; (unsigned int)n < packet_count; n++) {
            dp = xfer->io_ops->read_packet(xfer->io_context,
                                            (unsigned int)n);
            size = xfer->io_ops->read_length(xfer->io_context,
                                              (unsigned int)n);
            if (!dp || size < 4 || size > (int)xfer->blocksize + 4) {
                if (!errno)
                    errno = EIO;
                finish(xfer, result, TFTP_XFER_READ_ERROR, errno,
                       bytes);
                return;
            }
            if (xfer->ops->send(xfer->context, dp, size) < 0) {
                finish(xfer, result, TFTP_XFER_SEND_ERROR, errno,
                       bytes);
                return;
            }
        }

        xfer->ops->wait_begin(xfer->context, round_trip_time,
                              xfer->default_timeout);
        for (;;) {
            n = xfer->ops->recv(xfer->context, xfer->control,
                                xfer->control_size);
            if (n < 0) {
                finish(xfer, result, TFTP_XFER_RECV_ERROR, errno,
                       bytes);
                return;
            }
            if (n < 4)
                continue;
            ap = xfer->control;
            xfer->ops->received(xfer->context, ap, n);
            opcode = ntohs(ap->th_opcode);
            packet_block = ntohs(ap->th_block);
            if (opcode == ERROR) {
                result->packet = ap;
                finish(xfer, result, TFTP_XFER_PEER_ERROR, 0, bytes);
                return;
            }
            dp = xfer->io_ops->read_packet(xfer->io_context,
                                            packet_count - 1);
            if (!dp) {
                if (!errno)
                    errno = EIO;
                finish(xfer, result, TFTP_XFER_READ_ERROR, errno,
                       bytes);
                return;
            }
            expected_ack = ntohs(dp->th_block);
            if (opcode == ACK && packet_block == expected_ack) {
                uintmax_t now = time_us();

                if (now >= burst_started)
                    round_trip_time = update_round_trip_time(
                        round_trip_time, now - burst_started);
                break;
            }
            if (opcode == OACK && xfer->resend_oack) {
                resend_start = 0;
                goto resend_window;
            }
            if (opcode == ACK) {
                /*
                 * An ACK within this window cumulatively confirms its
                 * prefix.  Retransmit only the remaining suffix.
                 */
                for (n = 0; (unsigned int)n + 1 < packet_count; n++) {
                    dp = xfer->io_ops->read_packet(xfer->io_context,
                                                    (unsigned int)n);
                    if (!dp) {
                        if (!errno)
                            errno = EIO;
                        finish(xfer, result, TFTP_XFER_READ_ERROR, errno,
                               bytes);
                        return;
                    }
                    if (packet_block == ntohs(dp->th_block)) {
                        resend_start = n + 1;
                        goto resend_window;
                    }
                }
            }
        }
        for (n = 0; (unsigned int)n < packet_count; n++) {
            bytes += xfer->io_ops->read_length(xfer->io_context,
                                               (unsigned int)n) - 4;
        }
        xfer->io_ops->read_release(xfer->io_context);
        if (final) {
            finish(xfer, result, TFTP_XFER_OK, 0, bytes);
            return;
        }
    }
}

void tftp_xfer_recv(const struct tftp_xfer *xfer, struct tftphdr *ack,
                    struct tftphdr *input, int input_size,
                    const struct tftphdr *initial_reply,
                    int initial_reply_len, struct tftphdr *initial_packet,
                    int initial_packet_len, struct tftp_xfer_result *result)
{
    struct tftphdr * volatile dp;
    const struct tftphdr *reply;
    const struct tftphdr *packet;
    sigjmp_buf retrybuf;
    volatile uint16_t block = 1;
    volatile uint16_t last_received = 0;
    volatile uint16_t last_acked = 0;
    uint16_t opcode;
    volatile uint16_t packet_block;
    volatile uintmax_t bytes = 0;
    volatile int packets_in_window = 0;
    volatile bool initial_reply_pending = initial_reply != NULL;
    volatile bool initial_packet_pending = initial_packet != NULL;
    volatile bool wait_started = false;
    volatile unsigned long round_trip_time;
    volatile uintmax_t ack_sent;
    int reply_len;
    int n;
    int size;
    bool final;
    bool restarted;

    result->last_block = 0;
    result->packet = NULL;
    round_trip_time = xfer->default_timeout;
    ack_sent = 0;
    if (!xfer->io_ops || !xfer->io_ops->write_finish) {
        errno = EINVAL;
        finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno, 0);
        return;
    }
    if (!input || input_size < TFTP_XFER_MAX_PACKET_SIZE) {
        errno = EINVAL;
        finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno, 0);
        return;
    }
    dp = xfer->io_ops->write_reserve(xfer->io_context);
    if (!dp) {
        if (!errno)
            errno = EIO;
        finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno, 0);
        return;
    }

    for (;;) {
        restarted = !!sigsetjmp(retrybuf, 1);
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
                finish(xfer, result, TFTP_XFER_SEND_ERROR, errno,
                       bytes);
                return;
            }
            ack_sent = time_us();
            xfer->ops->wait_begin(xfer->context, round_trip_time,
                                  xfer->default_timeout);
            wait_started = true;
        }

        if (initial_packet_pending) {
            n = initial_packet_len;
            initial_packet_pending = false;
            packet = initial_packet;
            opcode = ntohs(packet->th_opcode);
            packet_block = ntohs(packet->th_block);
        } else {
            for (;;) {
                n = xfer->ops->recv(xfer->context, input,
                                    TFTP_XFER_MAX_PACKET_SIZE);
                if (n < 0) {
                    finish(xfer, result, TFTP_XFER_RECV_ERROR, errno,
                           bytes);
                    return;
                }
                if (n < 4)
                    continue;
                xfer->ops->received(xfer->context, input, n);
                packet = input;
                opcode = ntohs(input->th_opcode);
                packet_block = ntohs(input->th_block);
                if (opcode == ERROR) {
                    result->packet = input;
                    finish(xfer, result, TFTP_XFER_PEER_ERROR, 0,
                           bytes);
                    return;
                }
                if (opcode != DATA)
                    continue;
                if (packet_block == block)
                    break;
                /*
                 * RFC 7440 recovery is driven by the last contiguous
                 * block, including when it falls within the current window.
                 */
                ack->th_opcode = htons((uint16_t)ACK);
                ack->th_block = htons(last_received);
                (void)xfer->ops->send(xfer->context, ack, 4);
            }
        }

        initial_reply_pending = false;
        if (n - 4 > (int)xfer->blocksize) {
            finish(xfer, result, TFTP_XFER_BAD_DATA, 0, bytes);
            return;
        }
        size = n - 4;
        memcpy(dp, packet, (size_t)n);
        if (xfer->io_ops->write_publish(xfer->io_context, size) < 0) {
            if (!errno)
                errno = EIO;
            finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno, bytes);
            return;
        }
        bytes += size;
        final = size != (int)xfer->blocksize;
        packets_in_window++;
        last_received = packet_block;
        block = next_block(block, xfer->rollover);
        if (final || packets_in_window == (int)xfer->windowsize) {
            uintmax_t now = time_us();

            if (wait_started && now >= ack_sent)
                round_trip_time = update_round_trip_time(round_trip_time,
                                                          now - ack_sent);
            last_acked = packet_block;
            packets_in_window = 0;
            ack->th_opcode = htons((uint16_t)ACK);
            ack->th_block = htons(last_acked);
            if (xfer->io_ops->write_drain(xfer->io_context) < 0) {
                if (!errno)
                    errno = EIO;
                finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno,
                       bytes);
                return;
            }
            if (final &&
                xfer->io_ops->write_finish(xfer->io_context) < 0) {
                if (!errno)
                    errno = EIO;
                finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno,
                       bytes);
                return;
            }
            if (xfer->ops->send(xfer->context, ack, 4) < 0) {
                finish(xfer, result, TFTP_XFER_SEND_ERROR, errno,
                       bytes);
                return;
            }
            ack_sent = time_us();
            xfer->ops->wait_begin(xfer->context, round_trip_time,
                                  xfer->default_timeout);
            wait_started = true;
            if (final) {
                result->last_block = last_acked;
                finish(xfer, result, TFTP_XFER_OK, 0, bytes);
                return;
            }
        }
        dp = xfer->io_ops->write_reserve(xfer->io_context);
        if (!dp) {
            if (!errno)
                errno = EIO;
            finish(xfer, result, TFTP_XFER_WRITE_ERROR, errno, bytes);
            return;
        }
    }
}
