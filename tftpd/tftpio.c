/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 Intel Corporation; Author: H. Peter Anvin
 */

#include "config.h"
#include "tftpd.h"
#include "tftpio.h"

#ifdef HAVE_PTHREAD

#include <pthread.h>

enum io_direction {
    IO_READ,
    IO_WRITE
};

enum slot_state {
    SLOT_EMPTY,
    SLOT_RESERVED,
    SLOT_READY,
    SLOT_WRITING
};

struct tftpio {
    FILE *file;
    char *packets;
    int *lengths;
    enum slot_state *states;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    unsigned int slots;
    unsigned int packetsize;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    int convert;
    int error;
    int eof;
    int stopped;
    unsigned int held;
    int lock_initialized;
    int cond_initialized;
    int newline;
    int prevchar;
};

static unsigned int io_next(const struct tftpio *io, unsigned int n)
{
    return n + 1 == io->slots ? 0 : n + 1;
}

static void io_error(struct tftpio *io, int error)
{
    if (!error)
        error = EIO;
    if (!io->error)
        io->error = error;
}

static int read_packet(struct tftpio *io, struct tftphdr *dp)
{
    char *p;
    int c;
    unsigned int i;
    ssize_t n;

    if (!io->convert) {
        n = read(fileno(io->file), dp->th_data, io->packetsize - 4);
        if (n < 0)
            return -1;
        return (int)n;
    }

    p = dp->th_data;
    for (i = 0; i < io->packetsize - 4; i++) {
        if (io->newline) {
            c = io->prevchar == '\n' ? '\n' : '\0';
            io->newline = 0;
        } else {
            c = getc(io->file);
            if (c == EOF) {
                if (ferror(io->file)) {
                    errno = EIO;
                    return -1;
                }
                break;
            }
            if (c == '\n' || c == '\r') {
                io->prevchar = c;
                c = '\r';
                io->newline = 1;
            }
        }
        *p++ = c;
    }
    return (int)(p - dp->th_data);
}

static int write_packet(struct tftpio *io, const struct tftphdr *dp, int count)
{
    const char *p;
    int c;
    int ct;
    ssize_t n;

    if (!io->convert) {
        p = dp->th_data;
        ct = count;
        while (ct) {
            n = write(fileno(io->file), p, (size_t)ct);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (!n) {
                errno = EIO;
                return -1;
            }
            p += n;
            ct -= (int)n;
        }
        return count;
    }

    p = dp->th_data;
    ct = count;
    while (ct--) {
        c = *p++;
        if (io->prevchar == '\r') {
            if (c == '\n') {
                if (fseek(io->file, -1, SEEK_CUR)) {
                    errno = EIO;
                    return -1;
                }
            } else if (c == '\0') {
                io->prevchar = c;
                continue;
            }
        }
        if (putc(c, io->file) == EOF) {
            errno = EIO;
            return -1;
        }
        io->prevchar = c;
    }
    return count;
}

static void *reader_thread(void *arg)
{
    struct tftpio *io = arg;
    struct tftphdr *dp;
    int length;

    for (;;) {
        pthread_mutex_lock(&io->lock);
        while (io->count == io->slots && !io->stopped)
            pthread_cond_wait(&io->changed, &io->lock);
        if (io->stopped) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        dp = (struct tftphdr *)(io->packets +
                                (size_t)io->tail * io->packetsize);
        pthread_mutex_unlock(&io->lock);

        length = read_packet(io, dp);

        pthread_mutex_lock(&io->lock);
        if (length < 0) {
            io_error(io, errno);
            pthread_cond_broadcast(&io->changed);
            pthread_mutex_unlock(&io->lock);
            break;
        }
        io->lengths[io->tail] = length + 4;
        io->tail = io_next(io, io->tail);
        io->count++;
        if (length != (int)(io->packetsize - 4))
            io->eof = 1;
        pthread_cond_broadcast(&io->changed);
        if (io->eof || io->stopped) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        pthread_mutex_unlock(&io->lock);
    }
    return NULL;
}

static void *writer_thread(void *arg)
{
    struct tftpio *io = arg;
    struct tftphdr *dp;
    int length;

    for (;;) {
        pthread_mutex_lock(&io->lock);
        while (io->states[io->head] != SLOT_READY &&
               !io->stopped && !io->error)
            pthread_cond_wait(&io->changed, &io->lock);
        if (io->stopped || io->error) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        io->states[io->head] = SLOT_WRITING;
        dp = (struct tftphdr *)(io->packets +
                                (size_t)io->head * io->packetsize);
        length = io->lengths[io->head];
        pthread_mutex_unlock(&io->lock);

        if (write_packet(io, dp, length) != length) {
            pthread_mutex_lock(&io->lock);
            io_error(io, errno);
            pthread_cond_broadcast(&io->changed);
            pthread_mutex_unlock(&io->lock);
            break;
        }

        pthread_mutex_lock(&io->lock);
        io->states[io->head] = SLOT_EMPTY;
        io->head = io_next(io, io->head);
        io->count--;
        pthread_cond_broadcast(&io->changed);
        pthread_mutex_unlock(&io->lock);
    }
    return NULL;
}

static struct tftpio *io_start(FILE *file, int convert, unsigned int slots,
                               unsigned int blocksize,
                               enum io_direction direction)
{
    struct tftpio *io;
    int error;

    io = xcalloc(1, sizeof(*io));
    io->file = file;
    /*
     * The read ring has one spare slot.  It preserves the current
     * retransmission window while allowing the I/O thread to prefetch the
     * first packet of the next window.
     */
    io->slots = direction == IO_READ ? slots + 1 : slots;
    io->packetsize = ((size_t)blocksize + 5) & ~(size_t)1;
    io->convert = convert;
    io->prevchar = -1;
    io->packets = xcalloc(io->slots, io->packetsize);
    io->lengths = xcalloc(io->slots, sizeof(*io->lengths));
    if (direction == IO_WRITE)
        io->states = xcalloc(io->slots, sizeof(*io->states));

    error = pthread_mutex_init(&io->lock, NULL);
    if (!error)
        io->lock_initialized = 1;
    if (!error)
        error = pthread_cond_init(&io->changed, NULL);
    if (!error)
        io->cond_initialized = 1;
    if (!error)
        error = pthread_create(&io->thread, NULL,
                               direction == IO_READ ? reader_thread : writer_thread,
                               io);
    if (!error)
        return io;

    errno = error;
    if (io->cond_initialized)
        pthread_cond_destroy(&io->changed);
    if (io->lock_initialized)
        pthread_mutex_destroy(&io->lock);
    xfree(io->states);
    xfree(io->lengths);
    xfree(io->packets);
    xfree(io);
    return NULL;
}

struct tftpio *tftpio_reader_start(FILE *file, int convert, unsigned int slots,
                                   unsigned int blocksize)
{
    return io_start(file, convert, slots, blocksize, IO_READ);
}

int tftpio_reader_window(struct tftpio *io, unsigned int *count, int *final)
{
    int error;

    pthread_mutex_lock(&io->lock);
    while (io->count < io->slots && !io->eof && !io->error)
        pthread_cond_wait(&io->changed, &io->lock);
    error = io->error;
    if (!error) {
        io->held = io->count < io->slots - 1 ? io->count : io->slots - 1;
        *count = io->held;
        *final = io->eof && io->held == io->count;
    }
    pthread_mutex_unlock(&io->lock);
    if (error)
        errno = error;
    return error ? -error : 0;
}

struct tftphdr *tftpio_reader_packet(struct tftpio *io, unsigned int n)
{
    return (struct tftphdr *)(io->packets +
                              (size_t)((io->head + n) % io->slots) *
                              io->packetsize);
}

int tftpio_reader_length(struct tftpio *io, unsigned int n)
{
    return io->lengths[(io->head + n) % io->slots];
}

void tftpio_reader_release(struct tftpio *io)
{
    pthread_mutex_lock(&io->lock);
    if (io->held) {
        while (io->held) {
            io->head = io_next(io, io->head);
            io->count--;
            io->held--;
        }
        io->held = 0;
        pthread_cond_broadcast(&io->changed);
    }
    pthread_mutex_unlock(&io->lock);
}

struct tftpio *tftpio_writer_start(FILE *file, int convert, unsigned int slots)
{
    return io_start(file, convert, slots, MAX_SEGSIZE, IO_WRITE);
}

struct tftphdr *tftpio_writer_reserve(struct tftpio *io)
{
    struct tftphdr *dp;

    pthread_mutex_lock(&io->lock);
    while (io->states[io->tail] != SLOT_EMPTY && !io->error)
        pthread_cond_wait(&io->changed, &io->lock);
    if (io->error) {
        errno = io->error;
        pthread_mutex_unlock(&io->lock);
        return NULL;
    }
    io->states[io->tail] = SLOT_RESERVED;
    dp = (struct tftphdr *)(io->packets +
                            (size_t)io->tail * io->packetsize);
    pthread_mutex_unlock(&io->lock);
    return dp;
}

void tftpio_writer_publish(struct tftpio *io, int length)
{
    pthread_mutex_lock(&io->lock);
    io->lengths[io->tail] = length;
    io->states[io->tail] = SLOT_READY;
    io->tail = io_next(io, io->tail);
    io->count++;
    pthread_cond_broadcast(&io->changed);
    pthread_mutex_unlock(&io->lock);
}

int tftpio_writer_drain(struct tftpio *io)
{
    int error;

    pthread_mutex_lock(&io->lock);
    while (io->count && !io->error)
        pthread_cond_wait(&io->changed, &io->lock);
    error = io->error;
    pthread_mutex_unlock(&io->lock);
    if (!error && fflush(io->file)) {
        error = errno ? errno : EIO;
        pthread_mutex_lock(&io->lock);
        io_error(io, error);
        pthread_mutex_unlock(&io->lock);
    }
    if (error)
        errno = error;
    return error ? -error : 0;
}

void tftpio_stop(struct tftpio *io)
{
    if (!io)
        return;
    pthread_mutex_lock(&io->lock);
    io->stopped = 1;
    pthread_cond_broadcast(&io->changed);
    pthread_mutex_unlock(&io->lock);
    (void)pthread_join(io->thread, NULL);
    pthread_cond_destroy(&io->changed);
    pthread_mutex_destroy(&io->lock);
    xfree(io->states);
    xfree(io->lengths);
    xfree(io->packets);
    xfree(io);
}

#endif /* HAVE_PTHREAD */
